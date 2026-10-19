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

/// @file scheduler_switch.cpp
/// @brief Deferred-switch arm machinery for Scheduler (issue #323).
///        Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_switch_internal.hpp>
#include <kernel/task/scheduler_integrity.hpp>
#include <kernel/task/scheduler_debug_internal.hpp> // issue #323 g_diag_switch_count
#include <kernel/task/scheduler_path.hpp>
#include <kernel/task/idle_monitor.hpp> // issue #323 idle_note_progress
#include <kernel/core/global_state.hpp>
#include <kernel/memory/vmm.hpp> // issue #323 VMM::get_kernel_pml4 (switch publish path)
#include <kernel/debug/ipc_sched_trace.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
#include <kernel/arch/x86_64/hal/pcid.hpp>
#endif

namespace kernel {

using sched_verify::task_stack_ptr;
using sched_verify::validate_switch;
using sched_verify::is_boot_stack_rsp;
using sched_verify::report_corruption;
using sched_verify::validate_task_ptr;
#if defined(CONFIG_ARCH_X86_64)
using sched_verify::validate_iret_frame;
#endif
// Issue #323 switch strand: current_sp()/IRET_* moved to the shared
// switch-internal home; import them so the moved bodies stay byte-identical.
using sched_switch::current_sp;
using sched_switch::IRET_CS_IDX;
using sched_switch::IRET_RFLAGS_IDX;
using sched_switch::IRET_RIP_IDX;
using sched_switch::IRET_RSP_IDX;
using sched_switch::IRET_SS_IDX;
using sched_switch::H2_EV_ARM;
using sched_switch::H2_EV_APPLY;
using sched_switch::H2_EV_CLR_RMS;
using sched_switch::H2_EV_CLR_SET;
using sched_switch::H2_EV_CLR_MISC;
using sched_switch::H2_EV_IDLE_ARM;
#if defined(CONFIG_DEBUG)
using sched_debug::g_diag_switch_count;
#endif

/// @brief Clear every deferred-switch atom and bump the switch generation.
///        Any ISR that captured the pre-clear generation fails its re-check
///        (isr_stubs.asm jne .restore); any ISR entering after sees
///        save_rsp_to==0.  Called from the apply-side liveness re-check and
///        from task removal paths.
void Scheduler::cancel_pending_switch() noexcept {
    H2_REC(H2_EV_CLR_MISC,
           __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_RELAXED),
           0, 0);
    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::next_task_id(), UINT64_MAX,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::force_apply(), (uint64_t)0,
                     __ATOMIC_RELEASE);
    uint64_t gen =
        __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
    __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                     __ATOMIC_RELEASE);
}

/// @brief Bill (ns_monotonic() - exec_stamp_ns) into the task's executed-ns
///        totals (issue #21 — feeds admission, WCET validation, SYS_TIMES).
///        Saturating adds (never wrap); always restamps.  A zero stamp means
///        "never stamped" (fresh task): stamp without billing so the first
///        charge never bills creation-to-first-dispatch as execution.
///        Caller must hold scheduler_lock_ (or run IRQ-off on the tick path)
///        so the add+restamp is atomic — never double-charge, never lose.
/// @param t Task to charge (must be the running task or a terminating task).
void charge_exec(TaskControlBlock &t) noexcept {
    const uint64_t now = arch::Timer::ns_monotonic();
    if (t.exec_stamp_ns == 0) {
        t.exec_stamp_ns = now;
        return;
    }
    const uint64_t delta =
        (now >= t.exec_stamp_ns) ? now - t.exec_stamp_ns : 0;
    if (delta > 0) {
        t.exec_ns_total = (t.exec_ns_total > UINT64_MAX - delta)
                              ? UINT64_MAX
                              : t.exec_ns_total + delta;
        t.exec_period_ns = (t.exec_period_ns > UINT64_MAX - delta)
                               ? UINT64_MAX
                               : t.exec_period_ns + delta;
    }
    t.exec_stamp_ns = now;
}

/// @brief Stamp a task being switched in: it starts accruing from now, and
///        any stale stamp (e.g. from a previous quantum) is discarded so no
///        descheduled interval is ever billed (issue #21).
/// @param t Task being dispatched.
static void stamp_exec(TaskControlBlock &t) noexcept {
    t.exec_stamp_ns = arch::Timer::ns_monotonic();
}

void Scheduler::set_current_index(uint64_t idx) noexcept {
    uint64_t i = 0;
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (i++ == idx) {
            set_current_ptr(t);
            return;
        }
    }
}

void Scheduler::set_current_task(TaskControlBlock *t) noexcept {
    if (t && t->magic == TaskControlBlock::TCB_MAGIC) {
        // Issue #172: adoption path publishes placement like a dispatch.
        __atomic_store_n(&t->running_on_cpu, Scheduler::sched_cpu(),
                         __ATOMIC_RELEASE);
        set_current_ptr(t);
    }
}

