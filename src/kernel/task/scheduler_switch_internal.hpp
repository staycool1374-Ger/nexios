#pragma once

/// @file scheduler_switch_internal.hpp
/// @brief Shared cross-TU declarations for the deferred-switch engine
///        (issue #323).  Pure move support: no logic changes.
///        Single-home declarations for the arm publish/consume path;
///        scheduler.cpp keeps using them (reschedule/RMS/on_tick) while
///        definitions live in scheduler_switch.cpp.

#include <kernel/task/scheduler.hpp>

namespace kernel {

// Deferred-switch arm publisher: resolves the live RSP owner, publishes
// the save/load slot pair, and returns with the caller still running
// (the ISR epilogue applies the switch).  Single definition in
// scheduler_switch.cpp.
void switch_to_task(TaskControlBlock *current, TaskControlBlock &next,
                    sync::SpinLock *held_lock);

// Bills (ns_monotonic() - exec_stamp_ns) into the task's executed-ns
// totals.  Single definition in scheduler_switch.cpp.
void charge_exec(TaskControlBlock &t) noexcept;

// Restore scheduler state after a deferred-switch arm is cleared without
// being applied.  Single definition in scheduler_switch.cpp.
void restore_preempted_current(TaskControlBlock *current,
                               uint64_t armed_target_id) noexcept;

namespace sched_switch {

// Deferred-switch event codes for the H2 ring (see kernel::debug::H2Event).
// Single home for both scheduler.cpp (RMS/dispatch use) and
// scheduler_switch.cpp (arm/apply paths).
inline constexpr uint64_t H2_EV_ARM      = 1;
inline constexpr uint64_t H2_EV_APPLY    = 2;
// H2_EV_SKIP and H2_EV_REENQ are now in global_state.hpp for cross-TU visibility
inline constexpr uint64_t H2_EV_CLR_RMS  = 4;
inline constexpr uint64_t H2_EV_CLR_SET  = 5;
inline constexpr uint64_t H2_EV_CLR_MISC = 6;
inline constexpr uint64_t H2_EV_IDLE_ARM = 7;

// Named constants for the iret-frame layout indices (moved byte-identical
// from scheduler.cpp anonymous namespace, issue #323 switch strand: the
// switch engine is now the sole user; scheduler.cpp no longer references
// them).  Two orderings exist: the "created" frame (rip first) and the
// "isr_common/CPU" frame (ss first); both are validated.
inline constexpr size_t IRET_RIP_IDX = 136 / 8;    ///< RIP (created order).
inline constexpr size_t IRET_CS_IDX = 144 / 8;     ///< CS.
inline constexpr size_t IRET_RFLAGS_IDX = 152 / 8; ///< RFLAGS.
inline constexpr size_t IRET_RSP_IDX = 160 / 8;    ///< RSP (created order).
inline constexpr size_t IRET_SS_IDX = 168 / 8;     ///< SS.

// Read the current stack pointer (moved byte-identical from scheduler.cpp,
// issue #323 switch strand: shared by the switch engine's owner-resolution
// and the remaining scheduler.cpp call sites).  Portable across arches.
inline uint64_t current_sp() noexcept {
#if defined(CONFIG_ARCH_X86_64)
    uint64_t sp{};
    asm volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t sp{};
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
#elif defined(CONFIG_ARCH_RISCV64)
    uint64_t sp{};
    asm volatile("mv %0, sp" : "=r"(sp));
    return sp;
#else
    return 0; // Unknown arch: no SP readable (counted as null, not an error).
#endif
}

}  // namespace sched_switch

}  // namespace kernel
