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
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/arch/gdt.hpp>
#include <kernel/arch/cpu_context.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/sync/spinlock_order.hpp>
#include <kernel/task/scheduler_lock_guard.hpp>
#include <kernel/task/scheduler_debug_internal.hpp>
#include <kernel/nexios_config.h>
#include <lib/logger.hpp>
#include <assert.hpp>
#if defined(CONFIG_ARCH_AARCH64)
#include <kernel/arch/aarch64/hal/fpu.hpp>
#elif defined(CONFIG_ARCH_RISCV64)
#include <kernel/arch/riscv64/hal/fpu.hpp>
#endif
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/scheduler_errors.hpp>
#include <kernel/task/scheduler_integrity.hpp> // issue #368: task_stack_ptr
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
// AsmSwitchState — deferred-context-switch state shared with isr_stubs.asm.
//
// Issue #25 C1: every atom EXCEPT scheduler_kernel_cr3 is per-CPU
// (arrays indexed by logical CPU id).  Each CPU's timer ISR publishes to
// its own slots; each CPU's ISR epilogue consumes its own.  C++ indexes
// by arch::cpu_index(); x86_64 asm indexes via gs:0x10; riscv asm keeps
// using the array base (== [0], single-core there).  scheduler_kernel_cr3
// stays scalar (read-only after boot).  scheduler_next_task_id[] is
// initialized to UINT64_MAX per CPU by Scheduler::init (see below).
// ---------------------------------------------------------------------------
extern "C" {
uint64_t *scheduler_save_rsp_to[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_load_rsp_from[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_load_cr3_from[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_load_tls_from[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_next_task_id[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_load_kstack_base[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_load_kstack_top[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_switch_generation[CONFIG_MAX_CPUS] = {};
uint64_t scheduler_kernel_cr3 = 0;
bool scheduler_need_resched[CONFIG_MAX_CPUS] = {};
// Issue #221: one-shot force-apply for terminate-driven arms (see
// switch_away_from_terminating).  A terminating task's trap epilogue can
// otherwise skip a pending arm (depth/source gate) and sret back into the
// dead task, re-faulting forever (double-fault park with IRQs masked).
uint64_t scheduler_force_apply[CONFIG_MAX_CPUS] = {};
// Issue #417: per-CPU scheduler_lock_ take depth (see
// task/scheduler_lock_guard.hpp).  Zero-initialised; slot [cpu]
// counts takes by that CPU not yet released.  Transitory by
// construction — never held across test boundaries or quiesce.
uint64_t scheduler_lock_depth[CONFIG_MAX_CPUS] = {};
// M1 follow-up / M2 (issues #545/#546): per-CPU arch::IrqGuard nesting
// depth (see arch/hal/irq_guard.hpp).  Zero-initialised; slot [cpu]
// counts live IrqGuard scopes on that CPU.  Always maintained (not
// DEBUG-gated): the lock-park predicate and the reschedule-entry
// contract check read it in release builds too.  Transitory by
// construction, same reasoning as scheduler_lock_depth.
uint64_t irq_guard_depth[CONFIG_MAX_CPUS] = {};
// M2 follow-up (#545 FIRE): per-CPU IrqGuard owner attribution + 32-entry
// transition ring (evidence only).  Owner stored on the 0->1 transition,
// cleared on the 1->0 transition; ring records {depth, owner, cur, site}.
uint64_t irq_guard_owner[CONFIG_MAX_CPUS] = {};
uint64_t irq_guard_trace_idx[CONFIG_MAX_CPUS] = {};
uint64_t irq_guard_trace_depth[CONFIG_MAX_CPUS][32] = {};
uint64_t irq_guard_trace_owner[CONFIG_MAX_CPUS][32] = {};
uint64_t irq_guard_trace_cur[CONFIG_MAX_CPUS][32] = {};
uint64_t irq_guard_trace_site[CONFIG_MAX_CPUS][32] = {};
// M2 follow-up (#545 FIRE): syscall-trap marker + trap-entry nesting
// (see arch/hal/irq_guard.hpp).  Zero-initialised; slot [cpu] is nonzero
// only while that CPU executes inside Syscall::handle.
uint64_t in_syscall_trap[CONFIG_MAX_CPUS] = {};
uint64_t syscall_trap_nesting[CONFIG_MAX_CPUS] = {};
// Issue #25 (Phase A): on x86_64, isr_nesting_depth / irq_entry_tsc live
// in per_cpu[0] and are accessed by isr_stubs.asm via gs:0x20 / gs:0x28.
// Other architectures keep plain definitions here until their per-CPU
// migration.
#if !defined(CONFIG_ARCH_X86_64)
uint64_t isr_nesting_depth = 0;
uint64_t irq_entry_tsc = 0;
#endif
// Issue #513: saturating-depth event counter (rv64 trap tail + verbatim
// resume). Counts traps that found isr_nesting_depth already zero and
// skipped the decrement instead of driving it to -1 (which wedged the
// deferred-switch gate). Cold in steady state; nonzero proves the
// self-heal engaged.
uint64_t g_depth_sat = 0;
uint64_t scheduler_corruption_count = 0;
uint64_t deadline_detection_integrity = 0;
// Tracks which task's FPU state is currently in the registers, single-core
// archs only (declared in scheduler.hpp's extern "C" block).
// Issue #151: on x86_64 the owner lives in per_cpu[cpu].fpu_owner (gs:0x30);
// no global definition here, no linker alias — bare users fail the link.
#if !defined(CONFIG_ARCH_X86_64)
kernel::TaskControlBlock *fpu_owner = nullptr;
#endif
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

// Opaque fault-recovery sink for safe_copy_* (issue #539). See the header:
// noinline keeps it a real call so the trap-entered recover path cannot be
// merged or constant-folded by the optimizer (fail-open true was observed
// when the inlined body lost its zeroing). Qualified definitions (no new
// namespace block) to keep the style gate's short-name count stable.
__attribute__((noinline)) bool gs::fault_recover_false() noexcept {
    g_user_access_recover_ip = 0;
    return false;
}
// Opaque never-taken probe for the recovery keep-alive edge (issue #539).
// noinline keeps the KEEP's conditional goto a live edge: the optimizer
// cannot fold the condition, so the recover block is never deleted as
// unreachable. Always false at runtime (never taken).
__attribute__((noinline)) bool gs::recovery_probe() noexcept {
    return false;
}

namespace test {
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
const char *g_current_class = nullptr;
bool g_filter_bench = false;
bool g_class_auto_shutdown = false;
bool g_vfs_touched = false;
uint64_t g_kernel_entry_ns = 0;
// #578 park-test seam storage (DEBUG-only; absent in release so the
// release ELF carries no seam state — accessors are constants there).
#if defined(CONFIG_DEBUG)
bool g_park_test_ctx[CONFIG_MAX_CPUS] = {};
uint64_t g_park_test_ctx_waive_count = 0;
uint64_t g_park_test_ctx_unexpected_count = 0;
#endif
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

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
TrapOwnerTrip g_trap_owner_trip{};

TrapOwnerTrip &trap_owner_trip() noexcept {
    return g_trap_owner_trip;
}

void note_trap_owner_trip(uint64_t expected_id, uint64_t trapped_id,
                          uint64_t site, uint64_t frame_owner_id,
                          uint64_t slot_top, uint64_t live_root) noexcept {
    g_trap_owner_trip.expected_id = expected_id;
    g_trap_owner_trip.trapped_id = trapped_id;
    g_trap_owner_trip.site = site;
    g_trap_owner_trip.frame_owner_id = frame_owner_id;
    g_trap_owner_trip.slot_top = slot_top;
    g_trap_owner_trip.live_root = live_root;
    ++g_trap_owner_trip.count;
}

uint64_t trap_owner_trip_count() noexcept {
    return __atomic_load_n(&g_trap_owner_trip.count, __ATOMIC_RELAXED);
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

bool get_park_test_ctx() noexcept {
#if defined(CONFIG_DEBUG)
    return test::g_park_test_ctx[arch::cpu_index() % CONFIG_MAX_CPUS];
#else
    return false;
#endif
}

bool set_park_test_ctx(bool value) noexcept {
#if defined(CONFIG_DEBUG)
    // The flag is meaningless outside an active suite: setting it there
    // would arm waiver checks with no scope owning the teardown.
    if (!Scheduler::is_test_active())
        return false;
    test::g_park_test_ctx[arch::cpu_index() % CONFIG_MAX_CPUS] = value;
    return true;
#else
    (void)value;
    return false;
#endif
}

uint64_t park_test_ctx_waive_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&test::g_park_test_ctx_waive_count,
                           __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

void note_park_test_ctx_waive() noexcept {
#if defined(CONFIG_DEBUG)
    __atomic_fetch_add(&test::g_park_test_ctx_waive_count, 1,
                       __ATOMIC_RELAXED);
#endif
}

uint64_t park_test_ctx_unexpected_count() noexcept {
#if defined(CONFIG_DEBUG)
    return __atomic_load_n(&test::g_park_test_ctx_unexpected_count,
                           __ATOMIC_RELAXED);
#else
    return 0;
#endif
}

void note_park_test_ctx_unexpected() noexcept {
#if defined(CONFIG_DEBUG)
    __atomic_fetch_add(&test::g_park_test_ctx_unexpected_count, 1,
                       __ATOMIC_RELAXED);
#endif
}

// ---------------------------------------------------------------------------
// SchedGateState — issue #582 apply-gate defer windows + park latch.
// Named (not anonymous) so the kernel::-scope gate below and the
// Scheduler:: accessors can name it as gs::s582::.
// ---------------------------------------------------------------------------
namespace s582 {

// Thresholds (issue #582 INV-APPLY-2 / INV-PARK-2).
inline constexpr uint64_t kApplyDeferWarnTicks = 1;
inline constexpr uint64_t kApplyDeferFailClosedTicks = 8;
inline constexpr uint64_t kLockParkCoveredLimit = 8;
inline constexpr uint64_t kLockParkWarnTicks = 1;
inline constexpr uint64_t kFailClosedCensusMax = 8;

// Apply-gate per-CPU defer windows: since_tick == 0 means no streak.
uint64_t s_apply_defer_since_tick[CONFIG_MAX_CPUS] = {};
uint64_t s_apply_defer_warned[CONFIG_MAX_CPUS] = {};
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_defer_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_defer_warn_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_defer_rearm_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_defer_failclosed_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_term_hold_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_arm_under_lock_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_apply_force_denied_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
bool s_apply_defer_announced = false;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
bool s_apply_term_hold_announced = false;

// Per-CPU lock-park latch (issue #582 §4.3).  task_id == UINT64_MAX means
// unarmed.  covered_ticks is monotonic per latch lifetime; a same-task
// re-arm preserves it (rearm can never restart the clock, §4.5).
struct LockParkLatch {
    uint64_t task_id = UINT64_MAX;
    uint64_t generation = 0;
    uint8_t latch_class = 0;
    uint64_t since_tick = 0;
    uint32_t covered_ticks = 0;
    bool warned = false;
};

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
LockParkLatch s_lockpark_latch[CONFIG_MAX_CPUS] = {};
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_arm_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_covered_max = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_serviced_hook = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_serviced_tick = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_stale_cleared = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_woke_cleared = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_escalated = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_warn_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_rearm_count = 0;
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
uint64_t s_lockpark_pending = 0;

// Pre-RMS delivery watch (issue #582 §4.4 S2): the pre-RMS clear stashes
// the record; the post-RMS check confirms dispatch or restores it.
struct LockParkWatch {
    bool armed = false;
    uint64_t task_id = UINT64_MAX;
    uint64_t generation = 0;
    uint8_t latch_class = 0;
    uint32_t covered_ticks = 0;
    bool warned = false;
};

// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
LockParkWatch s_lockpark_watch[CONFIG_MAX_CPUS] = {};

} // namespace s582

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

/// @brief M2 follow-up (#545 ghost containment): a depth-1 scope owned by
///        neither the task in question nor nobody cannot belong to that
///        task — its own take would own the slot or nest to depth 2 — so
///        the scope is an orphan with no live frame behind it.  Used on
///        both ends of the 1b travel seam (save + load) so a singleton
///        ghost converges out of the system within one switch per task
///        instead of shuttling between slots forever.  (Owners are always
///        task pointers or 0 — ISR takes attribute to the interrupted task
///        and always unwind before the epilogue — so no ISR-owned case
///        needs exempting.)
static bool is_foreign_singleton(uint64_t depth, uint64_t owner,
                                 uint64_t own_token) noexcept {
    return (depth == 1) && (owner != 0) && (owner != own_token);
}

extern "C" void scheduler_on_context_switch() noexcept {

    uint64_t id = __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_ACQUIRE);

    __atomic_store_n(&Scheduler::SwSlots::next_task_id(), UINT64_MAX, __ATOMIC_RELEASE);

    if (id == UINT64_MAX)

        return;

    auto *t = kernel::Scheduler::find_task(id);

#if !defined(CONFIG_ARCH_X86_64)
    // Issue #353: eager FP switch (non-x86; x86 keeps lazy #NM).  The
    // epilogue already switched stacks/tables; FP regs still hold the
    // outgoing task's live state (handlers emit no FP use — verified by
    // build scan), so save-first here preserves everything via memory.
    // Kernel↔kernel switches skip (kernel never uses FP); a null current
    // with a user target still restores (never leaks stale regs).
    auto *old_fp = kernel::Scheduler::current_task();
    const bool fp_old_user = (old_fp != nullptr) && old_fp->is_user_;
    const bool fp_new_user = (t != nullptr) && t->is_user_;
    const bool fp_old_valid =
        (old_fp != nullptr) &&
        (old_fp->magic == kernel::TaskControlBlock::TCB_MAGIC);
    const bool fp_new_valid =
        (t != nullptr) && (t->magic == kernel::TaskControlBlock::TCB_MAGIC);
    // Audit S3: skip entirely when FP is absent (degraded path) — the
    // save/restore instructions would fault where FS can never set.
    const bool fp_hw = arch::fpu_available();
    if (fp_hw && (fp_old_user || fp_new_user) && fp_old_valid &&
        fp_new_valid) {
        arch::fpu_ensure_enabled();
        arch::fpu_switch_area(old_fp->fpu_state, t->fpu_state);
        old_fp->fpu_used = true;
        ++old_fp->fpu_state_gen;
    } else if (fp_hw && fp_new_user && fp_new_valid) {
        arch::fpu_ensure_enabled();
        arch::fpu_restore(t->fpu_state);
    }
#endif

    // M2 follow-up (#545 Phase-1b): the IrqGuard depth travels with the
    // task.  Save the outgoing task's live depth/owner into its slots and
    // restore the incoming task's.  Epilogue-only (isr_nesting != 0):
    // test harnesses call this function directly in task context to
    // exercise the switch mechanics, and must not disturb the live count.
    // Runs with IF=0 in the ISR epilogue on all three arches (single C
    // seam: x86 isr_stubs.asm, aarch64 vectors.S, riscv64 syscall_entry.S
    // all funnel through here).  A suspended scope resumes with its owner
    // intact; an abandoned stack is reaped by the self-heal in
    // validate_pending_switch.
    auto *outgoing_task = kernel::Scheduler::current_task();
    if (t && t->magic == kernel::TaskControlBlock::TCB_MAGIC)

        kernel::Scheduler::set_current_task(t);

    // M2 follow-up (#545 Phase-1b + ghost containment): the IrqGuard depth
    // travels with the task, minus orphaned singletons on both ends.
    // Epilogue-only (isr_nesting observed with interrupts masked):
    // test harnesses call this function directly in task context to
    // exercise the switch mechanics and must not disturb the live count.
    // The IF conjunct keeps a stale nesting balance (rv64 suite baseline)
    // from arming the seam in task context — interrupt gates clear IF.
    // Runs with IF=0 in the ISR epilogue on all three arches (single C
    // seam: x86 isr_stubs.asm, aarch64 vectors.S, riscv64
    // syscall_entry.S all funnel through here).
    // SAVE: a single-level foreign scope (depth==1, owner is neither the
    // outgoing task nor nobody/ISR) cannot belong to the outgoing task —
    // its own take would own the slot or nest to depth 2 — so it is an
    // orphan (observed shape: depth 1 owned by the idle task with no live
    // frame behind it).  Drop it and clear the slots instead of saving;
    // a suspended own scope (owner == outgoing) still travels untouched.
    // LOAD: same rule for the incoming slots — a stale singleton ghost in
    // another task's slots is zeroed instead of published (converges the
    // whole system within one switch per task; the incoming task holds
    // nothing of its own at depth==1-foreign by the same argument).
    // An abandoned stack is reaped by the self-heal in
    // validate_pending_switch; the saturated dtor (irq_guard.hpp) makes a
    // scope resumed after its count was dropped fail-safe instead of
    // wrapping to UINT64_MAX.
    if (kernel::isr_nesting_own() != 0 && !arch::interrupts_enabled()) {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        const bool incoming_valid =
            (t != nullptr) && (t->magic == kernel::TaskControlBlock::TCB_MAGIC);
        const bool outgoing_valid =
            (outgoing_task != nullptr) &&
            (outgoing_task->magic == kernel::TaskControlBlock::TCB_MAGIC);
        // Paired save+load only: a null outgoing (boot transitory) leaves
        // the live count untouched instead of dropping it.
        if (outgoing_valid && incoming_valid && outgoing_task != t) {
            const uint64_t live_depth = irq_guard_depth[cpu];
            const uint64_t live_owner = irq_guard_owner[cpu];
            const uint64_t outgoing_token =
                reinterpret_cast<uint64_t>(outgoing_task);
            if (!is_foreign_singleton(live_depth, live_owner,
                                      outgoing_token)) {
                outgoing_task->irq_depth_save = live_depth;
                outgoing_task->irq_owner_save = live_owner;
            } else {
                outgoing_task->irq_depth_save = 0;
                outgoing_task->irq_owner_save = 0;
            }
            const uint64_t saved_depth = t->irq_depth_save;
            const uint64_t saved_owner = t->irq_owner_save;
            const uint64_t incoming_token = reinterpret_cast<uint64_t>(t);
            if (!is_foreign_singleton(saved_depth, saved_owner,
                                      incoming_token)) {
                irq_guard_depth[cpu] = saved_depth;
                irq_guard_owner[cpu] = saved_owner;
            } else {
                t->irq_depth_save = 0;
                t->irq_owner_save = 0;
                irq_guard_depth[cpu] = 0;
                irq_guard_owner[cpu] = 0;
            }
        }
    }

    if (t && t->magic == kernel::TaskControlBlock::TCB_MAGIC) {

        kernel::Scheduler::canary_check_in_scheduler_hooks(t, 0);

    }

#if defined(CONFIG_DEBUG_IPC_SCHED)

    if (id == 1) {

        uint64_t nsp = sched_verify::task_stack_ptr(t);

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

    kernel::Logger::print_dec(arch::Timer::ticks());

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

    current_cpu().last_switch_tick = arch::Timer::ticks();

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
    uint64_t gen = __atomic_load_n(&Scheduler::SwSlots::generation(),
                                   __ATOMIC_ACQUIRE);
    kernel::Logger::raw_write("[H2-PRE] gen=0x");
    kernel::Logger::print_hex(gen);
    kernel::Logger::raw_write(" depth=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(arch::Timer::ticks());
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
    uint64_t id = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                  __ATOMIC_ACQUIRE);
    uint64_t rfl = 0;
#if defined(CONFIG_ARCH_X86_64)
    asm volatile("pushfq; pop %0" : "=r"(rfl));
#endif
    kernel::Logger::raw_write("[H2-DEPTH] id=");
    kernel::Logger::print_dec(id);
    kernel::Logger::raw_write(" depth=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" rfl=0x");
    kernel::Logger::print_hex(rfl);
    kernel::Logger::raw_write(" if=");
    kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(arch::Timer::ticks());
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
    uint64_t id = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
                                  __ATOMIC_ACQUIRE);
    uint64_t rsp = __atomic_load_n(&Scheduler::SwSlots::load_rsp_from(),
                                   __ATOMIC_ACQUIRE);
    uint64_t base = __atomic_load_n(&Scheduler::SwSlots::load_kstack_base(),
                                    __ATOMIC_ACQUIRE);
    uint64_t top = __atomic_load_n(&Scheduler::SwSlots::load_kstack_top(),
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
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" rfl=0x");
    kernel::Logger::print_hex(rfl);
    kernel::Logger::raw_write(" if=");
    kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(arch::Timer::ticks());
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
        uint64_t id = __atomic_load_n(&Scheduler::SwSlots::next_task_id(),
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
            __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED));
        kernel::Logger::raw_write(" rfl=0x");
        kernel::Logger::print_hex(rfl);
        kernel::Logger::raw_write(" if=");
        kernel::Logger::print_dec((rfl & 0x200) ? 1u : 0u);
        kernel::Logger::raw_write(" tick=");
        kernel::Logger::print_dec(arch::Timer::ticks());
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
/// @brief Issue #582 §4.9 FailClosed dump: single bounded dump + panic.
///        <=10 unconditional lines (fatal + <=8 census + lock state) +
///        <=8 DEBUG-only lines (held-stack + H2/op-ring tails).
///        ISR-safe (raw_write only, then panic() cli+hlts).
/// @param tag Fatal-line tag (apply vs park path marker).
/// @param id Arm id (apply path) or latched task id (park path).
/// @param streak Ticks deferred (apply) or covered ticks (park).
/// @param own_held True when the own-depth conjunct held (apply path).
/// @note Runs with IRQs disabled (interrupt gate); must not re-enable IRQs.
[[noreturn]] void s582_failclosed(const char *tag, uint64_t id,
                                  uint64_t streak, bool own_held) noexcept {
    using namespace gs::s582;
    const uint64_t now = arch::Timer::ticks();
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    const uint64_t force =
        __atomic_load_n(&kernel::Scheduler::SwSlots::force_apply(),
                        __ATOMIC_ACQUIRE);
    kernel::log::dmesg_push_sev(
        kernel::log::ErrorSubsystem::SCHED,
        kernel::log::kDmesgBase_SCHED + 17, kernel::log::LogSeverity::FATAL,
        "sched: scheduler_lock_ unreleased (fail-closed)", id);
    auto *cur = kernel::Scheduler::current_task();
    const bool cur_ok =
        (cur != nullptr && cur->magic == kernel::TaskControlBlock::TCB_MAGIC);
    kernel::Logger::raw_write(tag);
    kernel::Logger::raw_write(" tick=");
    kernel::Logger::print_dec(now);
    kernel::Logger::raw_write(" arm=");
    kernel::Logger::print_dec(id);
    kernel::Logger::raw_write(" slock_depth=");
    kernel::Logger::print_dec(kernel::scheduler_lock_depth_own());
    kernel::Logger::raw_write(" irq_depth=");
    kernel::Logger::print_dec(arch::irq_guard_depth_own());
    kernel::Logger::raw_write(" spl_depth=");
    kernel::Logger::print_dec(sync::spinlock_held_depth_own());
    kernel::Logger::raw_write(" is_locked=");
    kernel::Logger::print_dec(kernel::Scheduler::scheduler_lock_held() ? 1u
                                                                       : 0u);
    kernel::Logger::raw_write(" holder=0x");
    kernel::Logger::print_hex(reinterpret_cast<uint64_t>(
        kernel::Scheduler::scheduler_lock_holder()));
    kernel::Logger::raw_write(" if=");
    kernel::Logger::print_dec(arch::interrupts_enabled() ? 1u : 0u);
    kernel::Logger::raw_write(" isr_nesting=");
    kernel::Logger::print_dec(
        __atomic_load_n(&kernel::isr_nesting_own(), __ATOMIC_RELAXED));
    kernel::Logger::raw_write(" streak=");
    kernel::Logger::print_dec(streak);
    kernel::Logger::raw_write(" force=");
    kernel::Logger::print_dec(force);
    kernel::Logger::raw_write(" own=");
    kernel::Logger::print_dec(own_held ? 1u : 0u);
    kernel::Logger::raw_write(" cur=");
    kernel::Logger::print_dec(cur_ok ? cur->id : 0u);
    kernel::Logger::raw_write("\n");
    uint64_t shown = 0;
    for (auto *t = kernel::Scheduler::all_tasks().first_ptr();
         t != nullptr && shown < kFailClosedCensusMax;
         t = kernel::Scheduler::all_tasks().next_ptr(t)) {
        if (t->magic != kernel::TaskControlBlock::TCB_MAGIC)
            continue;
        if (kernel::Scheduler::is_idle_task(t))
            continue;
        if (kernel::Scheduler::queue_target(*t) != cpu)
            continue;
        kernel::Logger::raw_write("[S582-CENSUS] id=");
        kernel::Logger::print_dec(t->id);
        kernel::Logger::raw_write(" st=");
        kernel::Logger::print_dec(static_cast<uint64_t>(t->state));
        kernel::Logger::raw_write(" inrq=");
        kernel::Logger::print_dec(t->in_ready_queue_ ? 1u : 0u);
        kernel::Logger::raw_write(" rq=");
        kernel::Logger::print_dec(t->rq_priority_);
        kernel::Logger::raw_write(" eff=");
        kernel::Logger::print_dec(kernel::Scheduler::effective_priority(t));
        kernel::Logger::raw_write("\n");
        ++shown;
    }
    kernel::Logger::raw_write("[S582-LOCK] depth=");
    kernel::Logger::print_dec(kernel::scheduler_lock_depth_own());
    kernel::Logger::raw_write(" holder=0x");
    kernel::Logger::print_hex(reinterpret_cast<uint64_t>(
        kernel::Scheduler::scheduler_lock_holder()));
    kernel::Logger::raw_write("\n");
#if defined(CONFIG_DEBUG)
    {
        uint64_t addrs[8] = {};
        const char *sites[8] = {};
        uint64_t owners[8] = {};
        uint8_t ranks[8] = {};
        uint8_t cpus[8] = {};
        uint8_t take_ifs[8] = {};
        const uint64_t n = sync::spinlock_dump_held(
            addrs, sites, owners, ranks, cpus, take_ifs, 8);
        for (uint64_t i = 0; i < n; ++i) {
            kernel::Logger::raw_write("[S582-HELD] lock=0x");
            kernel::Logger::print_hex(addrs[i]);
            kernel::Logger::raw_write(" site=");
            kernel::Logger::raw_write(sites[i] != nullptr ? sites[i] : "?");
            kernel::Logger::raw_write(" owner=");
            kernel::Logger::print_dec(owners[i]);
            kernel::Logger::raw_write("\n");
        }
    }
    kernel::sched_debug::h2_dump_tail(4);
    kernel::sched_debug::sched_runq_trace_dump(UINT64_MAX, 4);
#endif
    panic("sched: scheduler_lock_ unreleased");
    __builtin_unreachable();
}

/// @return 1 = apply the switch, 0 = abort it (atoms already invalidated).
/// @note Runs with IRQs disabled (interrupt gate); must not re-enable IRQs.
extern "C" int scheduler_validate_pending_switch() noexcept {
    uint64_t id =
        __atomic_load_n(&Scheduler::SwSlots::next_task_id(), __ATOMIC_ACQUIRE);
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
        using namespace gs::s582;
        // Issue #25 C1: on APs, SKIP the drop (leave the arm pending).
        // The drop mutates the own queue lock-free, racing BSP task-context
        // writers of queue[1] (which hold the global lock the AP must not
        // block on).  The next AP tick's rate_monotonic_schedule CLR path
        // performs the identical surgery (restore + clear + re-enqueue)
        // under proper locking — delaying the drop by ≤1 tick is harmless
        // (the arm cannot apply: validate fails identically next ISR).
        // BSP path unchanged (single-core IF=0 rules).
        if (arch::cpu_index() != 0)
            return;
        // Issue #582: an aborted arm ends any defer streak on this CPU.
        {
            const uint64_t dcpu = arch::cpu_index() % CONFIG_MAX_CPUS;
            if (__atomic_load_n(&s_apply_defer_since_tick[dcpu],
                                __ATOMIC_ACQUIRE) != 0) {
                __atomic_store_n(&s_apply_defer_since_tick[dcpu], (uint64_t)0,
                                 __ATOMIC_RELEASE);
            }
            if (__atomic_load_n(&s_apply_defer_warned[dcpu],
                                __ATOMIC_ACQUIRE) != 0) {
                __atomic_store_n(&s_apply_defer_warned[dcpu], (uint64_t)0,
                                 __ATOMIC_RELEASE);
            }
        }
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
        // Issue #25 C1: any idle (BSP or AP) is the fallback, never
        // re-enqueued.
        if (target && !kernel::Scheduler::is_idle_task(target) &&
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
#if CONFIG_LOCK_PARK_SELFHEAL == 1
    // M2 follow-up (#545 FIRE): drop orphaned IrqGuard depth whose owner
    // is gone.  A suspended scope on a live task's stack resumes and
    // releases itself — only a dead/gone owner is cleared (a recycled
    // address is still orphaned: the new task never took the scope).
    // Runs with IF=0 (epilogue); the walk is lock-free magic-checked
    // iteration (wake_reply_waiters_of precedent).
    {
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        if (irq_guard_depth[cpu] != 0) {
            const uint64_t owner = irq_guard_owner[cpu];
            bool owner_live = false;
            kernel::TaskControlBlock *owner_task = nullptr;
            if (!owner_live && owner != 0) {
                kernel::Scheduler::TaskIter iter{};
                for (auto *w = iter.next(); w != nullptr; w = iter.next()) {
                    if (reinterpret_cast<uint64_t>(w) == owner &&
                        w->state != kernel::TaskState::TERMINATED &&
                        w->state != kernel::TaskState::REAPED) {
                        owner_live = true;
                        owner_task = w;
                        break;
                    }
                }
            }
            // Idle-owned exception: the idle task never suspends a guard
            // scope (ticks are masked under guards and idle never
            // reschedules inside one), so idle-owned depth while another
            // task runs is an orphaned misattribution, not a live
            // suspended scope — clear it like a dead owner.
            if (owner_live && owner_task != nullptr &&
                kernel::Scheduler::is_idle_task(owner_task) &&
                owner_task != kernel::Scheduler::current_task()) {
                owner_live = false;
            }
            if (!owner_live) {
                irq_guard_depth[cpu] = 0;
                irq_guard_owner[cpu] = 0;
            }
        }
    }
#endif
    uint64_t rsp =
        __atomic_load_n(&Scheduler::SwSlots::load_rsp_from(), __ATOMIC_ACQUIRE);
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

    // Issue #582 INV-APPLY-1/2/3: the interrupted context must hold no
    // scheduler_lock_ take for the apply to proceed.  Both conjuncts are
    // required (mark-after-take / clear-before-release leaves an
    // unmarked-but-locked window for each alone).  Fail direction is defer:
    // the arm AND the force bit are left untouched for the next epilogue
    // (deny-and-rearm, INV-APPLY-3 — force never overrides the deny).
    // Return ABI: 1 = apply, 0 = abort/clear (existing reasons only),
    // 2 = defer (leave the arm untouched).
    {
        using namespace gs::s582;
        const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
        const bool own_held = (kernel::scheduler_lock_depth_own() != 0);
        const bool word_held = kernel::Scheduler::scheduler_lock_held();
        const uint64_t now = arch::Timer::ticks();
        if (own_held || word_held) {
            __atomic_fetch_add(&s_apply_defer_count, 1ULL, __ATOMIC_RELAXED);
            if (__atomic_load_n(&kernel::Scheduler::SwSlots::force_apply(),
                                __ATOMIC_ACQUIRE) != 0) {
                __atomic_fetch_add(&s_apply_force_denied_count, 1ULL,
                                   __ATOMIC_RELAXED);
            }
            // One-shot first-defer line: reason 1 = own depth,
            // 2 = lock word.  No logging on the allow path.
            if (!__atomic_exchange_n(&s_apply_defer_announced, true,
                                     __ATOMIC_RELAXED)) {
                auto *cur = kernel::Scheduler::current_task();
                const bool cur_ok =
                    (cur != nullptr &&
                     cur->magic == kernel::TaskControlBlock::TCB_MAGIC);
                kernel::Logger::raw_write("[S582-APPLY-DEFER] id=");
                kernel::Logger::print_dec(id);
                kernel::Logger::raw_write(" reason=");
                kernel::Logger::print_dec(own_held ? 1u : 2u);
                kernel::Logger::raw_write(" cur=");
                kernel::Logger::print_dec(cur_ok ? cur->id : 0u);
                kernel::Logger::raw_write(" st=");
                kernel::Logger::print_dec(
                    cur_ok ? static_cast<uint64_t>(cur->state) : 99u);
                kernel::Logger::raw_write(" inrq=");
                kernel::Logger::print_dec(
                    (cur_ok && cur->in_ready_queue_) ? 1u : 0u);
                kernel::Logger::raw_write(" depth=");
                kernel::Logger::print_dec(
                    kernel::scheduler_lock_depth_own());
                kernel::Logger::raw_write(" tick=");
                kernel::Logger::print_dec(now);
                kernel::Logger::raw_write("\n");
            }
            uint64_t since = __atomic_load_n(&s_apply_defer_since_tick[cpu],
                                             __ATOMIC_ACQUIRE);
            if (since == 0) {
                __atomic_store_n(&s_apply_defer_since_tick[cpu], now,
                                 __ATOMIC_RELEASE);
                since = now;
            }
            const uint64_t streak = (now >= since) ? (now - since) : 0;
            if (streak >= kApplyDeferFailClosedTicks) {
                __atomic_fetch_add(&s_apply_defer_failclosed_count, 1ULL,
                                   __ATOMIC_RELAXED);
                s582_failclosed("[S582-APPLY-FAILCLOSED]", id, streak,
                                own_held);
            }
            if (streak >= kApplyDeferWarnTicks &&
                __atomic_load_n(&s_apply_defer_warned[cpu],
                                __ATOMIC_ACQUIRE) == 0) {
                __atomic_store_n(&s_apply_defer_warned[cpu], (uint64_t)1,
                                 __ATOMIC_RELEASE);
                __atomic_fetch_add(&s_apply_defer_warn_count, 1ULL,
                                   __ATOMIC_RELAXED);
                kernel::Logger::raw_write("[S582-APPLY-DEFER-STUCK] id=");
                kernel::Logger::print_dec(id);
                kernel::Logger::raw_write(" streak=");
                kernel::Logger::print_dec(streak);
                kernel::Logger::raw_write(" tick=");
                kernel::Logger::print_dec(now);
                kernel::Logger::raw_write("\n");
            }
            return 2;
        }
        // Lock free with a stale pending streak: unreachable in health
        // (the RMS CLR path clears/republishes pending arms every tick
        // with a free lock).  Re-arm loudly and keep the arm (the
        // rollback tripwire, §11): never cancel on a deferral streak.
        const uint64_t since = __atomic_load_n(
            &s_apply_defer_since_tick[cpu], __ATOMIC_ACQUIRE);
        if (since != 0 &&
            ((now >= since) ? (now - since) : 0) >=
                kApplyDeferFailClosedTicks) {
            __atomic_fetch_add(&s_apply_defer_rearm_count, 1ULL,
                               __ATOMIC_RELAXED);
            __atomic_store_n(&s_apply_defer_since_tick[cpu], (uint64_t)0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&s_apply_defer_warned[cpu], (uint64_t)0,
                             __ATOMIC_RELEASE);
            kernel::log::dmesg_push_sev(
                kernel::log::ErrorSubsystem::SCHED,
                kernel::log::kDmesgBase_SCHED + 19,
                kernel::log::LogSeverity::WARN,
                "sched: apply defer re-armed (lock free)", id);
            kernel::Logger::warn("sched: apply defer re-armed id=%lu", id);
            return 2;
        }
        // Healthy apply: end any streak, clear the one-shot force (C-side
        // clear #1 of §B.5 — the arm is consumed), apply.
        if (since != 0) {
            __atomic_store_n(&s_apply_defer_since_tick[cpu], (uint64_t)0,
                             __ATOMIC_RELEASE);
        }
        if (__atomic_load_n(&s_apply_defer_warned[cpu], __ATOMIC_ACQUIRE) !=
            0) {
            __atomic_store_n(&s_apply_defer_warned[cpu], (uint64_t)0,
                             __ATOMIC_RELEASE);
        }
        __atomic_store_n(&kernel::Scheduler::SwSlots::force_apply(),
                         (uint64_t)0, __ATOMIC_RELEASE);
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
    // Issue #25 C1: CPU0-gated — the AP shares this TSS and runs kernel
    // tasks only (rsp0 irrelevant there); an AP-side rebind would corrupt
    // the BSP's ring-3→0 stack.
    if (arch::cpu_index() != 0)
        return;
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

// ---------------------------------------------------------------------------
// SchedGateState definitions — issue #582 (production).  This region is
// already inside kernel::gs (the function-scope } above closes a function,
// not gs — the stale comment is pre-existing); s582 below reopens the
// state block, and the gs:: API follows unqualified.
namespace s582 {

// Clear one latch (stale/woke/delivered): disarm + zero the clock.
void s582_latch_clear(uint64_t cpu) noexcept {
    LockParkLatch &latch = s_lockpark_latch[cpu % CONFIG_MAX_CPUS];
    latch.task_id = UINT64_MAX;
    latch.generation = 0;
    latch.latch_class = 0;
    latch.since_tick = 0;
    latch.covered_ticks = 0;
    latch.warned = false;
}

// Rule 4/5/6 accounting for one unserviced instant (§4.5): covered++ with
// the max witness, rearm (since = now, clock preserved), then the
// escalation pair — FailClosed at the limit (rule 5, SCHED+17 + dump +
// panic) or the one-shot stuck Warn (rule 6, SCHED+18).  Warn reads the
// pre-rearm since so a latch pending >= 1 tick warns on its first
// unserviced instant.
void s582_cover_instant(uint64_t cpu) noexcept {
    LockParkLatch &latch = s_lockpark_latch[cpu % CONFIG_MAX_CPUS];
    const uint32_t covered = latch.covered_ticks + 1;
    latch.covered_ticks = covered;
    const uint64_t now = arch::Timer::ticks();
    const uint64_t since = latch.since_tick;
    const uint64_t pending =
        (latch.task_id == UINT64_MAX) ? 0 : latch.task_id;
    const uint64_t covered64 = covered;
    const uint64_t mx =
        __atomic_load_n(&s_lockpark_covered_max, __ATOMIC_RELAXED);
    if (covered64 > mx) {
        __atomic_store_n(&s_lockpark_covered_max, covered64,
                         __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&s_lockpark_rearm_count, 1ULL, __ATOMIC_RELAXED);
    if (covered >= kLockParkCoveredLimit) {
        __atomic_fetch_add(&s_lockpark_escalated, 1ULL, __ATOMIC_RELAXED);
        s582_failclosed("[S582-LOCKPARK-FAILCLOSED]", pending, covered64,
                        false);
    }
    if (!latch.warned && now >= since &&
        (now - since) >= kLockParkWarnTicks) {
        latch.warned = true;
        __atomic_fetch_add(&s_lockpark_warn_count, 1ULL, __ATOMIC_RELAXED);
        kernel::log::dmesg_push_sev(
            kernel::log::ErrorSubsystem::SCHED,
            kernel::log::kDmesgBase_SCHED + 18,
            kernel::log::LogSeverity::WARN, "sched: lock-park latch stuck",
            pending);
        kernel::Logger::warn("sched: lock-park latch stuck id=%lu covered=%lu",
                             pending, covered64);
    }
    latch.since_tick = now;
}

} // namespace s582

uint64_t apply_gate_defer_count() noexcept {
    return __atomic_load_n(&s582::s_apply_defer_count, __ATOMIC_RELAXED);
}

uint64_t apply_gate_warn_count() noexcept {
    return __atomic_load_n(&s582::s_apply_defer_warn_count, __ATOMIC_RELAXED);
}

uint64_t apply_gate_rearm_count() noexcept {
    return __atomic_load_n(&s582::s_apply_defer_rearm_count,
                           __ATOMIC_RELAXED);
}

uint64_t apply_gate_failclosed_count() noexcept {
    return __atomic_load_n(&s582::s_apply_defer_failclosed_count,
                           __ATOMIC_RELAXED);
}

uint64_t apply_gate_term_hold_count() noexcept {
    return __atomic_load_n(&s582::s_apply_term_hold_count, __ATOMIC_RELAXED);
}

uint64_t apply_gate_arm_under_lock_count() noexcept {
    return __atomic_load_n(&s582::s_apply_arm_under_lock_count,
                           __ATOMIC_RELAXED);
}

uint64_t apply_gate_force_denied_count() noexcept {
    return __atomic_load_n(&s582::s_apply_force_denied_count,
                           __ATOMIC_RELAXED);
}

void note_arm_under_lock() noexcept {
    __atomic_fetch_add(&s582::s_apply_arm_under_lock_count, 1ULL,
                       __ATOMIC_RELAXED);
}

void note_apply_term_hold() noexcept {
    __atomic_fetch_add(&s582::s_apply_term_hold_count, 1ULL,
                       __ATOMIC_RELAXED);
}

bool apply_term_hold_announced_swap() noexcept {
    return !__atomic_exchange_n(&s582::s_apply_term_hold_announced, true,
                                __ATOMIC_RELAXED);
}

void apply_gate_reset_for_test() noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        __atomic_store_n(&s582::s_apply_defer_since_tick[c], (uint64_t)0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&s582::s_apply_defer_warned[c], (uint64_t)0,
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s582::s_apply_defer_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_defer_warn_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_defer_rearm_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_defer_failclosed_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_term_hold_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_arm_under_lock_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_force_denied_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_apply_defer_announced, false,
                     __ATOMIC_RELAXED);
}

void lockpark_latch_arm(uint64_t task_id, uint64_t generation,
                         uint8_t cls) noexcept {
    if (task_id == UINT64_MAX)
        return;
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    s582::LockParkLatch &latch = s582::s_lockpark_latch[cpu];
    // Same-task re-arm preserves the monotonic clock (rearm can never
    // restart it, §4.5); a new task starts a fresh lifetime.
    const bool same = (latch.task_id == task_id &&
                       latch.generation == generation);
    const uint32_t covered = same ? latch.covered_ticks : 0;
    const bool warned = same ? latch.warned : false;
    latch.task_id = task_id;
    latch.generation = generation;
    latch.latch_class = cls;
    latch.since_tick = arch::Timer::ticks();
    latch.covered_ticks = covered;
    latch.warned = warned;
    __atomic_fetch_add(&s582::s_lockpark_arm_count, 1ULL, __ATOMIC_RELAXED);
}

uint64_t lockpark_arm_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_arm_count, __ATOMIC_RELAXED);
}

uint64_t lockpark_covered_ticks(uint64_t cpu) noexcept {
    return s582::s_lockpark_latch[cpu % CONFIG_MAX_CPUS].covered_ticks;
}

uint64_t lockpark_covered_ticks_max() noexcept {
    return __atomic_load_n(&s582::s_lockpark_covered_max, __ATOMIC_RELAXED);
}

uint64_t lockpark_serviced_hook_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_serviced_hook, __ATOMIC_RELAXED);
}

uint64_t lockpark_serviced_tick_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_serviced_tick, __ATOMIC_RELAXED);
}

uint64_t lockpark_stale_cleared_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_stale_cleared, __ATOMIC_RELAXED);
}

uint64_t lockpark_woke_cleared_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_woke_cleared, __ATOMIC_RELAXED);
}

uint64_t lockpark_escalated_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_escalated, __ATOMIC_RELAXED);
}

uint64_t lockpark_warn_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_warn_count, __ATOMIC_RELAXED);
}

