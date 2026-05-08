# Capabilities

Capabilities are the only authority in Quantum. There is no global PID, no
file descriptor table, no `chmod`, no ambient `root`. A task can act on a
resource only if it holds an unforgeable, typed, rights-bearing reference to
it. The cap model is the substrate; everything else
([memory objects](memory_objects.md), [tasks and scheduling](tasks_and_scheduling.md),
[IPC](ipc.md), [accounting](accounting.md)) is layered on it.

## Premise

Capability models have been studied since the 1960s (Dennis & Van Horn) and
shipped at scale in KeyKOS, EROS, seL4, Genode, and (partially) FreeBSD's
Capsicum. The lessons we keep:

- **No ambient authority.** A subject's powers are exactly the caps it
  holds, no more.
- **Typed.** Caps name a kind of resource; ops are dispatched by type.
- **Rights as a strict subset of intrinsic.** Derivation only narrows.
- **Hierarchical revocation.** Pulling a parent cap pulls its descendants.
- **Delegation is explicit.** No accidental authority leak through global
  state.

The lessons we update:

- **Compile-time type checks where possible.** seL4 checks rights at
  runtime because it has to (C). We are in Rust; the kernel can encode a
  great deal of cap-rights composition in the type system, with a
  runtime table only at the user/kernel boundary and where dynamic
  delegation forces it.
- **Sealed values for principals and tokens.** Cleaner than reinventing
  per-resource ACLs.
- **Caps carry budget.** Quotas (CPU period/budget, memory bytes, IPC
  rate) are part of the cap, not external policy. See
  [accounting.md](accounting.md).

## Inside the kernel

`Cap<T, R>` is a phantom-typed reference checked at compile time:

```rust
pub struct Cap<T: Resource, R: RightsSet> {
    handle: ResourceHandle<T>,
    _rights: PhantomData<R>,
}
```

Where `Resource` is `MemObject | Task | Endpoint | ComputeUnit | Clock |
CapTable | …` and `RightsSet` is a marker type for a fixed set of rights
flags. Operations that require a right are gated on the type:

```rust
impl<T: Resource> Cap<T, R> where R: Has<Read> {
    pub fn read(&self, ...) -> ...;
}
```

The kernel never calls `cap.has(Right::Read)` at runtime — the type system
forbids constructing the call site otherwise. This eliminates a class of
TOCTOU bugs around rights checks (the check and the use are the same
expression).

Where dynamic dispatch is unavoidable (a generic delegation, a message
that carries an unspecified cap), we fall back to a runtime check:

```rust
pub struct DynCap {
    kind: ResourceKind,
    handle: u32,
    rights: Rights,
    generation: u32,
}
```

The runtime `DynCap` is fatter (16 bytes typed-and-rights, give or take)
and slower (one branch per check), but it appears only at boundaries
where the static type couldn't track it.

## At the user/kernel boundary

User tasks see `CapHandle`s — opaque `u32` indices into the task's cap
table. Syscalls take handles, not pointers, not paths, not names:

```rust
sys_send(endpoint: CapHandle, message: CapHandle) -> Result<...>;
sys_map(addrspace: CapHandle, object: CapHandle, rights: Rights) -> Result<MapToken>;
```

The cap table is per-task. It is a small array of slots, each carrying:

```rust
struct CapSlot {
    occupant: Option<DynCap>,
    delegations: SmallVec<[CapHandle; 4]>,  // children that derived from this
    parent: Option<CapHandle>,              // nullable; root caps have no parent
}
```

This shape pays for hierarchical revocation: a slot tracks its descendants
so revocation walks the tree. We size cap tables modestly (default 256
slots, expandable on retype) and lean on the fact that desktop tasks
typically hold a small handful of caps.

## Cap operations

The five primitives:

### `derive`

```rust
fn cap_derive(
    parent: CapHandle,
    restrict: Rights,
) -> Result<CapHandle, CapError>;
```

Produces a child cap with `child.rights = parent.rights & restrict`. Always
narrowing. The child is recorded as a descendant of the parent for
revocation tracking.

### `delegate`

```rust
fn cap_delegate(
    target: CapHandle,            // an `Endpoint` to the target task
    cap:    CapHandle,
    mode:   DelegateMode,
) -> Result<(), CapError>;
```

`DelegateMode` is one of:

- **Move** — sender loses, receiver gains. Default for messages.
- **Copy** — both have it after. Requires `Rights::SHARE` on the source.
- **Grant** — receiver gets the cap with the caveat that they can derive
  but cannot themselves delegate further. Useful for "you can use this
  resource, but don't pass it along."

### `revoke`

```rust
fn cap_revoke(cap: CapHandle) -> Result<(), CapError>;
```

