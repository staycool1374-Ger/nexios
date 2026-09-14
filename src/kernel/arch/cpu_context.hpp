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
#include <kernel/nexios_config.h>
#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/percpu.hpp>
#else
namespace arch {
/// @brief Current CPU index — single-core archs are always CPU 0.
inline uint64_t cpu_index() noexcept {
    return 0;
}
} // namespace arch
#endif

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

/// @brief Per-CPU execution context array (issue #25 C1).
/// Namespace-scope (not function-static) so teardown/snapshot paths can
/// scan all CPUs' `current` (spare-any-current, capture). First touch is
/// on the BSP during boot (single-threaded; -fno-threadsafe-statics safe).
inline CpuContext &cpu_ctx(uint64_t cpu) {
    static CpuContext ctx[CONFIG_MAX_CPUS]{};
    return ctx[cpu % CONFIG_MAX_CPUS];
}

/// @brief Returns the current CPU's execution context.
inline CpuContext &current_cpu() {
    return cpu_ctx(arch::cpu_index());
}

#if defined(CONFIG_ARCH_X86_64)
/// @brief Own-CPU ISR nesting depth (isr_stubs.asm increments gs:0x20 on
///        every entry on every CPU; C++ readers must use the OWN slot).
///        Single-core builds resolve to per_cpu[0] (identical to the old
///        global); the x86 linker alias is removed once no C++ reader
///        uses the bare symbol (link error = proof of full migration).
inline uint64_t &isr_nesting_own() {
    return arch::per_cpu[arch::cpu_index()].isr_nesting_depth;
}
#else
extern "C" uint64_t isr_nesting_depth;
/// @brief Own-CPU ISR nesting depth (single-core archs: the plain global).
inline uint64_t &isr_nesting_own() {
    return isr_nesting_depth;
}
#endif

} // namespace kernel