uint64_t lockpark_rearm_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_rearm_count, __ATOMIC_RELAXED);
}

uint64_t lockpark_pending_count() noexcept {
    return __atomic_load_n(&s582::s_lockpark_pending, __ATOMIC_RELAXED);
}

uint64_t lockpark_class_of(uint64_t cpu) noexcept {
    return s582::s_lockpark_latch[cpu % CONFIG_MAX_CPUS].latch_class;
}

LockParkAction lockpark_service_hook() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    s582::LockParkLatch &latch = s582::s_lockpark_latch[cpu];
    if (latch.task_id == UINT64_MAX)
        return LockParkAction::None;
    auto *latched = kernel::Scheduler::find_task(latch.task_id);
    // Rules 1-2 run here (post-release, pre-warn/park): silent clears.
    if (latched == nullptr ||
        latched->magic != kernel::TaskControlBlock::TCB_MAGIC ||
        latched->generation != latch.generation) {
        s582::s582_latch_clear(cpu);
        __atomic_fetch_add(&s582::s_lockpark_stale_cleared, 1ULL,
                           __ATOMIC_RELAXED);
        return LockParkAction::None;
    }
    if (latched->state != kernel::TaskState::BLOCKED) {
        s582::s582_latch_clear(cpu);
        __atomic_fetch_add(&s582::s_lockpark_woke_cleared, 1ULL,
                           __ATOMIC_RELAXED);
        return LockParkAction::None;
    }
    // Five-term discriminator (§4.4 S1): all must hold to park in place.
    // Never reads the latch class (§4.3): service keys on {id,generation}
    // matching the current BLOCKED runner, which the SelfLock shape is.
    auto *cur = kernel::Scheduler::current_task();
    const bool admitted =
        (kernel::isr_nesting_own() == 0 ||
         arch::in_syscall_trap_level()) &&
        arch::interrupts_enabled() &&
        (arch::irq_guard_depth_own() == 0) &&
        (sync::spinlock_held_depth_own() == 0) &&
        (CONFIG_TICK_HZ != 0) && (cur != nullptr) &&
        (cur->magic == kernel::TaskControlBlock::TCB_MAGIC) &&
        !kernel::Scheduler::is_idle_task(cur) && (cur == latched) &&
        (cur->state == kernel::TaskState::BLOCKED) &&
        (cur->generation == latch.generation);
    if (!admitted) {
        // Retained for the tick; counted so a stuck retain is visible.
        __atomic_fetch_add(&s582::s_lockpark_pending, 1ULL,
                           __ATOMIC_RELAXED);
        return LockParkAction::None;
    }
    s582::s582_latch_clear(cpu);
    __atomic_fetch_add(&s582::s_lockpark_serviced_hook, 1ULL,
                       __ATOMIC_RELAXED);
    return LockParkAction::ParkCurrent;
}

