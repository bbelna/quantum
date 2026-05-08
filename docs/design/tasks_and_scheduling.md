# Tasks and scheduling

The execution model — and the scheduler — are where Quantum diverges hardest
from incumbent kernels. Linux's CFS (now EEVDF), Windows' priority-driven
scheduler, and the BSD ULE family all share a common ancestor: **threads
with stacks, plus heuristics**. Quantum is built around a different unit
(state-machine tasks), a different time granularity (deadline-aware from
the bottom), and a different policy mix (latency classes with hard
isolation, not nice-value soup).

This document is long. Each piece is independently designed, but they
compose: the task model is what makes the scheduler cheap, the scheduler is
what makes desktop interactivity tractable, and the heterogeneous /
energy-aware components are what make a 2026-class laptop actually a good
machine.

## Why this matters for desktop

Desktop is not server. Server scheduling optimizes throughput under
contention; desktop scheduling optimizes the **tail latency** of an
interaction — the time from a key press to a redrawn frame. The relevant
numbers are not "fairness over a 5-minute average" but "99.9th-percentile
input-to-photon at 240 Hz", "audio buffer underrun rate over an hour",
"first-frame latency for a freshly launched app on a battery-saving
config". A well-tuned Linux desktop hits these numbers — sometimes — by
fighting against a scheduler designed for fleet servers. We design for
them from the start.

The pieces:

