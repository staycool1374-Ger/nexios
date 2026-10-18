/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/// @file scheduler_watchdog.cpp
/// @brief Watchdog/deadline-monitor engine for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_watchdog_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>       // issue #369 T1c validate_task_ptr
#include <kernel/task/scheduler_debug_internal.hpp> // issue #323 g_diag_* (STALLDUMP beacon)
#include <kernel/task/sporadic_server.hpp>      // issue #323 State::EXHAUSTED
#include <kernel/task/test_context.hpp>         // issue #323 TestContext (static def)
#include <kernel/memory/kernel_object.hpp>      // issue #323 ScopedRef
#include <kernel/memory/mempool.hpp>            // issue #323 MemPool::is_block_pinned
#include <kernel/log/dmesg.hpp>                 // issue #323 dmesg ring
#include <kernel/debug/debug_stop.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp> // issue #323 arch::pause
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <signal.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>

namespace kernel {

using sched_verify::validate_task_ptr;

#if defined(CONFIG_DEBUG)
using sched_debug::g_diag_rms_count;
using sched_debug::g_diag_switch_count;
using sched_debug::g_diag_tick_count;
#endif

TestContext *Scheduler::test_context_ = nullptr;
#if CONFIG_DEADLINE_MONITOR_TASK
constinit TaskControlBlock *Scheduler::s_monitor_task_ = nullptr;
bool Scheduler::s_scan_requested_ = false;
#endif

// Sporadic-server budget-exhausted predicate (issue #255 H5: named for the
// deadline-miss conjunction in deadline_miss_handler).  Pure: no locks,
// no side effects.
static bool budget_exhausted_on_miss(const TaskControlBlock &task) noexcept {
    return task.get_sporadic_server() != nullptr &&
           static_cast<task::SporadicServer::State>(
               task.ss_state_on_deadline_miss) ==
               task::SporadicServer::State::EXHAUSTED;
}

// Watchdog due predicate (issue #255 H5: shared by scan_watchdogs_locked
// and fire_stall_watchdog — the due conjunction was duplicated).  Pure:
// no locks, no side effects; cheap-flag-first order preserved.
static bool watchdog_due(const TaskControlBlock &task,
                         uint64_t now) noexcept {
    return task.wdog_armed && task.wdog_gen != 0 &&
           now >= task.wdog_expiry_tick;
}

// Pinned-expiry ring record (issue #255 H7: shared by the tick scan and the
// idle-seam fire path — the TIMING+6 ERROR push was duplicated).  Ring-only
// (no shell/serial), severity-gated; production pins nothing.
static void
note_pinned_watchdog_expiry(const TaskControlBlock &task,
                            uint64_t overdue_by_ticks) noexcept {
    log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                        log::kDmesgBase_TIMING + 6,
                        log::LogSeverity::ERROR, task.name,
                        overdue_by_ticks);
}

// ---------------------------------------------------------------------------
// Deadline-monitor task + scan (CONFIG_DEADLINE_MONITOR_TASK gate moves intact).
// ---------------------------------------------------------------------------