void lockpark_service_tick_pre() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    s582::LockParkLatch &latch = s582::s_lockpark_latch[cpu];
    if (latch.task_id == UINT64_MAX)
        return;
    auto *latched = kernel::Scheduler::find_task(latch.task_id);
    if (latched == nullptr ||
        latched->magic != kernel::TaskControlBlock::TCB_MAGIC ||
        latched->generation != latch.generation) {
        s582::s582_latch_clear(cpu);
        __atomic_fetch_add(&s582::s_lockpark_stale_cleared, 1ULL,
                           __ATOMIC_RELAXED);
        s582::s_lockpark_watch[cpu].armed = false;
        return;
    }
    if (latched->state != kernel::TaskState::BLOCKED) {
        s582::s582_latch_clear(cpu);
        __atomic_fetch_add(&s582::s_lockpark_woke_cleared, 1ULL,
                           __ATOMIC_RELAXED);
        s582::s_lockpark_watch[cpu].armed = false;
        return;
    }
    // Rule 3 vs 4: deliverable when the lock looks free enough for the
    // immediately following RMS to take it and dispatch (word free and
    // no own take outstanding).
    const bool deliverable =
        !kernel::Scheduler::scheduler_lock_held() &&
        (kernel::scheduler_lock_depth_own() == 0);
    if (deliverable) {
        // Clear for the RMS dispatch; stash for the post-RMS confirm.
        s582::LockParkWatch &watch = s582::s_lockpark_watch[cpu];
        watch.armed = true;
        watch.task_id = latch.task_id;
        watch.generation = latch.generation;
        watch.latch_class = latch.latch_class;
        watch.covered_ticks = latch.covered_ticks;
        watch.warned = latch.warned;
        s582::s582_latch_clear(cpu);
        __atomic_fetch_add(&s582::s_lockpark_serviced_tick, 1ULL,
                           __ATOMIC_RELAXED);
        return;
    }
    s582::s582_cover_instant(cpu);
}

