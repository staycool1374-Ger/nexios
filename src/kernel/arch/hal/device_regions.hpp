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

/// @file device_regions.hpp
/// @brief Single home for SDF device literals (issue #48).
///        Static QEMU pc/virt platform addresses used by the SDF rows in
///        boot_sdf.cpp and the PD bring-up gates. Values mirror the
///        existing arch drivers (cited per constant); no new hardware
///        discovery — DTB/ECAM paths stay authoritative where they exist.
///        Arch-gated: each constant exists only on its arch.

#include <types.hpp>

namespace arch {
namespace device {

#if defined(CONFIG_ARCH_X86_64)
/// 16550A COM1 (serial.cpp drives arch::COM1; classic PC mapping).
inline constexpr uint16_t COM1_PORT = 0x3F8;
/// i8042 PS/2 (mirrors Keyboard::DATA_PORT/STATUS_PORT, hal/keyboard.hpp).
inline constexpr uint16_t KBD_DATA_PORT = 0x60;
inline constexpr uint16_t KBD_STATUS_PORT = 0x64;
/// CMOS RTC (mirrors rtc.hpp CMOS_INDEX/CMOS_DATA).
inline constexpr uint16_t CMOS_INDEX_PORT = 0x70;
inline constexpr uint16_t CMOS_DATA_PORT = 0x71;
#endif

#if defined(CONFIG_ARCH_AARCH64)
/// PL011 UART in the QEMU virt 2 MiB Device block (boot.S maps
/// VA 0xFFFF800009000000 -> PA 0x9000000; GIC INTID_UART0 = 33).
inline constexpr uint64_t PL011_BASE = 0x09000000ULL;
inline constexpr uint64_t PL011_SIZE = 0x200000ULL;
/// PL031 RTC (rtc.cpp: PL031_BASE 0x09010000; GIC INTID_RTC = 34).
inline constexpr uint64_t PL031_BASE = 0x09010000ULL;
inline constexpr uint64_t PL031_SIZE = 0x1000ULL;
#endif

#if defined(CONFIG_ARCH_RISCV64)
/// virt 16550A UART0 (mirrors riscv64/hal/uart.hpp UART0_BASE).
inline constexpr uint64_t UART0_BASE = 0x10000000ULL;
/// Goldfish RTC (rtc.cpp: DTB reg 0x101000/0x1000; PLIC IRQ_RTC = 11).
inline constexpr uint64_t GOLDFISH_RTC_BASE = 0x101000ULL;
inline constexpr uint64_t GOLDFISH_RTC_SIZE = 0x1000ULL;
#endif

} // namespace device
} // namespace arch