- [The task model](#the-task-model) — state machines, not stacks.
- [Lifecycle](#lifecycle) — create, ready, run, wait, terminate.
- [Scheduling classes](#scheduling-classes) — what we ship.
- [Class disciplines](#class-disciplines) — the policy per class.
- [Frame-deadline awareness](#frame-deadline-awareness) — the desktop hot
  path.
- [Heterogeneous compute](#heterogeneous-compute) — P-cores, E-cores, GPUs.
- [Energy-aware scheduling](#energy-aware-scheduling) — laptop reality.
- [Wake-up mechanics](#wake-up-mechanics) — async semantics at the kernel.
- [Cooperation and preemption](#cooperation-and-preemption) — the lazy
  fast path.
- [Run queues](#run-queues) — per-core, lock-free.
- [Work stealing](#work-stealing) — when and how.
- [Group scheduling](#group-scheduling) — apps and task graphs.
- [Latency budgets](#latency-budgets) — the kernel as a partner.
- [Direct switch](#direct-switch-on-ipc) — IPC and scheduling collapse.
- [What we don't have](#what-we-deliberately-dont-have).
- [Observability](#observability).
- [Verification](#verification-properties).

## The task model

A `Task` is a state machine. It is not a thread. It does not own a
kernel stack.

```rust
pub struct Task {
    id: TaskId,
    state: AtomicEnum<TaskState>,    // Ready | Running | Awaiting(_) | Suspended | Done
    cont:  Continuation,             // function + small frame
    frame: SavedContext,             // CPU regs, FPU lazily
    class: SchedClass,
    bind:  ComputeBinding,           // affinity / pin / any
    cap_table: CapTable,
    budgets: Budgets,                // CPU, memory, IPC, energy
    sched_state: PerClassState,      // discriminated by class
}
```

The `Continuation` is the function the kernel calls to resume the task.
Resuming a task is: restore registers, jump to continuation. Suspending
is the inverse, plus saving any liveness the continuation declared
(small frame, kilobytes max — typically tens of bytes). There is no
kernel stack to switch.

User-mode threads still exist; they live in userspace (libraries
implement them on top of the IPC and async primitives). When a userspace
thread blocks on a kernel syscall, the kernel-side task that backs the
syscall yields its state machine; the kernel runs the next ready task.
On reply, the kernel-side task wakes the user-mode thread.

### The cost equation

Reference numbers (rough; exact figures vary by hardware):

| Operation | Linux thread switch | L4-style task switch | Quantum target |
|---|---|---|---|
| Same-CPU context switch | ~1.5 μs | ~250 ns | ~200-300 ns |
| Cross-CPU wakeup | ~3-5 μs | ~1-2 μs | ~800 ns |
| Sync IPC round-trip | ~5-10 μs (pipe) | ~300 ns (L4) | ~400 ns |

The order-of-magnitude difference is exactly what makes the scheduling
strategies in this document feasible. We can afford to make scheduling
decisions at every wake-up because each decision costs single-digit
percent of an interactive frame budget at 60 Hz, and we can afford the
fine grain because the unit cost is small.

### Why state machines, specifically

The naïve question: "Can't we just use threads with smaller stacks?"

The answer is that the state-machine model fits the actual structure of
desktop work. An input event runs through a chain — input driver,
input service, compositor, app — and *each link in the chain is itself a
state machine*: receive event, do constant work, dispatch to the next.
There is no recursion, no long-lived stack, no need for the locality of
a thread-with-stack model. State machines compose with async/await
(Rust at user level, hand-coded at kernel level), giving us a unified
continuation passing model from interrupt to GPU command.

## Lifecycle

```
                      spawn
                  ┌───────────┐
                  │           │
                  ▼           │
              Suspended       │
                  │           │
              resume          │
                  │           │
                  ▼           │
              Ready ◄──── wake│
              │  ▲            │
       schedule  │            │
              │  │ yield/preempt
              ▼  │            │
            Running ──────────┤
              │               │
            block             │
              │               │
              ▼               │
           Awaiting(reason) ──┘  (wake source fires)
              │
            terminate
              │
              ▼
            Done
```

`Awaiting(reason)` carries the reason: an endpoint (waiting for a
message), a timer (waiting for a deadline), a wake source cap, or
a cap-revoke barrier. The scheduler does not poll awaiting tasks;
the source moves them to `Ready` directly.

Termination is cap-driven: the holder of the `Task` cap with `Kill`
right may transition the task to `Done`. Resources held by the task
(memory objects via owner caps, endpoints, etc.) are revoked, which
cascades.

## Scheduling classes

We ship **six** classes. Each is its own discipline; classes are
strictly ordered for preemption priority. The order:

1. **Reflex** — interrupt handlers, IPC fast paths, kernel quiesce
   work. Run in the context that wakes them. Not preemptible by other
   classes (but cooperate via short, bounded execution).
2. **Latency-Critical** — the smallest possible amount of work that
   *must* meet a deadline: audio mixer top-up, vsync-pending compositor
   commit, input driver decode. CBS-managed, EDF-ordered.
3. **Interactive** — foreground app work, input handlers above the
   driver layer, UI thread work in apps the user is looking at. CBS-
   managed with longer periods, with a wake-up boost on user input.
4. **Throughput** — compilers, video transcoders, background page
   layout, anything CPU-bound where progress matters more than
   responsiveness. Fair-share, no wake-boost.
5. **Background** — indexers, defrag, cache warmers, telemetry.
   Capped budget, runs only when CPU is otherwise idle and
   frequency is low.
6. **Opportunistic** — strictly idle work. Speculative prefetch,
   pre-rendering bets. Discarded with no signal if anything else
   wants the CPU.

The class is a property of the task, set at spawn from a class cap.
A task's class is upgrade-able only with a higher-class cap and is
downgrade-able freely (lowering authority). Class is *not* a number to
tune. The user-visible API is "what is this task for", not "what's its
nice value". The kernel does not expose flat priority numbers to
userspace.

### Why six and not seven

We considered:

- A separate `RealTime` class above Reflex. Rejected: we are not an RT
  kernel, we are a desktop, and desktop "RT" needs (audio, video) are
  served by Latency-Critical with CBS. Adding a class that promises
  more than we deliver would be a footgun.
- Splitting Interactive into "focused-app" and "background-app".
  Rejected: focus is a userspace policy. The window manager
  derives an Interactive cap with appropriate budget for the focused
  app and a smaller one for unfocused apps; the kernel doesn't need
  to know what a window is.
- A `Server` class between Throughput and Background. Rejected: the
  same Throughput discipline serves servers correctly; the difference
  is in policy (CPU budget, memory tier, energy class), not class.

## Class disciplines

### Reflex

A Reflex task runs to completion, in the context that posted it, with no
scheduling decision. Its budget is *cycles*, not time — typically a few
hundred cycles cap, enforced by HAL-side instrumentation. If a Reflex
overflows its budget, that's a bug; the kernel logs and downgrades the
class for the offending task.

This class is for the kernel itself and for very thin user-mode work
that the kernel decided is reflex-grade (e.g., a registered IPC
fast-path callback). Most user code is not Reflex.

### Latency-Critical

CBS (Constant Bandwidth Server, Abeni & Buttazzo, 1998) with EDF
ordering across L-C tasks on a core.

Each L-C task has `(Q, P)`: budget `Q` (microseconds) per period `P`
(microseconds). The task is guaranteed to receive `Q` of CPU within
each `P` window. When `Q` is consumed, the task's deadline is
postponed by `P`; it goes to the back of the EDF queue and waits.

Properties:

- **Hard isolation.** A runaway L-C task cannot consume more than `Q/P`
  of the core. Other L-C tasks are unaffected.
- **EDF optimality.** Among L-C tasks on a single core, if a feasible
  schedule exists EDF finds it. (This is provably optimal for
  preemptive single-core deadline scheduling; Liu and Layland, 1973.)
- **Composable.** New L-C tasks admitted only if `Σ Qᵢ/Pᵢ ≤ U_max`
  where `U_max < 1` (we leave headroom for Reflex and class jitter,
  default 0.85). Admission control runs at task-create / class-change.

L-C tasks declare their `(Q, P)` from a `LatencyCap`, not by user fiat.
The cap is delegated by a system service that owns the budget — for
desktop, this is the audio service, the compositor, and the input
service.

### Interactive

CBS with `(Q, P)` larger than L-C, plus a **wake-up boost**:

- On wake (input event, IPC reply, timer fire), the task gets a
  one-shot deadline of `min(now + Q, original_deadline)` and runs at
  the head of the Interactive queue.
- The boost expires when the task either yields voluntarily or
  consumes `Q`.

The boost makes Interactive feel responsive without making it
preemptive over L-C. A foreground app that wakes on a key press gets
its handler running fast; a foreground app doing layout work stays in
Interactive's CBS pool with normal priority.

Inter-Interactive ordering within a core: CBS deadline; tie-broken by
recent CPU consumption (not strictly fair, biased toward recently-
woken tasks).

### Throughput

Stride scheduling (Waldspurger 1995): proportional share, deterministic.

Each Throughput task has a `share` (a small integer). Time is given out
in proportion: with `Σ shares = S` and a quantum of length `q`, task
`i` gets `(share_i / S) × q` over each scheduling cycle.

Stride is preferred over CFS-style virtual runtime here because:

- Determinism. A user can reason about "this build will get half the
  CPU when I open a video call" — stride says yes, exactly.
- Cheaper bookkeeping. Stride uses integer pass tracking; CFS uses
  rbtree-keyed virtual runtimes. Single-digit-percent CPU difference,
  but compile-the-kernel benchmarks count.
- Composable with group scheduling (see below).

A Throughput task that starves an Interactive task would be a problem,
except that the strict class ordering preempts before the share math
matters.

### Background

Hard-cap budget. Background tasks have `(Q, P)` like CBS but with the
admission rule `Σ Q/P ≤ U_bg` (default 0.05 — 5% of the core).
Background tasks run only on idle CPU at idle frequency (the energy
governor refuses to bump P-state for them).

Background is also the class for kernel housekeeping that isn't time-
critical: TLB cleanup, page table compaction, working-set scanning for
reclaim.

### Opportunistic

No budget at all. Opportunistic runs only when the run queue has
nothing else for *several quanta* and the energy governor is at
deepest idle. Any wake of any other class evicts Opportunistic
instantly (no budget refund, no runnable signal — the work is just
discarded if it didn't finish).

This is the "speculative work" class. Pre-rendering the next slide of
a presentation, reading-ahead from a file the user might open, training
a tiny on-device model. None of it is required; some of it pays off.

## Frame-deadline awareness

This is the desktop differentiator.

A modern compositor knows the frame deadline. It runs at 60 / 120 / 144 /
240 Hz; it knows when the next vsync is; and it knows that everything
that contributes to that frame must be done by then. In Linux today this
information stops at the compositor — every layer below it (the
scheduler, the GPU driver, the kernel I/O path) is unaware.

In Quantum the compositor publishes the deadline through a kernel-
visible primitive:

```rust
pub struct FrameDeadline {
    target: Time,                  // monotonic, when vsync fires
    kind:   FrameKind,             // Mandatory | BestEffort
    sources: SmallVec<[TaskId]>,   // tasks whose work feeds this frame
}
```

The compositor obtains a `FrameDeadlineCap` (root: from `RootClock`
delegations) and attaches it to tasks that contribute. When a tagged
task wakes, the scheduler's L-C admission considers it: if the task's
remaining work (estimated from history) would miss `target`, the
scheduler:

1. Schedules it immediately, preempting non-critical class work.
2. Optionally boosts the compute unit's frequency via the energy
   governor (if power policy allows).
3. If miss is unavoidable, increments a per-deadline `miss_count`
   the compositor reads — the compositor degrades gracefully (drop a
   frame, lower quality) rather than juddering blindly.

Compositors today do all of this in user space, with second-class
visibility into kernel scheduling. By making the deadline a first-class
kernel object we collapse a layer of indirection and put the scheduler
on the same beat as the display.

### Deadline inheritance

Tasks marked as feeding `FrameDeadline D` *inherit D's deadline* for
the duration of the work. If task A holds `D` and IPC-calls task B, B
runs as if it had `D` until the reply. This generalizes priority
inheritance over the IPC graph.

Deadline inheritance ends at the IPC reply. A long-lived background
task that received a deadline-tagged request gets the deadline only for
that request's processing.

## Heterogeneous compute

Modern desktop CPUs are not uniform. Intel Alder Lake / Raptor Lake /
Meteor Lake / Arrow Lake / Lunar Lake all mix P-cores and E-cores;
AMD Phoenix / Strix mixes Zen 4 and Zen 4c; Apple Silicon is big.LITTLE.
AArch64 desktops follow the same pattern. NPUs are arriving as a third
unit type. dGPUs are a fourth.

Linux's EAS (Energy Aware Scheduling) is the closest production answer
and is a clear improvement over uniform CFS, but it makes assumptions
that don't hold on desktop: that the energy model is exposed by
firmware (it isn't on most x86), that workloads are characterizable
into "small" / "big" by recent utilization (they aren't — interactive
work is bursty), and that the kernel scheduler can be reactive
(it can't — by the time the migration heuristic fires, the user has
seen the lag).

Quantum's approach:

### Tasks declare what they are

Two pieces of metadata, declared at task spawn (and updateable):

- `compute_intensity` — measured: cycles per real second over a
  rolling window. Cheap to maintain; the HAL gives us a per-task
  cycle counter via `RDPMC`-class instructions.
- `responsiveness_target` — declared in the cap: `Latency`,
  `Throughput`, `Either`, `Background`.

### Compute units are first-class

The scheduler does not think in "CPUs". It thinks in `ComputeUnit`s.
Each unit has properties published by the HAL:

```rust
pub struct UnitProps {
    kind:        UnitKind,        // Cpu | iGpu | dGpu | Npu | …
    perf_class:  PerfClass,       // P | E | uncore-detached
    coherence:   Coherence,       // Local | Global
    max_freq:    Hz,
    nominal_pwr: mW,
    home_node:   NumaNode,
}
```

Tasks bind to a *set* of acceptable units (from a `ComputeUnitCap`,
delegated by a parent or by the system). The scheduler picks within
the set.

### Pick policy

When a task is `Ready`, the scheduler looks at its class and metadata
and picks a unit:

- **Latency-Critical / Reflex**: P-class CPU, same NUMA node as task's
  memory placement if known. Wakes a sleeping P-core if necessary
  (energy budget permitting).
- **Interactive**: P-class preferred. Will spill to E-class only under
  high load on P-class units.
- **Throughput, low intensity**: E-class. Pack onto E-cores.
- **Throughput, high intensity**: P-class if available, otherwise
  E-class. Heavy compute that can use a P-core wants one.
- **Background / Opportunistic**: E-class only, lowest available
  frequency.

The picker is invoked at every wake. With state-machine tasks the
choice is cheap — picking a unit and enqueueing on its run queue is a
handful of comparisons and an atomic store.

### Migration

Migration is cheap (small saved frame, no kernel stack to move), so we
re-pick aggressively at wake. We don't migrate running tasks
mid-quantum: the cost of cache reload outweighs any benefit on
desktop-grain workloads.

Migration is biased: switching units within the same NUMA node is
free; crossing nodes is paid for by halving the migration credit
counter, which acts as a hysteresis. Without this, two cores can
ping-pong a task indefinitely.

### NPUs and dGPUs

These are scheduling targets that the task gets a cap to. The scheduler
sends the task's continuation as a command on the unit's submission
queue (HAL-exposed); the HAL signals back when the unit completes.
From the kernel's point of view the task is `Awaiting(Unit)` for the
duration; the unit's HAL adapter wakes it on completion.

The unit's queue is itself a scheduling problem (multiple tasks
contending for one GPU). We handle it with the same class machinery —
the GPU's `RunQueue` (HAL-internal) is class-aware; a Latency-Critical
GPU job preempts a Throughput one (preemption is per-unit; modern GPUs
support it natively at command-buffer granularity).

## Energy-aware scheduling

A laptop kernel that ignores energy is a laptop kernel that gets two
hours of battery life.

We treat energy as a first-class resource. Per task: an `energy_class`,
declared like scheduling class:

- **Race-to-idle** — finish ASAP, then sleep deeply. Most desktop
  work, where lowest total energy is achieved by full speed for short
  duration.
- **Coast** — run at low frequency, no rush. Background processing
  that's throughput-bounded, e.g., a slow indexer.
- **Bursty** — many short wake-ups, consolidate. Polling work,
  notification listeners.

Per system: an `EnergyBudget` set by user policy ("battery saver",
"balanced", "performance"), and a current `PowerSource` (mains /
battery / undervolt-rail).

The scheduler interacts with the energy governor (HAL `Energy` trait)
on every nontrivial decision:

```rust
fn pick_unit_and_freq(task: &Task, ctx: &SchedCtx) -> (UnitId, Hz) {
    let unit = unit_pick_policy(task, ctx);
    let freq = match (task.class(), task.energy_class()) {
        (LatencyCritical | Interactive, RaceToIdle) => unit.max_freq,
        (Throughput, RaceToIdle) => unit.efficient_high_freq,
        (Throughput, Coast)      => unit.efficient_freq,
        (Background, _)          => unit.idle_freq,
        (Opportunistic, _)       => unit.idle_freq,
        // …
    };
    (unit, freq)
}
```

The actual policy is more nuanced (predicted task duration, deadline
slack, current node thermal headroom, battery state) but the principle
holds: the scheduler chooses both the unit and the frequency, and these
choices are coupled.

### Bursty consolidation

If the system is mostly idle but a few Bursty tasks wake every 10 ms,
the kernel coalesces wake-ups: it shifts each task's wake by a few
hundred microseconds so they fire in the same wake window, runs them
together, then idles. Energy savings on a laptop are large — sleep
deeper, sleep longer.

This requires the bursty task to tolerate jitter on its wake time. The
task declares `wake_jitter_ok: Duration` at task-create; the kernel
respects the bound (default: 0 — no shift unless explicitly allowed).

## Wake-up mechanics

A task in `Awaiting(reason)` is woken by the source.

```rust
pub struct Waker {
    target: TaskId,
    source: WakeSource,    // for diagnostics + dedup
}

impl Waker {
    pub fn wake(self);     // moves target to Ready, schedules unit pick
}
```

Wakers are caps (`Cap<Waker, Wake>`). The source obtains a Waker when
it accepts the wait registration. Internally:

- `wake()` does an atomic CAS on the task's `state` from `Awaiting` to
  `Ready`. Spurious wakes (already `Ready`) are silently dropped.
- After the CAS, the waker enqueues the task onto its target unit's
  run queue. Cross-core wakes go through `InterCore::wake_remote`
  (HAL): on x86 this is an IPI plus a per-CPU MPSC enqueue.

Wake latency target: ~800 ns cross-core, ~150 ns same-core. Same-core
wakes are nearly free because the run queue is in this core's L1/L2
and the scheduler is already invoked at quantum boundaries.

### Wake-on-event ergonomics

Most desktop work is fundamentally event-driven. Quantum's IPC and
async primitives produce wakers naturally — `endpoint.recv().await`
registers a waker on the endpoint, `timer.deadline(t).await` registers
on the clock, etc. User-facing libraries can reuse Rust's `Future`
trait directly; the kernel does the right thing.

## Cooperation and preemption

Tasks yield voluntarily on every async suspension point. This is the
**fast path**: most context switches are at well-known boundaries
where the task has nothing live except its declared frame. The
scheduler picks the next task and resumes.

Preemption is the **safety net**:

- At every quantum boundary (per-class quantum, see below), the
  scheduler interrupts the running task, saves its frame, and
  re-picks.
- A higher-class wake (L-C wakes while Throughput runs) preempts the
  lower class.
- Reflex events run inline, deferring class-grade scheduling decisions
  until they complete.

Quantum lengths by class:

| Class            | Quantum | Notes |
|------------------|---------|-------|
| Reflex           | n/a     | runs to completion within a budget |
| Latency-Critical | 0.5 ms  | small; CBS does the heavy lifting |
| Interactive      | 5 ms    | aligns with 60-Hz frame intervals |
| Throughput       | 50 ms   | enough to amortize cache fill |
| Background       | 200 ms  | rare preemption, idle-only |
| Opportunistic    | 200 ms  | runs only when nothing else does |

Quantum choice trades off context-switch overhead against tail latency.
Linux defaults to ~1 ms (ouch on cache, fine on latency); we bias
shorter for latency-sensitive classes and longer for throughput,
because we can — our context switch is 5-10x cheaper.

## Run queues

One run queue per core, per class. Six classes, N cores → 6N queues
total. Each queue is a single-producer-multiple-consumer-ish structure:
the owning core consumes; other cores enqueue (cross-core wakes) using
a wait-free MPSC.

Within a class, the queue's order depends on the discipline:

- **Reflex**: FIFO (it's a small queue; ordering rarely matters).
- **Latency-Critical**: priority queue keyed on deadline.
- **Interactive**: priority queue keyed on (deadline, recent-cpu).
- **Throughput**: stride order (smallest pass first).
- **Background**: deadline (CBS).
- **Opportunistic**: FIFO.

The scheduler's pick-next is:

```rust
fn pick_next(core: &Core) -> Option<&Task> {
    for class in &[Reflex, LatencyCritical, Interactive,
                   Throughput, Background, Opportunistic] {
        if let Some(t) = core.queues[class].pop() {
            return Some(t);
        }
    }
    None
}
```

Cost: six pointer reads, branch on the first non-empty. In the steady
state, only a couple of queues are non-empty.

### Lock-freedom

The per-core run queues are accessed:

- By the owning core (consumer): no synchronization.
- By other cores (producers, on cross-core wakes): wait-free MPSC.

This means the scheduler hot path holds no locks. Priority queue
modifications (insertions on wake, removals on quantum end) are
single-threaded by core.

For inter-class moves (a task changes class) the kernel does a remove
+ insert on this core's queues; the moves are local.

## Work stealing

When a core's run queues are empty (no Reflex, L-C, Interactive,
Throughput, Background) it offers itself for stealing.

Stealing rules:

1. Same NUMA node first. Cross-node steals only after a few empty
   spins.
2. Steal only from `Throughput` and below. Latency-Critical and
   Interactive are placement-bound; stealing them from another core's
   queue would defeat the placement decision.
3. A steal moves a single task's home unit to the stealer. Future
   wakes go to the stealer.
4. Stealer's energy budget must permit. A core in deep idle stays idle
   if the only work is `Background`/`Opportunistic` and the energy
   governor wants to keep the core asleep.

Counter-intuitive: aggressive stealing on desktop is *bad*, because it
churns caches and wakes cores. We steal lazily.

## Group scheduling

A `SchedGroup` is a set of tasks sharing a budget cap. Three uses:

1. **Per-app sandboxing.** A whole application's tasks share a CPU
   budget; one runaway task doesn't starve the rest of the system.
2. **Task-graph gang scheduling.** A group can request co-scheduling
   on adjacent units (gang). Useful for parallel rendering or build
   workers — they make progress when scheduled together, less when
   scattered.
3. **Subsystem isolation.** A driver subsystem's tasks live in a
   group with a hard cap.

Groups nest. The cap chain — every group's budget is a child of its
parent's — gives clean accounting and revocation.

Group disciplines:

- **Default**: budget split evenly among ready tasks within the group;
  stride scheduling within the budget.
- **Gang**: all-or-nothing — the group runs only if `≥ N` units are
  free for it, otherwise waits. Frame-deadline-tagged groups bypass
  the gang requirement (it's better to run partial than miss the
  deadline).

## Latency budgets

This is novel-ish — we do not know an existing kernel that exposes
this in quite this shape.

A task can declare an end-to-end latency budget:

```rust
pub struct LatencyBudget {
    name:  StaticStr,        // for diagnostics
    bound: Duration,         // wall-clock cap
    on_overrun: Action,      // Report | BoostClass | Both
}
```

The kernel measures the task's wall-clock time across blocking, IPC,
and scheduling delay. If the budget would be exceeded, the kernel
acts:

- `Report`: writes an event into the task's `SchedStats`, increments a
  counter. The app reads this and degrades.
- `BoostClass`: temporarily lifts the task to the next class up,
  preserving rights. Used carefully — a task that does this must hold
  a `BoostCap` granting the right.
- `Both`: report and boost.

The scheduler also publishes *predicted* overrun: when a task's slack
gets close to zero (based on historical run length), the budget event
fires *before* the overrun, giving the app time to react.

Why this matters for desktop: a click-handler that wants "this UI loop
must close in 16 ms or I'll show a spinner instead of finishing" has,
today, no kernel partner. It runs blind. Quantum gives the partner —
the kernel measures, the kernel reports, the app stays responsive.

## Direct switch on IPC

When task `A` synchronously sends to task `B`:

1. Validate the endpoint cap, validate any payload caps.
2. Move A to `Awaiting(reply)`.
3. Set B's continuation to "deliver this message", set B's frame to
   include A's quantum remaining (modulo IPC overhead).
4. Resume B directly. No scheduling decision.

This collapses a scheduling round-trip into a single bounded operation.
On reply (B → A), the inverse: B suspends, A resumes with B's residual.
Total cost on a hot core: ~400 ns sync round-trip (both directions).

For cross-core IPC: A enqueues the message on B's home core's wake
path (IPI + MPSC). A continues with its quantum. This is more
expensive (~800-1500 ns one-way) but avoids the round-trip cost on
A. Co-locate or pay.

CBS budgets are charged appropriately: B's CBS pool is drawn down by
the work it does on A's behalf, not A's. (This prevents A from
"laundering" CPU through a B server.) Frame deadline inherits as
described above.

## What we deliberately don't have

- **Nice values.** Use class + budget. No flat priority.
- **`SCHED_OTHER` / `SCHED_FIFO` / `SCHED_RR`.** A long-list of
  scheduler classes whose interactions are subtle and bug-prone.
- **`SCHED_DEADLINE` exposed to userspace as flat parameters.** CBS
  is internal; userspace requests deadlines through the
  `LatencyCritical` cap.
- **Autogroup.** Per-tty heuristics are 2010-Linux nonsense. Group
  scheduling is explicit and cap-driven.
- **CPU sets / cgroups.** Subsumed by `ComputeUnitCap` and
  `SchedGroup`.
- **Boosting via PI mutexes.** Caps that carry deadlines do this
  cleanly (deadline inheritance over IPC). We don't expose mutexes as
  a kernel primitive.
- **Threads, in the OS sense.** Userspace can build them; kernel
  schedules tasks.

## Observability

Every task carries a `SchedStats` block, available via a stats cap:

```rust
pub struct SchedStats {
    runtime:        Duration,         // total CPU consumed
    wake_to_run:    Histogram,        // P50/P99/P999 latency
    quantum_used:   Counter,          // exhausted quanta
    deadline_miss:  Counter,          // for L-C / frame-tagged
    budget_overrun: Counter,
    migrations:     Counter,
    waker_sources:  HashMap<WakeSource, Counter>,
}
```

System-level stats roll these up by class, group, and unit. The desktop
shell uses these to surface "this app is jittering" without ad-hoc
profiling tools — the visibility is a first-class kernel feature.

## Verification properties

Statements we want to prove on the scheduler:

1. **Class ordering**: a Reflex task always preempts a Latency-Critical
   task wanting CPU. L-C always preempts Interactive. Etc. (Trivial
   on the structural definition; verify against the implementation.)
2. **CBS bandwidth**: an L-C task with `(Q, P)` consumes at most
   `Q + ε` per period `P`, where `ε` bounds quantum overshoot.
3. **EDF feasibility**: if `Σ Qᵢ/Pᵢ ≤ U_max`, every L-C deadline is
   met assuming admission accepted the task.
4. **No deadlock from direct switch**: A→B→A direct switching cannot
   loop indefinitely; either A or B blocks within bounded steps. (The
   kernel detects cycles; deeper cycles in the IPC graph are
   userspace pathology — they manifest as a fairness issue, not
   deadlock, because at quantum end the scheduler intervenes.)
5. **Budget conservation**: charged time equals consumed time within
   `± measurement_jitter`. (Empirical, not formal — but instrumented.)

The scheduler is the part of `core` we most want to verify. The class
discipline is small enough to model, and the per-core lock-free
structure makes each core's behavior independently provable. CBS
admission is a closed-form check; EDF optimality is a textbook proof
we re-prove on our specific data structures.

## Open questions

- **Hyperthread / SMT siblings.** Are SMT siblings the same
  ComputeUnit (one queue, scheduler picks slots) or distinct
  (two queues, kernel knows about contention)? Modern CPUs:
  distinct, with shared L1/L2; classes that share data benefit
  from co-placement. Likely model: distinct units with a
  `sibling` link in `UnitProps`, picker prefers same-task gang
  on siblings only when they share data (heuristic from cap
  delegations).
- **Class boost via cap revoke?** A user task that holds a
  Latency-Critical cap could keep it forever and starve the system.
  We bound by admission control (`Σ Q/P ≤ U_max`). A cap holder
  can over-budget by handing the cap to many tasks; this is
  detected by the system service that issued the L-C cap, which
  monitors aggregate usage.
- **GPU preemption granularity.** Modern GPUs preempt at command-
  buffer or wave-launch boundary. CBS-on-GPU is feasible but
  preemption latency is hundreds of microseconds, which forces
  longer quanta on the GPU. We will pick a class set per unit
  type rather than universal — desktop CPU has the six above,
  GPU has a smaller set (LatencyCritical for compositor, Throughput
  for compute, Background for indexer).
- **Cold-start latency.** A freshly woken task on a deep-asleep
  P-core pays the wake-up cost (microseconds). For Interactive
  wakes we want to hit a P-core; for cold systems we may pre-warm
  one P-core on user-presence detection. Userspace policy via a
  `KeepWarmCap`.