void lockpark_service_tick_post() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    s582::LockParkWatch &watch = s582::s_lockpark_watch[cpu];
    if (!watch.armed)
        return;
    watch.armed = false;
    const uint64_t wid = watch.task_id;
    const uint64_t wgen = watch.generation;
    auto *t = kernel::Scheduler::find_task(wid);
    const bool live =
        (t != nullptr && t->magic == kernel::TaskControlBlock::TCB_MAGIC &&
         t->generation == wgen);
    if (!live || t->state != kernel::TaskState::BLOCKED) {
        // Dispatched-and-progressed, woken, or gone: rule 1/2 equivalent,
        // silent (serviced_tick was already counted pre-RMS).
        return;
    }
    if (t == kernel::Scheduler::current_task()) {
        // On CPU now (running while BLOCKED is the §4.8 F1 shape, not a
        // dispatch freeze): progress, keep the pre-RMS clear.
        return;
    }
    // Not delivered (RMS try_lock skipped after the clear): restore the
    // record with rules 4/5/6 — unless a newer arm already won the slot.
    s582::LockParkLatch &latch = s582::s_lockpark_latch[cpu];
    if (latch.task_id != UINT64_MAX)
        return;
    latch.task_id = wid;
    latch.generation = wgen;
    latch.latch_class = watch.latch_class;
    latch.since_tick = arch::Timer::ticks();
    latch.covered_ticks = watch.covered_ticks;
    latch.warned = watch.warned;
    s582::s582_cover_instant(cpu);
}

