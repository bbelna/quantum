# Memory objects

The keystone of `core`. Every other subsystem holds memory objects — never
addresses, never page numbers. This document defines the model, the
operations, and the boundary with the HAL.

## Premise

Most kernels weld their memory model to the MMU. "A page is 4 KiB", "a region
is a `(start_va, length)` pair", "RSS is summed across PTEs". The MMU then
becomes load-bearing for everything: scheduling, accounting, IPC, even
filesystem. Replacing the MMU — or running without one — means a redesign.

Quantum inverts this. `core` operates on **memory objects**: opaque, sized,
typed, capability-named handles to storage. Mapping an object into a
running context is an arch concern (the `AddressSpace` HAL trait family).
Translation between an object and its physical extent is the HAL's
business; `core` cannot ask.

This buys us:

- The same `core` runs on hardware MMUs, software-isolated processes
  (Singularity SIP-style — Rust types + verifier do the protection), or
  no isolation at all (debug builds, embedded targets).
- `core` invariants (object lifetime, rights composition, accounting) are
  expressible without a page table model. That makes them tractable for
  Kani / Creusot / Prusti.
- Tier-aware placement (DRAM / VRAM / CXL.mem / NVMe-as-memory) is a
  property of the object, not a special case glued onto the page allocator.

## What a memory object is

Conceptually:

```rust
struct MemObject {
    id: MemObjectId,        // stable for the object's lifetime
    kind: MemObjectKind,    // Anon | Device | Foreign | Computed
    bytes: u64,             // size — known at retype time
    placement: Placement,   // tier and locality hint
    parent: Option<MemObjectId>,  // None for objects from `Untyped`
    rights: Rights,         // intrinsic ceiling — caps cannot exceed
    state: ObjectState,     // Live | Quiescing | Reclaimed
}
```

`core` never reads or writes object contents directly. Operations on objects
are:

- **retype** — derive a typed object from an `Untyped` extent.
- **split** / **join** — partition an object's extent into two, or fuse
  two adjacent ones.
- **derive** — produce a sub-object of a parent, with restricted rights.
- **map** — request the HAL introduce the object into a given
  `AddressSpace` with given rights. Returns an opaque address token whose
  meaning is only valid within that address space.
- **unmap** — symmetric.
- **destroy** — release the object. Cascades to derivatives. Memory
  returns to the parent `Untyped` (or the original tier pool).

Note what is missing: there is no `read_object_byte`, no `object_to_pa`, no
`pin`. Those are HAL operations.

## Kinds

The kind is set at retype and never changes. We pick a small, closed set:

### `Untyped`

A precursor — not really a "memory object" but the input to retype. Carries
a sized extent of raw resource (RAM bytes, MMIO range, foreign donation)
and the right to retype it. Lineage: seL4's untyped capabilities. We keep
the discipline: the kernel never *allocates* memory; it *retypes*
caller-provided untyped into the kind the caller wants.

This means the kernel has no `kalloc`. Boot fills the system's untyped
pool from the firmware memory map; everything is downstream of that pool.

### `Anon`

Anonymous storage, zero-initialized at first observation. Backed by the
object's placement tier. Default for "I want some memory."

Anon objects are the only kind that can be *swapped* — moved between
tiers under pressure — because they carry no off-kernel referent. (See
`Tiering`.)

### `Device`

MMIO, foreign DMA, GPU registers, anything where reads and writes have
side effects on hardware. The HAL guarantees uncached or write-combined
mappings as the platform requires; `core` only knows that this object
must not be reordered, deduplicated, or speculatively prefetched.

A `Device` object cannot be split, derived (except along arch-defined
register banks), or migrated. Its `bytes` and physical extent are fixed.

### `Foreign`

Handed in by something outside `core`'s allocation discipline: the
bootloader, firmware tables, a peer task via shared mapping, or a host
when running paravirtualized. `core` doesn't get to decide its lifetime;
the contributor does.

The distinction between `Anon` and `Foreign` matters: an `Anon` object's
backing is in `core`'s untyped pool and can be reclaimed when the cap is
revoked; a `Foreign` object's backing belongs to someone else and stays
visible until they say otherwise.

### `Computed`

Backed by a generator function (registered with the HAL). First touch
materializes contents lazily; subsequent reads see the stable result.

The motivating use: read-only data sections (zero-fill, decompress on
demand, stamp constants), but it generalizes to any deterministic
materializer. The HAL maps a `Computed` object copy-on-touch and calls
back into the generator for the missing pages.

