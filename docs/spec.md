# Quantum — Project Specification

## What this is

Quantum is a research-grade desktop OS kernel written in Rust. It is not a
Unix clone, not a microkernel reimplementation, not a hobby teaching kernel.
The goal is to ship a desktop kernel whose core data model and execution model
reflect what we have learned about systems software since the POSIX/Win32 era
hardened around 1990 — and to do it with the discipline to actually run real
user-facing workloads (interactive UIs, audio, GPU compute, modern
peripherals) on commodity `x86_64` and `AArch64` hardware.

"Research-grade" means the design is justified by stated tradeoffs, not by
precedent, and that the boundaries are drawn so that pieces can be replaced or
formally verified independently.

## What it is not

- A Linux/BSD/Mach reimplementation.
- A unikernel or library OS — Quantum runs untrusted user code with hard
  isolation.
- A strict microkernel. Drivers are user-mode by default, but the design
  allows measured, capability-gated kernel-mode hot paths where the
  coordination cost of a server crossing is genuinely too high. We will not
  pretend "everything is a server" is free.
- A real-time kernel. Latency-aware, yes; bounded-WCET-required, no.

## Tentpoles

### 1. Memory in `core` is objects, not addresses.

`core` does not know about physical pages, virtual pages, or page tables. It
allocates and tracks **memory objects** — opaque, typed, sized handles — and
asks the HAL to make them reachable from a given execution context. This
means:

- The same `core` runs unchanged on hardware MMUs, software-isolated
  processes (Singularity SIP-style), or no isolation at all (embedded debug
  builds).
- Address-space management is one HAL trait family, swappable per arch.
- Verification of `core` invariants does not require modeling page tables.

Lineage: Singularity (SIPs), Barrelfish (capabilities to RAM regions).
Departure: more aggressive than seL4, which still threads "untyped memory"
through its core spec as a sized region.

### 2. Capabilities are the only authority.

Every kernel-mediated resource — a memory object, a task, an IPC endpoint, a
hardware device, a CPU budget — is named by an unforgeable, transferable,
typed capability. There is no global PID, no global FD, no `chmod`, no
ambient authority. A task can only act on what it has been handed.

Rust's type system carries as much of this as possible at compile time inside
the kernel; the runtime cap table exists for the user/kernel boundary and for
delegations the type system cannot express.

Lineage: KeyKOS, EROS, seL4, Genode.

### 3. No dynamic allocation in `core`. Storage is caller-provided.

The `Bitmap` already establishes the pattern: kernel data structures borrow
their backing store. `core` does not own a heap, does not call into a global
allocator, does not have GFP-flag-style allocation policy. Backing memory is
provided by the caller — boot-time static, retyped from a memory object, or
stack in tests. This is what makes `core` `cargo test`-able on the host
without a custom target, and what makes its memory behavior provable.

Lineage: seL4 retype model, Barrelfish.

### 4. Per-core state. No shared mutable state in `core`.

Each CPU runs its own instance of the kernel against its own state.
Cross-core coordination is explicit message passing over kernel-internal
channels; there are no kernel-wide spinlocks protecting kernel-wide mutable
state. Lock contention as a kernel scaling bottleneck is eliminated by
construction, not by tuning.

