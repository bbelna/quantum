# Accounting

Resources in Quantum are *accounted*, not assumed. Every CPU cycle
consumed, every byte allocated, every IPC message sent, every joule
metered is charged to a budget. Budgets ride on capabilities. There
is no "best-effort" tier the kernel doesn't track — if a task uses a
resource, the kernel knows whose budget shrank.

This is the difference between a kernel that says "you're using too
much memory, your process is killed" and one that says "the budget you
were granted is exhausted; you can request more, degrade gracefully,
or fail predictably." Accounting is what makes the latter possible.

## Premise

Modern desktops run dozens of competing pieces of software, mostly
written by people who assume they own the machine. Without accounting,
the loudest neighbor wins: a runaway tab, an indexer with no mercy, an
update daemon that decided 2:00 PM was a fine time to compress a few
hundred GB. Linux's cgroups bolt accounting on after-the-fact, exposed
through opt-in interfaces. Quantum makes accounting the substrate.

The lessons we keep:

- **Per-resource budgets, not aggregate quotas.** CPU is a budget,
  memory is a budget, IPC bandwidth is a budget, and they're tracked
  separately because they're consumed separately.
- **Cap-bound budgets.** A task's authority to consume a resource is
  the cap that grants it; the cap carries the budget. Revoking the
  cap revokes the budget.
- **Hierarchical delegation.** A parent task can sub-allocate from its
  budget to children. The kernel enforces conservation: a parent
  cannot give what it doesn't hold; a child's usage charges the
  parent's pool too.
- **Bounded overshoot, predictable behavior on exhaustion.** No surprise
  kills. The exhaustion event is part of the API.

## What we account

Five resources, with hooks for more:

### CPU

CBS-style: `(budget Q, period P)` measured in microseconds. Per task,
per group. Charged at quantum end (also at IPC direct switch
boundaries — see [ipc.md](ipc.md)).

For Reflex-class work, charged in cycles, not microseconds, because
microseconds is too coarse and the work is bounded by spec.

### Memory

Bytes held, summed across:

- `MemObject`s the task owns directly.
- `MemObject`s mapped into the task's address spaces (mapping is a
  charge against the *mapper*, not the owner — this disambiguates
  shared memory).

Per task and per group. Memory budget exhaustion at retype time fails
the retype with `BudgetExceeded`; existing mappings are unaffected.

### IPC

Messages per period, both posted and received. Per endpoint and per
task. Endpoints can carry per-cap rate limits ("this delegated cap may
post 100 msgs/sec"); the system service that issued the cap chose the
limit.

The point isn't to slow down hot IPC — Quantum's IPC is fast and most
caps will have unlimited rate. The point is to stop a buggy / hostile
sender from flooding a service. A flood is detected, rate-limited at
the cap, and reported.

### Energy

Joules consumed, measured by the HAL via platform energy counters
(RAPL on x86, equivalent on AArch64). Per task. The HAL aggregates
energy at scheduling-decision boundaries; `core` redistributes to the
running task's pool.

Energy is the resource that matters most for laptop desktop. A
"battery saver" policy is exactly an energy budget cap with a slow
period; tasks that exceed get demoted to E-cores or throttled.

### I/O

Bytes per period, per device cap. NVMe, USB, network — all granted by
device caps. Servers (the filesystem service, the network stack)
hold the device caps and propagate I/O budgets to clients via their
own cap delegations.

The kernel itself doesn't directly account I/O; the device-owning
service does. We expose the *primitive* (a `RateLimit` field on caps)
and let services use it.

## The Budget cap

A `BudgetCap` carries the per-resource pools:

```rust
pub struct Budget {
    cpu:    CpuBudget,         // (Q, P) for CBS, plus accumulated usage
    memory: MemoryBudget,      // bytes allowed, bytes held
    ipc:    IpcBudget,         // msg/period
    energy: EnergyBudget,      // J/period
    parent: Option<BudgetId>,  // hierarchical
}
```

Every task has exactly one Budget, supplied at spawn time. The task's
cap table is initialized with a `Cap<Budget, Read | Spend>`. Spending
happens implicitly as the task runs.

### Sub-allocation

A parent task with `Cap<Budget, Spawn>` can derive a child Budget for
a new task:

```rust
let child = budget_derive(parent_cap, BudgetSpec {
    cpu:    CpuBudget { q: 5_000, p: 100_000 },     // 5%
    memory: 64 << 20,                               // 64 MiB
    ipc:    1_000,                                   // 1k msg/sec
    energy: 100,                                     // 100 J/period
})?;
```

Constraints checked at derive:

- Each component must be `≤` parent's available. (Available =
  granted − sum of derivatives.)