void Scheduler::set_current(TaskControlBlock &task) noexcept {
    ENSURE(validate_task_ptr(&task, "set_current"));
    auto *old = current_task();
    if (old == &task) {
        // Same-task set_current: a no-op, but a pending deferred-switch arm
        // must not survive — and if the preempted current was set READY +
        // enqueued by switch_to_task (boot-stack harness), restore it to
        // RUNNING so next_task() cannot skip it and fall to idle (INV-4).
        uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                         __ATOMIC_RELAXED);
        H2_REC(H2_EV_CLR_SET, armed, 0, 0);
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                         __ATOMIC_RELEASE);
        restore_preempted_current(old, armed);
        return;
    }
    // Invariant: a task that is physically executing (current) must never sit
    // in the ready queue, or next_task() will re-select it.  The idle/harness
    // task runs at priority 0xFFFFFFFF, so leaving it queued makes it always
    // preempt — wedging the `all` suite (e.g. deadlocking at
    // ipc_send_sync_no_cli / test 536).  Remove the previous current so it is
    // no longer eligible; it stays resumable because next_task() returns it as
    // the idle/default task when no other task is ready.
    if (old && old->magic == TaskControlBlock::TCB_MAGIC && old->in_ready_queue_) {
        // remove() unlinks old from the intrusive list AND clears
        // in_ready_queue_/rq_priority_ itself.  Clearing in_ready_queue_ here
        // first would make TaskQueue::remove early-return (it guards on
        // !in_ready_queue_), leaving old physically linked with a stale
        // runq_next_/runq_prev_ — a dangling node that later corrupts the list
        // (pop_front dereferences a freed/reused TCB → #GP) once old is freed.
        rq_own().remove(*old, old->rq_priority_);
    }
    // State symmetry: a pending deferred-switch arm is being discarded here.
    // If the preempted current (the boot-stack harness) was set READY +
    // enqueued by switch_to_task, restore it to RUNNING and re-enqueue the
    // armed target so neither is stranded (INV-4 / INV-2, H2 residual).
    uint64_t armed = __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_RELAXED);
    H2_REC(H2_EV_CLR_SET, armed, 0, 0);
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), (uint64_t *)nullptr,
                     __ATOMIC_RELEASE);
    restore_preempted_current(old, armed);

    // Re-enqueue the previous task if it is still runnable but NOT current
    // or idle.  Without this, a higher-priority task that was preempted by a
    // lower-priority one (e.g. IPC receiver after wakeup) cannot be found
    // again — it is not in the ready queue and not current, so the scheduler
    // has no path to dispatch it (lazy rebuild in next_task only runs when
    // dequeue_highest returns null, which never happens while the receiver
    // sits in the queue).  See docs/specs/ipc.md §4 (H2) and
    // docs/specs/scheduler.md §6 (VIOL-1).
    // Issue #250: consume a wake-while-current deferral the same way as
    // switch_to_task (clear-then-inner: enqueue_ready would re-defer
    // since old is still current until set_current_ptr below).
    if (old && old != &task && !is_idle_task(old) &&
        old->magic == TaskControlBlock::TCB_MAGIC &&
        (old->state == TaskState::READY ||
         old->state == TaskState::RUNNING)) {
        old->in_ready_queue_ = false;
        old->rq_priority_ = 0;
        old->ready_deferred_ = false;
        enqueue_ready_inner(*old);
    }

    set_current_ptr(&task);
}

/// @brief Restore scheduler state after a deferred-switch arm is cleared
///        WITHOUT being applied (CLR-RMS / CLR-SET / CLR-MISC symmetry).
///
/// switch_to_task() performs two side effects on the preempted current task
/// before publishing the arm (scheduler.cpp:2176-2178): it sets the current
/// task READY and enqueues it (when it was RUNNING or on the boot stack), and
/// sets the target RUNNING.  When the arm is later cleared instead of applied,
/// those side effects must be undone, otherwise the physically-running harness
/// (boot-stack current) is left `state=READY, in_ready_queue_=true` — INV-4 —
/// so next_task() skips it and falls through to idle (the H2 residual hang).
///
/// Caller must hold scheduler_lock_ or have IRQs disabled.  All runq ops used
/// here are lock-free and idempotent (TaskQueue::remove early-returns for a
/// node not physically in the queue; enqueue_ready refuses double-enqueues),
/// matching the drop_arm path (scheduler.cpp:3054-3074).
///
/// @param current        The preempted current task (the physical runner).
/// @param armed_target_id The `scheduler_next_task_id` captured BEFORE the
///                        atoms were cleared; UINT64_MAX if none.
void restore_preempted_current(TaskControlBlock *current,
                                      uint64_t armed_target_id) noexcept {
    if (current && current->magic == TaskControlBlock::TCB_MAGIC) {
        if (current->in_ready_queue_) {
            Scheduler::dequeue_ready(*current);
        }
        // Only undo switch_to_task's publish side effect (READY) — never
        // resurrect a TERMINATED or BLOCKED current (self-terminated trampoline
        // task or a blocked-impersonated peer); forcing those to RUNNING would
        // re-enqueue them at the next switch_to_task and dispatch a zombie.
        if (current->state == TaskState::READY) {
#ifdef CONFIG_DEBUG
            if (current == Scheduler::get_harness_task()) {
                kernel::debug::trace("[H2-RESTORE] harness READY->RUNNING armed=");
                // trace() takes one uint64; format the id into a small buffer.
                char buf[24];
                int p = 0;
                kernel::debug::fmt_u64(buf, p, armed_target_id);
                buf[p] = 0;
                kernel::debug::trace(buf);
            }
#endif
            current->state = TaskState::RUNNING;
        }
    }
    // Re-enqueue the armed target (undo switch_to_task's RUNNING + the dequeue
    // next_task() performed when selecting it) so it is not stranded (INV-2).
    if (armed_target_id != UINT64_MAX && armed_target_id != 0) {
        auto *target = Scheduler::find_task(armed_target_id);
        if (target && target != current && !Scheduler::is_idle_task(target) &&
            target->magic == TaskControlBlock::TCB_MAGIC &&
            (target->state == TaskState::READY ||
             target->state == TaskState::RUNNING)) {
            Scheduler::set_task_ready(*target);
        }
    }
}

