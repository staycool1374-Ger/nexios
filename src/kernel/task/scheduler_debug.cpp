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

/// @file scheduler_debug.cpp
/// @brief Debugger support + id-table debug helpers for Scheduler
///        (issue #323).  Pure move out of scheduler.cpp: no logic changes.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>
#include <kernel/core/global_state.hpp>
#include <lib/logger.hpp>
#include <assert.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/sync/spinlock_guard.hpp>

namespace kernel {

namespace sched_debug {
constinit uint64_t debug_tick_pc_[CONFIG_MAX_CPUS] = {};
}  // namespace sched_debug

using sched_debug::debug_tick_pc_;

bool Scheduler::debug_id_table_references(void *block) noexcept {
    auto *b = static_cast<TaskControlBlock *>(block);
    for (uint64_t i = 0; i < ID_TABLE_SIZE; ++i) {
        if (id_table_[i] == b)
            return true;
    }
    return false;
}

void Scheduler::note_debug_tick_pc(uint64_t interrupted_pc) noexcept {
    uint64_t cpu = sched_cpu();
    if (cpu < CONFIG_MAX_CPUS)
        __atomic_store_n(&debug_tick_pc_[cpu], interrupted_pc,
                         __ATOMIC_RELAXED);
}

// Issue #226: debugger fault-stop park. ISR-safe: takes scheduler_lock_
// directly (same position as switch_away_from_terminating, which runs
// after with the lock released — never nested). The tick park path never
// takes g_bind_lock, so router order (scheduler -> stop-queue) matches the
// documented scheduler_lock_ -> registry order.
bool Scheduler::debug_park_stop(TaskControlBlock &tgt, uint64_t kind,
                               uint64_t va) noexcept {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    if (__atomic_load_n(&tgt.debugger_id, __ATOMIC_ACQUIRE) == 0)
        return false;
    if (tgt.magic != TaskControlBlock::TCB_MAGIC)
        return false;
    __atomic_store_n(&tgt.debug_stop_kind, kind, __ATOMIC_RELEASE);
    __atomic_store_n(&tgt.debug_stop_va, va, __ATOMIC_RELEASE);
    // A step-over re-arm belongs to exactly one pending step: any fresh
    // non-STEP stop supersedes it (otherwise a later unrelated STEP
    // completion would re-insert an unasked breakpoint).
    if (kind != 2) // StopKind::STEP numerics (debug_stop.hpp)
        __atomic_store_n(&tgt.debug_rearm_va, 0, __ATOMIC_RELEASE);
    tgt.debug_parked = true;
    tgt.state = TaskState::BLOCKED;
    dequeue_ready(tgt);
    tgt.has_utrap_frame = true; // the trap wrote the U-frame slot
    return true;
}

// Issue #225: debugger detach resume. Task-context only (takes IrqGuard +
// scheduler_lock_); the tick park path never takes g_bind_lock, so the
// detach order (bind -> scheduler) cannot invert.
void Scheduler::debugger_resume(TaskControlBlock &tgt) noexcept {

    ENSURE(scheduler_lock_.holder() == nullptr); // issue #369 T2b: no reentrant take (non-recursive)
    arch::IrqGuard irq_guard{};
    SpinLockGuard<sync::SpinLock> guard(scheduler_lock_);
    __atomic_store_n(&tgt.debug_stop_requested, false, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&tgt.debug_parked, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&tgt.debug_parked, false, __ATOMIC_RELEASE);
    // Resume only what the park hook still owns: BLOCKED with a debugger
    // attached. Any other BLOCKED channel keeps its waiter.
    if (tgt.state != TaskState::BLOCKED ||
        __atomic_load_n(&tgt.debugger_id, __ATOMIC_ACQUIRE) == 0)
        return;
    // The park hook dequeued before blocking and enqueue_ready self-unlinks
    // the EDF case: READY, never RUNNING (the runq owns READY tasks only).
    tgt.state = TaskState::READY;
    enqueue_ready(tgt);
}


// Diagnostics tail (moved from scheduler.cpp, issue #323).
/// @brief MP-3 canary sampling for the scheduler hooks (issue #92 canary
///        relocation).  Verifies the user-segment canaries of a task that is
///        about to run (context switch) or currently running (timer tick).
///        Runs only for user tasks, only under CONFIG_CANARY_GUARD, and is a
///        pure page-table read (no locks, no allocation) — safe in ISR/tick
///        context.  Test mode latches g_canary_trip (the harness survives);
///        production panics.  This restores corruption detection for FAST
///        syscalls, which no longer consult canaries on entry.
/// @param task    Task to verify (must be magic-valid).
/// @param rip     Approximate fault RIP for the latch (0 in scheduler hooks).
/// @return true if verified (or no user task / no canary check enabled).
bool Scheduler::canary_check_in_scheduler_hooks(TaskControlBlock *task, uint64_t rip) {
#if CONFIG_CANARY_GUARD
    if (!task || !task->is_user_)
        return true;
    uint8_t bad_seg = 0;
    uint64_t bad_va = 0;
    if (!canary_verify_user_segments(task, bad_seg, bad_va)) [[unlikely]] {
        // Canary negative-test path (issue #255 H3): under test the trip is
        // recorded, not fatal, so the canary suite can assert the latch.
        if (kernel::Scheduler::is_test_active()) {
            kernel::gs::set_canary_trip(task->id, bad_seg, rip);
            return false;
        }
        kernel::Logger::fatal(
            "CANARY TRIP (sched): task '%s' id=%u segment=%u va=0x%lx",
            task->name, static_cast<unsigned>(task->id),
            static_cast<unsigned>(bad_seg), bad_va);
        panic("software sentinel canary violated");
    }
#else
    (void)task;
    (void)rip;
#endif
    return true;
}

// ---------------------------------------------------------------------------
// Global deferred-switch event ring (H2 instrumentation, CONFIG_DEBUG).
// Extends the per-TCB debug_switch_ring[4] idiom to the GLOBAL scheduler event
// stream: every ARM / APPLY / SKIP / CLEAR of the deferred-switch pair, with
// the atoms (load_rsp_from / save_rsp_to / next_task_id) + generation + ISR
// depth.  In-memory only (no serial writes in the hot path — QEMU gdb-stub
// hardware watchpoints are broken and per-event serial output perturbs the
// race away).  Dump post-hang via GDB memory read, e.g.:
//   x/120gx &kernel::debug::g_h2_ring
// (entries indexed by g_h2_idx % H2_RING_SIZE; see h2_dump_ring()).
// ---------------------------------------------------------------------------
#ifdef CONFIG_DEBUG
namespace debug {
inline constexpr size_t H2_RING_SIZE = 512;

/// @brief One deferred-switch event.  `ev` selects the meaning of a/b/c:
///        ARM=1 (a=next.id, b=load_rsp_from, c=save_target),
///        APPLY=2 (a=applied id), SKIP=3 (a=captured gen, b=current gen),
///        CLR-RMS=4 / CLR-SET=5 / CLR-MISC=6 (a=pending next_task_id),
///        IDLE-ARM=7 (a=current id).
struct H2Event {
    uint64_t ev;
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t gen;   ///< scheduler_switch_generation at record time
    uint64_t depth; ///< isr_nesting_depth at record time
};
static constinit H2Event g_h2_ring[H2_RING_SIZE] = {};
static constinit uint64_t g_h2_idx = 0;

// External linkage (declared in debug/ipc_sched_trace.hpp under
// CONFIG_DEBUG_IPC_SCHED, issue #368); the single definition for all
// H2_REC users.  Ring storage stays TU-local below.
void h2_record(uint64_t ev, uint64_t a, uint64_t b, uint64_t c) noexcept {
    // Issue #25 C1: atomic index (two CPUs append concurrently; a torn
    // ENTRY is possible but debug-only — the generation recorded is the
    // CALLER's CPU slot, so ordering per CPU stays coherent).
    uint64_t idx =
        __atomic_fetch_add(&g_h2_idx, 1ULL, __ATOMIC_RELAXED);
    g_h2_ring[idx % H2_RING_SIZE] = {
        ev, a, b, c,
        __atomic_load_n(&Scheduler::SwSlots::generation(), __ATOMIC_RELAXED),
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED)};
}

/// @brief Dump the most recent H2_RING_SIZE events to the debug console.
void h2_dump_ring() noexcept {
    uint64_t idx = g_h2_idx;
    uint64_t start = (idx > H2_RING_SIZE) ? (idx - H2_RING_SIZE) : 0;
    for (uint64_t i = start; i < idx; ++i) {
        const H2Event &r = g_h2_ring[i % H2_RING_SIZE];
        Logger::raw_write("[H2EV] i=");
        Logger::print_dec(i);
        Logger::raw_write(" ev=");
        Logger::print_dec(r.ev);
        Logger::raw_write(" a=0x");
        Logger::print_hex(r.a);
        Logger::raw_write(" b=0x");
        Logger::print_hex(r.b);
        Logger::raw_write(" c=0x");
        Logger::print_hex(r.c);
        Logger::raw_write(" gen=");
        Logger::print_dec(r.gen);
        Logger::raw_write(" depth=");
        Logger::print_dec(r.depth);
        Logger::raw_write("\n");
    }
}
}  // namespace debug
#endif  // CONFIG_DEBUG