#if CONFIG_DEADLINE_MONITOR_TASK
void Scheduler::scan_deadlines() noexcept {
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
#if CONFIG_DEADLINE_MISS_DETECTION
    // Scan every live task rather than only deadline_list_.  A task's deadline
    // may be (re)configured after add_task() returned, in which case it is not
    // a member of deadline_list_ yet — walking only that list would miss its
    // deadline and silently suppress the miss detection.  Scanning all tasks
    // is O(n) but n is small and keeps detection correct for every periodic
    // task regardless of when its deadline was set.
    const uint64_t now = arch::Timer::ticks();
    for (auto *task = all_tasks_.first_ptr(); task;
         task = all_tasks_.next_ptr(task)) {
        if (task->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        if (task->state == TaskState::TERMINATED)
            continue;
        if (task->period_ticks == 0 || task->deadline_ticks == 0)
            continue;
        if (task->deadline_missed)
            continue;
        if (task->deadline_ticks >= now)
            continue;
        if (task->get_sporadic_server()) {
            kernel::ScopedRef ss_ref{task->get_sporadic_server()};
            task->ss_state_on_deadline_miss = static_cast<uint8_t>(
                task->get_sporadic_server()->state());
            task->ss_budget_on_deadline_miss =
                task->get_sporadic_server()->remaining_budget();
        }
        task->deadline_missed = true;
        ++task->deadline_miss_count;
        deadline_miss_handler(*task, now - task->deadline_ticks);
        task->deadline_ticks += task->period_ticks;
        task->deadline_missed = false;
#if CONFIG_WCET_OVERRUN_DETECTION
        task->wcet_overrun_fired = false;
#endif
    }
#endif
    __atomic_fetch_add(&deadline_detection_integrity, 1, __ATOMIC_RELEASE);
}

void Scheduler::monitor_task_entry() noexcept {
    auto *me = Scheduler::current_task();
    while (true) {
        arch::pause();
        {
            arch::IrqGuard guard{};
            scheduler_lock_.lock();
            dequeue_ready(*me);
            // Set BLOCKED while STILL holding scheduler_lock_ so the on_tick
            // wake path (which also takes the lock before enqueueing) cannot
            // observe a half-blocked task and re-enqueue it -> BLOCKED+inrq
            // (the [WEDGE] INV-5 violation).  dequeue + state change must be
            // atomic with respect to the wake handshake.
            me->state = TaskState::BLOCKED;
            scheduler_lock_.unlock();
        }
        Scheduler::reschedule();

        if (__atomic_exchange_n(&s_scan_requested_, 0, __ATOMIC_ACQUIRE)) {
            scan_deadlines();
        }
    }
}

void Scheduler::ensure_monitor() noexcept {
    bool need_spawn = true;
    if (s_monitor_task_) {
        if (s_monitor_task_->magic == TaskControlBlock::TCB_MAGIC &&
            s_monitor_task_->state != TaskState::TERMINATED) {
            need_spawn = false;
        }
    }
    if (!need_spawn)
        return;

    Logger::info("[MON] Re-spawning deadline-monitor task");
    auto *tcb = TaskControlBlock::create(monitor_task_entry, 127,
                                         TaskControlBlock::NO_PERIOD);
    if (tcb) {
        __builtin_strncpy(tcb->name, "monitor", CONFIG_TASK_NAME_LEN - 1);
        tcb->name[CONFIG_TASK_NAME_LEN - 1] = '\0';
        s_monitor_task_ = tcb;
        __atomic_store_n(&s_scan_requested_, 0, __ATOMIC_RELEASE);
        add_task(*tcb);
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
        tcb->state = TaskState::BLOCKED;
        dequeue_ready(*tcb);
    }
}
#endif // CONFIG_DEADLINE_MONITOR_TASK

// ---------------------------------------------------------------------------
// Deadline-miss / WCET handlers (weak defaults)
// ---------------------------------------------------------------------------

#if CONFIG_DEADLINE_MISS_DETECTION
__attribute__((weak)) void
deadline_miss_handler(TaskControlBlock &task,
                      uint64_t missed_by_ticks) noexcept {
    bool budget_exhausted = budget_exhausted_on_miss(task);

#if defined(CONFIG_DEBUG)
    // Issue #299/#280: one-shot stall-state beacon.  The pre-suite stall
    // signature is a flood of "[DMD] Task 1 (init/reaper) missed deadline"
    // with zero S: lines — i.e. ticks still run but the first test never
    // starts.  Dump the full task set + monitor state once, from this
    // tick context, so a stalled run leaves reopen-grade evidence in the
    // serial log (no GDB attach needed).  Guarded so it fires at most once.
    static constinit uint64_t s_init_miss = 0;
    if (task.id == 1) {
        ++s_init_miss;
        if (s_init_miss == 100) {
        Logger::raw_write("[STALLDUMP] init miss=");
        Logger::print_dec(s_init_miss);
        Logger::raw_write(" ticks=");
        Logger::print_dec(arch::Timer::ticks());
        Logger::raw_write(" tick_ctr=");
        Logger::print_dec(g_diag_tick_count);
        Logger::raw_write(" rms=");
        Logger::print_dec(g_diag_rms_count);
        Logger::raw_write(" sw=");
        Logger::print_dec(g_diag_switch_count);
        Logger::raw_write("\n");
        Logger::raw_write("[STALLDUMP] suite state:\n");
        for (auto *t = Scheduler::all_tasks().first_ptr(); t;
             t = Scheduler::all_tasks().next_ptr(t)) {
            if (t->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            Logger::raw_write("[STALLDUMP] id=");
            Logger::print_dec(t->id);
            Logger::raw_write(" prio=");
            Logger::print_dec(t->priority);
            Logger::raw_write(" eff=");
            Logger::print_dec(Scheduler::effective_priority(t));
            Logger::raw_write(" state=");
            Logger::print_dec(static_cast<uint64_t>(t->state));
            Logger::raw_write(" inrq=");
            Logger::print_dec(t->in_ready_queue_ ? 1 : 0);
            Logger::raw_write(" blkrecv=");
            Logger::print_dec(t->blocked_in_recv ? 1 : 0);
            Logger::raw_write(" name=");
            Logger::raw_write(t->name);
            Logger::raw_write("\n");
        }
#if CONFIG_DEADLINE_MONITOR_TASK
        auto *m = Scheduler::get_monitor_task();
        Logger::raw_write("[STALLDUMP] monitor=");
        Logger::print_dec(m ? 1 : 0);
        if (m) {
            Logger::raw_write(" mstate=");
            Logger::print_dec(static_cast<uint64_t>(m->state));
            Logger::raw_write(" minrq=");
            Logger::print_dec(m->in_ready_queue_ ? 1 : 0);
        }
        Logger::raw_write("\n");
#endif
        auto *cur = Scheduler::current_task();
        Logger::raw_write("[STALLDUMP] current=");
        Logger::print_dec(cur ? cur->id : 0xFFFF);
        Logger::raw_write(" curstate=");
        Logger::print_dec(cur ? static_cast<uint64_t>(cur->state) : 0);
        Logger::raw_write(" curinrq=");
        Logger::print_dec(cur && cur->in_ready_queue_ ? 1 : 0);
        Logger::raw_write("\n");
        Scheduler::diag_ready_probe();
        }
    }
#endif

    // Issue #234: every deadline/budget event enters the dmesg ring (the
    // Logger lines above stay for the serial console). Severity ERROR: a
    // missed deadline is a fault whatever the configured response.
    log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                        log::kDmesgBase_TIMING + (budget_exhausted ? 2 : 1),
                        log::LogSeverity::ERROR, task.name, missed_by_ticks);

#if CONFIG_DEADLINE_ACTION == 1
    if (budget_exhausted)
        Logger::error("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                      "action=PANIC)",
                      task.id, task.name);
    else
        Logger::error("[DMD] Task %lu (%s) missed deadline by %lu ticks "
                      "(action=PANIC)",
                      task.id, task.name, missed_by_ticks);
    panic("[DMD] deadline miss (action=PANIC)");
#elif CONFIG_DEADLINE_ACTION == 2
    if (budget_exhausted)
        Logger::warn("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                     "action=DEMOTE)",
                     task.id, task.name);
    else
        Logger::warn(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=DEMOTE)",
            task.id, task.name, missed_by_ticks);
    if (task.priority > 1) {
        uint64_t old_prio = effective_priority(&task);
        task.priority >>= 1;
        uint64_t new_prio = effective_priority(&task);
        if (old_prio != new_prio)
            rq_task(task).move_priority(task, old_prio, new_prio);
    }
