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

/// @file scheduler_path.hpp
/// @brief Dispatch-path step registry (issue #347 Slice B).
///
/// One traced segment per dispatch phase; segments are independent
/// scratch records (tests own the reset discipline — production never
/// resets and never validates). Early-return legs (EDF detour, idle
/// fallback, skipped-current drop, bad-frame dispatch guard, self-switch
/// no-ops) are explicit terminal paths: they record nothing and validate
/// against nothing. switch_to_task() is file-static, so its apply
/// segment is exercised by the suite (observe-only) and covered by
/// inspection; reschedule()/next_task() segments are driven by tests.
///
/// The registry lives in its own header (not function-scoped) so the
/// #323 split moves it together with the dispatch code, zero renumbering.

#pragma once

#include <types.hpp>
#include <kernel/core/path_tracer.hpp>

namespace kernel {

/// @brief Dispatch-path steps — one unique bit each (issue #347).
enum class SchedDispatchSteps : uint64_t {
    Arm         = 1ULL << 0, ///< reschedule() published need_resched
    Select      = 1ULL << 1, ///< next_task() found an eligible candidate
    Dequeue     = 1ULL << 2, ///< next_task() dequeued and returns it
    Validate    = 1ULL << 3, ///< switch_to_task entry checks passed
    ChargeStamp = 1ULL << 4, ///< quantum billed + incoming stamped
    OwnerResolve = 1ULL << 5, ///< RSP owner resolved, no self-switch
    FrameGuard  = 1ULL << 6, ///< target iret frame validated
    Publish     = 1ULL << 7, ///< load_* slots published, current re-enqueued
    ArmSwitch   = 1ULL << 8, ///< generation bumped, save_rsp_to published
};

/// @brief reschedule() arm segment: peek + publish only.
inline constexpr uint64_t k_dispatch_arm_expected =
    path_expected(SchedDispatchSteps::Arm);

/// @brief next_task() select segment: eligible peek + dequeue.
inline constexpr uint64_t k_dispatch_select_expected =
    path_expected(SchedDispatchSteps::Select, SchedDispatchSteps::Dequeue);

/// @brief switch_to_task() apply segment: validate through arm.
inline constexpr uint64_t k_dispatch_apply_expected =
    path_expected(SchedDispatchSteps::Validate,
                  SchedDispatchSteps::ChargeStamp,
                  SchedDispatchSteps::OwnerResolve,
                  SchedDispatchSteps::FrameGuard, SchedDispatchSteps::Publish,
                  SchedDispatchSteps::ArmSwitch);

// Nine distinct single bits covering 0..8 — a collision (two steps
// sharing a bit, e.g. after the #323 split moves code) breaks the build.
static_assert((static_cast<uint64_t>(to_underlying(SchedDispatchSteps::Arm)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::Select)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::Dequeue)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::Validate)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::ChargeStamp)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::OwnerResolve)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::FrameGuard)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::Publish)) |
               static_cast<uint64_t>(to_underlying(SchedDispatchSteps::ArmSwitch))) ==
              0x1FFULL);

#if defined(CONFIG_DEBUG)
/// @brief Fault-injection mask (test-only): steps whose bit is set here
///        are skipped by dispatch_step(). Compiled out in release —
///        production can never observe or set it.
extern uint64_t g_test_skip_dispatch_step;
#endif

/// @brief Record one dispatch step (honors the test skip mask in debug).
inline void dispatch_step(uint64_t step_bit) noexcept {
#if defined(CONFIG_DEBUG)
    if ((g_test_skip_dispatch_step & step_bit) != 0) {
        return;
    }
#endif
    path_step(step_bit);
}

/// @brief Record one dispatch step from the registry.
inline void dispatch_step(SchedDispatchSteps step_value) noexcept {
    dispatch_step(static_cast<uint64_t>(to_underlying(step_value)));
}

} // namespace kernel
