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

/// @file path_tracer.hpp
/// @brief XOR path-tracing primitive (issue #347).
///
/// Each hot-path step flips a unique bit in a running per-CPU XOR chain;
/// checkpoints and the path end compare the chain against the expected
/// value, so a skipped or duplicated step is detected at validation.
///
/// Detection power (by construction — read this before writing a path):
/// a MISSING or DUPLICATED step flips the chain; a pure REORDERING of
/// distinct steps does NOT (a^b == b^a). Order violations are therefore
/// only visible where a checkpoint splits the path — each segment is an
/// independent missing/duplicate check. A path that needs order proof
/// must place a checkpoint between the order-sensitive steps.
///
/// Rules: observe-only (never gates production control flow — test
/// validators excepted); own-CPU slot only; one XOR per step (~1 cycle);
/// no allocation; no locks. Preemption between step() and checkpoint()
/// on the same CPU is the caller's contract (functional paths already
/// hold IrqGuard where they step). Cross-CPU handoffs pass checkpoint
/// values by value — never index another CPU's slot.

#pragma once

#include <types.hpp>
#include <kernel/arch/cpu_context.hpp>

namespace kernel {

// Backing store: one chain per CPU (defined in path_tracer.cpp).
extern uint64_t g_path_chain[CONFIG_MAX_CPUS];

/// @brief Own-CPU chain slot (clamped — the tracer must never fault).
[[nodiscard]] inline uint64_t& path_chain_slot() noexcept {
    uint32_t cpu = static_cast<uint32_t>(arch::cpu_index());
    if (cpu >= static_cast<uint32_t>(CONFIG_MAX_CPUS)) {
        cpu = 0;
    }
    return g_path_chain[cpu];
}

/// @brief Start (or restart) a path on this CPU.
inline void path_reset() noexcept {
    path_chain_slot() = 0;
}

/// @brief Record one step: flip its unique bit into the running chain.
inline void path_step(uint64_t step_bit) noexcept {
    path_chain_slot() ^= step_bit;
}

/// @brief Record one step from a scoped-enum registry (issue #348-D1).
template <Enum E>
inline void path_step(E step_value) noexcept {
    path_step(static_cast<uint64_t>(to_underlying(step_value)));
}

/// @brief Check the chain against the expected value (checkpoint or final).
[[nodiscard]] inline bool path_validate(uint64_t expected_chain) noexcept {
    return path_chain_slot() == expected_chain;
}

/// @brief Snapshot the chain for a break/resume handoff (passed by value).
[[nodiscard]] inline uint64_t path_checkpoint() noexcept {
    return path_chain_slot();
}

/// @brief Re-validate after a preemptible window: the chain must equal
///        the handoff checkpoint XOR exactly the resumed segment's bits.
[[nodiscard]] inline bool path_resume_validate(uint64_t checkpoint,
                                               uint64_t expected_suffix) noexcept {
    return path_chain_slot() == (checkpoint ^ expected_suffix);
}

} // namespace kernel
