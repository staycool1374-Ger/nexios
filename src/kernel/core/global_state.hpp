#pragma once

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

/// @file global_state.hpp
/// @brief Central home for cross-TU kernel globals — consolidated from the
/// former ad-hoc `extern` symbols scattered across TUs.  Every variable lives
/// in exactly one owning state group (BootState / FaultState / TestState /
/// AsmSwitchState / NetState / VfsState) and is accessed through documented
/// getters and verified setters.
///
/// Why one file: maintainability.  A single owner per global makes it possible
/// to (a) grep who reads/writes it, (b) attach a per-write verification rule,
/// and (c) guarantee nobody reaches around the accessor.  Grouping follows the
/// synchronization model each variable needs:
///   - BootState      : written once in higherhalf_entry, then immutable
///                      (no locking required; const-correct getters).
///   - FaultState     : fault-atomic single-writer (SMAP recovery, canary latch).
///   - TestState      : single-threaded test harness state (no locking).
///   - AsmSwitchState : deferred-context-switch globals shared with
///                      isr_stubs.asm — exact extern "C" symbols, atomics +
///                      publish-generation protocol.
///   - VfsState       : cross-module FAT32 partition pointer, RANGE_CHECKED.
///   - NetState       : NIC instance pointer, RANGE_CHECKED.

#include <types.hpp>
#include <kernel/boot/bootinfo.hpp>
#include <kernel/syscall/syscall.hpp>

// The Filesystem singletons (initrd_fs/dev_fs/proc_fs/tmpfs_fs/fat32_fs) are
// module-owned objects defined with file-static ops tables in their own
// modules — they remain there (correct encapsulation).  This header adds
// verified access only to the cross-module FAT32 partition pointer.
namespace kernel {
namespace fat32 {
struct Fat32Partition;
} // namespace fat32
} // namespace kernel

// The NIC abstraction lives in the GLOBAL `net` namespace (net.hpp uses
// `namespace net { struct Nic {...}; }`; virtio_net.hpp re-exports it as
// kernel::net::Nic via `using ::net::Nic;`).
namespace net {
struct Nic;
} // namespace net

// H2 ring event codes and recording macro (mirrored from scheduler.cpp for
// cross-TU ISR diagnostic hooks).
inline constexpr uint64_t H2_EV_SKIP     = 3;
inline constexpr uint64_t H2_EV_REENQ    = 8;
#if defined(CONFIG_DEBUG_IPC_SCHED)
#define H2_REC(ev, a, b, c) kernel::debug::h2_record((ev), (a), (b), (c))
#else
#define H2_REC(ev, a, b, c) ((void)0)
#endif

// Boot stack bounds (linker symbols) — C linkage for assembly.
extern "C" {
extern char _stack_start[];
extern char _stack_end[];
}