// ---------------------------------------------------------------------------
// switch_to_task
// ---------------------------------------------------------------------------

#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
/// @brief Lazily assign a user PCID at first CR3 publish (issue #156).
///        Single choke point covering create/clone/elf births uniformly
///        (deviation D1 from the creation-site plan: the elf::load path
///        also births user address spaces).  Runs under scheduler_lock_
///        at both publish sites; fires once per task lifetime (later
///        publishes see nonzero pcid_).  Returns 0 when unsupported so
///        the publish stays raw-phys (full flush, today's behavior).
static uint16_t publish_pcid_for(TaskControlBlock &next) noexcept {
    if (next.is_user_ && next.pcid_ == 0 && arch::pcid_supported())
        next.pcid_ = arch::pcid_alloc();
    return next.pcid_;
}
#endif

void switch_to_task(TaskControlBlock *current, TaskControlBlock &next,
                    sync::SpinLock *held_lock = nullptr) {
#if defined(CONFIG_DEBUG)
    ++g_diag_switch_count;
#endif
    auto release_lock = [&]() {
        if (held_lock) {
            held_lock->unlock();
            held_lock = nullptr;
        }
    };

    if (!validate_switch(current, &next, "switch")) {
        report_corruption("switch");
        release_lock();
        return;
    }
    if (next.state != TaskState::READY && next.state != TaskState::RUNNING) {
        report_corruption("switch next state");
        release_lock();
        return;
    }
    if (current == &next) {
        release_lock();
        return;
    }
    kernel::dispatch_step(kernel::SchedDispatchSteps::Validate); // issue #347
    // Issue #21: bill the outgoing task's quantum, stamp the incoming one.
    // Both run under the caller's scheduler_lock_/IRQ-off discipline, so the
    // add+restamp pairs are atomic — no double-charge, no lost quantum.
    charge_exec(*current);
    stamp_exec(next);
    // Issue #285: dispatch = forward progress for the stall detector.
    // Runs under the caller's scheduler_lock_/IRQ-off discipline.
    idle_note_progress(next, arch::Timer::ticks());
    kernel::dispatch_step(kernel::SchedDispatchSteps::ChargeStamp); // #347

    uint64_t *save_target = &task_stack_ptr(current);
    bool cur_is_boot_stack = false;
    {
        uint64_t cur_rsp{};
        cur_rsp = current_sp();
        TaskControlBlock *owner = nullptr;
        const uint64_t cbase = reinterpret_cast<uint64_t>(current->kernel_stack);
        const bool cur_in_own_stack =
            current->kernel_stack && current->kernel_stack_top &&
            cur_rsp >= cbase && cur_rsp < current->kernel_stack_top;
        // Stack low-water mark: track the deepest RSP ever observed on this
        // task's kernel stack (grows down).  Sampled on every switch-out so
        // the `tasks` command can report high-water (used) / low-water (free).
        if (cur_in_own_stack &&
            (current->kstack_low_water_ == 0 ||
             cur_rsp < current->kstack_low_water_)) {
            current->kstack_low_water_ = cur_rsp;
        }
        if (is_boot_stack_rsp(cur_rsp)) {
            // H2 (docs/specs/ipc.md §4): the live RSP is on the kernel
            // boot stack — the harness (init/PID 1) runs there in test mode and
            // never switched onto its TCB kernel_stack, so NO TCB kernel_stack
            // covers this RSP.  Owner-resolution must NOT scan peers here: if
            // current_task() has drifted onto a peer TCB, saving into
            // `&task_stack_ptr(current)` would write the boot-stack RSP into the
            // peer's context.rsp (deferred-switch corruption).  Bind the save to
            // the harness TCB explicitly; the boot stack belongs to the harness,
            // not to any peer.
            auto *h = Scheduler::get_harness_task();
            owner = (h && h->magic == TaskControlBlock::TCB_MAGIC) ? h : current;
            cur_is_boot_stack = true;
        } else if (!cur_in_own_stack) {
            // The live RSP is not on the current task's own kernel stack.  Scan
            // all tasks for the real owner (drift correction: current_task()
            // may point at a peer TCB while the CPU actually runs on another
            // task's stack).
            for (uint64_t ti = 0; ti < Scheduler::task_count(); ++ti) {
                auto *tt = Scheduler::task_at(ti);
                if (!tt || tt->magic != TaskControlBlock::TCB_MAGIC)
                    continue;
                uint64_t tb = reinterpret_cast<uint64_t>(tt->kernel_stack);
                if (tt->kernel_stack && tt->kernel_stack_top &&
                    cur_rsp >= tb && cur_rsp < tt->kernel_stack_top) {
                    owner = tt;
                    break;
                }
            }
            if (owner == nullptr) {
                // The RSP sits on a foreign stack owned by NO TCB — the harness
                // (PID 1) running in test mode on a non-TCB boot stack (e.g.
                // outside the linker .boot_stack section).  Bind the save to the
                // harness and mark it for re-enqueue below so it is not stranded
                // (INV-2) when preempted.
                auto *h = Scheduler::get_harness_task();
                owner = (h && h->magic == TaskControlBlock::TCB_MAGIC) ? h
                                                                       : current;
                cur_is_boot_stack = true;
            }
        }
        if (owner && owner != current) {
            current = owner;
            Scheduler::set_current(*owner);
        }
        // H2 root fix (ROADMAP §v0.4.0): the owner-resolution above can
        // correct `current` to the PHYSICAL runner while `next` is that same
        // task — a condition that was already checked at entry (see above) but
        // can become true HERE, after the check.  Publishing a deferred switch
        // in this state is a SELF-switch: the ISR would save a fresh RSP into
        // the runner's context.rsp (save_target == &task_stack_ptr(next)) and
        // then iretq the PRE-SAVE (stale) load_rsp_from value, displacing the
        // runner onto a stale/fossil iret frame (docs/specs/ipc.md §4 H2).  No-op
        // instead: the runner keeps executing.  Mirror the bad-frame no-op's
        // queue-membership cleanup (state=RUNNING, out of the runq) so an
        // INV-4 leftover cannot strand it.
        if (current == &next) {
            next.state = TaskState::RUNNING;
            next.in_ready_queue_ = false;
            next.rq_priority_ = 0;
            release_lock();
            return;
        }
        kernel::dispatch_step(kernel::SchedDispatchSteps::OwnerResolve); // #347
        // H2: keep the resolved owner's context.rsp LIVE.  The original
        // scratch-save (layer 2) sent the harness's RSP to
        // s_foreign_rsp_scratch whenever it was not detected on the linker
        // boot stack, permanently freezing context.rsp at the FIRST switch-out
        // frame (e.g. arch_hlt in the daemon wait).  A later deferred switch
        // TO the harness then iretq'd it onto that stale frame, freezing the
        // suite (observed at `all` test 78 / 477, priority_inheritance).  The
        // harness's live RSP IS its true context regardless of which region it
        // runs on — always save it into the owner's own context.rsp.  The
        // apply-side liveness + ownership re-check
        // (scheduler_validate_pending_switch) and the dispatch-guard both
        // reject a stale/foreign arm before it can iretq, so keeping
        // context.rsp current is safe.
        save_target = &task_stack_ptr(current);
#ifdef CONFIG_DEBUG
        // H2 residual-race recorder (debug-only, fires ONLY on the orphaned
        // displacement — the harness physically executing on a non-boot-stack,
        // non-TCB page — never on the normal linker-boot-stack phase).  The
        // iret frame at cur_rsp+136..168 is the CPU-pushed pre-interrupt frame:
        // rip/cs/rflags/rsp/ss of the displaced execution.
        if (cur_is_boot_stack && !is_boot_stack_rsp(cur_rsp)) {
            static bool s_h2w_fired = false;
            if (!s_h2w_fired) {
                s_h2w_fired = true;
                Logger::raw_write("[H2W] orphan-displaced tick=");
                Logger::print_dec(current_cpu().ticks);
                Logger::raw_write(" cur_rsp=0x");
                Logger::print_hex(cur_rsp);
                Logger::raw_write(" ctx_rsp=0x");
                Logger::print_hex(task_stack_ptr(current));
                Logger::raw_write(" callsite=0x");
                Logger::print_hex(
                    reinterpret_cast<uint64_t>(__builtin_return_address(0)));
                Logger::raw_write(" kst=0x");
                Logger::print_hex(
                    reinterpret_cast<uint64_t>(current->kernel_stack));
                Logger::raw_write("-0x");
                Logger::print_hex(current->kernel_stack_top);
                Logger::raw_write("\n");
                const uint64_t *s =
                    reinterpret_cast<const uint64_t *>(cur_rsp);
                for (unsigned i = 0; i < 56; ++i) {
                    Logger::raw_write("  [rsp+");
                    Logger::print_hex(static_cast<uint64_t>(i * 8));
                    Logger::raw_write("]=0x");
                    Logger::print_hex(s[i]);
                    Logger::raw_write("\n");
                }
                // The harness's stored kslot iret frame — the rsp field at
                // +160 is what the NEXT dispatch's iretq would load.
                const uint64_t *cf =
                    reinterpret_cast<const uint64_t *>(task_stack_ptr(current));
                Logger::raw_write("  ctx-frame: rip=0x");
                Logger::print_hex(cf[136 / 8]);
                Logger::raw_write(" cs=0x");
                Logger::print_hex(cf[144 / 8]);
                Logger::raw_write(" rflags=0x");
                Logger::print_hex(cf[152 / 8]);
                Logger::raw_write(" rsp=0x");
                Logger::print_hex(cf[160 / 8]);
                Logger::raw_write(" ss=0x");
                Logger::print_hex(cf[168 / 8]);
                Logger::raw_write("\n");
#if defined(CONFIG_ARCH_X86_64)
                // Walk the harness's kslot stack PTE chain (read via the
                // direct map) to see WHICH phys the kslot VA maps right now.
                // The CR3/PM4L walk is x86_64-specific.
                uint64_t cr3 = 0;
                asm volatile("mov %%cr3, %0" : "=r"(cr3));
                uint64_t kva = reinterpret_cast<uint64_t>(current->kernel_stack);
                const uint64_t HHDM = CONFIG_HHDM_OFFSET;
                uint64_t l4e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (cr3 & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 39) & 0x1FF) * 8);
                uint64_t l3e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l4e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 30) & 0x1FF) * 8);
                uint64_t l2e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l3e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 21) & 0x1FF) * 8);
                uint64_t l1e = *reinterpret_cast<const uint64_t *>(
                    HHDM + (l2e & 0xFFFFFFFFFF000ULL) +
                    ((kva >> 12) & 0x1FF) * 8);
                uint64_t kphys = l1e & 0xFFFFFFFFFF000ULL;
                Logger::raw_write("  kslot-kva=0x");
                Logger::print_hex(kva);
                Logger::raw_write(" maps-phys=0x");
                Logger::print_hex(kphys);
                Logger::raw_write(" orphan-phys=0x");
                Logger::print_hex(cur_rsp - HHDM);
                Logger::raw_write(" SAME=");
                Logger::print_dec(kphys == (cur_rsp & ~0xFFFULL) ? 1u : 0u);
                Logger::raw_write("\n");