#elif CONFIG_DEADLINE_ACTION == 3
    if (budget_exhausted)
        Logger::warn("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                     "action=KILL)",
                     task.id, task.name);
    else
        Logger::warn(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=KILL)",
            task.id, task.name, missed_by_ticks);
    task.state = TaskState::TERMINATED;
    task.exit_code =
        static_cast<uint64_t>(-static_cast<int64_t>(Signal::SIGKILL));
    wake_waiting_parent(task);
    Scheduler::defer_kill(&task);
#elif CONFIG_DEADLINE_ACTION == 4
    if (budget_exhausted)
        Logger::info("[DMD] Task %lu (%s) budget exhausted (state=EXHAUSTED, "
                      "action=NOTIFY_MONITOR)",
                      task->id, task->name);
    else
        Logger::info("[DMD] Task %lu (%s) missed deadline by %lu ticks "
                      "(action=NOTIFY_MONITOR)",
                      task->id, task->name, missed_by_ticks);
    // Resolve the monitor PID: compile-time CONFIG_DEADLINE_MONITOR_PID when
    // set, otherwise the test-only override so the config matrix can target
    // the live [deadline-mon] task without a fixed compile-time PID.
    uint64_t monitor_pid = (CONFIG_DEADLINE_MONITOR_PID > 0)
                               ? static_cast<uint64_t>(CONFIG_DEADLINE_MONITOR_PID)
                               : (test_context_ ? test_context_->deadline_monitor_pid
                                                : 0);
    auto *monitor = Scheduler::find_task(monitor_pid);
    if (monitor && monitor->magic == TaskControlBlock::TCB_MAGIC &&
        monitor->state != TaskState::TERMINATED) {
        monitor->pending_signals |=
            (1ULL << static_cast<uint64_t>(Signal::SIGUSR1));
    }