`Computed` objects cannot be written. (A copy-on-write derivative is an
`Anon` whose initial state is the `Computed`'s — a kind transition done
explicitly by `derive` with a `Mut` rights upgrade.)

### Why not more

We considered `Code`, `Pinned`, `Shared`, etc. as separate kinds. They are
all expressible as `Anon` or `Foreign` with the right rights bits and
placement. Adding kinds for them would force a switch in every call site
that handles objects generically. We pay the discipline cost once at
retype to keep the runtime model small.

## Placement

Every object has a `Placement` hint covering tier and locality:

```rust
struct Placement {
    tier: Tier,           // Dram | Vram | CxlMem | Persistent | DeviceLocal
    node: NumaNode,       // socket / package / chiplet
    affinity: Affinity,   // bound to a ComputeUnit, or any
}
```

It is a *hint*. The HAL is allowed to place differently if the requested
tier is exhausted, falling back along a published preference order
(typically: requested → same node, neighbor tier → cross-node same tier
→ generic). Migration is asynchronous and cap-controlled (see `Tiering`).

Placement is what makes desktop GPU compute first-class. A `MemObject` in
`Vram` is the same object, with the same lifecycle, as one in `Dram` —
just with a different tier hint and a different HAL backend. A task that
holds a cap to an iGPU ComputeUnit can ask for an object placed in
`DeviceLocal` for that unit, and the scheduler can co-place compute work
on the same unit.

## Tiering and reclaim

Tiers are explicit, ranked, and visible to policy code in `core`. The HAL
publishes per-tier pressure (how full, how hot the working set is) and
`core` runs the migration / eviction policy.

The reclaim contract:

- HAL emits a `TierPressure { tier, level }` event when usage crosses a
  threshold.
- `core` consults a working-set estimator (LRU-K-ish, fed by HAL
  access-bit aggregation) and selects victim objects.
- For each victim, `core` calls `hal.demote(object, target_tier)` — the
  HAL rewrites the mapping atomically.
- `Anon` objects in a fully-pressured system spill to a backing tier
  (typically NVMe-as-memory). This is the kernel's swap path, but it's
  not glued to the page table — it's a property of the object's
  placement.

`Foreign` and `Device` objects cannot be reclaimed. `Computed` objects
can be discarded (their contents are reproducible) — discard is cheaper
than demote.

Pressure-driven migration runs on a per-core scheduling class (see
[design/tasks_and_scheduling.md](tasks_and_scheduling.md), the
`Background` class), so it never preempts interactive work.

## Rights

Per-object rights, set at retype and immutable on the object itself.
Capabilities to the object can carry a strict subset.

```rust
bitflags! {
    struct Rights: u32 {
        const READ        = 1 << 0;  // map readable
        const WRITE       = 1 << 1;  // map writable
        const EXEC        = 1 << 2;  // map executable
        const SHARE       = 1 << 3;  // delegate cap to other tasks
        const DERIVE      = 1 << 4;  // produce sub-objects
        const REVOKE      = 1 << 5;  // revoke derivatives
        const MIGRATE     = 1 << 6;  // request tier change
        const PIN         = 1 << 7;  // forbid migration
        const COHERENT    = 1 << 8;  // require strong cache coherence
    }
}
```

Rights compose by intersection. `derive(parent, restrict)` produces a child
whose ceiling is `parent.rights & restrict`, never more. `Rights::REVOKE`
is the meta-right that lets a holder unmake derivatives.

`PIN` and `MIGRATE` are mutually exclusive in any single mapping decision;
`PIN` is required for DMA, JIT'd code, and anything where a moving
mapping would be unsound.

## Mapping into address spaces

`core` does not map. It asks.

```rust
trait AddressSpace {
    fn map(
        &mut self,
        obj: &MemObject,
        rights: Rights,
        hint: Option<MapHint>,
    ) -> Result<MapToken, MapError>;

    fn unmap(&mut self, token: MapToken) -> Result<(), MapError>;

    fn flush(&mut self, token: MapToken, scope: FlushScope);
}
```

A `MapToken` is opaque outside the address space that issued it. It is
*not* a virtual address — addresses are not a `core` concept. Userspace
sees an arch-specific address through a syscall layer that translates
the token; that translation lives in the platform crate, not in `core`.