#endif // CONFIG_ARCH_X86_64
            }
        }
#endif
    }

#ifdef CONFIG_DEBUG
    {
        auto &ring = current->debug_switch_ring;
        auto &idx = current->debug_switch_idx;
        auto &rec = ring[idx % TaskControlBlock::DEBUG_SWITCH_RING_SIZE];
        rec.entry_addr =
            reinterpret_cast<uint64_t>(__builtin_return_address(0));
#if defined(CONFIG_ARCH_X86_64)
        rec.exit_rip = current->context.rip;
#elif defined(CONFIG_ARCH_AARCH64)
        rec.exit_rip = current->context.elr_el1;
#endif
        rec.regs = current->context;
        rec.thread_id = current->id;
        rec.consumed_ticks = current->executed_ticks;
        ++idx;
    }
#endif
    {
        uint64_t nsp = task_stack_ptr(&next);
        uint64_t nbase = reinterpret_cast<uint64_t>(next.kernel_stack);
        uint64_t npg = reinterpret_cast<uint64_t>(next.get_page_table());
        // The harness's context.rsp may be a LIVE boot-stack RSP (its genuine
        // test-mode stack, kept current by the boot-stack save in
        // switch_to_task) rather than a TCB kernel-stack address — that is
        // valid, not bad.  Exempt it from the initial nsp-vs-kernel_stack
        // check; frame_ok() below still validates the frame fields (with its
        // own harness boot-stack allowance).
        bool harness_boot_ctx =
            (&next == Scheduler::get_harness_task() && nsp != 0 &&
             nsp >= reinterpret_cast<uint64_t>(_stack_start) &&
             nsp < reinterpret_cast<uint64_t>(_stack_end));
        bool bad =
            (!nsp ||
             (!harness_boot_ctx &&
              (nsp < nbase || nsp >= next.kernel_stack_top)) ||
              (npg != 0 && (npg & 0xFFF) != 0));
        if (!bad) {
            // riscv64 has no frame-validation branch below (x86_64 iret /
            // aarch64 ELR+SPSR only) — the pointer is unused there.
            [[maybe_unused]] const uint64_t *f =
                reinterpret_cast<const uint64_t *>(nsp);
#if defined(CONFIG_ARCH_X86_64)
            uint64_t f_rflags = 0;
            // The iret frame sits above the saved register frame.  Two valid
            // layouts exist: a freshly-created task (task.cpp builds rip first)
            // and a task saved by isr_common (CPU order: ss first).  Both place
            // rflags at +152; rip/cs/rsp/ss are swapped between the two
            // orderings, so validate either one instead of assuming a single
            // fixed order (the wrong order made valid RUN-task frames look
            // corrupt and dropped their switch — wedging the `all` suite).
            uint64_t rip_a = f[IRET_RIP_IDX], cs_a = f[IRET_CS_IDX],
                     rsp_a = f[IRET_RSP_IDX],
                     ss_a = f[IRET_SS_IDX]; // created order (rip first)
            uint64_t rip_b = f[IRET_SS_IDX], cs_b = f[IRET_RSP_IDX],
                     rsp_b = f[IRET_CS_IDX],
                     ss_b = f[IRET_RIP_IDX]; // isr_common/CPU order (ss first)
            f_rflags = f[IRET_RFLAGS_IDX];
            if (!validate_iret_frame(next, nbase, f_rflags, rip_a, cs_a, rsp_a,
                                     ss_a) &&
                !validate_iret_frame(next, nbase, f_rflags, rip_b, cs_b, rsp_b,
                                     ss_b))
                bad = true;
#elif defined(CONFIG_ARCH_AARCH64)
            // AArch64 saved/initial context is the vectors.S save area:
            // x0-x30 (+0..240), sp_el0 (+248), elr_el1 (+256), spsr_el1
            // (+264).  Validate the exception-return state instead of an
            // x86 iret frame: ELR must be non-zero and SPSR must encode a
            // sane AArch64 exception-return mode (EL0t = 0b0000 or
            // EL1h = 0b0101) — a garbage frame (zeroed fresh stack) must
            // never be dispatched.
            uint64_t elr = f[256 / 8];
            uint64_t spsr = f[264 / 8];
            uint64_t mode = spsr & 0xFULL;
            if (elr == 0 || (mode != 0x0 && mode != 0x5)) {
                bad = true;
            }
#endif
        }
        if (bad) {
            // ---- Dispatch guard ----
            // Never iretq into a task whose iret frame is invalid.  This can
            // happen when `next` is the physically-running task but
            // current_task() has drifted onto a peer TCB (the running task
            // was never switched out, so its stack slot at context.rsp+136 is
            // live data, not a CPU-written iret frame).  In that case treat it
            // as a no-op self-switch: keep the physical runner going and correct
            // current_task().  Otherwise skip the dispatch and let the
            // current task continue (the bad task stays queued and is retried
            // once it has a real iret frame).
            uint64_t phys_rsp{};
            phys_rsp = current_sp();
            uint64_t nb = reinterpret_cast<uint64_t>(next.kernel_stack);
            bool next_is_runner =
                (&next == current) ||
                (next.kernel_stack && next.kernel_stack_top &&
                 phys_rsp >= nb && phys_rsp < next.kernel_stack_top);
            if (next_is_runner) {
                // `next` IS the physically-running task (current or a task whose
                // stack the live RSP sits on) but current_task() has drifted
                // onto a peer TCB.  Treat as a no-op self-switch: keep the
                // physical runner going and clear its queue membership so
                // next_task() does not exclude it as `current` and fall through
                // to idle (which would permanently starve the live runner).  A
                // RUNNING task has no valid iret frame (its stack slot at
                // context.rsp+136 is live data), so we must NOT iretq into it —
                // just keep it running.  The cache is NOT updated here (Rule 4):
                // the timer ISR sets it via set_current_task after the real swap
                // lands.
                next.state = TaskState::RUNNING;
                next.in_ready_queue_ = false;
                next.rq_priority_ = 0;
            } else {
                // D2 fix (INV-2 / VIOL-5): next_task() already dequeued `next`
                // from the runq.  The old comment claimed "the bad task stays
                // queued and is retried", but it is NOT queued anymore — it was
                // dequeued at scheduler.cpp:395.  Without re-enqueueing here it
                // becomes READY + in_ready_queue_=false + not in any bucket and
                // is stranded forever (the next_task() lazy rebuild only runs
                // when dequeue_highest returns null).  Put it back so it stays
                // eligible and is retried once it has a real iret frame.  Never
                // re-enqueue the idle task (it is the default fallback) or the
                // current physical runner.
                if (!Scheduler::is_idle_task(&next) && &next != current) {
                    Scheduler::set_task_ready(next);
                }
            }
            release_lock();
            return; // do not set scheduler_load_rsp_from -> no switch
        }
    }
    kernel::dispatch_step(kernel::SchedDispatchSteps::FrameGuard); // #347
    __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), task_stack_ptr(&next),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(),
                     reinterpret_cast<uint64_t>(next.kernel_stack),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), next.kernel_stack_top,
                     __ATOMIC_RELEASE);
    if (next.get_page_table()) {
        // Issue #156: publish tagged (PCID 0 = raw phys, full flush).
        uint64_t cr3_value = next.get_page_table();
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
        cr3_value = arch::pcid_tag(next.get_page_table(),
                                   publish_pcid_for(next));
#endif
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), cr3_value,
                         __ATOMIC_RELEASE);
        // Issue #74: publish the TLS base alongside CR3 (0 for unset;
        // kernel/harness tasks publish via the else branch below).
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                         next.is_user_ ? next.tls_base_ : 0, __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        {
            auto *c = Scheduler::current_task();
            IPC_SCHED_TRACE("[SW]", "cur=", c ? c->id : 0u, "next=", next.id,
                            "rsp=", (uint64_t)task_stack_ptr(&next), "x=", 0u);
        }
