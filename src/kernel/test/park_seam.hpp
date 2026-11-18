/// @file kernel/test/park_seam.hpp
/// @brief #578 park-test seam: timed-park waiver for depth-0 test coverage.
///
/// The suite runs at irq_guard_depth >= 1 everywhere, so the timed-park
/// path (ParkLockGuard park + reschedule C2 park) is unreachable in-harness
/// and only the denied legs are covered. This seam lets a flagged test
/// (TF_PARK_DEPTH0) waive the harness-phantom depth for marked tasks.
///
/// The seam is a WAIVER, never a depth mutation: no irq_guard_depth[],
/// owner, or save-slot is ever written, so a phantom depth can never leak,
/// saturate, or double-restore. All bodies compile out under release
/// (accessors return false / no-ops); production behavior is unchanged.
#pragma once

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace test {

/// @brief True when the waiver may cover @p tcb: every conjunct must hold.
///        CONFIG_DEBUG + armed on this CPU + suite active + live RUNNING
///        non-idle task + marked member + depth exactly 1 owned by a
///        harness phantom (owner 0 or non-own token) + IF==1 (proves no
///        live guard on this CPU) + no tracked spinlock held + not in a
///        syscall-trap level. Any conjunct failing => fail-closed denial.
bool park_ctx_waivable(const TaskControlBlock &tcb) noexcept;

/// @brief The CPU-local half of the waiver (no task): armed + active +
///        IF==1 + depth==1 + non-own owner + no held lock + not in trap.
///        Used for the OQ3 tripwire: a park under an open window by an
///        UNMARKED task falsifies the per-TCB narrowing claim.
bool park_ctx_window_open() noexcept;

/// @brief True while this CPU's seam flag is armed and the suite is active
///        (what QE asserts: the test opted into the seam). No task checks.
bool park_seam_active() noexcept;

/// @brief True in DEBUG builds (the seam exists); false in release.
bool park_seam_supported() noexcept;

/// @brief RAII arming scope for TF_PARK_DEPTH0 tests. Ctor marks the
///        harness task and arms the own-CPU flag; dtor disarms, unmarks
///        the harness, and clears every task marker (bounded walk).
///        Ctor asserts the arming preconditions; violations fail the test
///        (recorded, never panic — the seam must never wedge a suite).
class ParkSeamScope {
  public:
    ParkSeamScope() noexcept;
    ~ParkSeamScope() noexcept;
    ParkSeamScope(const ParkSeamScope &) = delete;
    ParkSeamScope &operator=(const ParkSeamScope &) = delete;

    /// @brief True when the scope armed successfully (ctor checks passed).
    bool armed() const noexcept {
        return armed_;
    }

  private:
    bool armed_ = false;
};

} // namespace test
} // namespace kernel