- The derive consumes those resources from the parent's pool until
  the child is destroyed (or its budget revoked).

### Charging

Every quantum boundary, every `retype`, every `ipc_post`, every
energy-meter sample charges the appropriate component of the running
task's budget. Charges propagate up the parent chain.

This means a parent always sees the *aggregate* usage of its subtree.
A user session's root budget sees every byte and every cycle the
session's apps consume.

### Exhaustion

When a component reaches its bound, the kernel takes an
exhaustion-class action:

- **CPU**: CBS naturally backs off — the task waits until the next
  period. No event needed; this is normal CBS operation.
- **Memory**: `retype` fails with `BudgetExceeded`. Existing
  allocations unaffected. The task may free, request more (from a
  service that can grow the budget), or report and fail.
- **IPC**: `ipc_post` fails with `BudgetExceeded`. Receiver-side rate
  limits queue the message but apply backpressure (sync IPC blocks
  the sender until budget restores).
- **Energy**: scheduler intervenes — task demoted to E-core, frequency
  capped. Exhaustion is a soft policy, not a hard stop.

In every case, the exhaustion is *explicit* — typed in the API, no
sigkill in the night. Apps can be written to handle it.

### Refresh

Budgets refresh per period. Exact dynamics are per-component:

- **CPU**: Q replenishes at every period boundary (CBS rule).
- **Memory**: bytes are not periodically replenished; they are
  reclaimed only on free.
- **IPC**: messages-per-period refreshes at period boundary.
- **Energy**: J/period refreshes at period boundary; depending on
  policy, unused budget either accumulates (saving for a peak) or
  resets (use-it-or-lose-it).

## Group accounting