#endif
#if defined(CONFIG_ARCH_X86_64)
        // Issue #25 C1: CPU0-gated (shared TSS; AP runs kernel tasks only).
        // (File-static context: arch::cpu_index() directly.)
        if (arch::cpu_index() == 0)
            arch::GDT::set_tss_rsp0(next.kernel_stack_top);
#endif
    } else {
        __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), VMM::get_kernel_pml4(),
                         __ATOMIC_RELEASE);
        // Issue #74: kernel/harness targets never carry TLS (apply loads
        // nothing); publish 0 so no stale user base survives.
        __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                         __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        {
            auto *c = Scheduler::current_task();
            IPC_SCHED_TRACE("[SW]", "cur=", c ? c->id : 0u, "next=", next.id,
                            "rsp=", (uint64_t)task_stack_ptr(&next), "x=", 0u);
        }
#endif
    }

    // Apply the target task's I/O permission bitmap (issue #3).  No-op for
    // kernel tasks and when the owner is unchanged.
    arch::iopb_switch_to(next);

    // Re-enqueue the current task if it must remain schedulable.  A task with
    // state RUNNING is always re-queued (normal preemption).  The boot-stack
    // harness is a special case: its TCB state can be READY while it physically
    // runs on the boot stack (it was never dispatched via a switch_to_task that
    // set RUNNING — snapshot restore leaves it READY).  If it is preempted
    // without re-enqueueing, it is stranded (INV-2: live but not in the runq
    // and not current after the switch) and next_task() falls through to idle,
    // freezing the `all` suite.  enqueue_ready() refuses double-enqueues, so
    // this is safe even if the harness is already queued.

