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
/// @brief Order-sensitive path-tracing primitive (issue #347, #362).
///
/// Each hot-path step folds its unique bit into a running per-CPU chain;
/// checkpoints and the path end compare the chain against the expected
/// value, so a deviation is detected at validation.
///
/// Fold: `chain = chain * K + bit` (K = golden-ratio odd constant). Unlike
/// the original XOR fold (`a^b == b^a`), this is ORDER-SENSITIVE — a
/// permutation of distinct steps changes the value — while still detecting
/// a MISSING or DUPLICATED step. Expected values MUST be computed by the
/// same fold via `path_expected(...)`, never hand-derived, so runtime and
/// validator cannot drift.
///
/// Rules: observe-only (never gates production control flow — test
/// validators excepted); own-CPU slot only; one multiply+add per step
/// (a few cycles); no allocation; no locks. Preemption between step() and
/// checkpoint() on the same CPU is the caller's contract (functional paths
/// already hold IrqGuard where they step). Cross-CPU handoffs pass
/// checkpoint values by value — never index another CPU's slot.

#pragma once

#include <types.hpp>
#include <kernel/arch/cpu_context.hpp>

namespace kernel {

/// @brief Odd golden-ratio constant for the order-sensitive fold.
inline constexpr uint64_t k_path_mix = 0x9E3779B97F4A7C15ULL;

/// @brief One fold step: `chain * K + bit`. Order-sensitive and
///        missing/duplicate-sensitive (K is odd, so `bit != 0` moves the
///        chain and a repeated bit is not idempotent).
[[nodiscard]] constexpr uint64_t path_mix(uint64_t chain,
                                          uint64_t step_bit) noexcept {
    return chain * k_path_mix + step_bit;
}

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

/// @brief Record one step: fold its unique bit into the running chain.
inline void path_step(uint64_t step_bit) noexcept {
    uint64_t &chain = path_chain_slot();
    chain = path_mix(chain, step_bit);
}

/// @brief Record one step from a scoped-enum registry (issue #348-D1).
template <Enum E>
inline void path_step(E step_value) noexcept {
    path_step(static_cast<uint64_t>(to_underlying(step_value)));
}

/// @brief Compute the expected chain for an ordered sequence of steps
///        (constexpr; the SAME fold the runtime uses). Registries express
///        their expected constants through this — never hand-derive.
template <typename... E>
[[nodiscard]] constexpr uint64_t path_expected(E... steps) noexcept {
    uint64_t chain = 0;
    ((chain = path_mix(chain, static_cast<uint64_t>(to_underlying(steps)))),
     ...);
    return chain;
}

/// @brief Check the chain against the expected value (checkpoint or final).
[[nodiscard]] inline bool path_validate(uint64_t expected_chain) noexcept {
    return path_chain_slot() == expected_chain;
}

/// @brief Snapshot the chain for a break/resume handoff (passed by value).
[[nodiscard]] inline uint64_t path_checkpoint() noexcept {
    return path_chain_slot();
}

/// @brief Re-validate after a preemptible window: the chain must be
///        UNCHANGED since the handoff (a step taken inside the window is a
///        deviation). The fold is not invertible, so the caller compares
///        the unchanged chain — a later full-path validate() covers the
///        post-resume steps.
[[nodiscard]] inline bool path_resume_validate(uint64_t handoff) noexcept {
    return path_chain_slot() == handoff;
}

} // namespace kernel
