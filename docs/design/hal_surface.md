# HAL surface

The HAL is the kernel's only `unsafe`-for-hardware boundary. Every trait
here is a contract that an arch / platform crate fulfills; every line of
`core` reaches the hardware through these traits and through nothing else.

This document enumerates the trait families, their shape, and their
contracts. Implementations go in `quantum-arch-*` (architecture-specific:
`x86_64`, `aarch64`) and `quantum-platform-*` (board / firmware glue:
`platform-pc`, etc.). The split: arch crates implement everything that
depends on the ISA (registers, MMU, cache, IPI mechanisms); platform
crates implement everything that depends on firmware / topology (ACPI,
device-tree, IRQ routing, board-specific clocks).

## Premise

The HAL has two design constraints in tension:

- **Be small enough to verify.** Every trait method is a place where
  `core`'s reasoning meets hardware reality. The smaller the surface,
  the smaller the trust.
- **Be expressive enough that `core` doesn't end up smuggling
  hardware concepts through arbitrary back-channels.** A HAL that's
  too thin forces `core` to invent shadow models of hardware — which
  defeats the purpose.

We resolve this by drawing trait boundaries around *concepts* `core`
uses ("an address space", "a way to wake another core", "a clock with
a deadline timer"), not around hardware features ("page tables", "IPI
delivery mechanism"). The arch implements the concept; how it uses
hardware is its business.

Existing trait: `Cpu`, with `halt()` and `pause()`. Documented for the
flavor; we keep it and extend.

## Trait families

```
Cpu              ← halt, pause, identify
Context          ← task save/restore frames
AddressSpace     ← per-task address space + mem object mapping
InterCore        ← cross-core wake/send
Time             ← monotonic clock, deadlines, vsync
Interrupt        ← IRQ registration + dispatch
Memory           ← physical pool, tiers, NUMA, access tracking
ComputeUnit      ← enumerate units, submit work
Energy           ← P-state governor + per-domain meter
Cache            ← coherence ops for non-coherent tiers
BootInfo         ← what the bootloader / firmware handed us
```

### Cpu

Already in tree at [crates/hal/src/cpu.rs](../crates/hal/src/cpu.rs).
Extends to:

```rust
pub trait Cpu {
    fn halt();
    fn pause();
    fn id() -> CpuId;             // logical id
    fn flags() -> CpuFlags;       // SMT pair, perf class, available ISA features
}
```

`flags()` lets `core` distinguish P-cores vs E-cores, SMT pairs, and
available ISA features (AVX-512, SVE, AMX) without hardcoding. The arch
crate populates the value from CPUID / system registers at boot.

### Context

Save and restore a task's small frame. The frame is an arch-defined
opaque struct, but its size is bounded and known to `core`:

```rust
pub trait Context {
    type Frame: ContextFrame;

    fn save(into: &mut Self::Frame);
    unsafe fn resume(from: &Self::Frame) -> !;
    fn init(frame: &mut Self::Frame, entry: ContEntry);
}

pub trait ContextFrame: Sized + Send {
    const SIZE: usize;          // bytes; verified at compile time
    const ALIGN: usize;
}
```

`save` writes the active CPU registers into the frame and is meant to
be called from a known kernel context (after preemption, before
yielding to the scheduler). `resume` jumps to the continuation
recorded in the frame; it diverges (the calling context is gone).

`init` prepares a frame to enter a continuation for the first time —
sets up the entry point, stack pointer (if the task uses an ad-hoc
small kernel stack for its continuation; defaults to none on x86_64),
and zero or saved register state.

FPU / SIMD state is *not* part of the basic frame. `Context` exposes
optional extensions:

```rust
pub trait FpuContext {
    fn save_fpu(into: &mut FpuFrame);
    unsafe fn restore_fpu(from: &FpuFrame);
}
```

Save / restore are called only when the task crosses an FPU-using
boundary. Most state-machine tasks don't touch FPU per quantum, so
the cost is amortized to near zero.

### AddressSpace

Per-task address space. The single most contract-laden trait, because
it defines what "memory object mapping" means without leaking page
tables.

```rust
pub trait AddressSpace {
    type MapToken: Copy + Eq + 'static;

    fn create(node: NumaNode) -> Result<Self, CreateError> where Self: Sized;
    fn drop(self);

    fn map(
        &mut self,
        obj: &MemObject,
        rights: Rights,
        hint: Option<MapHint>,
    ) -> Result<Self::MapToken, MapError>;

    fn unmap(&mut self, token: Self::MapToken) -> Result<(), MapError>;

    fn flush(&mut self, scope: FlushScope);

    fn translate(&self, token: Self::MapToken, offset: u64) -> Option<RawAddr>;
}
```

Notable contracts:

- `MapToken` is opaque to `core`. It is not a virtual address.
- `translate` is the *only* way to obtain an `unsafe` address from a
  token, and it is intended for a tightly bounded set of call sites
  (kernel-internal access to an object's bytes).
- `flush` covers TLB shootdown, cache writeback, etc. The scope tells
  the HAL whether it must include other cores. `core` calls `flush`
  at strategic points (after `unmap`, after rights downgrade) and
  the HAL is free to defer or coalesce.
- `drop` releases all mappings; HAL ensures any per-core state is
  invalidated (cross-core flushes if necessary).

For platforms without an MMU (debug builds, software-isolated mode),
the impl is essentially identity: `MapToken` carries the object's
canonical address; `flush` is a no-op; `translate` returns the canonical
address directly.

### InterCore

Cross-core wakes and ad-hoc remote operations. This is what makes the
multikernel structure work — there is no shared mutable state in
`core`, only messages between cores.

```rust
pub trait InterCore {
    fn wake(remote: CpuId);

    fn send_message(remote: CpuId, msg: InterCoreMessage);

    fn broadcast(scope: BroadcastScope, msg: InterCoreMessage);
}

pub enum InterCoreMessage {
    WakeTask(TaskId),
    Revoke(CapHandle, RevokeEpoch),
    FlushAddressSpace(AddressSpaceId),
    StealOffer(StealHint),
    // …
}
```

On x86 each `wake` is an IPI; on AArch64 a Software-Generated Interrupt
(SGI). The HAL handles vector allocation and dispatch; `core` provides
the message handler.

### Time

Monotonic time, deadlines, and frame events.

```rust
pub trait Time {
    fn now() -> Time;

    fn arm_deadline(deadline: Time, cb: TimerCallback) -> TimerHandle;
    fn cancel(handle: TimerHandle);

    fn frame_source(id: FrameSourceId) -> Option<FrameSource>;
}

pub trait FrameSource {
    fn next_vsync(&self) -> Time;
    fn period(&self) -> Duration;
    fn subscribe(&self, cb: FrameCallback) -> Subscription;
}
```

`now` is a monotonic counter; the HAL guarantees it never goes
backward and is consistent across cores within a small bound.

`arm_deadline` lets `core` schedule a wakeup at an absolute time.
On x86 the impl is the LAPIC TSC-deadline timer; on AArch64 the
generic timer. `core` doesn't care.

`frame_source` provides the kernel with displays' vsync timing
(see [tasks_and_scheduling.md#frame-deadline-awareness](tasks_and_scheduling.md#frame-deadline-awareness)).
This is platform-specific (the platform crate enumerates displays
from ACPI / DT and the GPU driver publishes the framesource caps);
HAL is the trait shape.

### Interrupt

IRQ registration and dispatch.

```rust
pub trait Interrupt {
    fn register(
        line: IrqLine,
        handler: IrqHandler,
        affinity: Affinity,
    ) -> Result<IrqRegistration, IrqError>;

    fn unregister(reg: IrqRegistration);

    fn mask(line: IrqLine);
    fn unmask(line: IrqLine);

    fn eoi(line: IrqLine);
}
```

Handlers run as Reflex-class work — they execute in the interrupted
context with a tight cycle budget. The handler typically wakes a Task
that does the actual work; the IRQ handler itself does just enough to
acknowledge the device.

The platform crate provides line allocation (legacy 8259, IO-APIC,
GIC) and routing; HAL is the trait shape `core` uses.

### Memory

Physical resource management. `core` does not directly allocate
physical memory — it retypes from `Untyped`s — but the boot path needs
HAL-side primitives to enumerate and prepare the initial untyped pool.

```rust
pub trait Memory {
    fn enumerate_regions(out: &mut [PhysRegion]) -> usize;
    fn enumerate_tiers(out: &mut [TierDescriptor]) -> usize;
    fn enumerate_nodes(out: &mut [NodeDescriptor]) -> usize;

    fn tier_pressure(tier: TierId) -> Pressure;
    fn drain_access_bits(into: &mut AccessBitmap);

    fn pin(token: AddressSpace::MapToken) -> Result<DmaHandle, PinError>;
    fn unpin(handle: DmaHandle);
}
```

`enumerate_regions` populates the boot-time untyped pool from firmware
memory map. `enumerate_tiers` and `enumerate_nodes` give us topology.

`tier_pressure` and `drain_access_bits` feed the working-set
estimator (see [memory_objects.md#tiering-and-reclaim](memory_objects.md#tiering-and-reclaim)).
The HAL gathers access bits from page tables and aggregates them
periodically; `core` reads the aggregate and runs reclaim policy.

`pin` / `unpin` resolve a `MapToken` to a stable physical handle for
DMA. Subject to `Rights::PIN` on the underlying memory object. This is
the *only* place outside the HAL where `core`'s code paths reach
toward physical addresses.

### ComputeUnit

Enumerate compute units (CPUs, iGPUs, dGPUs, NPUs); query properties;
submit work to non-CPU units.

```rust
pub trait ComputeUnit {
    fn enumerate(out: &mut [UnitDescriptor]) -> usize;
    fn props(unit: UnitId) -> UnitProps;

    fn submit(
        unit: UnitId,
        work: WorkPacket,
        completion: WorkCompletion,
    ) -> Result<WorkHandle, SubmitError>;

    fn cancel(handle: WorkHandle);
    fn pause(unit: UnitId);
    fn resume(unit: UnitId);
}
```

For CPU units, `submit` is the scheduler's normal enqueue — the work
packet is a continuation pointer + small frame.

For non-CPU units, `submit` translates the work packet through a
unit-specific HAL adapter (the GPU driver's command buffer encoder,
the NPU's scheduler shim) and signals completion via a notification.

This is how Quantum makes accelerators first-class scheduling targets
without `core` knowing anything about ring buffers, command parsing,
or shader compilers — those live in the unit-specific HAL adapters.

### Energy

Power-state governor and energy metering. Likely the most
heterogeneous trait across arch / platform combinations, because
energy mechanisms are highly platform-specific.

```rust
pub trait Energy {
    fn meter(domain: EnergyDomain) -> Joules;

    fn set_p_state(unit: UnitId, freq: Hz) -> Result<(), EnergyError>;
    fn p_state_range(unit: UnitId) -> (Hz, Hz);

    fn enter_idle(unit: UnitId, depth: IdleDepth);

    fn power_source() -> PowerSource;
    fn battery() -> Option<BatteryState>;
}
```

`meter` reads the platform's energy counter (RAPL-class on x86,
Energy Aware Counter on modern AArch64). The HAL polls at microsecond
cadence and the scheduler charges deltas to the running task; the
trait method is what gets called per poll.

`set_p_state` is the policy lever. The scheduler calls it after
picking a unit (see [tasks_and_scheduling.md#energy-aware-scheduling](tasks_and_scheduling.md#energy-aware-scheduling)).
The HAL clamps to its allowable range; `core` doesn't need to know
the exact P-state encoding.

`enter_idle` is what the kernel calls when a core has nothing to run.
`IdleDepth` is a hint (`Light`, `Medium`, `Deep`); the HAL maps to the
right C-state / WFI variant. Deeper idle saves more power but costs
more wake latency.

### Cache

Coherence operations for non-coherent memory regions. On modern x86 and
ARMv8 with proper coherency this trait is a near-no-op for `Anon`; on
dGPU memory and CXL.mem with Type 2/3 it has work to do.

```rust
pub trait Cache {
    fn writeback(token: AddressSpace::MapToken, range: Range);
    fn invalidate(token: AddressSpace::MapToken, range: Range);
    fn writeback_invalidate(token: AddressSpace::MapToken, range: Range);

    fn coherence(token: AddressSpace::MapToken) -> Coherence;
}
```

`core` calls these around IPC bulk transfer when the message specifies
explicit coherence intent. For coherent memory the HAL impl is a
no-op (and the compiler inlines the call to nothing).

### BootInfo

What the bootloader / firmware handed us. Parsed once, by the platform
crate, at boot.

```rust
pub trait BootInfo {
    fn memory_map() -> &'static [PhysRegion];
    fn rsdp() -> Option<PhysAddr>;        // ACPI root, x86
    fn dtb() -> Option<PhysAddr>;         // device tree, AArch64 / RISC-V
    fn cmdline() -> &'static str;
    fn modules() -> &'static [BootModule];
    fn frame_buffer() -> Option<FrameBuffer>;
}
```

This is the platform crate's translation of bootloader output into a
neutral form `core` can inspect at boot to populate the initial cap
table.

## Composition — how `core` uses the HAL

A representative scheduler hot path:

```rust
fn dispatch(core_id: CpuId, queues: &mut RunQueues) -> ! {
    loop {
        let task = match queues.pick_next() {
            Some(t) => t,
            None => {
                Energy::enter_idle(core_id, IdleDepth::Medium);
                continue;
            }
        };

        let unit = pick_unit_and_freq(&task);
        if unit.id != core_id {
            // Migrate: enqueue on remote core.
            InterCore::send_message(unit.id, InterCoreMessage::WakeTask(task.id));
            continue;
        }

        Energy::set_p_state(core_id, unit.freq);
        unsafe { Context::resume(&task.frame) };
    }
}
```

Notable: every line that touches hardware goes through a HAL trait
method. There is no `asm!` in `core`, no MSR access, no IPI sending,
no port I/O.

A representative memory-object map call site:

```rust
fn ipc_recv_with_bulk(self_task: &mut Task, ep: &Endpoint) -> Result<...> {
    let msg = pull_from_endpoint(ep)?;
    let bulk_cap = msg.bulk?;
    let obj = self_task.cap_table.get(bulk_cap)?;

    let token = self_task.address_space.map(
        &obj,
        Rights::READ,
        Some(MapHint::FaultSensitive),
    )?;

    Ok((msg, token))
}
```

`core` walked from receiving an IPC to having a usable mapping
without ever asking what a virtual address is.

## Conventions

All HAL traits comply with these (verified by lint, where possible):

- **`#![no_std]`.** No heap. No `alloc`-crate primitives in trait
  signatures.
- **No panicking from inside HAL methods on release builds.** A panic
  in HAL code is a bug in the arch / platform impl. Debug builds may
  panic with diagnostic info; release builds set a fault flag and
  return an error.
- **Pure-of-allocation.** HAL methods receive caller-provided storage
  (the `enumerate_*` pattern with `&mut [Descriptor]`). They do not
  allocate.
- **Bounded execution.** Every method has a documented worst-case
  step count (or notes "unbounded by hardware" with explanation —
  e.g., a busy-wait on a controller register). The scheduler classes
  for HAL-internal work follow this bound.
- **`Send`-correct types.** Anything `core` carries between cores
  (frame data, descriptors, handles) is `Send` and trivially copyable
  where possible.

## Verification surface

The HAL is where `core`'s pure invariants meet hardware. Verification
strategy:

- `core`-level proofs assume the HAL contracts and prove `core`-only
  invariants (cap conservation, scheduling discipline, object
  lifecycle).
- HAL-level proofs (where feasible) prove the contracts hold on
  specific impls. This is harder — modeling page tables, IPI delivery
  semantics, RAPL counter monotonicity is nontrivial — and we will
  not promise it across the board. We *will* promise it on small,
  load-bearing pieces: `Context::save` / `Context::resume` correctness,
  `AddressSpace` rights enforcement.
- Anything we cannot verify formally we cover with directed tests
  on the host (using a stub HAL) and on QEMU (using the real impl).

The split keeps verification effort proportional to the surface that
matters: `core` is small and pure, the HAL is small and explicit,
and the line between them is mechanical.

## What's not in HAL

- **Drivers.** USB, NVMe, NIC, GPU drivers are user-mode tasks (with
  device caps). They reach hardware through the HAL's `Memory::pin`
  and `Interrupt::register` and live in their own crates outside the
  HAL.
- **Filesystems.** Userspace.
- **Network stacks.** Userspace.
- **Schedulers.** In `core`, behind the class disciplines.
- **Allocators.** In `core` (over caller-provided storage) and in
  userspace.
- **Crypto.** Userspace, with `ComputeUnit` caps to crypto
  accelerators when present.
- **Boot.** The platform crate provides `BootInfo`; `core` runs
  arch-/platform-agnostic boot logic on top.

## Testing the HAL

The HAL has three testbeds:

1. **Stub HAL.** A pure-Rust no-hardware impl that runs on the host.
   `core` runs against it under `cargo test`. Stub address spaces use
   identity mappings; stub timers use monotonic time; stub
   inter-core uses thread-local queues. Used for everything in
   `core` that doesn't need real hardware.
2. **QEMU integration.** Real `arch-x86_64` impl under QEMU, with
   `cargo xtask test-qemu` running directed tests over IPC, mapping,
   wake, etc. Validates that the HAL impls satisfy their contracts.
3. **Hardware bringup.** Same impl, real hardware, with a smaller
   set of "does it boot, does it stay booted" tests.

The stub HAL is doing most of the heavy lifting in CI; QEMU tests run
on every PR; hardware tests run weekly on a small lab fleet.

## Open questions

- **Trait object vs generics.** Today the docs sketch traits as
  associated-type-bearing, with `core` parameterized over the impl.
  This avoids vtable dispatch on every HAL call but couples `core`'s
  build to a specific arch. Alternative: `dyn`-based with the kernel
  binary picking impls at link time. Likely answer: generics for hot
  paths (`Context`, `Cpu`, `AddressSpace`), trait objects for cold
  paths (`BootInfo`).
- **Multiple ComputeUnit kinds in one impl.** A platform can host
  multiple GPU vendors; ComputeUnit needs vendor-specific submission
  paths. Probably solved by per-vendor HAL adapter crates that all
  implement the `ComputeUnit` trait, with the platform crate
  registering each at boot.
- **HAL evolution.** When we add a trait method, every arch impl must
  add it. We might want a "fallback" pattern — default-impl methods
  that return `Unsupported` so older impls keep compiling. Risk: the
  default lulls us into shipping ungrounded `core` features. Probably
  no defaults; explicit unimplemented stubs per arch, gated by a
  feature flag.
- **Handling firmware-side energy reporting variance.** RAPL on Intel
  is well-defined; AMD's `energy/PWR` MSRs are different; AArch64
  vendors vary widely. The trait shape is right, the impl is the
  research project. Worth flagging as a v1.5 problem.