void lockpark_service_tick_miss() noexcept {
    const uint64_t cpu = arch::cpu_index() % CONFIG_MAX_CPUS;
    // Double-read stability (§S3.6): proceed only when both reads agree —
    // 64-bit aligned loads are single-copy-atomic on all three arches, so
    // agreement implies no concurrent rewrite mid-read.
    const uint64_t id_a = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].task_id, __ATOMIC_ACQUIRE);
    const uint64_t gen_a = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].generation, __ATOMIC_ACQUIRE);
    const uint64_t gen_b = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].generation, __ATOMIC_ACQUIRE);
    const uint64_t id_b = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].task_id, __ATOMIC_ACQUIRE);
    if (id_a == UINT64_MAX || id_a != id_b || gen_a != gen_b)
        return;
    auto *t = kernel::Scheduler::find_task(id_a);
    // Re-read before any clear: a concurrent arm must never be cleared.
    const uint64_t id_c = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].task_id, __ATOMIC_ACQUIRE);
    const uint64_t gen_c = __atomic_load_n(
        &s582::s_lockpark_latch[cpu].generation, __ATOMIC_ACQUIRE);
    const bool same_record = (id_c == id_a && gen_c == gen_a);
    if (t == nullptr || t->magic != kernel::TaskControlBlock::TCB_MAGIC ||
        t->generation != gen_a) {
        if (same_record) {
            s582::s582_latch_clear(cpu);
            __atomic_fetch_add(&s582::s_lockpark_stale_cleared, 1ULL,
                               __ATOMIC_RELAXED);
        }
        return;
    }
    if (t->state != kernel::TaskState::BLOCKED) {
        if (same_record) {
            s582::s582_latch_clear(cpu);
            __atomic_fetch_add(&s582::s_lockpark_woke_cleared, 1ULL,
                               __ATOMIC_RELAXED);
        }
        return;
    }
    // No delivery from the miss path (rule 3 unreachable): rules 4/5/6.
    s582::s582_cover_instant(cpu);
}

