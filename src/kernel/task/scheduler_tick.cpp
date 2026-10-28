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

/// @file scheduler_tick.cpp
/// @brief Timer-tick dispatch heart for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_tick_internal.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>    // issue #323 tick/dispatch diagnostics
#include <kernel/task/scheduler_queue_internal.hpp>     // issue #323 edf_* dispatch helpers
#include <kernel/task/scheduler_switch_internal.hpp>    // issue #323 current_sp/H2 engine consts
#include <kernel/task/scheduler_lifecycle_internal.hpp> // issue #323 deferred-kill count
#include <kernel/task/scheduler_path.hpp>               // issue #347 dispatch_step
#include <kernel/task/sporadic_server.hpp>
#include <kernel/task/idle_monitor.hpp>                 // issue #323 idle_note_* hooks
#include <kernel/core/global_state.hpp>                 // issue #323 H2_REC
#include <kernel/log/dmesg.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <kernel/debug/ipc_sched_trace.hpp>
#include <kernel/time/posix_time.hpp>                   // issue #323 PosixTime
#include <kernel/ipc/pager_registry.hpp>                // issue #323 PagerRegistry
#include <kernel/arch/timer.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/memory/tlb_shootdown.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <signal.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>

namespace kernel {

using sched_queue::edf_eligible;
using sched_queue::edf_peek_valid_own;
using sched_queue::edf_preempt_candidate;
using sched_queue::edf_resort;
using sched_switch::current_sp;
using sched_switch::H2_EV_CLR_RMS;
using sched_lifecycle::s_deferred_kill_count;
using sched_debug::debug_tick_pc_;
using sched_debug::is_poisoned_block;
#if defined(CONFIG_DEBUG)
using sched_debug::g_diag_rms_count;
using sched_debug::g_diag_tick_count;
#endif

void Scheduler::on_tick() noexcept {
#if defined(CONFIG_DEBUG)
    ++g_diag_tick_count;
#endif
    // Issue #25 C1: APs run dispatch-only ticks (spec §3.4.4).  The full
    // body below is BSP-only (unchanged); the AP skips accounting,
    // deadlines, watchdogs, zombies and sporadic (non-real-time in C1).
    if (sched_cpu() != 0) {
        ap_tick();
        return;
    }
    uint64_t current_tick = arch::Timer::ticks();
#if defined(CONFIG_DEBUG_IPC_SCHED)
    if (all_tasks_.size() >= 6) {
        const bool lk_held = scheduler_lock_.try_lock();
        if (lk_held) {
            SchedulerLockGuard guard(scheduler_lock_, adopt_lock);
            IPC_SCHED_TRACE("[TICK]", "t=", current_tick, "lk=",
                            (uint64_t)lk_held, "h=",
                            (uint64_t)scheduler_lock_.holder(), "nt=",
                            (uint64_t)all_tasks_.size());
        }
    }
    // ==== Lock-contention detector (diagnostic only) ====
    // NOTE: on_tick may be called from task context (e.g. tests calling
    // Scheduler::on_tick() in a loop).  In that case the lock is legitimately
    // held by the *caller's* on_tick body, not by the ISR.  The timer ISR's
    // on_tick then repeatedly finds lk=0 until the caller's on_tick returns.
    // This is NOT a stuck lock — it is temporary contention.  Track the holder
    // address across invocations: if the holder changes, the lock was released
    // and re-acquired — not stuck.
    {
        bool held = scheduler_lock_.try_lock();
        const void *curr_holder = scheduler_lock_.holder();
        if (held) {
            SchedulerLockGuard guard(scheduler_lock_, adopt_lock);
            current_cpu().lk0_count = 0;
            current_cpu().last_holder = nullptr;
        } else {
            if (curr_holder != current_cpu().last_holder) {
                // Holder changed — lock was released and re-acquired
                current_cpu().lk0_count = 1;
                current_cpu().last_holder = curr_holder;
            } else {
                ++current_cpu().lk0_count;
            }
            if (current_cpu().lk0_count >= 200) {
                char buf[128];
                int p = 0;
                kernel::debug::fmt_str(buf, p, "[LK-CONTEND] h=");
                p = kernel::debug::fmt_u64(buf, p,
                    (uint64_t)curr_holder);
                kernel::debug::fmt_str(buf, p, " cur=");
                p = kernel::debug::fmt_u64(buf, p,
                    current_task() ? (uint64_t)current_task()->id : 0u);
                kernel::debug::fmt_str(buf, p, " st=");
                p = kernel::debug::fmt_u64(buf, p,
                    current_task()
                        ? (uint64_t)current_task()->state
                        : 99u);
                kernel::debug::fmt_str(buf, p, " sched=");
                p = kernel::debug::fmt_u64(buf, p,
                    (uint64_t)Scheduler::SwSlots::need_resched());
                buf[p] = 0;
                kernel::debug::trace(buf);
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC) continue;
                    char tb[96];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(tb, tp,
                        (uint64_t)t->state);
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(tb, tp,
                        t->in_ready_queue_ ? 1u : 0u);
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                // Log warning but do NOT halt — high contention is not
                // necessarily a stuck lock (e.g. task-context on_tick).
                kernel::debug::trace("[LK-CONTEND] possible lock contention");
                current_cpu().lk0_count = 0;
                current_cpu().last_holder = nullptr;
            }
        }
    }
#endif

    if (!preempt_enabled_) {
        return;
    }