namespace sched_debug {

// TEMP DEBUG (BUGS.md#020): detect a SporadicServer pointer that aliases a
// freed/poisoned MemPool block (first 8 bytes == 0xDDDDDDDDDDDDDDDD).  A freed
// server still referenced by a live TCB is the use-after-free behind the
// intermittent GPF (sporadic_server.hpp:114) / #PF / #NM panic in the `memory`
// class.  This converts the silent corruption into a precise, catchable fault
// naming the offending task instead of corrupting the stack with a 0xDDDD
// return address.
bool is_poisoned_block(const void *p) noexcept {
#ifdef CONFIG_DEBUG
    if (!p)
        return false;
    const uint64_t *q = static_cast<const uint64_t *>(p);
    return *q == 0xDDDDDDDDDDDDDDDDULL;
#else
    (void)p;
    return false;
#endif
}


#if defined(CONFIG_DEBUG)
// Issue #299/#280 stall diagnostics: count timer-tick dispatcher entries and
// actual switch_to_task calls, to distinguish "ticks dead" (counters frozen)
// from "ticks alive but switch not applied" (rms advances, switch does not).
constinit uint64_t g_diag_rms_count = 0;
constinit uint64_t g_diag_switch_count = 0;
constinit uint64_t g_diag_tick_count = 0;
#endif

}  // namespace sched_debug

