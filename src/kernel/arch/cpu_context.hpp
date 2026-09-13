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

/// @file cpu_context.hpp
/// @brief Per-CPU execution context (PfA-B, Phase 8 SMP groundwork).

#include <types.hpp>

namespace kernel {

struct TaskControlBlock; // fwd (task.hpp includes arch/task_fwd)

/// @brief Per-CPU execution context.
///
/// PfA-B (PARAMETERISE FROM ABOVE): state that the timer ISR and the
/// context-switch epilogue touch is owned per-CPU instead of being a global
/// every ISR "greps".  Today the system is single-core, so exactly one
/// instance exists; in Phase 8 this becomes an array indexed by CPU id and
/// reached via the per-CPU GS/TPIDR/tp base.
///
/// Synchronisation discipline (design §4.B): each field is either owned by the
/// single CPU that runs its own ISR (plain, with IRQs-off guarantees) or
/// accessed with an explicit __atomic_* operation.  `current` is published via
/// `__atomic_store_n(RELEASE)` and read with `__atomic_load_n(ACQUIRE)`; the
/// RSP-ownership scan stays the authority (INV-1).
struct CpuContext {
    /// @brief The physically-running task (per-CPU cache).  INV-1: the
    ///        RSP-ownership scan in switch_to_task remains authoritative; this
    ///        cache is only published atomically.
    TaskControlBlock *current = nullptr;
    /// @brief Per-CPU tick counter (timer ISR RMW; readers use atomic load).
    uint64_t ticks = 0;
    /// @brief Last tick an actual context switch ran (debug).
    uint64_t last_switch_tick = 0;
#if CONFIG_DEBUG
    /// @brief Throttle counter for [WEDGE] diagnostics (debug).
    uint64_t wedge_emitted = 0;
    /// @brief Reentrancy diagnostic counters for lock/IRQ nesting (debug).
    uint64_t lk0_count = 0;
    const void *last_holder = nullptr;
#endif
};

/// @brief Returns the current CPU's execution context.
///
/// Single-core today: a single static instance.  Phase 8 threads this through
/// the per-CPU GS base (x86_64) / TPIDR (aarch64) / tp (riscv64).
inline CpuContext &current_cpu() {
    static CpuContext cpu{};
    return cpu;
}

} // namespace kernel