A `SchedGroup` (see [tasks_and_scheduling.md#group-scheduling](tasks_and_scheduling.md#group-scheduling))
shares a budget. Every task in the group draws from the group's pool
*in addition* to its own. Both must be in budget for an operation to
proceed.

This is how application-level isolation works: an app's tasks have
small individual budgets that don't matter much, plus a shared app-
group budget that does. A runaway task in app X cannot affect app Y.

Groups nest: a session group contains app groups contains task
budgets. Charges flow up the chain at every accounting point.

## Energy budgeting in detail

Energy is the desktop accounting story. A laptop user cares about
"how long until the battery is dead" much more than about "how many
microseconds did this routine take". The kernel's job is to translate
high-level user intent into per-task budgets that aggregate cleanly.

User policy levels:

- **Performance**: large energy budgets, full P-state range, no
  Background-class throttling.
- **Balanced**: medium budgets, full P-state range with bias toward
  efficient frequencies under load.
- **Battery saver**: small budgets, P-state capped, Background tasks
  paused, Bursty tasks coalesced aggressively.

The policy is a system-level cap delegation. It rewrites the root
session's `EnergyBudget` parameters; budgets cascade. The transition
is instant from the kernel's perspective — no per-task notifications;
when the task next runs the new budget applies.

### Why Joules and not "performance level"

Joules are conserved and meaningful. "Performance level" is a vibe.
With energy budgets we can say "this app was given 50 J this hour and
used 47" — concrete. The user can drag a slider in their UI and the
slider is wired to a real number that controls a real consumption.

### What we measure

The HAL exposes per-domain energy meters (RAPL on x86 has package /
DRAM / GPU domains; equivalents on AArch64). The HAL polls at
microsecond cadence and emits a delta on every poll. `core` charges
the delta to the currently-running task.

Granularity matters: charging energy at quantum boundary (every
0.5–200 ms) is too coarse for fast-scheduled tasks. We charge per HAL
poll, with a small per-charge overhead amortized.

For idle, energy is "free" in the sense that it's not charged to any
task — but it *is* tracked, attributed to the platform overhead.
This gives us a real number for "how much battery did this idle hour
cost" — useful for the user's mental model.

## Reservation patterns

Some workloads need a budget *reserved* — not "I'll get up to X" but
"I have X, guaranteed". Two cases:

- **L-C audio.** Audio service holds an L-C cap with a reserved
  budget; CBS admission control guarantees it. Adding another L-C
  task that would push admission over `U_max` fails — the audio
  service's reservation isn't disturbed.
- **Frame-deadline tasks.** The compositor reserves enough CPU per
  frame interval to do the worst-case frame; admission fails new L-C
  tasks if they'd jeopardize the reservation.

Reservation is encoded as the cap that grants the budget — its mere
existence means the budget is reserved. Revocation frees the budget
back to the pool.

## Observability

Per task: a `BudgetStats` block, accessible via a stats cap:

```rust
pub struct BudgetStats {
    cpu_used:       Duration,        // total since spawn
    cpu_period_used: Duration,       // current period
    memory_held:    u64,
    memory_peak:    u64,
    ipc_sent:       Counter,
    ipc_recv:       Counter,
    energy_used:    Joules,
    exhaustions:    HashMap<Resource, Counter>,
}
```

Aggregated by group. The desktop shell reads these to surface
"this app is using a lot of energy" or "this background job is
running unusually long." Same data the kernel uses for policy is the
data the user sees.

## What we deliberately don't have

- **Sigkill on overuse.** No silent OOM, no random kill. Exhaustion
  is typed, predictable, recoverable.
- **`getrusage`.** A weak ad-hoc spec we don't need; `BudgetStats` is
  the typed alternative.
- **Globally-summed fairness.** Fairness is between siblings of a
  budget, not across the whole system. The user explicitly grants the
  ratios.
- **Ad-hoc nice values.** Class + budget. No global priority knob.

## Open questions

- **Energy attribution under shared work.** When task A sends an
  IPC to task B and B does work on A's behalf, *whose* energy is
  charged? Today: B's CBS pool is charged for CPU (B did the work).
  Energy is harder — the user's mental model is "energy A spent",
  but the work happened in B. Probably charge energy to B's pool by
  default, with an explicit `EnergyTransferCap` for cases where the
  caller wants the energy bill (e.g., a service that explicitly bills
  back to clients).
- **Memory budget overhead.** Tracking bytes-held requires updating
  the budget on every retype/destroy. With many small objects,
  that's a per-op cost. We're betting that desktop workloads have
  few-but-large objects (graphics buffers, app heaps), but worth
  measuring.
- **Cross-budget memory sharing.** Two tasks that share a memory
  object: who's charged? Today: the *mapper*, each side independently.
  This means shared memory is "double-counted" against the system's
  total. Alternative: charge one side only (the owner), with a
  `Mapped` flag on the other; cleaner for shared-memory-heavy
  workloads, dirtier accounting for IPC. Open.
- **Period alignment.** All periodic budgets refresh per their own
  period. With many tasks at differing periods, the kernel handles
  many small wake-ups for refresh. We coalesce: the refresh wake is
  Background-class and aligned to a global tick (default 10 ms).
  Latency-Critical reservations bypass — they refresh exactly on
  their period.