#if defined(CONFIG_DEBUG)
void Scheduler::diag_ready_probe() noexcept {
    // Lock contract: called only from the CONFIG_DEBUG stall beacon inside
    // deadline_miss_handler, which itself runs in tick context (on_tick holds
    // scheduler_lock_) or in the monitor's scan_deadlines (also holds it) — so
    // the ready-queue read below is serialized against mutation.
    // Issue #299/#280: ready-queue view at the stall.  peek_highest() returns
    // the physical head of the highest non-empty priority queue WITHOUT
    // checking state — so a physically-queued-but-BLOCKED current task is
    // still "peeked", which makes reschedule() early-return (next==current)
    // and never arm the deferred switch (the "save_rsp_to clean" spin).
    uint64_t hi = rq_own().highest_ready_priority();
    auto *peek = rq_own().peek_highest();
    Logger::raw_write("[STALLDUMP] rq_hi=");
    Logger::print_dec(hi);
    Logger::raw_write(" peek=");
    Logger::print_dec(peek ? peek->id : 0xFFFF);
    Logger::raw_write(" peekst=");
    Logger::print_dec(peek ? static_cast<uint64_t>(peek->state) : 0);
    Logger::raw_write("\n");
    for (auto *t = all_tasks_.first_ptr(); t; t = all_tasks_.next_ptr(t)) {
        if (t->magic != TaskControlBlock::TCB_MAGIC)
            continue;
        uint64_t p = t->rq_priority_;
        bool phys = (p <= CONFIG_PRIORITY_CEILING) &&
                    rq_own().queue(p).contains(*t);
        if (t->state == TaskState::READY || t->in_ready_queue_ || phys) {
            Logger::raw_write("[STALLDUMP] rq id=");
            Logger::print_dec(t->id);
            Logger::raw_write(" p=");
            Logger::print_dec(p);
            Logger::raw_write(" inrq=");
            Logger::print_dec(t->in_ready_queue_ ? 1 : 0);
            Logger::raw_write(" phys=");
            Logger::print_dec(phys ? 1 : 0);
            Logger::raw_write(" state=");
            Logger::print_dec(static_cast<uint64_t>(t->state));
            Logger::raw_write("\n");
        }
    }
}
#endif // CONFIG_DEBUG

}  // namespace kernel