    bool lock_acquired = scheduler_lock_.try_lock();
    if (lock_acquired) {
        SchedulerLockGuard guard(scheduler_lock_, adopt_lock);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        // Universal hang detector: if no actual context switch has occurred for
        // STALL_LIMIT ticks while a non-idle task is still runnable, the
        // scheduler is frozen (any of: deferred-switch CR3 race, blocked-in-runq
        // live-lock, stale-trigger self-deadlock).  Dump full state and halt so
        // ONE run yields complete evidence.
        {
            const uint64_t now = arch::Timer::ticks();
            if (current_cpu().last_switch_tick == 0)
                current_cpu().last_switch_tick = now; // prime on first tick
            // A stall is only real if a context switch is actually REQUIRED
            // (needs_switch()) but none has occurred.  The previous predicate
            // ("any non-idle task READY/RUNNING") fired during legitimate
            // long-running kernel tests (e.g. PmmExhaustion's 100k-iteration
            // allocation loop) where the current RUNNING task is the highest
            // effective priority and no preemption is due — that is correct
            // scheduler behavior, not a freeze.  Use needs_switch() so only a
            // genuine failure to dispatch a due task is reported.
            bool runnable = needs_switch();
            const uint64_t since_switch =
                (now > current_cpu().last_switch_tick) ? (now - current_cpu().last_switch_tick) : 0;
            if (runnable && since_switch > 300 && current_cpu().wedge_emitted < 8) {
                ++current_cpu().wedge_emitted;
                char wb[128];
                int wp = 0;
                 kernel::debug::fmt_str(wb, wp, "[STALL] ticks_since_switch=");
                 wp = kernel::debug::fmt_u64(wb, wp, since_switch);
                 kernel::debug::fmt_str(wb, wp, " cur=");
                 wp = kernel::debug::fmt_u64(
                     wb, wp,
                     (current_task()
                          ? static_cast<uint64_t>(current_task()->id)
                          : 0u));
                kernel::debug::fmt_str(wb, wp, " bm_lo=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_lo());
                kernel::debug::fmt_str(wb, wp, " bm_hi=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_hi());
                wb[wp] = 0;
                kernel::debug::trace(wb);
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC)
                        continue;
                    char tb[128];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, static_cast<uint64_t>(t->state));
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, t->in_ready_queue_ ? 1u : 0u);
                    kernel::debug::fmt_str(tb, tp, " rq=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->rq_priority_);
                    kernel::debug::fmt_str(tb, tp, " eff=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, effective_priority(t));
                    kernel::debug::fmt_str(tb, tp, " pg=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->get_page_table());
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                kernel::debug::trace("[STALL] HALT");
                arch::cli();
                for (;;)
                    arch::hlt();
            }
        }
        // H2 diagnostic detectors.  Both halt the CPU on catch so a SINGLE run
        // freezes QEMU with the full serial evidence (no brute-forcing).
        {
            uint64_t phys_rsp{};
            phys_rsp = current_sp();
            const uint64_t save_to = reinterpret_cast<uint64_t>(
                __atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE));

            // --- (C) INV-2 desync: a BLOCKED/WAITING task still in the runq
            //     (in_ready_queue_==1).  next_task() will keep selecting it,
            //     producing the live-lock (constant RSP, next=X repeated).
            bool blocked_in_runq = false;
            for (auto *t = all_tasks_.first_ptr(); t;
                 t = all_tasks_.next_ptr(t)) {
                if (t->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                if (is_idle_task(t))
                    continue;
                if (queue_target(*t) != sched_cpu())
                    continue; // issue #25 C1: own-CPU tasks only (no remote
                              // queue reads — torn metadata false-alarms)
                if (t->state == TaskState::BLOCKED ||
                    t->state == TaskState::WAITING) {
                    if (t->in_ready_queue_) {
                        blocked_in_runq = true;
                        if (current_cpu().wedge_emitted < 8) {
                            char wb[128];
                            int wp = 0;
                            kernel::debug::fmt_str(wb, wp,
                                                  "[WEDGE] blocked-in-runq id=");
                            wp = kernel::debug::fmt_u64(wb, wp, t->id);
                            kernel::debug::fmt_str(wb, wp, " st=");
                            wp = kernel::debug::fmt_u64(
                                wb, wp,
                                static_cast<uint64_t>(t->state));
                            kernel::debug::fmt_str(wb, wp, " inrq=1 phys=");
                            bool phys = false;
                            for (uint64_t p = 0;
                                 p <= CONFIG_PRIORITY_CEILING && !phys; ++p) {
                                if (rq_task(*t).queue(p).contains(*t))
                                    phys = true;
                            }
                            wp = kernel::debug::fmt_u64(wb, wp,
                                                        phys ? 1u : 0u);
                            wb[wp] = 0;
                            kernel::debug::trace(wb);
                        }
                    }
                }
            }

            // --- orphan: READY/RUNNING task flagged in_ready_queue_ but not
            //     physically linked in any priority queue.
            bool orphan_found = false;
            for (auto *t = all_tasks_.first_ptr(); t;
                 t = all_tasks_.next_ptr(t)) {
                if (t->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                if (is_idle_task(t) || t == current_task())
                    continue;
                if (queue_target(*t) != sched_cpu())
                    continue; // issue #25 C1: own-CPU tasks only
                if (t->state != TaskState::READY &&
                    t->state != TaskState::RUNNING)
                    continue;
                if (!t->in_ready_queue_)
                    continue;
                bool phys = false;
                for (uint64_t p = 0;
                     p <= CONFIG_PRIORITY_CEILING && !phys; ++p) {
                    if (rq_task(*t).queue(p).contains(*t))
                        phys = true;
                }
                if (!phys) {
                    orphan_found = true;
                    if (current_cpu().wedge_emitted < 8) {
                        IPC_SCHED_TRACE(
                            "[WEDGE]", "orphan=", t->id, "st=",
                            static_cast<uint64_t>(t->state), "inrq=",
                            t->in_ready_queue_ ? 1u : 0u, "phys=", 0u);
                    }
                }
            }

            if ((orphan_found || blocked_in_runq) && current_cpu().wedge_emitted < 8) {
                ++current_cpu().wedge_emitted;
                // Full dump then halt so this ONE run is sufficient evidence.
                char wb[128];
                int wp = 0;
                kernel::debug::fmt_str(wb, wp, "[WEDGE] save_to=");
                wp = kernel::debug::fmt_u64(wb, wp, save_to);
                kernel::debug::fmt_str(wb, wp, " cur=");
                wp = kernel::debug::fmt_u64(
                    wb, wp,
                    (current_task()
                         ? static_cast<uint64_t>(current_task()->id)
                         : 0u));
                kernel::debug::fmt_str(wb, wp, " bm_lo=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_lo());
                kernel::debug::fmt_str(wb, wp, " bm_hi=");
                wp = kernel::debug::fmt_u64(
                    wb, wp, rq_own().bitmap().raw_hi());
                kernel::debug::fmt_str(wb, wp, " physrsp=");
                wp = kernel::debug::fmt_u64(wb, wp, phys_rsp);
                wb[wp] = 0;
                kernel::debug::trace(wb);
                // Per-task summary.
                for (auto *t = all_tasks_.first_ptr(); t;
                     t = all_tasks_.next_ptr(t)) {
                    if (t->magic != TaskControlBlock::TCB_MAGIC)
                        continue;
                    char tb[128];
                    int tp = 0;
                    kernel::debug::fmt_str(tb, tp, "  T");
                    tp = kernel::debug::fmt_u64(tb, tp, t->id);
                    kernel::debug::fmt_str(tb, tp, " st=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, static_cast<uint64_t>(t->state));
                    kernel::debug::fmt_str(tb, tp, " inrq=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, t->in_ready_queue_ ? 1u : 0u);
                    kernel::debug::fmt_str(tb, tp, " rq=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->rq_priority_);
                    kernel::debug::fmt_str(tb, tp, " eff=");
                    tp = kernel::debug::fmt_u64(
                        tb, tp, effective_priority(t));
                    kernel::debug::fmt_str(tb, tp, " pg=");
                    tp = kernel::debug::fmt_u64(tb, tp, t->get_page_table());
                    tb[tp] = 0;
                    kernel::debug::trace(tb);
                }
                // Only halt for orphans (INV-2 violation: READY/RUNNING task
                // not physically linked).  BLOCKED+inrq is benign — the
                // scheduler handles it via next_task()'s while loop.
                if (!orphan_found) {
                    kernel::debug::trace("[WEDGE] block-in-runq skipped (benign)");
                } else {
                    kernel::debug::trace("[WEDGE] HALT");
                    arch::cli();
                    for (;;)
                        arch::hlt();
                }
            }
        }
#endif
#if CONFIG_DEADLINE_MONITOR_TASK
        // Harness owns lifecycle (see harness_owns_lifecycle): the reaper
        // must not race test ScopeGuards (double-free UAF otherwise).
        if (!harness_owns_lifecycle()) {
            __atomic_store_n(&s_scan_requested_, 1, __ATOMIC_RELEASE);
            // on_tick already holds scheduler_lock_ (acquired at line 548 and
            // released at line 839).  The monitor's block transition also takes
            // scheduler_lock_ (around both dequeue and the BLOCKED store), so
            // this wake's READY+enqueue is mutually exclusive with the monitor's
            // block — no half-blocked task can be re-enqueued (the [WEDGE] INV-5
            // violation).  Do NOT take the lock again here (non-recursive).
            if (s_monitor_task_ &&
                s_monitor_task_->magic == TaskControlBlock::TCB_MAGIC &&
                s_monitor_task_->state == TaskState::BLOCKED) {
                // G2: magic-guarded, provably live — bind a reference
                // (docs/irqguard-ledger.md §G2-A).
                TaskControlBlock &m = *s_monitor_task_;
                m.state = TaskState::READY;
                enqueue_ready(m);
            }
        }
#else
#if CONFIG_DEADLINE_MISS_DETECTION
        // O(1) deadline scan via DeadlineList.  Issue #25 C1: skip tasks
        // not affine to this CPU (re-insert + stop: the list is
        // time-ordered, so later entries expire later — another CPU's
        // tick services them; AP-affine tasks are not serviced in C1).
        while (auto *task = deadline_list_.pop_earliest_if_expired()) {
            if (queue_target(*task) != sched_cpu()) {
                deadline_list_.insert(*task);
                break;
            }
            if (task->get_sporadic_server()) {
                task->ss_state_on_deadline_miss =
                    static_cast<uint8_t>(task->get_sporadic_server()->state());
                task->ss_budget_on_deadline_miss =
                    task->get_sporadic_server()->remaining_budget();
            }
            task->deadline_missed = true;
            ++task->deadline_miss_count;
            deadline_miss_handler(*task,
                                  arch::Timer::ticks() - task->deadline_ticks);
        }
#endif
#endif // CONFIG_DEADLINE_MONITOR_TASK

        // Issue #17: event-timer wheel expiries for this CPU. Pure callee
        // of the tick (no timer programming, no source reads): now_ns
        // comes from the #16 monotonic clock. Bounded pop cap; skipped
        // expiries stay armed for the next tick. Callbacks run under the
        // outer scheduler_lock_: they must be non-blocking and must not
        // take scheduler services or reschedule (audit #17 contract).
        time::TimerWheel::on_tick(arch::Timer::ns_monotonic(),
                                  sched_cpu());

        // Issue #18: bounded-receive timeout apply. Wheel callbacks only
        // set recv_timed_out; the wake is applied here under the
        // already-held scheduler_lock_ (deadline-monitor wake precedent —
        // never take the non-recursive lock again). Guards mirror the
        // accounting walk below (magic, liveness, affinity).
        // Level-triggered (not edge): the wake re-applies every tick until
        // the waiter observes it (resume cancel) or dies (cleanup cancel).
        // A silently dequeued waiter is thus re-queued next tick instead
        // of stranding: enqueue_ready refuses duplicates, so re-apply is
        // idempotent. Never clobber a RUNNING state.
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (!__atomic_load_n(&task->recv_timeout_armed,
                                 __ATOMIC_ACQUIRE) ||
                !__atomic_load_n(&task->recv_timed_out, __ATOMIC_ACQUIRE))
                continue;
            if (task->generation != task->recv_timeout_gen)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;
            if (task->state == TaskState::BLOCKED) {
                task->state = TaskState::READY;
            } else if (task->state == TaskState::READY) {
                if (task->in_ready_queue_) {
                    // #345: already queued — duplicate per-tick re-wake
                    // would starve lower priorities (wdwake 1:1). Skip;
                    // stranded READY (!in_queue) still rescued below.
                    continue;
                }
                // Stranded READY (!in_queue) -> rescue via enqueue
            } else {
                // SIL 3 guard (audit issue #18): never queue RUNNING etc.
                // (current-never-queued invariant).
                continue;
            }
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }

        // Issue #76: bounded-nanosleep apply + timerfd waiter wakes. Wheel
        // callbacks only set sleep_expired / waiter_done; the wakes are
        // applied here under the already-held scheduler_lock_ (same
        // contract as the #18 block above). Level-triggered: a missed wake
        // re-applies next tick; enqueue_ready is idempotent. Never clobber
        // a RUNNING state (H2 guard, copied from above).
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (!__atomic_load_n(&task->sleep_armed, __ATOMIC_ACQUIRE) ||
                !__atomic_load_n(&task->sleep_expired, __ATOMIC_ACQUIRE))
                continue;
            if (task->generation != task->sleep_gen)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;
            if (task->state == TaskState::BLOCKED)
                task->state = TaskState::READY;
            else if (task->state != TaskState::READY)
                continue;
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }
        // Timerfd waiter wakes live in the PosixTime registry (slots are
        // not TCB fields): same lock position, leaf posix lock only.
        time::PosixTime::reapply_wakes(sched_cpu());

        // Issue #249: stranded-READY rescue. A non-current, non-idle task
        // that is READY but physically queued nowhere is undispatchable
        // (observed: shell id 6 wedged after an enqueue-while-current was
        // evicted by next_task's candidate==current branch, which pops and
        // clears in_ready_queue_; the unconditional dequeue_ready in
        // Notify::wait cannot repair a loss that happens after the last
        // dequeue, and the orphan detector above requires inrq==1 so it
        // is blind to this signature). Wake paths that strand a task do
        // not recur, so re-apply level-triggered every tick until the
        // task dispatches or dies. Detection-first: count + warn per
        // rescue (H2-ENQDEAD precedent) so a recurring wake-loss can never
        // hide behind the heal. EDF-eligible tasks are owned by
        // edf_peek_valid_own — never touch them here. Bounded by
        // STRANDED_RESCUE_SCAN_MAX; no foreign locks, no allocation,
        // never clobbers a non-READY state (H2 guard, copied from #18).
        uint64_t sweep_count = 0;
        for (auto *task = all_tasks_.first_ptr();
             task != nullptr && sweep_count < STRANDED_RESCUE_SCAN_MAX;
             task = all_tasks_.next_ptr(task)) {
            ++sweep_count;
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (is_idle_task(task) || task == current_task())
                continue;
            if (queue_target(*task) != sched_cpu())
                continue; // issue #25 C1: own-CPU tasks only
            if (edf_eligible(*task))
                continue; // EDF path owns these (edf_peek_valid_own)
            if (task->state != TaskState::READY)
                continue; // never clobber RUNNING/BLOCKED/etc (H2)
            if (task->in_ready_queue_ || task->in_edf_queue_)
                continue; // only the wedge signature
            bool phys = false;
            for (uint64_t prio_idx = 0;
                 prio_idx <= CONFIG_PRIORITY_CEILING && !phys; ++prio_idx) {
                if (rq_task(*task).queue(prio_idx).contains(*task))
                    phys = true;
            }
            if (phys)
                continue; // flag/link desync the other way — not ours
            // Issue #250: a legitimately-deferred task is always current
            // while flagged (wake-side mirror), and the sweep already
            // skips current — so non-current + flagged is a lost-consume
            // bug, not a valid deferral. Clear and rescue anyway (never
            // skip: skipping on flag would reintroduce the wedge on flag
            // loss).
            task->ready_deferred_ = false;
            uint64_t rescue_no = ++stuck_rescue_count_;
            if (rescue_no == 1 ||
                (rescue_no % STRANDED_RESCUE_WARN_EVERY) == 0) {
                Logger::warn(
                    "sched: stranded READY rescued #%lx id=%lx eff=%lx",
                    rescue_no, task->id, effective_priority(task));
            }
            enqueue_ready(*task);
            __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                             __ATOMIC_RELEASE);
        }

        // Accounting, WCET, alarms — common to both paths.  Issue #25 C1:
        // only tasks affine to this CPU (AP-affine tasks are not serviced).
        // Issue #154: executed_ticks is execution time — only the
        // interrupted (running) task is charged.  remaining_ticks,
        // deadline re-arm and alarms stay wall-clock for every affine
        // task (a parked READY task still consumes period and alarms).
        TaskControlBlock *const running = current_task();
        for (auto *task = all_tasks_.first_ptr(); task;
             task = all_tasks_.next_ptr(task)) {
            if (task->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (task->state == TaskState::TERMINATED)
                continue;
            if (queue_target(*task) != sched_cpu())
                continue;

            if (task == running) {
                ++task->executed_ticks;
                charge_exec(*task); // issue #21: ns billing, running task only
                // Issue #283: lifetime observed-maximum + exceed log live
                // in the idle_monitor seam (M5: unit-testable); the tick
                // path only invokes it for the running task.
                (void)idle_note_exec_sample(*task);
            }

            if (task->state == TaskState::RUNNING ||
                task->state == TaskState::READY) {
                uint64_t prev_rem = task->remaining_ticks;
                if (task->remaining_ticks > 0)
                    --task->remaining_ticks;
                if (prev_rem == 0 && task->period_ticks > 0) {
                    task->remaining_ticks = task->period_ticks;
                    // Issue #283: clean-boundary meet accounting lives in
                    // the idle_monitor seam (M5: unit-testable). Caveat:
                    // under CONFIG_DEADLINE_MONITOR_TASK the miss flag is
                    // transient (scan_deadlines sets then clears it in one
                    // pass), so meets there count reloads while misses stay
                    // exact in deadline_miss_count — the ratio still trends
                    // correctly; meets are a monitoring aid, never a
                    // safety invariant.
                    idle_note_period_reload(*task);
                    // Issue #21: new period, fresh budget clock (this
                    // tick was already billed into the old period above).
                    task->exec_period_ns = 0;
                    // Issue #331: stamp the new window phase (the
                    // blocked-branch advance below measures from here).
                    task->exec_window_start_tick = current_tick;
                    // Issue #19: the absolute deadline moved — re-sort a
                    // queued EDF task so dispatch order tracks it.
                    edf_resort(*task);
#if CONFIG_DEADLINE_MISS_DETECTION && !CONFIG_DEADLINE_MONITOR_TASK
                    task->deadline_ticks += task->period_ticks;
                    task->deadline_missed = false;
                    if (task->deadline_ticks > 0) {
                         deadline_list_.insert(*task);
                     }
#if CONFIG_WCET_OVERRUN_DETECTION
                    task->wcet_overrun_fired = false;
#endif
#endif
                }
            }

            // Issue #331: advance the PD_USE window for periodic tasks
            // that are neither RUNNING nor READY (e.g. BLOCKED in
            // sys_receive): their window never reloads above, so
            // exec_period_ns would accumulate lifetime exec and read
            // 100% forever. Observability only — no remaining_ticks /
            // deadline / EDF / queue changes (dispatch frozen).
            if (task->state != TaskState::RUNNING &&
                task->state != TaskState::READY &&
                task->period_ticks > 0 &&
                task->period_ticks != TaskControlBlock::NO_PERIOD) {
                const uint64_t elapsed =
                    current_tick - task->exec_window_start_tick;
                if (elapsed >= task->period_ticks) {
                    // Sample-then-reset (#154 binding): the monotonic-max
                    // WCET seam must see the closing window first.
                    (void)idle_note_exec_sample(*task);
                    task->exec_period_ns = 0;
                    idle_note_period_reload(*task);
                    task->exec_window_start_tick +=
                        (elapsed / task->period_ticks) *
                        task->period_ticks;
                }
            }

#if CONFIG_WCET_OVERRUN_DETECTION
            // Issue #154: a starved READY task must never trip the
            // overrun latch — only execution counts toward WCET.
            if (task == running && task->wcet_ticks > 0 &&
                !task->wcet_overrun_fired &&
                task->executed_ticks > task->wcet_ticks) {
                task->wcet_overrun_fired = true;
                wcet_overrun_handler(task,
                                     task->executed_ticks - task->wcet_ticks);
            }
#endif

            if (task->alarm_armed) {
                if (task->alarm_ticks > 0)
                    --task->alarm_ticks;
                if (task->alarm_ticks == 0) {
                    task->alarm_armed = false;
                    task->pending_signals |=
                        (1ULL << static_cast<uint64_t>(Signal::SIGALRM));
                    // Issue #278: same lost-wakeup hole as sys_kill — a
                    // wheel-blocked sleeper would sit until wheel expiry.
                    // Sleepers only; set_task_ready is deferral-safe.
                    if (task->state == TaskState::BLOCKED &&
                        task->sleep_armed)
                        Scheduler::set_task_ready(*task);
                }
            }
        }

        // Issue #172: load-average decay (top triplet).  Integer EMA of
        // the per-tick non-idle sample at LOADAVG_SCALE precision (the
        // ×1000 display scale alone truncates every step to zero).
        // Runs under the already-held scheduler_lock_; additive only, no
        // alloc/log.  Tickless windows skip decay (stale, bounded —
        // documented, not incorrect).
        {
            const uint64_t instant =
                (running && !is_idle_task(running)) ? LOADAVG_SCALE : 0;
            // N = ticks per window at CONFIG_TICK_HZ (guard: HZ is a
            // non-zero build constant; windows ordered 1m < 5m < 15m).
            constexpr uint64_t kTicks1m =
                60ULL * static_cast<uint64_t>(CONFIG_TICK_HZ);
            constexpr uint64_t kTicks5m = 5ULL * kTicks1m;
            constexpr uint64_t kTicks15m = 15ULL * kTicks1m;
            loadavg_1min_ =
                loadavg_step(loadavg_1min_, instant, kTicks1m,
                             LOADAVG_SCALE);
            loadavg_5min_ =
                loadavg_step(loadavg_5min_, instant, kTicks5m,
                             LOADAVG_SCALE);
            loadavg_15min_ =
                loadavg_step(loadavg_15min_, instant, kTicks15m,
                             LOADAVG_SCALE);
        }
#if !CONFIG_DEADLINE_MONITOR_TASK
        __atomic_fetch_add(&deadline_detection_integrity, 1, __ATOMIC_RELEASE);
#endif
    }

    // FIX(sched-race): The deferred-kill flush, sporadic budget management,
    // and zombie reap/flush all mutate scheduler state (ready-queue priority
    // buckets via move_priority, sporadic server state read by
    // effective_priority(), zombie list).  on_tick() normally runs from the
    // timer ISR where IRQs are off, but it is ALSO invoked from task context
    // (tests).  When the scheduler lock was not acquired (a task holds it
    // mid-mutation), these tail sections must not run concurrently with the
    // lock holder.  Gate them on lock_acquired and hold IrqGuard so they are
    // atomic against both nested IRQs and the lock holder's partial writes.
    if (lock_acquired) {
        arch::IrqGuard irq_guard{};

#if defined(CONFIG_SNAPSHOT_CANARY_WATCH)
        // Snapshot-buffer canary watchdog: a stray write into the snapshot
        // region (observed as canary corruption at test ~846 in the `all`
        // suite) is otherwise only detected at the next snapshot_restore —
        // long after the corrupting instruction ran.  Polling here (every
        // tick) narrows the window to one tick and lets us dump the current
        // tick + task at the moment of detection.
        // Gated behind CONFIG_SNAPSHOT_CANARY_WATCH (default off): per-tick
        // reads are unnecessary overhead for normal suites; enable with
        // -DCONFIG_SNAPSHOT_CANARY_WATCH for a targeted corruption run.
        if (kernel::test::snapshot_canary_corrupted()) {
            auto *cc = current_task();
            Logger::raw_write("[SNAP-CANARY] corrupted at tick=");
            Logger::print_dec(current_tick);
            Logger::raw_write(" cur=");
            Logger::print_dec(cc ? cc->id : 0u);
            Logger::raw_write(" st=");
            Logger::print_dec(cc ? static_cast<uint64_t>(cc->state) : 99u);
            Logger::raw_write(" nt=");
            Logger::print_dec(all_tasks_.size());
            Logger::raw_write("\n");
        }
        // Per-tick TCB-magic watchdog (DEBUG): a corrupted current-task TCB is
        // otherwise only caught at the next switch/remove_task.  Detect it
        // here so the corrupting test/tick is attributable.  Skip when
        // all_tasks_ is empty: snapshot_restore() transiently points
        // current_task() at a not-yet-restored TCB with magic==0 while it
        // rewinds the task list (nt==0), which is a false positive.
        if (current_task() && all_tasks_.size() > 0 &&
            current_task()->magic != TaskControlBlock::TCB_MAGIC) {
            Logger::raw_write("[TCB-MAGIC] corrupt current at tick=");
            Logger::print_dec(current_tick);
            Logger::raw_write(" ptr=0x");
            Logger::print_hex(reinterpret_cast<uint64_t>(current_task()));
            Logger::raw_write(" magic=0x");
            Logger::print_hex(current_task()->magic);
            Logger::raw_write(" nt=");
            Logger::print_dec(all_tasks_.size());
            Logger::raw_write("\n");
        }
#endif

        // Issue #92 canary relocation: sample the current user task's segment
        // canaries on a bounded tick cadence (CONFIG_CANARY_SAMPLE_TICKS).
        // Catches corruption for FAST-only tasks that never hit a FULL syscall.
        // Pure reads; test-mode latch; production panic.  This runs inside the
        // scheduler_lock_ + IrqGuard window of on_tick, exactly like the
        // snapshot/TCB-magic watchdogs above.
#if CONFIG_CANARY_GUARD
        {
            auto *cc = current_task();
            if (cc && cc->is_user_ &&
                cc->magic == TaskControlBlock::TCB_MAGIC &&
                (current_tick & (CONFIG_CANARY_SAMPLE_TICKS - 1)) == 0) {
                canary_check_in_scheduler_hooks(cc, 0);
            }
        }
#endif

        // Issue #107: external-pager fault watchdog — expire overdue delegated
        // faults (per-fault fail-closed, registration KEPT, VA poison latch).
        // Runs inside the scheduler_lock_ + IrqGuard window; records mid-map
        // (map_in_progress) are deferred this tick.
        kernel::ipc::PagerRegistry::watchdog_scan(current_tick);

        // Issue #41: per-task software watchdog expiry scan — single
        // bounded pass, disarmed/generation-mismatched tasks skipped.
        // Runs lock-free after the dispatch window closes (issue #369 T2a:
        // convicted — no lock held here; the scan loop itself takes no
        // locks, though KILL arms reach wake_waiting_parent -> set_task_ready
        // downstream with nothing held); KILL defers via defer_kill, never
        // inline.
        scan_watchdogs_locked(current_tick);

        if (s_deferred_kill_count > 0)
            Scheduler::process_deferred_kills();

        // Sporadic Server budget management.  Issue #25 C1: only tasks
        // affine here (AP-affine sporadic tasks are not serviced).
        {
        auto *cur = current_task();
        uint64_t found = 0;
        for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
            if (found >= sporadic_task_count_)
                break;
            if (t->magic != TaskControlBlock::TCB_MAGIC)
                continue;
            if (queue_target(*t) != sched_cpu())
                continue;
            if (t->get_sporadic_server()) {
                found++;
                if (reinterpret_cast<uint64_t>(t->get_sporadic_server()) ==
                    0xFFFFFFFFFFFFFFFFULL) {
                    Logger::raw_write("[BUG] on_tick: sporadic_server=-1\n");
                    continue;
                }
// FIX(ss-race): is_poisoned_block checks for a freed TCB whose
                // sporadic_server field has been poisoned (0xDDDD...) by the
                // MemPool free path.  During daemon teardown the sporadic server
                // object may be destroyed while a terminated task still has a
                // stale pointer.  Without this guard the subsequent
                // process_replenishments() call would dereference freed memory.
                // This check is safe because no active task has a valid pointer
                // that matches the poison pattern — the poison value is chosen
                // to be outside any valid allocation.
                if (is_poisoned_block(t->get_sporadic_server())) {
                    continue;
                }
                // Ownership contract: the getter returns the O(1) read-cache;
                // teardown is serialized against on_tick by scheduler_lock_,
                // and effective_priority() guards the pointer with
                // is_poisoned_block() + the canonical-address check, so no
                // ScopedRef is needed on this hot path (it would add two
                // atomics per sporadic task per tick, perturbing the
                // timing-sensitive H2 race — ROADMAP §v0.3.9).
                task::SporadicServer *ss = t->get_sporadic_server();
                // FIX(rms-o1): Effective priority may change after replenishment
                // (e.g. budget restored → sporadic server priority rises).
                // The O(1) queue does not re-derive position from tcb.priority,
                // so we MUST call move_priority explicitly.  Skipping this leaves
                // the task at its depleted (lower) bucket, starving it until the
                // next reschedule, which may never come if the task is blocked.
                // Skip when t == cur — the current task is not in the ready
                // queue and move_priority on a non-enqueued task is undefined.
                {
                    uint64_t old_eff = effective_priority(t);
                    ss->process_replenishments(current_tick);
                    uint64_t new_eff = effective_priority(t);
                    // Issue #19: route through Scheduler::move_priority so
                    // EDF-queued tasks re-sort instead of bitmap-moving.
                    if (old_eff != new_eff && t != cur)
                        Scheduler::move_priority(*t, old_eff, new_eff);
                }
                if (t == cur && ss->is_active()) {
                    if (!ss->consume(current_tick)) {
#if CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE
                        if (!t->deadline_missed) {
                            t->ss_state_on_deadline_miss =
                                static_cast<uint8_t>(ss->state());
                            t->ss_budget_on_deadline_miss =
                                ss->remaining_budget();
                            t->deadline_missed = true;
                            ++t->deadline_miss_count;
                            deadline_miss_handler(*t, 0);
                        }
#endif
                        uint64_t rsp{};
                        rsp = current_sp();
                        uint64_t base =
                            reinterpret_cast<uint64_t>(cur->kernel_stack);
                        if (cur->kernel_stack && cur->kernel_stack_top &&
                            rsp >= base && rsp < cur->kernel_stack_top) {
                            Scheduler::reschedule();
                        }
                    }
                }
            }
        }
    }

    static constinit uint64_t tick_counter = 0;
    ++tick_counter;
    // Harness owns lifecycle (see harness_owns_lifecycle): the reaper must
    // not race test ScopeGuards (double-free UAF otherwise).  Deferring to
    // snapshot_restore is safe: test tasks are short-lived and few.
        if (tick_counter % 100 == 0) {
            if (!harness_owns_lifecycle())
                reap_orphans();
            // ZombieList watchdog: force-flush ONLY when the zombie list has
            // outgrown the starvation limit (idle hasn't kept up).  Gating on
            // zcount > LIMIT keeps small zombie populations (e.g. 1-2 test
            // tasks still referenced by a test ScopeGuard) alive until the
            // post-test snapshot_restore drains them — flushing them early
            // frees the TCB while the ScopeGuard still holds the pointer,
            // producing a use-after-free (magic corruption) in tests like
            // preemption_under_syscall.
            uint64_t zcount = __atomic_load_n(&zombie_count_, __ATOMIC_RELAXED);
            if (zcount > CONFIG_ZOMBIE_STARVATION_LIMIT) {
                flush_zombies(CONFIG_ZOMBIE_STARVATION_LIMIT / 2);
            }
            daemon::restart_stale_daemons();
        }
    }  // end: gated tail sections (gated on lock_acquired && IrqGuard;
       // NOTE (issue #369 T2a): gated on the *bool*, but the adopted
       // scheduler_lock_ scope already closed above — the tail runs
       // lock-free (takes no scheduler/zombie/wake locks itself).

    // Harness owns lifecycle (see harness_owns_lifecycle): tests drive
    // balancer_tick() directly so placement asserts stay deterministic.
    static constinit uint64_t balancer_cadence = 0;
    ++balancer_cadence;
    if (!harness_owns_lifecycle() &&
        (balancer_cadence % BALANCER_TICK_PERIOD) == 0) {
        balancer_tick();
    }

    // Issue #158: quarantine timeout poll (BSP tick; internally
    // try-locked + bounded, safe to call every tick; no-op when empty).
    TlbShootdown::on_tick(arch::Timer::ticks());

    rate_monotonic_schedule();
}

// ---------------------------------------------------------------------------
// Rate-monotonic schedule / reschedule
// ---------------------------------------------------------------------------

void Scheduler::rate_monotonic_schedule() noexcept {
#if defined(CONFIG_DEBUG)
    ++g_diag_rms_count;
#endif
    // Issue #26: RMS never raises TPR itself.  It tolerates preemption
    // only from strictly higher classes (the tick at 0xE0 is above every
    // raised class, INV-TPR5); the try_lock + skip discipline below is
    // unchanged by TPR state.
    if (all_tasks_.size() <= 1)
        return;

    if (!scheduler_lock_.try_lock())
        return;
    SchedulerLockGuard guard(scheduler_lock_, adopt_lock);

    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return;

    // Issue #225: debugger deferred-stop park. Runs inside the normal
    // tick decision (lock held) so the battle-tested pick/switch flow
    // below handles the parked task (including empty-queue/idle) exactly
    // like any blocking path — no bespoke ISR switch dance (whose
    // reschedule() arm can legitimately decide there is nothing to
    // switch to yet, stranding a BLOCKED-marked task on CPU). The stop
    // is taken only at a user-mode boundary: stop requested + RUNNING +
    // recorded interrupted PC below the user limit (nested ticks record
    // kernel PCs and skip for a later tick). Consumes the request; marks
    // has_utrap_frame (the tick wrote the U-frame slot). The park is
    // notified via debug_note_requested_park (event + poke, mirroring
    // the trap path): without it a pre-fetch park strands the target —
    // the debugger never learns the stop and sel2 polls empty forever
    // (issue #295). Detached tasks (id 0) never park: nobody could
    // resume them.
    {
        uint64_t cpu = sched_cpu();
        uint64_t tick_pc = (cpu < CONFIG_MAX_CPUS)
                               ? __atomic_load_n(&debug_tick_pc_[cpu],
                                                 __ATOMIC_RELAXED)
                               : CONFIG_USER_SPACE_LIMIT;
        if (cpu < CONFIG_MAX_CPUS &&
            __atomic_load_n(&current->debug_stop_requested,
                            __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&current->debugger_id, __ATOMIC_ACQUIRE) !=
                0 &&
            current->state == TaskState::RUNNING &&
            tick_pc < CONFIG_USER_SPACE_LIMIT) {
            __atomic_store_n(&current->debug_stop_requested, false,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&current->debug_parked, true, __ATOMIC_RELEASE);
            __atomic_store_n(&current->has_utrap_frame, true, __ATOMIC_RELEASE);
            current->state = TaskState::BLOCKED;
            dequeue_ready(*current);
            kernel::debug::debug_note_requested_park(*current, tick_pc);
        }
    }

#if CONFIG_CANARY_GUARD
    // v0.4.0 MP-3: per-tick kernel-stack canary check.  Pure read under the
    // scheduler lock; catches a corrupted stack base within one tick.  This
    // per-tick probe also serves as the documented H2-residual timing mask
    // (docs/_archive/ipc_blocking-analysis.md §H2: a single per-tick
    // instruction changed the boot interleaving and masked the residual
    // 25/25 runs).
    if (!canary_verify_kernel_stack(current)) {
        kernel::Logger::fatal(
            "CANARY TRIP (kernel stack, tick): task '%s' id=%u top=0x%lx",
            current->name, current->id,
            current->kernel_stack_top);
        panic("kernel-stack canary violated");
    }
#endif

    // BUGS.md#021: during the test cycle, do NOT preemptively switch away from
    // the harness (init_task, PID 1) while it is RUNNING.  The harness runs the
    // synchronous test bodies; being preempted by lower-priority test tasks
    // orphans it ... (see full comment in original).
    bool harness_nonpreempt =
        (is_test_active() && harness_task_ptr_ != nullptr &&
         current == harness_task_ptr_ &&
         current->state == TaskState::RUNNING);
    // Issue #19: EDF urgency overrides harness protection (same
    // switch_to_task machinery as priority preemption — the harness is
    // requeued to the bitmap and resumes when the EDF queue drains).
    // Without this, an EDF-ready task never preempts a spinning harness
    // once need_resched is clear (bitmap is blind to the EDF queue).
    if (harness_nonpreempt &&
        !__atomic_load_n(&Scheduler::SwSlots::need_resched(), __ATOMIC_ACQUIRE) &&
        edf_preempt_candidate() == nullptr) {
        uint64_t cur_prio = effective_priority(current);
        uint64_t highest_ready = rq_own().highest_ready_priority();
        if (highest_ready < cur_prio)
            return;
    }

    // Clear any pending deferred switch.  State symmetry: switch_to_task()
    // set the preempted current READY + enqueued it (and the target RUNNING);
    // clearing the arm without applying it must undo BOTH, or the
    // physically-running harness stays READY+queued (INV-4) and next_task()
    // skips it, falling through to idle (H2 residual hang).  This mirrors the
    // drop_arm restore in scheduler_validate_pending_switch (CLR-MISC).
    if (__atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE) != 0) {
        uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                         __ATOMIC_RELAXED);
        H2_REC(H2_EV_CLR_RMS, armed, 0, 0);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), (uint64_t)-1,
                         __ATOMIC_RELEASE);
        // Issue #221 (audit): a cleared arm must not leave a standing
        // force-apply behind (see cancel_pending_switch_cpu) — otherwise a
        // terminate-era force survives onto the next published normal arm.
        __atomic_store_n(&Scheduler::SwSlots::force_apply(), (uint64_t)0,
                         __ATOMIC_RELEASE);
        restore_preempted_current(current, armed);
    }

