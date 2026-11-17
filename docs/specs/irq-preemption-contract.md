# Preemption, IRQ and Lock Contract (#546, implements §6.2 of the microkernel plan)

**Status:** normative for M2. Every claim cites `path:line`; line numbers are advisory.

## 1. Trap-entry interrupt state (per arch, verified)

| Arch | Entry contract | Evidence |
|---|---|---|
| x86_64 | SYSCALL is `int $0x80` through interrupt gate `0xEE` ⇒ **IF=0 on entry**. No handler may depend on entry-IF except as stated below. | `arch/x86_64/hal/idt.cpp:69-71` |
| aarch64 | Exception vectors mask interrupts on entry (`msr daifset` in the fault/park paths; IRQ entry runs masked). No handler may assume DAIF clear. | `arch/aarch64/vectors.S:112,241` |
| riscv64 | First trap instruction is `csrci sstatus, 2` ⇒ **SIE=0 on entry**, before any state change. | `arch/riscv64/syscall_entry.S:62,71` |

Rule: no path depends on entry-IF/DAIF/SIE except by declaring it. Interrupts are re-enabled only (a) by `arch::IrqGuard` scoping, or (b) by an explicit `arch::sti()` inside a declared wait loop that owns the IRQ state (syscall/trap entry with no locks held — #513).

## 2. Lock order and acquisition ranks

Outer → inner: `Sched(1)` → `Queue(2)` → `Leaf(3)`. Unranked locks are invisible to the order check (documented per site; M8 re-audits).

| Lock | Rank | Declared at |
|---|---|---|
| `scheduler_lock_` | Sched | `task/scheduler.cpp:189` |
| `MessageQueue::lock_` | Queue | `ipc/ipc.cpp:85` (`MessageQueue::init`) |
| `sync::Queue::lock_` | Queue | `sync/queue.cpp:35` (`Queue::init`) |
| `DeathNotify::s_lock_` | Leaf | `ipc/death_notify.cpp:36` |
| `g_iopb_lock` | Leaf | `arch/x86_64/hal/iopb.cpp:53` (order already documented there) |
| `wake_mailbox_lock_[]`, `zombie_lock_`, pager/registry locks | Unranked | Deliberately unchecked in M2; re-audited by M8 x86 IRQ hardening |

Enforcement: `sync::spinlock_order` records per-CPU acquire/release (owner-id matched, report-only, once per (outer,inner) pair). DEBUG-only stack; always-on depth counter (`spinlock_held_by_current_count`). Takes of `scheduler_lock_` itself are tracked by the separate per-CPU depth counter (`scheduler_lock_depth_own`), not the order stack, so Sched→* nesting is unchecked in M2 — the enforced invariant is the stronger one (no spinlock of any kind across `reschedule()` / BLOCKED), which INV-C1 asserts.

## 3. INV-C1: no spinlock across a blocking point

At every `Scheduler::reschedule()` entry (DEBUG, `CONFIG_IRQ_CONTRACT_ASSERT`):
no order-table spinlock may be held by the calling context
(`scheduler_tick.cpp`, INV-C1). Takes of `scheduler_lock_` itself are
tracked by the separate per-CPU depth counter with their own
no-reentrant-take ENSUREs (issue #369 T2b) — the on_tick adopted-guard
path legitimately calls `reschedule()` with it held. Level: warn +
dmesg by default (`SCHED+15`), `panic` only with
`CONFIG_IRQ_CONTRACT_ASSERT=2`. A preempted same-CPU holder is legal and
does not fire (owner-id, not per-CPU count).

Consequence: every BLOCKED transition happens lock-free (dequeue first, then
state, then reschedule — see §4), and every queue acquire that can sleep
goes through `ParkLockGuard` (M2), which parks only when INV-L1 holds.

## 4. INV-L1/L2/L3: bounded spin, then timed park, then re-arm

`ParkLockGuard` (sync/park_lock.{hpp,cpp}): try → bounded pause-spin
(`CONFIG_LOCK_SPIN_ROUNDS=1024`, tick-watchdog checkpoint every 256) → timed
park (≤ `CONFIG_LOCK_PARK_MAX_ATTEMPTS=8` × `CONFIG_LOCK_PARK_TICKS=2`) →
park re-arm (≤ `CONFIG_LOCK_PARK_REARM_BUDGET=64` windows, one loud line
per site when the budget is spent, then re-arming continues). Park
precondition (all must hold): park enabled, no ISR nesting, no live
`IrqGuard`, no held spinlock, current task live, non-idle, RUNNING. The
park wake is the existing level-triggered sleep-expiry scan (no waiter
registry, no unlock-side hooks — INV-L3). Resume restores RUNNING after
dequeue (INV-L2).

INV-L1' (wedge hunt, #545): the guard returns with the caller's
IRQ-enable state restored on **every** exit path (entry snapshot in the
ctor; `take_success` repairs after park windows). INV-W1' (strengthened):
no unbounded wait may run with interrupts disabled while a live holder
exists — parking (timer alive) is the only unbounded wait, so the
exhausted fallback re-arms instead of spinning with IF=0. An IF=0 spin
starves the tick that would dispatch the holder (observed:
`lock_spin_exhausted` + `if=0` + frozen ticks → self-completing wedge).
A dead timer degrades to an entry-IF-preserving bounded loud spin
(`ArmFailed` bridge), never to an unbounded IF=0 spin.

Conviction diagnostics (DEBUG, serial): per-site one-shot `denied-spin
escape` (order-stack holder task/rank), one-shot `exhausted` dump
(holder take site/cpu/take-IF + `find_task` snapshot: state, in-rq,
prio, blocked-on-queue/recv, reply-wait, sleep-armed — decides A1 hold
across BLOCKED vs A2 READY-but-starved vs A3 READY-not-queued), and
`rearm=` on the TICKWD FIRE line plus `lock_park_rearm_count()`.

INV-4b (new, binding): a task with `state == BLOCKED` takes **no
spinlock**.  The dequeue + BLOCKED marking + (where needed) unlock happen
as one atomic IF=0 step; every loop that can observe BLOCKED gates its
lock-taking paths on `state != BLOCKED` and falls through to
`reschedule()` + `hlt`-wait otherwise.  A BLOCKED task that runs anyway
(gate-denied reschedule) must not take locks — a tick-tail preemption
mid-take strands the holder with no INV-C1 warning, since the check
lives only on the reschedule path.  Try-once Dekker rule for
publish-then-wait loops: the post-publish recheck is a single non-blocking
`try_lock` inside the publish `IrqGuard` (IF=0 atomic vs tick
preemption); on Contended the waiter proceeds to block (the holder's
wake-gate read observes the publish, so no retry is needed and no IF=0
spin ever happens).

P1 residual follow-ups (same shape, other subsystems — tracked, not fixed
in this cycle): sync-queue/notify/eventgroup/semaphore take-while-BLOCKED
windows, `dequeue_ready` with IF=1 (RQ splice vs tick), `syscall_handlers_irq`
trap waits, `posix_time`/`devfs` sleep loops.  Same recipe applies: gate
takes on `state != BLOCKED`, or move them inside an IF=0 publish guard
with try-once semantics.

Converted sites (all task-context-verified, no ISR caller exists — see §6):
`MessageQueue::{push,pop,pop_clamped,pop_from_sender,pop_from_sender_clamped,
has_from,is_full_locked}`, `block_sender` list section, `wake_sender`
pop + PI-restore sections, endpoint `bound_receiver` read. Explicitly NOT
converted: `unblock_sender_rollback` (fail-fast path must stay instant;
parking there would turn a bounded rollback into a multi-tick sleep).

## 5. Tick watchdog (TSC domain)

`task/tick_watchdog.{hpp,cpp}`: fires iff raw TSC advances past
`grace = CONFIG_TICK_WATCHDOG_GRACE_TICKS (500)` timer-ticks while the tick
count `Timer::ticks()` is frozen and the CPU is out of idle. Independent
clock domain (`raw_counter`, never snapshot-rewound). Disabled when either
frequency is 0, before the first tick, and while the test harness owns the
clock. Fire = dmesg ERROR (TIMING+8) + serial evidence + one-shot latch +
`fire_count`; action 2 (`panic`) exists for the Q12-A fail-safe hook but M2
never wires it. `note_tick()` at the end of the BSP `on_tick` body;
`checkpoint()` on park spins/resumes and syscall exits; `idle_checkpoint()`
resyncs only (idle-current implies no stranded work).

Grace derivation for tuning: `grace = max(10 × largest measured IRQ-off
WCET tick-equivalent, 500 ticks)`. No measured WCET exists in-tree yet;
500 is the conservative default until `test_wcet_scheduler` produces one.

## 6. Guard-site ISR-reachability audit (M2 evidence)

All 12 converted sites live in `ipc.cpp` task-context paths (syscall
handlers `0x80`, `vfsd`/`iocd`/`ramdiskd`/`cap_broker`/`timer-srv`/`driver_pds`
task entries, reaper/test teardown). Verified absent: no `IPC::send`,
`MessageQueue`, `wake_sender` or `block_sender` use in `irq_delivery.cpp`
(own slot locks only), any `arch/*/hal/*isr*`, `interrupt_controller.cpp`
(all three arches), or driver ISR bodies. `Keyboard::try_forward_to_iocd`
(`arch/x86_64/keyboard.cpp:158-174`) is the fenced precedent: explicit
`isr_nesting_own()!=0` refusal + NONBLOCK send, so ISR keystrokes never
touch a queue lock. `virtio_net.cpp:464` uses the separate IRQ-disabling
`IrqSpinLockGuard` on a device lock (not converted, pure spin preserved).

## 7. Spin/wait-loop audit (all known task-context waits)

Convention per loop: `timer-driven/sti` (owns IF=0 entry, re-enables),
`bounded-poll` (trip cap, no deschedule), or `park` (reschedule-based).
The 13 syscall-adjacent sites plus sync-primitive, daemon, driver and
kernel-init loops are classified in the F8/M2 work notes on #545/#546;
any loop found with IF=0, no bound and no sti is a defect and gets its
own issue (not fixed inside M2 except the 12 converted sites).

## 8. WCET bounds for IRQ-off regions

No measured in-tree WCET exists yet (no unmeasured number is normative).
Recording method when `test_wcet_scheduler` lands: max cycles per region
→ ticks at `raw_counter_freq_hz` → ratchet `CONFIG_IRQ_STORM_LIMIT`,
`CONFIG_LOCK_SPIN_ROUNDS` and `CONFIG_TICK_WATCHDOG_GRACE_TICKS` against
it with ≥3× headroom. Until then the defaults above stand (conservative
by construction: park and watchdog both degrade to status-quo spinning,
never to new failure modes).
