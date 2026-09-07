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

/// @file global_state.cpp
/// @brief Single definition point for all cross-TU kernel globals.
///
/// Every symbol defined here has exactly one owner file (this one).  Symbols
/// are defined at the SAME scope as their historical extern declarations so
/// existing consumers keep linking; the gs:: accessors are the sanctioned way
/// to read/write them.

#include <kernel/core/global_state.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_errors.hpp>
#include <lib/constants.hpp>

// Boot stack bounds (linker symbols).
extern char _stack_start[];
extern char _stack_end[];

// ---------------------------------------------------------------------------
// Storage — defined at the scope matching the extern declarations.
// ---------------------------------------------------------------------------

// Multiboot2 globals are declared `extern "C"` at GLOBAL scope in
// multiboot2.hpp (mb2_find_tag and the boot path reference them).
extern "C" {
uint64_t multiboot_magic = 0;
uint64_t multiboot_info_ptr = 0;
}

// ---------------------------------------------------------------------------
// AsmSwitchState — deferred-context-switch globals shared with isr_stubs.asm.
//
// These symbols are read/written by the x86_64 ISR assembly (isr_stubs.asm)
// and by C++ via the extern declarations in scheduler.hpp.  They are defined
// here (single definition point) and MUST keep their exact symbol names and
// initializers — isr_stubs.asm accesses them by name.
// ---------------------------------------------------------------------------
extern "C" {
uint64_t *scheduler_save_rsp_to = nullptr;
uint64_t scheduler_load_rsp_from = 0;
uint64_t scheduler_load_cr3_from = 0;
uint64_t scheduler_next_task_id = UINT64_MAX;
uint64_t scheduler_load_kstack_base = 0;
uint64_t scheduler_load_kstack_top = 0;
uint64_t scheduler_switch_generation = 0;
uint64_t scheduler_kernel_cr3 = 0;
bool scheduler_need_resched = false;
uint64_t isr_nesting_depth = 0;
uint64_t irq_entry_tsc = 0;
uint64_t scheduler_corruption_count = 0;
uint64_t deadline_detection_integrity = 0;
// Tracks which task's FPU state is currently in the registers (declared in
// scheduler.hpp's extern "C" block).
kernel::TaskControlBlock *fpu_owner = nullptr;
// Highest isr_nesting_depth observed inside the #NM handler (issue #93,
// INV-FPU2 pin).  Reset in test_isolate restore; tests assert it stays
// <= baseline + 1 across a #NM storm (an interrupt-gate #NM can never nest a
// timer ISR inside the owner-swap).
uint64_t fpu_nm_depth_max = 0;
} // extern "C"

namespace kernel {

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
BootInfo g_boot_info{};

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t g_boot_epoch = 0; // RTC read_seconds() at boot

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
CanaryTrip g_canary_trip{};

// The SMAP recovery IP keeps the extern "C" symbol the fault handler uses.
extern "C" {
uint64_t g_user_access_recover_ip = 0;
}

namespace test {
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
const char *g_current_class = nullptr;
bool g_filter_bench = false;
bool g_class_auto_shutdown = false;
bool g_vfs_touched = false;
uint64_t g_kernel_entry_ns = 0;
} // namespace test

} // namespace kernel

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

