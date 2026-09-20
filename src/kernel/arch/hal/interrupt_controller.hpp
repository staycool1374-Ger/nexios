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

/// @file interrupt_controller.hpp
/// @brief Interrupt controller abstraction — PIC (x86_64), GIC (AArch64), PLIC
/// (RISC-V).

#pragma once

#include <types.hpp>
#include <constants.hpp>
#include <kernel/arch/hal/io.hpp>
#include <kernel/nexios_config.h>

namespace arch {

/// @cond
#if defined(CONFIG_ARCH_X86_64)
/// @endcond

/// @brief Snapshot of the x86-64 PIC mask registers.
struct IrqState {
    uint16_t pic1_mask; ///< Mask register of PIC #1 (master).
    uint16_t pic2_mask; ///< Mask register of PIC #2 (slave).
};

/// @brief x86-64 PIC (8259A) interrupt controller driver.
class ArchInterruptController {
  public:
    /// @brief Initialise both PICs in cascade mode with vector offsets.
    static inline void init() {
        outb(arch::PIC1_CMD, 0x11);
        outb(arch::PIC2_CMD, 0x11);
        outb(arch::PIC1_DATA, 0x20);
        outb(arch::PIC2_DATA, 0x28);
        outb(arch::PIC1_DATA, 0x04);
        outb(arch::PIC2_DATA, 0x02);
        outb(arch::PIC1_DATA, 0x01);
        outb(arch::PIC2_DATA, 0x01);
        outb(arch::PIC1_DATA, 0x00);
        outb(arch::PIC2_DATA, 0x00);
    }

    /// @brief Send end-of-interrupt to the PIC(s).
    /// @param vector The interrupt vector that was handled.
    static inline void eoi(uint8_t vector) {
        outb(arch::PIC1_CMD, 0x20);
        if (vector >= 40)
            outb(arch::PIC2_CMD, 0x20);
    }

    /// @brief Mask (disable) a specific IRQ line.
    /// @param irq IRQ number (0–15).
    static inline void mask(uint8_t irq) {
        uint16_t port = (irq < 8) ? arch::PIC1_DATA : arch::PIC2_DATA;
        uint8_t bit = 1 << (irq & 7);
        uint8_t m = inb(port) | bit;
        outb(port, m);
    }

    /// @brief Unmask (enable) a specific IRQ line.
    /// @param irq IRQ number (0–15).
    static inline void unmask(uint8_t irq) {
        uint16_t port = (irq < 8) ? arch::PIC1_DATA : arch::PIC2_DATA;
        uint8_t bit = ~(1 << (irq & 7));
        uint8_t m = inb(port) & bit;
        outb(port, m);
    }

    /// @brief Snapshot the current PIC mask state.
    /// @return IrqState containing both PIC masks.
    static inline IrqState snapshot() {
        IrqState s;
        s.pic1_mask = inb(arch::PIC1_DATA);
        s.pic2_mask = inb(arch::PIC2_DATA);
        return s;
    }

    /// @brief Restore a previously saved PIC mask state.
    /// @param state The IrqState to restore.
    static inline void restore(const IrqState &state) {
        outb(arch::PIC1_DATA, state.pic1_mask);
        outb(arch::PIC2_DATA, state.pic2_mask);
    }
};

/// @cond
#elif defined(CONFIG_ARCH_AARCH64)
/// @endcond

/// @brief Snapshot of the AArch64 GIC mask state.
struct IrqState {
    uint64_t gic_mask; ///< GIC interrupt mask.
};

/// @brief AArch64 GIC (Generic Interrupt Controller) driver.
class ArchInterruptController {
  public:
    static void init();
    static void eoi(uint8_t vector);
    static void mask(uint8_t irq);
    static void unmask(uint8_t irq);
    static IrqState snapshot();
    static void restore(const IrqState &state);
};

/// @brief Redistributor wake state (issue #198): true when the GICv3
///        redistributor reports Children_Online, or on the GICv2 path
///        (no redistributor exists). Consumed by early_irq_init().
/// @return true when interrupt delivery hardware is usable.
bool gic_redist_ready();

/// @cond
#elif defined(CONFIG_ARCH_RISCV64)
/// @endcond

/// @brief Snapshot of the RISC-V PLIC threshold + first enable word.
struct IrqState {
    uint32_t plic_threshold;  ///< PLIC priority threshold.
    uint32_t plic_enable_first; ///< PLIC enable word for IRQ lines 0–31.
};

/// @brief RISC-V PLIC (Platform-Level Interrupt Controller) driver.
class ArchInterruptController {
  public:
    static void init();
    static void eoi(uint8_t vector);
    static void mask(uint8_t irq);
    static void unmask(uint8_t irq);
    static IrqState snapshot();
    static void restore(const IrqState &state);
};

/// @cond
#else
#error "HAL: no interrupt_controller implementation for this architecture"
#endif
/// @endcond

} // namespace arch