namespace kernel {
namespace gs {

/// @brief Kernel lifecycle phase.  Drives the BOOT_ONLY write class: a global
/// may only be written while the system is in the matching phase.
enum class StatePhase : uint8_t {
    PRE_BOOT = 0, ///< Before higherhalf_entry (assembly / early boot).
    BOOT = 1,     ///< higherhalf_entry running (single CPU, IRQs off).
    RUNNING = 2,  ///< Post-init, normal operation (daemons + user tasks).
    TEST = 3,     ///< Test harness active (debug builds, selftest classes).
};

/// @brief Write-permission class attached to each registered global.
enum class WriteClass : uint8_t {
    BOOT_ONLY,   ///< Only valid during StatePhase::BOOT.
    IDEMPOTENT,  ///< Setting the same value again is a no-op (e.g. bool→true).
    OWNER_ONLY,  ///< Writer task id must match a fixed owner.
    RANGE_CHECKED, ///< Pointer/value must lie in a legal kernel range.
    NEVER_WRITE, ///< Read-only after construction (Filesystem table).
    PLAIN,       ///< No extra rule (documented hot/fault-atomic paths).
};

/// @brief Call-site context passed to verified setters.
struct WriteContext {
    StatePhase phase;        ///< Current lifecycle phase.
    uint64_t writer_task_id; ///< Calling task id (0 = boot / ISR / none).
};

/// @brief Record a rejected/committed write to the debug audit ring.
void audit_write(const char *name, bool accepted) noexcept;

/// @brief Verify-and-write primitive: apply @p cls rules, then commit @p value
///        into @p target.  Returns false (and skips the store) when the write
///        is illegitimate.  The caller must hold the same exclusion the
///        variable normally uses (the setter documents the required lock).
/// @tparam T        Value type (scalar / pointer / enum).
/// @param name     Debug name for the audit ring.
template <typename T>
bool verify_and_write(T &target, const T &value, WriteClass cls,
                      const WriteContext &ctx, const char *name) noexcept {
    bool accepted = true;
    switch (cls) {
    case WriteClass::IDEMPOTENT:
        // A bool already true (or any same-value write) need not be repeated.
        if (target == value)
            accepted = false;
        break;
    case WriteClass::BOOT_ONLY:
        if (ctx.phase != StatePhase::BOOT)
            accepted = false;
        break;
    case WriteClass::NEVER_WRITE:
        accepted = false;
        break;
    default:
        break;
    }
    if (accepted)
        target = value;
#ifdef CONFIG_DEBUG
    audit_write(name, accepted);
#else
    (void)name;
#endif
    return accepted;
}

// ---------------------------------------------------------------------------
// BootState — immutable after higherhalf_entry
// ---------------------------------------------------------------------------

/// @brief Read the boot information block (memory regions, cmdline, DTB).
BootInfo &boot_info() noexcept;
/// @brief Read-only access to the boot epoch (UNIX seconds at RTC init).
uint64_t get_boot_epoch() noexcept;
/// @brief Set the boot epoch.  BOOT_ONLY: rejected after boot.
bool try_set_boot_epoch(uint64_t epoch, const WriteContext &ctx) noexcept;
/// @brief Multiboot2 magic (0x36D76289 if booted by Multiboot2).
uint64_t get_multiboot_magic() noexcept;
/// @brief Physical pointer to the Multiboot2 info structure.
uint64_t get_multiboot_info_ptr() noexcept;
/// @brief Record multiboot magic + info pointer.  BOOT_ONLY.
bool try_set_multiboot(uint64_t magic, uint64_t info_ptr,
                       const WriteContext &ctx) noexcept;

// ---------------------------------------------------------------------------
// FaultState — fault-atomic single-writer
// ---------------------------------------------------------------------------

/// @brief Reference to the canary-trip latch (MP-3).  Set by the syscall
///        canary verifier; read/reset by memory-safety tests.  Single writer
///        (syscall fault path), fault-atomic — no lock required.
CanaryTrip &canary_trip() noexcept;
/// @brief Set the canary-trip latch fields atomically.
void set_canary_trip(uint64_t task_id, uint8_t segment, uint64_t rip) noexcept;
/// @brief Reset the canary-trip latch (tests only).
void reset_canary_trip() noexcept;
/// @brief SMAP recovery IP (extern "C" symbol kept for the fault handler).
uint64_t &user_access_recover_ip() noexcept;

// ---------------------------------------------------------------------------
// TestState — single-threaded test harness
// ---------------------------------------------------------------------------

/// @brief Name of the currently loaded test class (or nullptr).
const char *get_current_class() noexcept;
/// @brief Set the active test class name (harness only).
void set_current_class(const char *name) noexcept;
/// @brief True when run_filtered skips TF_BENCH tests.
bool get_filter_bench() noexcept;
void set_filter_bench(bool v) noexcept;
/// @brief True when the class auto-shuts down after completion.
bool get_class_auto_shutdown() noexcept;
void set_class_auto_shutdown(bool v) noexcept;
/// @brief True when the VFS was touched (test_isolate marker).
bool get_vfs_touched() noexcept;
void mark_vfs_touched(bool v) noexcept;
/// @brief Timer::ns() snapshot at kernel entry (timing printout).
uint64_t get_kernel_entry_ns() noexcept;
void set_kernel_entry_ns() noexcept;

// ---------------------------------------------------------------------------
// VfsState — cross-module FAT32 partition pointer
// ---------------------------------------------------------------------------

/// @brief Current FAT32 partition instance (nullptr when no FAT32 device is
///        mounted).  Written at boot by the block-driver probe and by tests.
///        RANGE_CHECKED: must be null or a kernel-half address.
kernel::fat32::Fat32Partition *get_fat32_partition() noexcept;
/// @brief Set the FAT32 partition instance.  Rejected unless @p p is null or
///        lies in the kernel half (>= CONFIG_HHDM_OFFSET) — guards against
///        writing a user-space / garbage pointer.
bool try_set_fat32_partition(kernel::fat32::Fat32Partition *p) noexcept;

// ---------------------------------------------------------------------------
// NetState — NIC instance pointer
// ---------------------------------------------------------------------------

/// @brief Current NIC instance (nullptr before net_init / without a device).
///        Written once at boot; read by the shell network commands.
///        RANGE_CHECKED: must be null or a kernel-half address.
::net::Nic *get_nic() noexcept;
/// @brief Set the NIC instance.  Rejected unless null or kernel-half.
bool try_set_nic(::net::Nic *nic) noexcept;

/// @brief Context-switch bridge called from isr_stubs.asm to apply
///        the deferred task switch.  Reads scheduler_next_task_id,
///        updates current_task(), clears the arm, and performs
///        canary/IPC checks.
extern "C" void scheduler_on_context_switch() noexcept;

/// @brief ISR-entry pre-save hook (isr_stubs.asm:217-220).  Fires on every ISR
/// entry, recording the interrupted task's RSP before any context switch.
/// Verifies the interrupted task's RSP lies within its kernel stack.  If the
/// check fails, logs a detailed report (current task, RSP, stack bounds, all
/// other tasks owning that RSP) — evidence for H2.  Runs with IF=0 (interrupt
/// gate); must not re-enable IRQs.
extern "C" void scheduler_diag_pre_save() noexcept;

/// @brief ISR-epilogue depth-skip hook (isr_stubs.asm:133-134).  Fires when a
///        pending deferred-switch apply is skipped because the ISR nesting
/// depth exceeds 2.  Cold path.  Dumps the arm target + the RFLAGS the
/// interrupted task will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_diag_depth_skip() noexcept;

/// @brief ISR-epilogue RSP-owner abort hook (isr_stubs.asm:270-275).  Fires
///        when a pending deferred-switch apply is aborted because the loaded
/// RSP lies outside [scheduler_load_kstack_base, top) — the asm-side
/// stale/foreign-frame guard.  Cold path.  Dumps the rejected RSP, the
/// kstack range, the arm target, and the RFLAGS the interrupted task
/// will return to (H2 IF=0 freeze hypothesis).
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.

/// @brief ISR-epilogue generation-skip hook (isr_stubs.asm): an ISR that
///        captured generation @p captured_gen at entry found it changed before
///        its epilogue, so it skipped applying the deferred switch — leaving
///        the dequeued target stranded until the next tick.  H2 ring event.
/// @note Runs with IF=0 (interrupt gate); must not re-enable IRQs.
extern "C" void scheduler_record_skip([[maybe_unused]] uint64_t captured_gen,
                                      [[maybe_unused]] uint64_t current_gen) noexcept;

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
extern "C" int scheduler_validate_pending_switch() noexcept;

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
extern "C" void scheduler_abort_switch_fixup() noexcept;

/// @brief Context-switch bridge called from isr_stubs.asm to apply
///        the deferred task switch.  Reads scheduler_next_task_id,
///        updates current_task(), clears the arm, and performs
///        canary/IPC checks.
extern "C" void scheduler_on_context_switch() noexcept;

} // namespace gs
} // namespace kernel
