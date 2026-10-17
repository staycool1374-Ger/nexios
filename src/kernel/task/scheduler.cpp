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

/// @file scheduler.cpp
/// @brief Scheduler implementation: task lifecycle, ready-queue management,
///        rate-monotonic dispatch, context switching, and test isolation.

#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_integrity.hpp>      // issue #323 validators
#include <kernel/task/scheduler_queue_internal.hpp> // issue #323 queue/EDF
#include <kernel/task/scheduler_admission_internal.hpp> // issue #323 admission
#include <kernel/task/scheduler_debug_internal.hpp> // issue #323 debugger
#include <kernel/task/scheduler_lifecycle_internal.hpp> // issue #323 lifecycle
#include <kernel/task/scheduler_switch_internal.hpp> // issue #323 switch engine (shared decls)
#include <kernel/task/scheduler_path.hpp>      // issue #347 Slice B
#include <kernel/task/idle_monitor.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/elf/elf.hpp>
#include <crc32.hpp>
#include <services/terminal/terminal.hpp>
#include <kernel/memory/tlb_shootdown.hpp>
#include <kernel/time/timer_wheel.hpp>
#include <kernel/time/posix_time.hpp>
#include <kernel/task/tcb_write_log.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/iopb.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/debug/dump.hpp>
#include <kernel/debug/debug_stop.hpp>
#include <kernel/debug/ipc_sched_trace.hpp>

extern "C" void debug_write(const char *s);
extern "C" void debug_write_hex(uint64_t value);
extern "C" void debug_write_dec(uint64_t value);
#include <kernel/memory/integrity.hpp>
#include <kernel/core/global_state.hpp>
#include <assert.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_watchdog.hpp>
#include <kernel/daemon/daemon_mgr.hpp>
#include <kernel/vfs/vfsd.hpp>
#include <kernel/driver/iocd.hpp>
#include <kernel/arch/apic.hpp>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/percpu.hpp>
#endif
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/smp.hpp>
#endif
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
#include <kernel/arch/x86_64/hal/pcid.hpp>
#endif

#if defined(CONFIG_DEBUG)
// Fault-injection mask for dispatch_step() (issue #347 Slice B,
// declared in scheduler_path.hpp). Test-only; absent in release.
uint64_t kernel::g_test_skip_dispatch_step = 0;
#endif

// Issue #323 switch strand: current_sp() moved to the shared
// scheduler_switch_internal.hpp home (sched_switch::current_sp); the three
// remaining call sites below use it via the using-decl in namespace kernel.

// Boot stack (section .boot_stack, bounded by the linker's _stack_start /
// _stack_end symbols).  The harness (init, PID 1) physically runs on this
// stack in test mode — it is never switched onto its TCB kernel_stack, so no
// task's kernel_stack range covers the live RSP while the harness executes.
extern "C" {
extern char _stack_start[];
extern char _stack_end[];
}

// Issue #323: validate_iret_frame() lives in scheduler_integrity.cpp.

#include <kernel/task/sporadic_server.hpp>

// Issue #323 switch strand: the IRET_*_IDX constants moved to the shared
// scheduler_switch_internal.hpp home (sched_switch::); the switch engine in
// scheduler_switch.cpp is the sole user.
#include <signal.hpp>
#include <logger.hpp>

#if defined(CONFIG_DEBUG_IPC_SCHED)
// Wedge diagnostics for deferred-switch / ready-queue desync analysis.
// PfA-B: folded into per-CPU CpuContext debug state (design §4.A VAR-13).
#endif