#if CONFIG_CANARY_GUARD
    // v0.4.0 MP-3: verify the current task's kernel-stack canary on every
    // context switch — pure read at kernel_stack[0..8), already under the
    // scheduler lock.  A mismatch means the stack base was overwritten (deep
    // stack growth / corruption): controlled panic, never silent corruption.
    // Runs BEFORE the deferred-switch arm (H2) so the panic cannot be masked.
    if (!canary_verify_kernel_stack(current)) {
        kernel::Logger::fatal(
            "CANARY TRIP (kernel stack): task '%s' id=%u top=0x%lx",
            current->name, current->id,
            current->kernel_stack_top);
        panic("kernel-stack canary violated");
    }
#endif

    if (current->state == TaskState::RUNNING || cur_is_boot_stack) {
        current->state = TaskState::READY;
        Scheduler::enqueue_ready(*current);
    }
    // Issue #250: consume a wake-while-current deferral. The task is
    // leaving the CPU here, so linking it cannot recreate the
    // queued-while-current window — but enqueue_ready would re-defer
    // (current is still current until the epilogue updates it), hence the
    // deschedule-side inner routing. Only runnable, unqueued states
    // consume; anything else just drops the stale flag (a concurrent
    // BLOCKED transition already cleared it via dequeue_ready).
    if (current->ready_deferred_) {
        current->ready_deferred_ = false;
        if (!current->in_ready_queue_ && !current->in_edf_queue_ &&
            (current->state == TaskState::READY ||
             current->state == TaskState::RUNNING)) {
            if (current->state == TaskState::RUNNING)
                current->state = TaskState::READY;
            Scheduler::enqueue_ready_inner(*current);
        }
    }
    next.state = TaskState::RUNNING;
    // Issue #172: snapshot dispatch placement for cpuinfo/top.  Single
    // writer (this CPU dispatching next), release-store; shell readers
    // use acquire loads.  Placed at the single real-dispatch commit
    // point (self-switch no-ops above return early and keep old value).
    __atomic_store_n(&next.running_on_cpu, Scheduler::sched_cpu(),
                     __ATOMIC_RELEASE);
    kernel::dispatch_step(kernel::SchedDispatchSteps::Publish); // issue #347

    {
        arch::IrqGuard ig{};
        // Issue #369 T4b: release stays INSIDE the IRQ-off window on
        // purpose. Releasing before the guard would open an
        // unlocked+unpublished+IRQ-on window where a timer ISR runs a
        // full nested tick (lock is free, so its try_lock succeeds) and
        // publishes its own arm — which this publish then overwrites
        // (stranded nested target + stale ArmSwitch record; the T3a
        // rejected-hoist finding). Generation ordering would keep it
        // correct, but the narrower window avoids the interleaving
        // entirely. Do not hoist without a re-audit.
        release_lock();
        // H2 ring: an ARM published while a previous arm is still pending
        // (save_rsp_to != 0) strands the previous arm's dequeued target; an
        // IDLE arm aimed at the harness strands the harness itself.
        H2_REC(H2_EV_ARM, next.id,
               __atomic_load_n(&Scheduler::SwSlots::load_rsp_from(), __ATOMIC_RELAXED),
               reinterpret_cast<uint64_t>(save_target));
        if (&next == Scheduler::own_idle() &&
            current == Scheduler::get_harness_task()) {
            H2_REC(H2_EV_IDLE_ARM, current->id, 0, 0);
        }
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), next.id, __ATOMIC_RELEASE);
#if defined(CONFIG_DEBUG_IPC_SCHED)
        // H2 arm-time liveness audit (cold): dump when a deferred-switch arm
        // is published to a target that is NOT a live id_table member — i.e.
        // the arm is stale at publish time (task already removed).
        {
            auto *tgt = Scheduler::find_task(next.id);
            if (!tgt || tgt != &next) {
                kernel::Logger::raw_write("[H2-ARMDEAD] cur=");
                kernel::Logger::print_dec(current->id);
                kernel::Logger::raw_write(" next=");
                kernel::Logger::print_dec(next.id);
                kernel::Logger::raw_write(" tgt=");
                kernel::Logger::print_hex(reinterpret_cast<uint64_t>(tgt));
                kernel::Logger::raw_write(" st=");
                kernel::Logger::print_dec(
                    static_cast<uint64_t>(next.state));
                kernel::Logger::raw_write(" tick=");
                kernel::Logger::print_dec(arch::Timer::ticks());
                kernel::Logger::raw_write("\n");
            }
        }