Lineage: Barrelfish (multikernel, SOSP '09).

### 5. Tasks are state machines, not stacks.

The kernel's unit of execution is a `Task`: a small state object with an
explicit suspend/resume protocol, not a thread with an associated kernel
stack. Context switches save and restore a fixed-size frame, not a page-sized
stack. User code can still be threaded; the threading model is a userspace
concept that lowers to kernel `Task`s at I/O and IPC boundaries.

This is the single biggest leverage point for desktop interactivity: input →
compositor → GPU is fundamentally event-driven, and a stack-per-thread kernel
pays for that with context-switch and memory-residency overhead it does not
need to.

Lineage: Synthesis kernel, Mach continuations, modern async/await.

### 6. The scheduler is a trait. The default is latency-aware.

Scheduling policy is a HAL-adjacent trait, not a hardcoded routine. The
shipping default for desktop is a deadline-aware policy (EDF / CBS family)
that understands frame deadlines, audio buffer deadlines, and input deadlines
as first-class. Server / batch / soft-RT policies are alternative impls.

Lineage: Bossa, K42, modern game-engine scheduling.

### 7. Time, CPU, and memory are accounted, not assumed.

Every resource is granted by a capability that carries quotas. CPU is granted
in `(budget, period)` pairs (CBS-style). Memory objects carry their own size.
IPC endpoints can be rate-limited. There is no "best-effort" tier that the
kernel does not track.

### 8. GPU and accelerators are first-class scheduling targets.

A modern desktop kernel that treats the GPU as a peripheral I/O device is
designing for 2005. Quantum's scheduler and capability model treat compute
units (CPU, integrated GPU, discrete GPU, NPU) symmetrically: a `Task` can be
scheduled on any unit it holds a capability for, and frame deadlines flow
through the same accounting.

The arch/platform layer still owns the actual ring submission and command
translation; what `core` owns is "this work, with this budget, on this
target, by this deadline."

### 9. Verification is a design constraint, not a future project.

The split — `core` as pure logic over borrowed memory with no `unsafe` for
hardware, HAL as the small `unsafe` interface — exists so we can reach for
Kani / Creusot / Prusti on `core` modules as soon as their surface
stabilizes. We will not introduce a design that we cannot, in principle,
verify.

## What this implies for the workspace

- `quantum-core` — object model, capability table, scheduler logic, task
  state machines, IPC machinery, accounting. No `unsafe` for hardware, no
  MMU concepts, host-testable.
- `quantum-hal` — trait contracts. Adds families on top of the existing
  `Cpu`: `AddressSpace`, `Context` (task save/restore), `InterCore`
  (cross-core wakeups), `Time`, `ComputeUnit`.
- `quantum-arch-*` — HAL impls. Owns all the `unsafe` for hardware.
  `x86_64` is v1; an `aarch64` skeleton exists as the abstraction's
  forcing function.
- `quantum-platform-*` — firmware / board glue (ACPI, device tree, serial,
  IRQ routing).
- `quantum-kernel` — the linkable binary. The only crate that picks an
  arch + platform.

## What we are explicitly rejecting

- A `kalloc` / `kfree` global heap in `core`.
- `#[cfg(target_arch = ...)]` outside arch crates.
- A POSIX compatibility layer in the kernel. (Belongs in userspace, if at
  all.)
- Locks as a coordination primitive in `core`. (Per-core state means
  cross-core coordination is messages.)
- A unified "device" abstraction in `core`. Devices are capabilities held by
  drivers, which are user-mode by default.
- Treating GPU / accelerators as peripheral I/O.
- Threads as the kernel's execution primitive.

## Order of work

1. **Memory object model** — design doc and `core` types. The keystone: every
   other subsystem holds memory objects.
2. **Capability table** — design doc and `core` types. Closes the authority
   model.
3. **Task state machine + scheduler trait** — design doc and `core` types.
4. **IPC primitive** — design doc and `core` types. Small once 1–3 are
   settled.
5. **Accounting (CPU / memory / IPC quotas)** — wired into 1–4 from the
   start, but specced explicitly here.
6. **HAL trait expansion** — `AddressSpace`, `Context`, `InterCore`, `Time`,
   `ComputeUnit`, plus stub `arch-x86_64` impls that compile but don't run
   on hardware. Validates that the trait shapes are right.
7. **Host test suite** — exhaustive `cargo test` coverage of 1–5 against the
   stub HAL. This is the artifact we ship before we ever boot.
8. **Boot path** — kernel binary, bootloader handoff, serial print, first
   real `arch-x86_64` impl behind the existing traits.

Steps 1–7 produce no bootable artifact. They produce a host-testable,
opinionated kernel core that we are confident in *before* we spend time on
the boot path. Step 8 is the moment of truth for the abstractions, and it
should be cheap because every trait it fills was designed against a working
host-tested core.

## Sub-specs to follow

Each item below gets its own design doc before code lands:

- `design/memory_objects.md`
- `design/capabilities.md`
- `design/tasks_and_scheduling.md`
- `design/ipc.md`
- `design/accounting.md`
- `design/hal_surface.md`

These docs are normative — implementation tracks the doc, and changes to
behavior come with a doc change in the same commit.