Invalidates `cap` and all descendants atomically (from each affected
core's perspective; see *Cross-core revocation* below). Requires
`Rights::REVOKE` on `cap`.

### `seal` / `unseal`

```rust
fn cap_seal(value: CapHandle, sealer: CapHandle) -> Result<CapHandle, CapError>;
fn cap_unseal(sealed: CapHandle, sealer: CapHandle) -> Result<CapHandle, CapError>;
```

A `Sealer` is a designated cap whose holder can wrap and unwrap other caps.
The wrapped cap is opaque to non-holders — they can pass it around as
data but cannot use it. This is how we model principals (the seal acts as
identity), tickets, and per-session credentials without a separate
namespace.

### `inspect`

```rust
fn cap_inspect(cap: CapHandle) -> Result<CapInfo, CapError>;
```

Returns kind and rights (but not implementation-internal handles). Used by
introspecting tools, debuggers, and the cap-aware shell.

## Generation counters and stale handles

Each cap slot has a generation. When the slot is freed, the generation
increments. A `DynCap` carries the generation it was minted with; the
table check rejects stale handles in `O(1)`.

Generations are 32-bit per slot. Wraparound is theoretical (4 billion
allocate/free cycles per slot) but we still recycle slots round-robin to
make wraparound require an attacker to exhaust the cap table billions of
times — fine for desktop.

## Hierarchical revocation

Revocation walks the descendant tree from the revoked cap downward. For
each descendant:

1. Mark the slot `Quiescing` (no new ops accepted).
2. Wait for in-flight ops to drain (RCU-flavored, per-core epoch).
3. Free the slot, increment generation.

For desktop workloads, descendant trees are shallow (single-digit depth in
realistic apps). For pathological depths we keep an iterative walker (no
stack blowup) and mark the whole subtree quiescent eagerly so the user
sees instant policy changes even if the actual reclamation is deferred.

## Cross-core revocation

The cap table is per-task; tasks are pinned to a home core. When a cap is
delegated cross-core, the recipient holds the cap on its own core. Revoking
a delegated cap requires:

1. Revoke local descendants (as above).
2. Send `RevokeRequest` over `InterCore` to each remote core that has a
   delegated descendant.
3. Wait for acks (epoch barrier).
4. Complete revocation.

This costs an IPI and a round trip — not free, but cap revocation is rare
on the hot path. For frequent operations (delegation, derivation) cross-
core delegation is also rare in practice on the desktop, where most
work is per-app and per-app work is per-core or near it.

## Sealed caps for principals

Identity in Quantum is a sealed cap. There is no `uid_t`, no
`getpwnam`, no `/etc/passwd`. A user session holds a sealed cap whose
sealer is the system's `Identity` authority; services that want to know
"who is this" inspect or unseal under their own delegation chain.

This makes the kernel's view of identity uniform with everything else
(typed, capability-based) and prevents the long tail of "who can I
impersonate" exploits that ambient identity systems acquire over time.

## Capabilities as protocol

A cap can carry a *protocol* type — a marker that says "this endpoint
speaks the `AudioBuf` protocol." Sender and receiver agree on the type
statically (or via a published schema cap); the kernel does not
interpret the payload but enforces the shape (length, included caps).

This is the runtime-vs-compile-time tradeoff replayed at the IPC layer.
Strong typing inside the kernel prevents whole categories of message
mishandling; see [ipc.md](ipc.md) for the wire format.

## Initial caps and bootstrap

At boot, the kernel creates a small set of root caps and hands them to
the first task (`init`):

- `RootMemory` — `Cap<Untyped, All>` covering the system's free memory.
- `RootCpu` — `Cap<ComputeUnit, All>` covering each CPU.
- `RootDevice` — `Cap<Untyped, All>` over MMIO regions enumerated from
  ACPI / DT.
- `RootClock` — `Cap<Clock, All>` for each platform clock.
- `RootInterrupt` — `Cap<InterruptDomain, All>`.
- `RootIdentity` — the principal sealer.

`init` is responsible for retyping these and delegating subsets to its
children (drivers, services, the user's session). Past `init`, no task
has access to anything it wasn't handed.

This is a hard contract. Pinning it down at boot means there is no
"escape hatch" — no special syscall returns ambient authority, no
`/proc/something` exposes free memory. If the kernel is going to be
secure-by-construction, the only path to authority must be the cap
chain from `init`.

## Performance

Cap-check cost on hot paths:

- Static `Cap<T, R>` — zero (compile-time check, no instructions).
- Dynamic `DynCap` from a handle — one table lookup + generation
  compare + rights mask compare. ~3-5 cycles on a hot L1 hit.
- IPC carrying caps — N+1 lookups (endpoint + N caps), each as above.

Compare to syscall overhead in the hundreds of nanoseconds: cap checks
are noise. The cost is real on the cap *table* layout (we need it L1-hot
for the running task) but achievable.

## What we deliberately don't have

- **POSIX-style permission bits.** rwx-on-files is an ACL, not a
  capability. Filesystem authority is a cap to a filesystem service
  whose namespace is private.
- **Anonymous global resources.** Nothing is reachable without a cap
  chain. No `/dev/null`, no `MAP_ANONYMOUS` flag, no "kernel allocator
  in the sky".
- **Cap forging.** No syscall mints a cap from raw bits. The only path
  to a cap is `derive` from one you already hold, or boot.
- **Confused-deputy mitigations bolted on.** Confused deputy doesn't
  exist when authority is per-call and explicit.

## Open questions

- **Cap table compaction.** Long-lived tasks accumulate slot churn;
  generations grow. Do we compact? Probably yes, opportunistically
  during scheduling slack, but it's deferred design.
- **Inheritance at task spawn.** What's the default cap set the child
  gets? Leaning toward "exactly what the parent enumerated" — no
  inheritance heuristic, no wildcard. Verbose at the call site, but
  unambiguous.
- **Sealing policy.** Are sealers themselves caps (recursive), or a
  primitive type? Recursive is more uniform but costs a layer of
  indirection on every seal/unseal. Probably a primitive with sealers
  managed in their own table.
