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

/// @file test_per_cpu.cpp
/// @brief Per-CPU foundation tests (issue #25, Phase A): GS_BASE wiring,
///        frozen slot offsets, BSP identity, nesting-depth live storage.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/irq_guard.hpp>

#if defined(CONFIG_ARCH_X86_64)
#include <kernel/arch/x86_64/hal/percpu.hpp>
#endif

using namespace kernel;

// Runmode: kernel
// Testidea: The PerCpu slot offsets frozen by the SMP design are honored by
//           the struct layout, so asm gs:0xXX accesses hit the right fields.
// Input: offsetof checks against the spec §3.1 table.
// Expect: user_rsp 0x00, kernel_rsp 0x08, cpu_id 0x10, lapic_id 0x18,
//         isr_nesting_depth 0x20, irq_entry_tsc 0x28, fpu_owner 0x30,
//         current_task 0x38; sizeof(PerCpu) == 4096.
// Depends: arch::PerCpu layout (issue #25)
JARVIS_TEST(per_cpu_slot_offsets_frozen, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    arch::PerCpu &pc = arch::per_cpu[0];
    const auto *base = reinterpret_cast<const uint64_t *>(&pc);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.user_rsp) == base + 0);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.kernel_rsp) ==
                  base + 1);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.cpu_id) == base + 2);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.lapic_id) == base + 3);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.isr_nesting_depth) ==
                  base + 4);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.irq_entry_tsc) ==
                  base + 5);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.fpu_owner) ==
                  base + 6);
    JARVIS_ASSERT(reinterpret_cast<const uint64_t *>(&pc.current_task) ==
                  base + 7);
    JARVIS_ASSERT(sizeof(arch::PerCpu) == 4096);
    // Issue #25 C1: the Phase-A linker aliases (isr_nesting_depth /
    // irq_entry_tsc) are REMOVED — C++ uses isr_nesting_own() (own slot),
    // asm uses gs:0x20/gs:0x28 numerically.  Only offsets are asserted
    // (fpu_owner alias remains for #151).
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: On the single-core build the BSP owns per_cpu[0]: cpu_id is 0,
//           per_cpu_current() resolves to &per_cpu[0], and GS_BASE points at
//           it (set by percpu_init_bsp during boot).
// Input: Read arch::cpu_id(), arch::per_cpu_current(), RDMSR(GS_BASE).
// Expect: cpu_id == 0, current == &per_cpu[0], GS_BASE == &per_cpu[0].
// Depends: arch::percpu_init_bsp boot wiring (issue #25)
JARVIS_TEST(per_cpu_bsp_identity, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_ASSERT(arch::cpu_id() == 0);
    JARVIS_ASSERT(arch::per_cpu_current() == &arch::per_cpu[0]);
    uint64_t lo = 0;
    uint64_t hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    uint64_t gs_base = lo | (hi << 32);
    JARVIS_ASSERT(gs_base ==
                  reinterpret_cast<uint64_t>(&arch::per_cpu[0]));
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The gs:0x20 slot is live storage for the nesting depth: a value
//           written through the C++ accessor reads back through a gs:0x20
//           load, proving GS_BASE routes ISR asm at the migrated field.
// Input: Save depth, write a sentinel via per_cpu_current(), load gs:0x20.
// Expect: gs-loaded value equals the sentinel; depth restored afterwards.
// Depends: arch::per_cpu_current, gs:0x20 slot (issue #25)
JARVIS_TEST(per_cpu_nesting_depth_live, "PRE: none | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    constexpr uint64_t k_sentinel = 0xA5A5A5A5A5A5A5A5ULL;
    auto *pc = arch::per_cpu_current();
    uint64_t saved = pc->isr_nesting_depth;
    uint64_t via_gs = 0;
    {
        // Freeze ticks across the sentinel window: a timer ISR would see
        // the sentinel in its depth guard (harmless skip, but avoid it).
        arch::IrqGuard irq_guard{};
        pc->isr_nesting_depth = k_sentinel;
        asm volatile("mov %%gs:0x20, %0" : "=r"(via_gs));
        pc->isr_nesting_depth = saved;
    }
    JARVIS_ASSERT(via_gs == k_sentinel);
    JARVIS_ASSERT(pc->isr_nesting_depth == saved);
#else
    JARVIS_TEST_PASS();
#endif
    JARVIS_TEST_PASS();
}

void register_per_cpu_tests() {
    Logger::info("Registering per-CPU foundation tests");
    JARVIS_REGISTER_TEST(per_cpu_slot_offsets_frozen);
    JARVIS_REGISTER_TEST(per_cpu_bsp_identity);
    JARVIS_REGISTER_TEST(per_cpu_nesting_depth_live);
}
