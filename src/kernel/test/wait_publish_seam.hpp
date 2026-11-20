/// @file kernel/test/wait_publish_seam.hpp
/// @brief #590 wait-publish test seam: deterministic publication-window
///        injection for waitpid missed-wake tests.
///
/// QE-owned surface (branch `testbed`): production owns only the
/// DEBUG-gated call site in sys_waitpid plus the DEBUG-gated
/// definitions in scheduler_lifecycle.cpp.  No production caller ever
/// arms it, and all bodies compile out under release.
///
/// Contract (same contract as the #578 park seam, header-gated
/// variant): debug-only; UP-only (no SMP test may arm it); one armer
/// (first arm wins, losers observe unarmed); the scope is the teardown
/// discipline (dtor clears an unfired hook); a re-entrant wait cannot
/// re-fire (fire is a single-shot exchange).
#pragma once

#include <types.hpp>

namespace kernel {

struct TaskControlBlock;

namespace test {

#if defined(CONFIG_DEBUG)

/// @brief Install the one-shot publish hook (first armer wins; a later
///        arm while one is installed is ignored).  UP-only, test-active
///        only — violations are ignored, never armed silently.
/// @param hook Test hook invoked once at the waitpid publication point.
void arm_wait_publish(void (*hook)(TaskControlBlock &)) noexcept;
/// @brief Fire the installed hook exactly once (single-shot exchange;
///        two concurrent publishers cannot both fire).  No-op when no
///        hook is installed.
/// @param publisher The publishing waiter (passed to the hook).
void fire_wait_publish(TaskControlBlock &publisher) noexcept;
/// @brief RAII arming scope for waitpid window tests.  Ctor arms with
///        the given hook (UP + test-active gated); dtor clears any
///        unfired hook.  Never armed in release (no-op scope).
class WaitPublishSeamScope {
  public:
    explicit WaitPublishSeamScope(void (*hook)(TaskControlBlock &)) noexcept;
    ~WaitPublishSeamScope() noexcept;
    WaitPublishSeamScope(const WaitPublishSeamScope &) = delete;
    WaitPublishSeamScope &operator=(const WaitPublishSeamScope &) = delete;

    /// @brief True when this scope's arm took effect.
    bool armed() const noexcept {
        return armed_;
    }

  private:
    bool armed_ = false;
};

#else // !defined(CONFIG_DEBUG)

/// @brief Release no-ops: zero load, zero indirect call, no hook.
inline void arm_wait_publish(void (*)(TaskControlBlock &)) noexcept {
}
/// @brief Release no-op.
inline void fire_wait_publish(TaskControlBlock &) noexcept {
}
/// @brief Release no-op scope (never armed).
class WaitPublishSeamScope {
  public:
    explicit WaitPublishSeamScope(void (*)(TaskControlBlock &)) noexcept {
    }
    ~WaitPublishSeamScope() noexcept = default;
    WaitPublishSeamScope(const WaitPublishSeamScope &) = delete;
    WaitPublishSeamScope &operator=(const WaitPublishSeamScope &) = delete;
    bool armed() const noexcept {
        return false;
    }
};

#endif // defined(CONFIG_DEBUG)

} // namespace test
} // namespace kernel