    // Issue #242: bounded starvation-breaker.  Strict RMS above starves
    // every lower level forever when the top READY level never drains
    // (convicted: two prio-11 never-blocking spinners alternate and the
    // prio-10 harness gets zero CPU over 3001 ticks).  When the top level H
    // is picked while lower non-idle READY levels wait for
    // kStarvationBreakerTicks consecutive ticks, force exactly one dispatch
    // below H, then reset to strict RMS.  Counter-only (task priorities are
    // never mutated: PI/admission untouched); O(1) bitmap ops; identical
    // debug/release flow; EDF keeps precedence (next_task checks EDF first,
    // and the breaker only overrides a strict bitmap pick, never an EDF one
    // — see below).  Worst-case interference is exactly one forced tick per
    // N = 20 backlogged ticks (docs/specs/scheduler.md).
    TaskControlBlock *breaker_next = nullptr;
    {
        uint64_t cpu = sched_cpu();
        uint64_t top = rq_own().highest_ready_priority();
        if (top == 0 ||
            rq_own().bitmap().highest_starvable_below(top) ==
                PriorityMap::STARVABLE_NONE) {
            starve_level_[cpu] = top;
            starve_ticks_[cpu] = 0;
        } else if (top == starve_level_[cpu]) {
            ++starve_ticks_[cpu];
        } else {
            starve_level_[cpu] = top;
            starve_ticks_[cpu] = 1;
        }
        if (starve_ticks_[cpu] >= kStarvationBreakerTicks) {
            // EDF keeps precedence: an EDF-pending tick is a non-strict
            // dispatch that breaks the starvation pattern, so the breaker
            // stands down (counter resets below) and next_task() serves the
            // EDF candidate as today.  edf_preempt_candidate() is safe here:
            // next_task() evaluates it in this same lock/IRQ context.
            if (edf_preempt_candidate() == nullptr) {
                breaker_next = next_task_below(top);
            }
            starve_level_[cpu] = PriorityMap::STARVABLE_NONE;
            starve_ticks_[cpu] = 0;
        }
    }
    auto *next = breaker_next ? breaker_next : next_task();
#if defined(CONFIG_DEBUG_IPC_SCHED)
    if (all_tasks_.size() == 7) {
        auto *r6 = Scheduler::find_task(6);
        IPC_SCHED_TRACE("[RMS]", "cur=", current->id, "next=",
                        next ? next->id : 0u, "t6=",
                        r6 ? (uint64_t)r6->state : 9u, "q6=",
                        r6 ? (uint64_t)r6->in_ready_queue_ : 9u);
    }
#endif
    // Defense-in-depth (H2 residual): never iretq the physically-running test
    // harness into the idle loop.  If the harness were left READY (INV-4) and
    // skipped by next_task(), the guard below would fall through to idle and
    // strand it; the harness must continue as the current task instead.  The
    // state-restore symmetry in the CLR paths above keeps it RUNNING, so this
    // is belt-and-braces for any residual path that still leaves it READY.
    bool harness_current =
        (is_test_active() && harness_task_ptr_ != nullptr &&
         current == harness_task_ptr_);
    if (next && next != current &&
        !(next == own_idle() &&
          (current->state == TaskState::RUNNING || harness_current))) {
        switch_to_task(current, *next, nullptr);
    }