namespace kernel {
namespace gs {

// -- BootState ---------------------------------------------------------------

BootInfo &boot_info() noexcept {
    return g_boot_info;
}

uint64_t get_boot_epoch() noexcept {
    return g_boot_epoch;
}

bool try_set_boot_epoch(uint64_t epoch, const WriteContext &ctx) noexcept {
    return verify_and_write(g_boot_epoch, epoch, WriteClass::BOOT_ONLY, ctx,
                            "boot_epoch");
}

uint64_t get_multiboot_magic() noexcept {
    return multiboot_magic;
}

uint64_t get_multiboot_info_ptr() noexcept {
    return multiboot_info_ptr;
}

bool try_set_multiboot(uint64_t magic, uint64_t info_ptr,
                       const WriteContext &ctx) noexcept {
    if (ctx.phase != StatePhase::BOOT)
        return false;
    multiboot_magic = magic;
    multiboot_info_ptr = info_ptr;
    return true;
}

// -- FaultState --------------------------------------------------------------

CanaryTrip &canary_trip() noexcept {
    return g_canary_trip;
}

void set_canary_trip(uint64_t task_id, uint8_t segment, uint64_t rip) noexcept {
    g_canary_trip.task_id = task_id;
    g_canary_trip.segment = segment;
    g_canary_trip.rip = rip;
    ++g_canary_trip.count;
}

void reset_canary_trip() noexcept {
    g_canary_trip = CanaryTrip{};
}

uint64_t &user_access_recover_ip() noexcept {
    return g_user_access_recover_ip;
}

// -- TestState ---------------------------------------------------------------

const char *get_current_class() noexcept {
    return test::g_current_class;
}

void set_current_class(const char *name) noexcept {
    test::g_current_class = name;
}

bool get_filter_bench() noexcept {
    return test::g_filter_bench;
}

void set_filter_bench(bool v) noexcept {
    test::g_filter_bench = v;
}

bool get_class_auto_shutdown() noexcept {
    return test::g_class_auto_shutdown;
}

void set_class_auto_shutdown(bool v) noexcept {
    test::g_class_auto_shutdown = v;
}

bool get_vfs_touched() noexcept {
    return test::g_vfs_touched;
}

void mark_vfs_touched(bool v) noexcept {
    test::g_vfs_touched = v;
}

uint64_t get_kernel_entry_ns() noexcept {
    return test::g_kernel_entry_ns;
}

void set_kernel_entry_ns() noexcept {
    test::g_kernel_entry_ns = arch::Timer::ns();
}

// ---------------------------------------------------------------------------
// VfsState definitions
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
kernel::fat32::Fat32Partition *g_fat32_partition = nullptr;

kernel::fat32::Fat32Partition *get_fat32_partition() noexcept {
    return g_fat32_partition;
}

bool try_set_fat32_partition(kernel::fat32::Fat32Partition *p) noexcept {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    // Legal range: null, or any kernel-half address (>= HHDM base).  User
    // pointers (low canonical) and garbage are rejected.
    if (addr != 0 && addr < CONFIG_HHDM_OFFSET)
        return false;
    g_fat32_partition = p;
    return true;
}

// ---------------------------------------------------------------------------
// Cross-TU context-switch bridge (called from isr_stubs.asm)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

// Cross-TU context-switch bridge (called from isr_stubs.asm)

// ---------------------------------------------------------------------------


extern "C" void scheduler_on_context_switch() noexcept {

    uint64_t id = __atomic_load_n(&scheduler_next_task_id, __ATOMIC_ACQUIRE);

    __atomic_store_n(&scheduler_next_task_id, UINT64_MAX, __ATOMIC_RELEASE);

    if (id == UINT64_MAX)

        return;

    auto *t = kernel::Scheduler::find_task(id);

    if (t && t->magic == kernel::TaskControlBlock::TCB_MAGIC)

        kernel::Scheduler::set_current_task(t);

    if (t && t->magic == kernel::TaskControlBlock::TCB_MAGIC) {

        kernel::Scheduler::canary_check_in_scheduler_hooks(t, 0);

    }

#if defined(CONFIG_DEBUG_IPC_SCHED)

    if (id == 1) {

        uint64_t nsp = task_stack_ptr(t);

        const uint64_t *f = reinterpret_cast<const uint64_t *>(nsp);

        kernel::Logger::raw_write("[H2-APPLY] id=1 ctx.rsp=0x");

    kernel::Logger::print_hex(nsp);

    kernel::Logger::print_hex(f[136 / 8]);

    kernel::Logger::print_hex(f[144 / 8]);

    kernel::Logger::print_hex(f[152 / 8]);

    kernel::Logger::print_hex(f[160 / 8]);

    kernel::Logger::print_hex(f[168 / 8]);

    kernel::Logger::raw_write("\n");

    }

#endif

#if defined(CONFIG_SNAPSHOT_CANARY_WATCH)

    if (kernel::test::snapshot_canary_corrupted()) {

        auto *cc = kernel::Scheduler::current_task();

        kernel::Logger::raw_write("[SNAP-CANARY-SW] corrupted cur=");

        kernel::Logger::print_dec(cc ? cc->id : 0u);

    kernel::Logger::raw_write(" tick=");

    kernel::Logger::print_dec(kernel::Timer::ticks());

    kernel::Logger::raw_write("\n");

    }

#endif

#if defined(CONFIG_DEBUG_IPC_SCHED)

    {

        auto *c = kernel::Scheduler::current_task();

        IPC_SCHED_TRACE("[APPLY]", "id=", id, "cur=", c ? c->id : 0u,

                        "x=", 0u, "y=", 0u);

    }

#endif

#if defined(CONFIG_DEBUG_IPC_SCHED)

    current_cpu().last_switch_tick = kernel::Timer::ticks();

#endif


} // namespace gs


// ---------------------------------------------------------------------------
// ISR-epilogue diagnostic hooks (called from isr_stubs.asm via C linkage)
// ---------------------------------------------------------------------------

/// @brief ISR-epilogue pre-save hook (isr_stubs.asm).  Fires on every ISR
///        entry that might attempt a deferred-switch apply.  Records the
///        generation captured at ISR entry.  Cold path (debug only).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_diag_pre_save() noexcept {
#if defined(CONFIG_DEBUG_IPC_SCHED)
    uint64_t gen = __atomic_load_n(&kernel::scheduler_switch_generation,
                                   __ATOMIC_ACQUIRE);
    kernel::Logger::raw_write("[H2-PRE] gen=0x");
    kernel::Logger::print_hex(gen);
    kernel::Logger::raw_write(" depth=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_depth, __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(kernel::Timer::ticks());
    kernel::Logger::raw_write("\n");
#else
    (void)0;
#endif
}

/// @brief ISR-epilogue depth-skip hook (isr_stubs.asm:133-134).  Fires when a
///        pending deferred-switch apply is skipped because the ISR nesting
/// depth exceeds 2.  Cold path.  Dumps the arm target + the RFLAGS the
/// interrupted task will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_diag_depth_skip() noexcept {
#if defined(CONFIG_DEBUG_IPC_SCHED)
    uint64_t id = __atomic_load_n(&kernel::scheduler_next_task_id,
                                  __ATOMIC_ACQUIRE);
    uint64_t rfl = 0;
#if defined(CONFIG_ARCH_X86_64)
    asm volatile("pushfq; pop %0" : "=r"(rfl));
#endif
    kernel::Logger::raw_write("[H2-DEPTH] id=");
    kernel::Logger::print_dec(id);
    kernel::Logger::raw_write(" depth=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_depth, __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" rfl=0x");
    kernel::Logger::print_hex(rfl);
    kernel::Logger::raw_write(" if=");
    kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(kernel::Timer::ticks());
    kernel::Logger::raw_write("\n");
#else
    (void)0;
#endif
}

/// @brief ISR-epilogue RSP-owner abort hook (isr_stubs.asm:270-275).  Fires
///        when a pending deferred-switch apply is aborted because the loaded
/// RSP lies outside [scheduler_load_kstack_base, top) — the asm-side
/// stale/foreign-frame guard.  Cold path.  Dumps the rejected RSP, the
/// kstack range, the arm target, and the RFLAGS the interrupted task
/// will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_diag_rsp_abort() noexcept {
#if defined(CONFIG_DEBUG_IPC_SCHED)
    uint64_t id = __atomic_load_n(&kernel::scheduler_next_task_id,
                                  __ATOMIC_ACQUIRE);
    uint64_t rsp = __atomic_load_n(&kernel::scheduler_load_rsp_from,
                                   __ATOMIC_ACQUIRE);
    uint64_t base = __atomic_load_n(&kernel::scheduler_load_kstack_base,
                                    __ATOMIC_ACQUIRE);
    uint64_t top = __atomic_load_n(&kernel::scheduler_load_kstack_top,
                                   __ATOMIC_ACQUIRE);
    uint64_t rfl = 0;
#if defined(CONFIG_ARCH_X86_64)
    asm volatile("pushfq; pop %0" : "=r"(rfl));
#endif
    kernel::Logger::raw_write("[H2-RSPABORT] id=");
    kernel::Logger::print_dec(id);
    kernel::Logger::raw_write(" rsp=0x");
    kernel::Logger::print_hex(rsp);
    kernel::Logger::raw_write(" base=0x");
    kernel::Logger::print_hex(base);
    kernel::Logger::raw_write(" top=0x");
    kernel::Logger::print_hex(top);
    kernel::Logger::raw_write(" depth=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_depth, __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" rfl=0x");
    kernel::Logger::print_hex(rfl);
    kernel::Logger::raw_write(" if=");
    kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(kernel::Timer::ticks());
    kernel::Logger::raw_write("\n");
#else
    (void)0;
#endif
}

/// @brief ISR-epilogue generation-skip hook (isr_stubs.asm): an ISR that
///        captured generation @p captured_gen at entry found it changed before
///        its epilogue, so it skipped applying the deferred switch — leaving
///        the dequeued target stranded until the next tick.  H2 ring event.
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_record_skip([[maybe_unused]] uint64_t captured_gen,
                                      [[maybe_unused]] uint64_t current_gen) noexcept {
    H2_REC(H2_EV_SKIP, captured_gen, current_gen, 0);
#if defined(CONFIG_DEBUG_IPC_SCHED)
    // H2 apply-skip audit (cold: fires only when the generation re-check
    // rejects a deferred-switch apply).  Dump the arm target, ISR depth and
    // the RFLAGS the interrupted task will return to (tests the IF=0
    // hypothesis for the freeze: the harness's hlt() must not run with IF off).
    {
        uint64_t id = __atomic_load_n(&kernel::scheduler_next_task_id,
                                      __ATOMIC_ACQUIRE);
        uint64_t rfl = 0;
#if defined(CONFIG_ARCH_X86_64)
        asm volatile("pushfq; pop %0" : "=r"(rfl));
#endif
        kernel::Logger::raw_write("[H2-SKIP] cap=0x");
        kernel::Logger::print_hex(captured_gen);
        kernel::Logger::raw_write(" cur=0x");
        kernel::Logger::print_hex(current_gen);
        kernel::Logger::raw_write(" id=");
        kernel::Logger::print_dec(id);
        kernel::Logger::raw_write(" depth=");
        kernel::Logger::print_dec(
            __atomic_load_n(&kernel::isr_nesting_depth, __ATOMIC_RELAXED));
        kernel::Logger::raw_write(" rfl=0x");
        kernel::Logger::print_hex(rfl);
        kernel::Logger::raw_write(" if=");
        kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
        kernel::Logger::raw_write(" tick=");
        kernel::Logger::print_dec(kernel::Timer::ticks());
        kernel::Logger::raw_write("\n");
    }
#endif
}

/// @brief Apply-side liveness + ownership re-check for the deferred switch
///        (called from isr_stubs.asm BEFORE `mov rsp,[load_rsp_from]`).  The
///        arm side (switch_to_task) validates the target's frame at publish
///        time, but the arm can survive past its ISR (nested-ISR depth guard or
///        generation-skip) into a later ISR, and in between the target task can
///        be terminated/freed (IRQs on) or the published RSP can drift from the
///        target's CURRENT kernel stack (snapshot restore / free+reuse).  The
///        [H2W] orphan-displacement fires when the apply then iretq's onto the
///        freed/foreign RSP (find_task(id)==null → current-cache lag → [H2W]
///        harness displacement).  This re-checks BOTH liveness (id_table_) AND
///        ownership (the published RSP lies inside the target's live kernel_stack,
///        or the harness boot stack) with IRQs disabled — so no task-context
///        removal can interleave — and aborts (clear atoms + bump generation) on
///        any mismatch.
/// @return 1 = apply the switch, 0 = abort it (atoms already invalidated).
/// @note Runs with IRQs disabled (interrupt gate); must not re-enable IRQs.
extern "C" int scheduler_validate_pending_switch() noexcept {
    uint64_t id =
        __atomic_load_n(&kernel::scheduler_next_task_id, __ATOMIC_ACQUIRE);
    if (id == UINT64_MAX)
        return 0;

    // Abort path shared by every drop reason below: cancel the arm AND undo
    // switch_to_task's current-task side effects.  switch_to_task set the
    // preempted task READY + enqueued it; when the apply is refused we abort
    // back into that task, so restore RUNNING + remove it from the runq.
    // Otherwise next_task() skips it (a RUNNING-current task) and falls
    // through to idle, iretq'ing the harness into the idle loop (the observed
    // H2 hang: [ARM a=6] -> [CLR-MISC] -> [ARM a=0] -> [IDLE-ARM] ->
    // [APPLY a=0]).  IF=0 here (interrupt gate) — the runq is not concurrently
    // modified, and dequeue_ready/set_task_ready are lock-free.
    auto drop_arm = [&](kernel::TaskControlBlock *target) {
        kernel::Scheduler::cancel_pending_switch();
        auto *cur = kernel::Scheduler::current_task();
        if (cur && cur->magic == kernel::TaskControlBlock::TCB_MAGIC) {
            if (cur->in_ready_queue_) {
                kernel::Scheduler::dequeue_ready(*cur);
            }
            // Only undo switch_to_task's READY publish side effect; never
            // resurrect a TERMINATED/BLOCKED current (see
            // restore_preempted_current).
            if (cur->state == kernel::TaskState::READY) {
                cur->state = kernel::TaskState::RUNNING;
            }
        }
        // Re-enqueue the dequeued target (if still alive) so it is not
        // stranded (INV-2); a dead/removed target is left to the reaper.
        if (target && target != kernel::Scheduler::get_idle_task() &&
            target != cur &&
            (target->state == kernel::TaskState::READY ||
             target->state == kernel::TaskState::RUNNING)) {
            H2_REC(H2_EV_REENQ, target->id,
                   static_cast<uint64_t>(target->state),
                   target->in_ready_queue_ ? 1u : 0u);
            kernel::Scheduler::set_task_ready(*target);
        }
    };

    auto *t = kernel::Scheduler::find_task(id);
    if (!t || t->magic != kernel::TaskControlBlock::TCB_MAGIC) {
        // Target removed/freed — a stale arm to a dead task.  Drop it.
#if defined(CONFIG_DEBUG_IPC_SCHED)
        kernel::Logger::raw_write("[H2-DEAD] id=");
        kernel::Logger::print_dec(id);
        kernel::Logger::raw_write(" t=");
        kernel::Logger::raw_write(" t=");
        kernel::Logger::raw_write(" t=");
        kernel::Logger::raw_write(" tick=");
        kernel::Logger::print_dec(arch::Timer::ticks());
        kernel::Logger::raw_write("\n");
#endif
        drop_arm(nullptr);
        return 0;
    }
    uint64_t rsp =
        __atomic_load_n(&kernel::scheduler_load_rsp_from, __ATOMIC_ACQUIRE);
    uint64_t base = reinterpret_cast<uint64_t>(t->kernel_stack);
    uint64_t top = t->kernel_stack_top;
    bool in_own = (base && top && rsp >= base && rsp <= top);
    bool harness_boot =
        (t == kernel::Scheduler::get_harness_task() &&
         rsp >= reinterpret_cast<uint64_t>(_stack_start) &&
         rsp < reinterpret_cast<uint64_t>(_stack_end));
    if (!in_own && !harness_boot) {
        // Stale arm: the published RSP no longer lies inside the target's
        // CURRENT kernel stack (it drifted to a foreign/direct-map address —
        // the H2 displacement).  Drop the arm and re-enqueue the target.
        kernel::Logger::raw_write("[H2-ABORT] id=");
        kernel::Logger::print_dec(t->id);
        kernel::Logger::raw_write(" st=");
        kernel::Logger::print_dec(static_cast<uint64_t>(t->state));
        kernel::Logger::raw_write(" inrq=");
        kernel::Logger::print_dec(t->in_ready_queue_ ? 1u : 0u);
        kernel::Logger::raw_write(" rsp=0x");
        kernel::Logger::print_hex(rsp);
        kernel::Logger::raw_write(" base=0x");
        kernel::Logger::print_hex(base);
        kernel::Logger::raw_write(" top=0x");
        kernel::Logger::print_hex(top);
        kernel::Logger::raw_write(" tick=");
        kernel::Logger::print_dec(arch::Timer::ticks());
        kernel::Logger::raw_write("\n");
        drop_arm(t);
        return 0;
    }

    return 1;
}

/// @brief ISR-epilogue callback for the `.abort_switch` path
///        (isr_stubs.asm): the deferred switch was refused because its load RSP
///        fell outside the dispatched task's kernel stack.  The arm side may
///        have already repointed TSS.RSP0 at the aborted `next` task's kernel
///        stack top (scheduler.cpp:1991, user-task dispatch).  Rebind RSP0 to
///        the CONTINUING task's own kernel stack so the next ring-3→ring-0
///        transition (int $0x80 trap gate) cannot push its iretq frame onto a
///        freed/foreign stack.  Harmless for ring-0-only runs (no privilege
///        transition ever consumes RSP0).
/// @note Runs with IRQs disabled (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_abort_switch_fixup() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    auto *cur = kernel::Scheduler::current_task();
    if (cur && cur->magic == kernel::TaskControlBlock::TCB_MAGIC &&
        cur->kernel_stack && cur->kernel_stack_top) {
        arch::GDT::set_tss_rsp0(cur->kernel_stack_top);
    }
#else
    (void)0;
#endif
}


// ---------------------------------------------------------------------------
// VfsState definitions
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
::net::Nic *g_nic = nullptr;

::net::Nic *get_nic() noexcept {
    return g_nic;
}

bool try_set_nic(::net::Nic *nic) noexcept {
    const uint64_t addr = reinterpret_cast<uint64_t>(nic);
    if (addr != 0 && addr < CONFIG_HHDM_OFFSET)
        return false;
    g_nic = nic;
    return true;
}

// -- Audit ring (CONFIG_DEBUG only) ------------------------------------------

#ifdef CONFIG_DEBUG
namespace {
constexpr size_t kAuditDepth = 64;
struct AuditEntry {
    const char *name;
    bool accepted;
    uint64_t tick;
};
AuditEntry g_audit[kAuditDepth];
uint64_t g_audit_idx = 0;
} // namespace

void audit_write(const char *name, bool accepted) noexcept {
    uint64_t i = g_audit_idx++ % kAuditDepth;
    g_audit[i].name = name;
    g_audit[i].accepted = accepted;
    g_audit[i].tick = arch::Timer::ticks();
}
#else
void audit_write(const char *name, bool accepted) noexcept {
    (void)name;
    (void)accepted;
}
#endif

} // namespace gs
} // namespace kernel