This split has a real cost: `core` cannot trivially say "the address of
the third byte of object O in address space A." It must ask the HAL.
Most code paths don't need this — they pass tokens around — and the few
that do (e.g., setting up an IPC reply that takes a string) go through
a single HAL call. The verification benefit is large; the runtime cost
on hot paths is zero (the HAL function is inlinable).

## Identity and the object table

Objects are referenced by `MemObjectId`, which is `(slot, generation)`.
The slot is a per-core allocation; the generation increments on
destruction so a stale id never resurrects.

The object table is per-core. Objects can be referenced from multiple
cores (their caps can be delegated freely), but the canonical record
lives on the core that retyped them. Cross-core operations on an object
go through `InterCore` HAL calls (see `hal_surface.md`).

This sounds expensive. It isn't, on the desktop workload mix:

- Most objects are never delegated cross-core.
- Hot objects (per-task working set) are placed on the task's home core
  by default.
- Cross-core ops are an explicit user choice — the cost is visible in
  the call shape.

## Lifecycle and verification

Every object passes through a small state machine:

```
                  retype                derive
   Untyped --------------> Live <----------------- Live(parent)
                            |
                            | quiesce (last cap revoked)
                            v
                        Quiescing
                            |
                            | reclaim (no outstanding maps)
                            v
                        Reclaimed
```

Invariants we want to verify:

1. No object is `Live` without at least one capability referencing it.
2. No `MapToken` outlives the object's `Live` state.
3. `derive` never produces a child whose rights exceed the parent's.
4. `revoke` on an object with derivatives transitions all of them
   atomically (no observer sees a "half-revoked" tree).
5. `Untyped` extents are partitioned: every byte of physical memory is
   in exactly one of `(free Untyped, retyped object's extent, kernel
   bookkeeping)`.

The kernel implementation should make these statements obvious; the
verification effort should make them mechanical.

## Operations

A representative shape — final API may differ but the surface is small:

```rust
impl MemObjectTable {
    fn retype(
        &mut self,
        untyped: Cap<Untyped, Retype>,
        kind: MemObjectKind,
        bytes: u64,
        placement: Placement,
        rights: Rights,
    ) -> Result<Cap<MemObject, Owner>, RetypeError>;

    fn derive(
        &mut self,
        parent: &Cap<MemObject, Derive>,
        offset: u64,
        bytes: u64,
        restrict: Rights,
    ) -> Result<Cap<MemObject, Owner>, DeriveError>;

    fn split(
        &mut self,
        obj: Cap<MemObject, Owner>,
        at: u64,
    ) -> Result<(Cap<MemObject, Owner>, Cap<MemObject, Owner>), SplitError>;

    fn join(
        &mut self,
        a: Cap<MemObject, Owner>,
        b: Cap<MemObject, Owner>,
    ) -> Result<Cap<MemObject, Owner>, JoinError>;

    fn destroy(
        &mut self,
        obj: Cap<MemObject, Owner>,
    ) -> Result<(), DestroyError>;
}
```

Note the pervasive `Cap<…, …>` types — this is how the cap model
(see [capabilities.md](capabilities.md)) folds into the API. Authority
to perform an operation is encoded in the cap type the caller presents.

## What `core` deliberately does not know

- Page sizes. (HAL chooses 4 KiB / 2 MiB / 1 GiB / 16 KiB / etc.
  per-mapping.)
- Page tables. (HAL.)
- TLB shootdowns. (HAL, scheduled around `flush` calls.)
- Physical addresses. (HAL.)
- Virtual addresses, except as `MapToken`s opaque to it. (HAL.)
- Cache topology beyond `Placement::node`. (HAL surfaces what we need.)
- DMA mapping mechanics. (HAL — `PIN` rights gate it.)

## Open questions

- **Coherence domains.** Strong assumption today: all `ComputeUnit`s see
  cache-coherent memory unless `Device`. iGPU is already cache-coherent
  on modern x86; dGPU is not. Do we model dGPU memory as `Device`, or
  introduce a `Coherence` field on `Placement`? Leaning toward the
  latter, but it adds complexity to the rights model.
- **Reclaim under pressure with derived mappings.** When a parent object
  is reclaimed, do we eagerly revoke derived caps (correct, expensive)
  or lazily (cheap, races)? Probably eager with a fast-path bulk
  revocation primitive.
- **NUMA migration as a regular event vs. a rare one.** On a
  single-socket desktop NUMA migration is near-irrelevant; on a
  workstation Threadripper or dual-socket Xeon it's hot. Place the cost
  on platform configs that need it.
