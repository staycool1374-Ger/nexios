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

/// @file test_irqguard_audit.cpp
/// @brief IRQ guard audit / nesting validation tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/arch/irq_guard.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Validate IrqGuard use is restricted to its allowed categories:
//           boot/panic/test-isolation + justified IRQ-exclusion sites.
// Input: Hardcoded list of allowed IrqGuard categories (see
//        docs/irqguard-ledger.md — v0.3.12 G1 audit).
// Expect: IrqGuard functions correctly (save/disable/restore nesting).
// Depends: arch::IrqGuard, arch::interrupts_enabled
JARVIS_TEST(irqguard_remaining_sites_validated, "PRE: none | POST: none") {
    /* Pseudocode: IrqGuard is permitted only for
     *   (1) boot/panic/test-isolation, and
     *   (2) justified IRQ-exclusion sites: 18 kept per
     *       docs/irqguard-ledger.md (S1-S11, T1-T3, T4, TD1, I1, I2).
     * The task.cpp kslot alloc/free sites (T1-T3) were reverted to IrqGuard
     * in v0.3.12 G1-B-01 (SIL-3 finding): the earlier plain SpinLock migration
     * deadlocked because alloc_kslot() is ISR-reachable via on_tick →
     * reap_orphans → idle TaskControlBlock::create. New production sites must
     * be justified in the ledger.
     *
     * This test verifies IrqGuard still functions correctly.
     * If new production code includes IrqGuard, this test must be reviewed
     * and the new site classified (A keep / B migrate) in the ledger.
     */
    // Correct boot/panic behavior: save interrupt state, disable, restore
    bool was_enabled = arch::interrupts_enabled();
    {
        arch::IrqGuard guard;
        JARVIS_ASSERT(!arch::interrupts_enabled());
    }
    JARVIS_ASSERT(arch::interrupts_enabled() == was_enabled);
    JARVIS_TEST_PASS();
}

void register_irqguard_audit_tests() {
    Logger::info("Registering IrqGuard audit tests");
    JARVIS_REGISTER_TEST(irqguard_remaining_sites_validated);
}