namespace kernel {

// Issue #323: the kernel-stack pointer accessor and the context-switch
// validators live in scheduler_integrity.{hpp,cpp} (single home).  Import
// them so the existing unqualified call sites in this TU stay unchanged.
using sched_verify::task_stack_ptr;
using sched_verify::validate_switch;
using sched_verify::report_corruption;
using sched_verify::is_boot_stack_rsp;
// Issue #323 switch strand: current_sp() moved to scheduler_switch_internal.hpp.
using sched_switch::current_sp;
// Issue #323 switch strand: H2_EV_CLR_RMS moved to the sched_switch:: shared
// home; the next_task drop-arm path below is the remaining scheduler.cpp user.
using sched_switch::H2_EV_CLR_RMS;
#if defined(CONFIG_ARCH_X86_64)
using sched_verify::validate_iret_frame;
#endif

// Issue #323: the ready-queue/EDF helpers moved to scheduler_queue.cpp;
// import them so the existing unqualified call sites in this TU stay
// unchanged.
using sched_queue::edf_earlier;
using sched_queue::edf_eligible;
using sched_queue::edf_insert;
using sched_queue::edf_peek_valid_own;
using sched_queue::edf_preempt_candidate;
using sched_queue::edf_ready_;
using sched_queue::edf_remove;
using sched_queue::edf_resort;
using sched_queue::invalidate_pending_switch_to;
using sched_queue::migrate_dispatch_queue;

// Issue #323: admission helpers moved to scheduler_admission.cpp; import the
// table + numerator helper so the legacy warn-only path in add_task() stays
// unchanged.  The admission_* member functions are declared in scheduler.hpp.
using sched_admission::LIU_LEYLAND_BOUNDS;
using sched_admission::LIU_LEYLAND_LIMIT;
using sched_admission::LIU_LEYLAND_MAX_TASKS;
using sched_admission::server_budget_for_admission;

// Issue #323: the per-CPU debugger park slot moved to scheduler_debug.cpp;
// import it so the rate_monotonic_schedule() park check stays unchanged.
using sched_debug::debug_tick_pc_;

// Issue #323: canary hook, poisoned-block probe, and dispatch counters
// moved to scheduler_debug.cpp; import them so the remaining tick/switch
// paths stay unchanged.
using sched_debug::is_poisoned_block;
#if defined(CONFIG_DEBUG)
using sched_debug::g_diag_rms_count;
using sched_debug::g_diag_switch_count;
using sched_debug::g_diag_tick_count;
#endif

// Issue #323: the deferred-kill worklist moved to scheduler_lifecycle.cpp;
// the on_tick tail observes only the count (drain happens there).
using sched_lifecycle::s_deferred_kill_count;

// H2_REC macro is now in global_state.hpp for cross-TU visibility

// Issue #323 watchdog strand: test_context_/s_monitor_task_/s_scan_requested_ defs live in scheduler_watchdog.cpp.

// Issue #323 registry strand: task-registry/static admin defs live in scheduler_registry.cpp.
ReadyQueueManager Scheduler::ready_queues_[CONFIG_MAX_CPUS];
DeadlineList Scheduler::deadline_list_;
Scheduler::MailboxEntry
    Scheduler::wake_mailbox_[CONFIG_MAX_CPUS][MAILBOX_SLOTS] = {};
constinit uint64_t Scheduler::wake_mailbox_count_[CONFIG_MAX_CPUS] = {};
sync::SpinLock Scheduler::wake_mailbox_lock_[CONFIG_MAX_CPUS];
constinit bool Scheduler::sched_quiesced_ = false;
constinit uint64_t Scheduler::sched_quiesce_depth_ = 0;
sync::SpinLock Scheduler::zombie_lock_;
constinit TaskControlBlock *Scheduler::zombie_head_ = nullptr;
constinit TaskControlBlock *Scheduler::zombie_tail_ = nullptr;
constinit uint64_t Scheduler::zombie_count_ = 0;
sync::SpinLock Scheduler::scheduler_lock_;


// ---------------------------------------------------------------------------
// Init / lifecycle
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// on_tick — timer tick handler
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Corruption / validation helpers
// ---------------------------------------------------------------------------

// Issue #323: report_corruption(), is_boot_stack_rsp(), rsp_in_stack_range(),
// and validate_iret_frame() live in scheduler_integrity.cpp (sched_verify).

// Issue #323: validate_switch() lives in scheduler_integrity.cpp.

/// @brief Corrects current_task() to the task physically executing on the
///        live kernel stack.  Some test helpers (e.g. yield_as) or context
///        switches can leave current_task() pointed at a peer TCB while the
///        CPU is actually running on another task's stack; saving the live
///        register state into the wrong TCB corrupts it and desyncs the
///        scheduler (the `all` suite wedging/hanging).  Call before selecting
///        the next task so saves land in the real running task.  The
///        set_current() invariant (the running task is never in the ready

// ---------------------------------------------------------------------------
// P6b: Deadline scan (monitor path)
// ---------------------------------------------------------------------------


} // namespace kernel

/// @brief ISR-entry pre-save hook (isr_stubs.asm:217-220).  Fires on every ISR
/// entry, recording the interrupted task's RSP before any context switch.
/// Verifies the interrupted task's RSP lies within its kernel stack.  If the
/// check fails, logs a detailed report (current task, RSP, stack bounds, all
/// other tasks owning that RSP) — evidence for H2.  Runs with IF=0 (interrupt
/// gate); must not re-enable IRQs.
/// @brief ISR-epilogue RSP-owner abort hook (isr_stubs.asm:270-275).  Fires
///        when a pending deferred-switch apply is aborted because the loaded
/// RSP lies outside [scheduler_load_kstack_base, top) — the asm-side
/// stale/foreign-frame guard.  Cold path.  Dumps the rejected RSP, the
/// kstack range, the arm target, and the RFLAGS the interrupted task
/// will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