    __atomic_store_n(&Scheduler::SwSlots::need_resched(), false, __ATOMIC_RELEASE);
}

void Scheduler::reschedule() noexcept {
    // Single-core UP: read-only ready-queue peek needs only IRQ-safety,
    // not the full scheduler_lock_ (issue #369 T4b: it is IF=0 — not a
    // held lock — that prevents the timer ISR from running at all here,
    // so its try_lock() in rate_monotonic_schedule() never even fires;
    // without this guard an ISR tick between the peek below and the
    // switch_to_task() call could dispatch a different task first,
    // stranding this reschedule's pick for a tick (latency, not
    // corruption — the generation re-verify still orders the applies).
    // Lock-free by design: the ipc_blocking harness spins in
    // `while (state != TERMINATED) reschedule();` — a blocking take here
    // would self-deadlock any holder (see T2b no-reentrant-take rule).
    arch::IrqGuard irq_guard{};

    if (all_tasks_.size() <= 1)
        return;

    auto *current = current_task();
    if (!current || current->magic != TaskControlBlock::TCB_MAGIC)
        return;

    // Peek the highest-priority ready task.  We do NOT dequeue here —
    // the switch is deferred (INV-4) and the actual dequeue happens in
    // rate_monotonic_schedule() -> next_task() on the next timer tick.
    auto *next = rq_own().peek_highest();
    // Issue #19: an EDF arrival preempts even when the bitmap peek shows
    // nothing better — the bitmap cannot see the EDF queue.  Arm
    // unconditionally: bitmap-head early returns below (idle/non-READY
    // head) must never swallow EDF urgency.  A spurious arm is harmless —
    // next_task() re-validates on the tick.
    bool edf_preempt = (edf_preempt_candidate() != nullptr);
#if defined(CONFIG_DEBUG_IPC_SCHED)
    {
        IPC_SCHED_TRACE("[RS]", "cur=", current->id, "next=",
                        next ? next->id : 0u, "hi=",
                        (uint64_t)rq_own().highest_ready_priority(),
                        "nt=", (uint64_t)all_tasks_.size());
    }
#endif
    if (edf_preempt) {
        __atomic_store_n(&Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        return;
    }

    if ((!next || next == current))
        return;

    if (next != nullptr) {
        if (next == own_idle() && current->state == TaskState::RUNNING)
            return;

        if (next->state != TaskState::READY &&
            next->state != TaskState::RUNNING)
            return;
    }

    // IrqGuard destructor re-enables IRQs here, allowing the timer ISR to
    // fire and acquire scheduler_lock_ for rate_monotonic_schedule().
    __atomic_store_n(&Scheduler::SwSlots::need_resched(), true, __ATOMIC_RELEASE);
    kernel::dispatch_step(kernel::SchedDispatchSteps::Arm); // issue #347
}
}  // namespace kernel