void lockpark_reset_for_test() noexcept {
    for (uint64_t c = 0; c < CONFIG_MAX_CPUS; ++c) {
        s582::s582_latch_clear(c);
        s582::s_lockpark_watch[c].armed = false;
    }
    __atomic_store_n(&s582::s_lockpark_arm_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_covered_max, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_serviced_hook, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_serviced_tick, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_stale_cleared, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_woke_cleared, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_escalated, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_warn_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_rearm_count, (uint64_t)0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s582::s_lockpark_pending, (uint64_t)0,
                     __ATOMIC_RELAXED);
}

} // namespace gs

// Scheduler:: #582 accessors (state owned by gs::s582 above).
void Scheduler::note_arm_under_lock() noexcept {
    gs::note_arm_under_lock();
}

uint64_t Scheduler::apply_defer_count() noexcept {
    return gs::apply_gate_defer_count();
}

uint64_t Scheduler::apply_defer_warn_count() noexcept {
    return gs::apply_gate_warn_count();
}

uint64_t Scheduler::apply_defer_rearm_count() noexcept {
    return gs::apply_gate_rearm_count();
}

uint64_t Scheduler::apply_defer_failclosed_count() noexcept {
    return gs::apply_gate_failclosed_count();
}

uint64_t Scheduler::term_hold_count() noexcept {
    return gs::apply_gate_term_hold_count();
}

