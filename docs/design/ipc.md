# IPC

IPC is the kernel's most-executed code path. In a microkernel-leaning
design like Quantum's — where drivers, the filesystem layer, network
stack, compositor, audio service, and the user's session all run as
distinct cap-isolated tasks — every interaction crosses an IPC. If IPC
costs 5 μs we have a 1990s microkernel and a slow desktop. If IPC costs
400 ns we have a fast desktop. The L4 family showed in the early '90s
that IPC can be that fast; we keep that bar and add caps, async, and
typed protocols on top.

## Premise

IPC is not a generic "send some bytes" primitive. It is the substrate for
all cross-isolation interaction, and it carries:

- **Capabilities** — caps move between tasks via IPC. The kernel
  accounts and re-tables them; the receiver gets fresh handles.
- **Memory objects** — bulk data is transferred by handing off a memory
  object cap, not by copy. Zero-copy is the default for anything
  larger than a few cache lines.
- **Type tags** — the endpoint declares a protocol; the kernel enforces
  message shape (cap count, payload size, tag). It does not interpret
  payload semantics.
- **Schedule** — sync IPC executes a direct switch (see
  [tasks_and_scheduling.md#direct-switch-on-ipc](tasks_and_scheduling.md#direct-switch-on-ipc)),
  collapsing the call into a near-immediate handoff.

## Endpoints

An `Endpoint` is the IPC primitive. Every IPC goes through one. An
endpoint has:

```rust
pub struct Endpoint {
    id: EndpointId,
    mode: EndpointMode,         // Sync | Async | Replyable
    protocol: ProtocolTag,      // for type-shape enforcement
    queue: MessageQueue,        // empty for pure-sync endpoints
    rights_required: Rights,    // sender must hold matching rights
}
```

Endpoints are owned by one task (the *receiver*); senders hold
delegated caps to it. A task can own many endpoints — typically one per
service it offers.

### Modes

- **Sync** — sender blocks until receiver accepts and replies. Direct
  switch where possible. Lowest latency; tightest coupling.
- **Async** — sender enqueues, never blocks. Receiver pulls at its own
  scheduling. Sender gets a `Future` cap if a reply is expected; the
  reply arrives via a wake on a separate endpoint.
- **Replyable** — like Sync, but the receiver can hand off the *reply
  capability* to a third task. Used for handoff patterns ("ask service
  A; A delegates to B which replies directly to caller"). This is the
  L4 reply-cap pattern and is needed for performant chains.

### Wire format

A message is small by design:

```rust
pub struct Message {
    tag:       u32,                // protocol-defined
    payload:   [u64; 6],           // up to ~48 bytes inline
    caps:      ArrayVec<DynCap, 4>,// up to 4 caps inline
    bulk:      Option<MemObjectCap>,// zero or one memory object
}
```

Inline payload + caps fit in registers / a single cache line and pass
through the sync fast path without a memory write to a shared queue.
Anything bigger goes via the `bulk` memory object — a single cap
transfer covers any size of data, with no per-byte cost in the
kernel.

We pick six u64s and four caps as a balance: covers the vast majority
of real RPCs (the kernel's syscall ABI fits in this), and is small
enough to live entirely in registers on x86_64 (six argument registers
+ return) and AArch64 (eight argument registers).

### Protocols

A `ProtocolTag` is a stable identifier (defined per service, registered
at endpoint creation time) that the kernel uses to enforce shape:

- Min/max payload bytes.
- Min/max cap count.
- Whether `bulk` is permitted.
- Reply protocol tag, if any.

The kernel does not interpret payload contents. It does not know what
"AudioBuf" is. It enforces that messages on an "AudioBuf" endpoint
match the declared shape — that's the integrity boundary. Semantics
are sender/receiver business.

A protocol can be versioned. The endpoint cap's rights set includes the
acceptable version range; admission rejects mismatches. This gives us
schema evolution without ABI breakage in the kernel.

## Operations

### Send (sync)

```rust
fn ipc_call(
    endpoint: CapHandle,
    msg: &Message,
) -> Result<Message, IpcError>;
```

Hot path:

1. Look up endpoint cap, validate rights.
2. Validate message against protocol tag.
3. Check for a waiting receiver:
   - Yes (common): direct switch into receiver, transfer message in
     registers + cap-table updates. Receiver runs; on reply, switch
     back. Round-trip ~400 ns.
   - No: enqueue sender on the endpoint's blocked-sender queue, mark
     `Awaiting(EndpointSender)`, schedule. Receiver pulls at next
     `recv`.
4. On reply, message returned to caller, scheduling resumes.

### Send (async)

```rust
fn ipc_post(
    endpoint: CapHandle,
    msg: Message,
) -> Result<Option<FutureCap>, IpcError>;
```

Sender doesn't block. Returns `Some(future)` if the protocol declares a
reply; the future is woken when the reply arrives. Returns `None` for
fire-and-forget endpoints.

Async cost: enqueue + (cross-core) IPI to wake receiver. ~600-1000 ns.
The async path exists for cases where sync would create avoidable
coupling (event publishing, notifications, log writes).

### Receive

```rust
fn ipc_recv(
    endpoint: CapHandle,
) -> Result<(Message, ReplyCap), IpcError>;
```

Receiver gets the message and a `ReplyCap` — a one-shot cap that
delivers the reply. `ReplyCap` is the capability holder for the
"reply slot" of the call; replying consumes it.

For Replyable mode, the receiver may delegate the `ReplyCap` to
another task, which then issues the reply directly to the caller. This
is the handoff pattern.

### Reply

```rust
fn ipc_reply(
    reply: ReplyCap,
    msg: Message,
) -> Result<(), IpcError>;
```

Consumes the reply cap; delivers the message. For sync calls, this
direct-switches back to the original caller.

### Receive with timeout

Recv doesn't take a timeout argument. Timed waits are built by the
caller using a clock cap and the async primitive: register a wake on
the endpoint, register a wake on a deadline timer, take whichever
fires first. The kernel deduplicates spurious wakes via the task's
state CAS; whichever loser fires becomes a no-op.

This is more code at the call site than `recv(timeout)` but avoids
welding timer semantics into IPC. It also produces better profiling
data — you can see which wake source fired.

## Capability transfer

Caps can ride messages. The transfer is part of the kernel-side ipc
hot path:

1. For each cap in `msg.caps`:
   - Look up sender's slot.
   - Decide mode: Move (default), Copy (if rights allow + flag set),
     or Grant.
   - Write to receiver's cap table; for Move, blank the sender's slot.
2. The receiver sees a fresh `CapHandle` for each transferred cap —
   the integer is local to the receiver's table.

Cap-bearing messages are still cheap: writing four cap-table entries
is < 50 cycles. The latency impact of carrying caps is dominated by
the cap-table cache locality, not the transfer itself.

For `bulk`, the same: the memory object's cap moves to the receiver,
who then maps it (in their address space, via `AddressSpace::map`)
when they want to read it. Mapping is a HAL call; the kernel doesn't
read or write the bytes.

## Direct switch — the fast path in detail

Sync IPC's direct switch is the single most-tuned operation in the
kernel. The flow:

```
Caller A (Running on core C)
    │
    │ ipc_call(ep, msg)
    │
    ▼
[A's frame saved into A.frame; A.state = Awaiting(Reply)]
    │
    │ Direct hand-off: receiver R is Awaiting(Endpoint EP)
    │
    ▼
[R.state = Running; R.cont = "deliver msg to recv site"]
[R.frame restored, msg + caps written to R's recv buffers]
[Quantum charged to R's CBS pool from this point]
    │
    ▼
R runs. Eventually:
    │
    │ ipc_reply(reply, reply_msg)
    │
    ▼
[R.frame saved; R.state = Awaiting(Endpoint EP)]
[A.state = Running; A.cont = "deliver reply"; A.frame restored]
[Quantum back to A's CBS pool]
    │
    ▼
A continues from after ipc_call.
```

We charge CBS to the running party at any moment. This is a clean
isolation rule: B cannot launder CPU through A. Frame deadline
*does* inherit (A has a deadline; B inherits for the call).

Implementation tricks:

- The continuation pointers ("deliver msg to recv site", "deliver
  reply") are pre-computed at endpoint setup time, not synthesized
  per-call.
- The receiver buffers (where `msg` lands) are pre-pinned in the
  receiver's task struct, not allocated.
- Payload (six u64s) goes in registers; the kernel does not write it
  to memory at all on the sync hot path.
- Cap transfer is a small fixed-size table copy (four slots × ~16
  bytes each).

The result on a modern x86_64: ~400 ns round-trip for a hot
sync IPC. On AArch64 we expect similar; the register file is bigger
but the pipeline is shorter.

## Cross-core IPC

When sender and receiver are on different cores:

1. Validate cap, build message in sender's frame.
2. Send a `WakeMessage` to receiver's home core via `InterCore`
   (HAL: x86 IPI, AArch64 SGI).
3. The wake message references the message location (sender's task
   struct) and the receiver task id.
4. Sender's core: enqueue sender as `Awaiting(Reply)`, return to
   scheduler. Sender's quantum continues to other ready work.
5. Receiver's core: IPI handler wakes the receiver task; receiver
   pulls the message at its next scheduling slot.

Cost: ~800-1500 ns one-way for the wake. We deliberately don't direct-
switch across cores because the cache pinball is worse than the wake-
and-go cost.

## Async completion model

For non-RPC patterns (subscribe, publish, notify, signal):

- Endpoint in async mode.
- Sender posts; receiver pulls when ready.
- No reply cap; messages are one-way.

Async also covers "I want to do work and check back later":

```rust
let fut = ipc_post(service, request)?;  // returns FutureCap
// do other work
let reply = fut.await;                  // suspends until reply
```

This is the same shape Rust-async users see in their applications. We
don't have separate kernel concepts for "async I/O" vs "futures" vs
"channels" — they all reduce to endpoints with appropriate modes and
wake semantics.

## Bulk transfer

The `bulk` field of `Message` is a single memory object cap. The
receiver, on receipt:

- Holds the cap in its cap table.
- Maps it into its address space (via `AddressSpace::map`) when needed.
- The mapping is a HAL operation: it might be lazy (mapped on first
  access) or eager.

Bulk isn't copy. The data is at *one* physical location; sender and
receiver both see it through their own address spaces. If the sender
wants to keep using the data, it should `derive` a sub-object first
and ship the derivative. If the sender wants the data gone after
transfer (handoff semantics), Move the cap and lose access.

Coherence is the HAL's problem. On cache-coherent shared memory (every
modern CPU/iGPU pair) it's free. On dGPU or non-coherent CXL.mem the
HAL emits the appropriate cache flush / invalidation; the IPC path
exposes a `coherence: CoherenceMode` field so senders can declare
intent (default: producer-side flush; receiver-side invalidate before
read).

## Notifications and edges

Some IPC patterns are pure signal — "the buffer is ready", "the timer
fired" — with no data. We model these as **notifications** on a
notification-mode endpoint:

```rust
pub struct Notification {
    set: AtomicU64,    // edge bitmap
}

fn notify(ep: CapHandle, bits: u64) -> Result<()>;
fn wait_notify(ep: CapHandle) -> Result<u64>;  // returns and clears the set
```

Lighter than a full message: no payload, no caps, just a 64-bit set
of edges. Wakes are coalesced — multiple `notify(ep, bits)` between
two `wait_notify` calls produce one wake with the OR of all bits.

This is the seL4 `Notification` primitive. We import it because it's
the right shape for interrupt delivery, completion signals from
hardware queues (NVMe, NIC), and any place where loss of edge count
is fine but loss of *any* edge is not.

## Error handling

IPC errors are not exceptions; they are typed return values. Categories:

- `EndpointGone` — receiver task terminated; cap is now inert.
- `ProtocolMismatch` — message shape doesn't match endpoint protocol.
- `RightsInsufficient` — sender's cap doesn't carry the required
  rights.
- `BudgetExceeded` — endpoint has a per-period rate limit; sender hit
  it.
- `Interrupted` — sender was cap-revoked or terminated mid-call.

The kernel does not expose timeout as an error directly (timeouts are
the caller's composition).

## Performance expectations

| Operation | Same-core cost | Cross-core cost |
|---|---|---|
| Sync call (no caps, no bulk) | ~400 ns rt | ~1500 ns rt |
| Sync call (4 caps) | ~500 ns rt | ~1700 ns rt |
| Sync call (with bulk; mapping deferred) | ~450 ns rt | ~1600 ns rt |
| Async post + wake | ~250 ns | ~1000 ns |
| Notification | ~150 ns | ~800 ns |

These are targets, not measurements. The L4 family hits comparable
numbers in production; we lose a few hundred nanoseconds to cap
checking and protocol validation that L4 doesn't do, and gain those
back in API ergonomics.

## What we deliberately don't have

- **Generic message queues with growable buffers in the kernel.** All
  buffering is fixed-size; senders block (sync) or fail (async,
  back-pressured) when the queue is full.
- **Mach-style port sets, port rights bitmasks.** Caps subsume this.
- **`SIGIO`, `signalfd`, `eventfd`.** The async + notification model
  covers these patterns.
- **Pipes / sockets as kernel primitives.** Userspace can layer them on
  endpoints with bulk objects.
- **Shared memory as a separate API.** Memory objects + bulk transfer
  is the same thing.

## Open questions

- **Reply-cap revocation under sender termination.** If A sends to B
  and dies before B replies, the reply cap should become inert. We
  track outstanding reply caps per task; on term, revoke. Cost
  appears to be small (one walk per terminated task).
- **Cap-transfer atomicity under failure.** If a message has 4 caps
  and the cap table runs out of slots on the third, we need to
  unwind the first two. Implementation: stage transfers in a per-IPC
  buffer, commit at the end. Costs a small extra copy.
- **GPU IPC.** A GPU cannot run our IPC path natively. Submission
  queues to a GPU are themselves modeled as endpoints — task → GPU is
  an async post; GPU → task on completion is a notification. This
  collapses GPU work into the same model. Needs validation against
  a real GPU driver shape.