#else
    if (budget_exhausted)
        Logger::info("[DMD] Task %lu (%s) budget exhausted (budget=%lu, "
                     "state=EXHAUSTED, action=LOG_ONLY)",
                     task.id, task.name, task.ss_budget_on_deadline_miss);
    else
        Logger::info(
            "[DMD] Task %lu (%s) missed deadline by %lu ticks (action=LOG_ONLY)",
            task.id, task.name, missed_by_ticks);
#endif
}
#endif

// Issue #41: per-task watchdog expiry scan. Lock-free by design (issue #369
// T2a: convicted — the on_tick caller runs it after the dispatch window
// closes, with no lock held; the scan loop itself (predicate/disarm/defer-
// append) takes no locks, so no order inversion is possible there. KILL /
// NOTIFY arms reach code that does take locks (wake_waiting_parent ->
// set_task_ready takes the global; monitor notify is IPC) — safe: nothing
// is held at those points. Single pass, no allocation, no blocking;
// expiry is one-shot (disarm first) and termination defers via defer_kill.
void Scheduler::scan_watchdogs_locked(uint64_t now) noexcept {
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        // NOTE (audit #41 iter-1, S1): no generation cross-check here.
        // t->generation is the global creation sequence, wdog_gen a per-TCB
        // arm counter — comparing them fail-open skips live watchdogs.
        // Staleness is covered by disarm-on-teardown + TaskFields rewind.
        if (!watchdog_due(*t, now))
            continue;
        // One-shot: disarm before dispatch so a slow handler cannot
        // re-fire on the same arm; re-CREATE re-arms with a fresh gen.
        t->wdog_armed = false;
        if (kernel::MemPool::is_block_pinned(t)) {
            // Test-isolation baseline (issue #277): pinned TCBs are part
            // of the snapshot (daemons in the test harness). Killing one
            // would free a pinned block and corrupt the baseline — the
            // same reason cleanup() skips teardown for pinned tasks.
            // Record the expiry in the ring, skip the action. In
            // production nothing is pinned, so this changes nothing
            // outside tests.
            note_pinned_watchdog_expiry(*t, now - t->wdog_expiry_tick);
            continue;
        }
        watchdog_expiry_handler(*t, now - t->wdog_expiry_tick);
    }
}

// Issue #285: idle-escalation fire path. Exact replica of the
// scan_watchdogs_locked fire sequence above (re-check under lock,
// disarm-first, pinned ring-only, same weak handler). Disarm-first
// ordering means the tick scan and the idle seam never double-fire,
// whichever observes the expiry first.
void Scheduler::fire_stall_watchdog(TaskControlBlock &task,
                                     uint64_t now) noexcept {
    ENSURE(validate_task_ptr(&task, "fire_stall_watchdog"));
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (!watchdog_due(task, now))
        return;
    task.wdog_armed = false;
    if (kernel::MemPool::is_block_pinned(&task)) {
        note_pinned_watchdog_expiry(task, now - task.wdog_expiry_tick);
        return;
    }
    watchdog_expiry_handler(task, now - task.wdog_expiry_tick);
}