uint64_t Scheduler::arm_under_lock_count() noexcept {
    return gs::apply_gate_arm_under_lock_count();
}

uint64_t Scheduler::force_denied_count() noexcept {
    return gs::apply_gate_force_denied_count();
}

uint64_t Scheduler::lockpark_arm_count() noexcept {
    return gs::lockpark_arm_count();
}

uint64_t Scheduler::lockpark_covered_ticks(uint64_t cpu) noexcept {
    return gs::lockpark_covered_ticks(cpu);
}

uint64_t Scheduler::lockpark_covered_ticks_max() noexcept {
    return gs::lockpark_covered_ticks_max();
}

uint64_t Scheduler::lockpark_serviced_hook_count() noexcept {
    return gs::lockpark_serviced_hook_count();
}

uint64_t Scheduler::lockpark_serviced_tick_count() noexcept {
    return gs::lockpark_serviced_tick_count();
}

uint64_t Scheduler::lockpark_stale_cleared_count() noexcept {
    return gs::lockpark_stale_cleared_count();
}

uint64_t Scheduler::lockpark_woke_cleared_count() noexcept {
    return gs::lockpark_woke_cleared_count();
}

uint64_t Scheduler::lockpark_escalated_count() noexcept {
    return gs::lockpark_escalated_count();
}

uint64_t Scheduler::lockpark_warn_count() noexcept {
    return gs::lockpark_warn_count();
}

uint64_t Scheduler::lockpark_rearm_count() noexcept {
    return gs::lockpark_rearm_count();
}

uint64_t Scheduler::lockpark_pending_count() noexcept {
    return gs::lockpark_pending_count();
}

uint64_t Scheduler::lockpark_latch_class(uint64_t cpu) noexcept {
    return gs::lockpark_class_of(cpu);
}

void Scheduler::scheduler_apply_gate_reset_for_test() noexcept {
    gs::apply_gate_reset_for_test();
}

void Scheduler::lockpark_reset_for_test() noexcept {
    gs::lockpark_reset_for_test();
}

} // namespace kernel