#endif
        // Generation-lock: commit the deferred-switch pair.  The generation is
        // bumped only after load_rsp_from / load_cr3_from / next_task_id are
        // visible (all written before this IRQ-guarded block), and the arm
        // (save_rsp_to) is published after the bump — so any ISR that observes
        // the arm also observes the complete pair and the current generation.
        // isr_stubs.asm re-verifies this generation before applying, so it
        // never applies a half-written / superseded pair.
        uint64_t gen =
            __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
        __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(), save_target,
                         __ATOMIC_RELEASE);
        kernel::dispatch_step(kernel::SchedDispatchSteps::ArmSwitch); // #347

        // Issue #369 T4b: CR0.TS set stays INSIDE the IRQ-off window on
        // purpose. It arms the lazy-FPU #NM trap for the incoming task.
        // After the guard closes, IRQs are live again: a timer ISR firing
        // before a hoisted TS-set would run with TS in the previous
        // task's state (possibly clear), and any FPU use there — or by
        // the next task before the deferred set — would silently use the
        // wrong registers instead of trapping (no proof exists that no
        // ISR path touches FPU). Do not hoist without a full ISR-FPU
        // audit plus GDB-captured ordering evidence.
        uint64_t cr0 = arch::read_cr0();
        cr0 |= (1ULL << 3);
        arch::write_cr0(cr0);
    }
}