__attribute__((weak)) void
watchdog_expiry_handler(TaskControlBlock &task,
                        uint64_t overdue_by_ticks) noexcept {
    // Issue #234/#41: every watchdog expiry enters the dmesg ring
    // (fail-closed record whatever the configured response).
    log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                        log::kDmesgBase_TIMING + 6, log::LogSeverity::ERROR,
                        task.name, overdue_by_ticks);

#if CONFIG_WATCHDOG_ACTION == 1
    Logger::error("[WDOG] Task %lu (%s) watchdog expired by %lu ticks "
                  "(action=PANIC)",
                  task.id, task.name, overdue_by_ticks);
    panic("[WDOG] watchdog expiry (action=PANIC)");
#elif CONFIG_WATCHDOG_ACTION == 2
    Logger::warn("[WDOG] Task %lu (%s) watchdog expired by %lu ticks "
                 "(action=DEMOTE)",
                 task.id, task.name, overdue_by_ticks);
    if (task.priority > 1) {
        uint64_t old_prio = effective_priority(&task);
        task.priority >>= 1;
        uint64_t new_prio = effective_priority(&task);
        if (old_prio != new_prio)
            rq_task(task).move_priority(task, old_prio, new_prio);
    }
#elif CONFIG_WATCHDOG_ACTION == 3
    Logger::warn("[WDOG] Task %lu (%s) watchdog expired by %lu ticks "
                 "(action=KILL)",
                 task.id, task.name, overdue_by_ticks);
    task.state = TaskState::TERMINATED;
    task.exit_code =
        static_cast<uint64_t>(-static_cast<int64_t>(Signal::SIGKILL));
    Scheduler::wake_waiting_parent(task);
    Scheduler::defer_kill(&task);
#elif CONFIG_WATCHDOG_ACTION == 4
    Logger::info("[WDOG] Task %lu (%s) watchdog expired by %lu ticks "
                 "(action=NOTIFY_MONITOR)",
                 task.id, task.name, overdue_by_ticks);
    // Monitor resolution mirrors deadline ACTION==4 (compile-time PID
    // when set, otherwise the test-only override).
    {
        uint64_t monitor_pid = (CONFIG_DEADLINE_MONITOR_PID > 0)
                                   ? static_cast<uint64_t>(
                                         CONFIG_DEADLINE_MONITOR_PID)
                                   : (test_context_
                                          ? test_context_
                                                ->deadline_monitor_pid
                                          : 0);
        auto *monitor = Scheduler::find_task(monitor_pid);
        if (monitor && monitor->magic == TaskControlBlock::TCB_MAGIC &&
            monitor->state != TaskState::TERMINATED) {
            monitor->pending_signals |=
                (1ULL << static_cast<uint64_t>(Signal::SIGUSR1));
        }
    }
#else
    Logger::info("[WDOG] Task %lu (%s) watchdog expired by %lu ticks "
                 "(action=LOG_ONLY)",
                 task.id, task.name, overdue_by_ticks);
#endif
}

#if CONFIG_WCET_OVERRUN_DETECTION
__attribute__((weak)) void
wcet_overrun_handler(TaskControlBlock *task,
                     uint64_t overrun_by_ticks) noexcept {
    Logger::info(
        "[WCET] Task %lu (%s) exceeded WCET by %lu ticks (action=LOG_ONLY)",
        task->id, task->name, overrun_by_ticks);
    // Issue #234: WCET overrun enters the ring as WARN (observed only).
    log::dmesg_push_sev(log::ErrorSubsystem::TIMING,
                        log::kDmesgBase_TIMING + 5, log::LogSeverity::WARN,
                        task->name, overrun_by_ticks);
}
#endif

}  // namespace kernel