void Scheduler::switch_away_from_terminating(TaskControlBlock &exiting) noexcept {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    TaskControlBlock *next;

    // Scope for scheduler_lock_ — released before the IRQ-guarded publish step.
    {
        SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);

        // A deferred switch is already published.  Do NOT publish a second.
        // Issue #221: force-apply the standing arm — the terminating
        // task's trap epilogue must not gate-skip it (double-fault park).
        if (__atomic_load_n(&Scheduler::SwSlots::save_rsp_to(), __ATOMIC_ACQUIRE) != 0) {
            __atomic_store_n(&Scheduler::SwSlots::force_apply(), (uint64_t)1,
                             __ATOMIC_RELEASE);
            return;
        }

        next = next_task();
        if (!next || next == &exiting)
            // Issue #25 C1: fall back to the OWN CPU's idle (idle_task_ is
            // the BSP idle — parking an AP on it runs two CPUs on one
            // stack).
            next = own_idle();
        if (!next || next->magic != TaskControlBlock::TCB_MAGIC)
            return;

        // Validate `next`'s iret frame.
        {
            uint64_t nsp = task_stack_ptr(next);
            uint64_t nbase = reinterpret_cast<uint64_t>(next->kernel_stack);
            // Harness boot-stack context.rsp is valid (see switch_to_task).
            bool harness_boot_ctx =
                (next == Scheduler::get_harness_task() && nsp != 0 &&
                 nsp >= reinterpret_cast<uint64_t>(_stack_start) &&
                 nsp < reinterpret_cast<uint64_t>(_stack_end));
            bool bad =
                (!nsp ||
                 (!harness_boot_ctx &&
                  (nsp < nbase || nsp >= next->kernel_stack_top)) ||
                 (next->get_page_table() != 0 &&
                  (next->get_page_table() & 0xFFF) != 0));
            if (!bad) {
                // Unused on riscv64 (no validation branch below).
                [[maybe_unused]] const uint64_t *f =
                    reinterpret_cast<const uint64_t *>(nsp);
#if defined(CONFIG_ARCH_X86_64)
                uint64_t rip_a = f[IRET_RIP_IDX], cs_a = f[IRET_CS_IDX],
                         rsp_a = f[IRET_RSP_IDX], ss_a = f[IRET_SS_IDX];
                uint64_t rip_b = f[IRET_SS_IDX], cs_b = f[IRET_RSP_IDX],
                         rsp_b = f[IRET_CS_IDX], ss_b = f[IRET_RIP_IDX];
                uint64_t f_rflags = f[IRET_RFLAGS_IDX];
                if (!validate_iret_frame(*next, nbase, f_rflags, rip_a, cs_a,
                                         rsp_a, ss_a) &&
                    !validate_iret_frame(*next, nbase, f_rflags, rip_b, cs_b,
                                         rsp_b, ss_b))
                    bad = true;
#elif defined(CONFIG_ARCH_AARCH64)
                // AArch64 saved/initial context is the vectors.S save area:
                // x0-x30 (+0..240), sp_el0 (+248), elr_el1 (+256), spsr_el1
                // (+264).  Validate the exception-return state (mirrors the
                // switch_to_task dispatch guard): ELR must be non-zero and
                // SPSR must encode a sane AArch64 return mode (EL0t = 0b0000
                // or EL1h = 0b0101) — a garbage frame (zeroed fresh stack)
                // must never be dispatched.
                uint64_t elr = f[256 / 8];
                uint64_t spsr = f[264 / 8];
                uint64_t mode = spsr & 0xFULL;
                if (elr == 0 || (mode != 0x0 && mode != 0x5))
                    bad = true;
#endif
            }
            if (bad)
                next = idle_task_;
        }
        if (!next || next->magic != TaskControlBlock::TCB_MAGIC)
            return;

        // Publish the deferred switch globals (load side only under lock).
        __atomic_store_n(&Scheduler::SwSlots::load_rsp_from(), task_stack_ptr(next),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_kstack_base(),
                         reinterpret_cast<uint64_t>(next->kernel_stack),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::load_kstack_top(), next->kernel_stack_top,
                         __ATOMIC_RELEASE);
        if (next->get_page_table()) {
            // Issue #156: tagged publish (same contract as above).
            uint64_t cr3_value = next->get_page_table();
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
            cr3_value = arch::pcid_tag(next->get_page_table(),
                                       publish_pcid_for(*next));
#endif
            __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), cr3_value,
                             __ATOMIC_RELEASE);
            // Issue #74: publish the TLS base alongside CR3 (second publish
            // site — switch_away_from_terminating tick path).
            __atomic_store_n(&Scheduler::SwSlots::load_tls_from(),
                             next->is_user_ ? next->tls_base_ : 0,
                             __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&Scheduler::SwSlots::load_cr3_from(), VMM::get_kernel_pml4(),
                             __ATOMIC_RELEASE);
            // Issue #74: kernel/harness targets never carry TLS.
            __atomic_store_n(&Scheduler::SwSlots::load_tls_from(), (uint64_t)0,
                             __ATOMIC_RELEASE);
        }

        if (exiting.state == TaskState::RUNNING) {
            exiting.state = TaskState::READY;
            Scheduler::enqueue_ready(exiting);
        }
        next->state = TaskState::RUNNING;

        // Issue #25 C1: CPU0-gated (shared TSS; see above).
        if (next->get_page_table() && sched_cpu() == 0)
            arch::GDT::set_tss_rsp0(next->kernel_stack_top);

        // Apply the target task's I/O permission bitmap (issue #3).
        arch::iopb_switch_to(*next);
    }

    // IRQ-guarded publish step (lock released).
    {
        arch::IrqGuard ig{};
        __atomic_store_n(&Scheduler::SwSlots::next_task_id(), next->id, __ATOMIC_RELEASE);
        // Generation-lock: bump the generation after the load side is published
        // (written above under the lock) and before arming save_rsp_to.  The
        // ISR epilogue verifies the generation before applying the pair.
        uint64_t gen =
            __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED);
        __atomic_store_n(&Scheduler::SwSlots::generation(), gen + 1,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&Scheduler::SwSlots::save_rsp_to(),
                         &task_stack_ptr(&exiting), __ATOMIC_RELEASE);
        // Issue #221: terminate-driven arms force-apply (see SwSlots doc).
        __atomic_store_n(&Scheduler::SwSlots::force_apply(), (uint64_t)1,
                         __ATOMIC_RELEASE);
#if defined(CONFIG_ARCH_X86_64)
        uint64_t cr0 = arch::read_cr0();
        cr0 |= (1ULL << 3);
        arch::write_cr0(cr0);
#endif
    }
}

}  // namespace kernel
