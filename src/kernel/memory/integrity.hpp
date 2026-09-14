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

#pragma once

#include <types.hpp>

namespace kernel {
namespace integrity {

// NOLINTBEGIN(bugprone-dynamic-static-initializers)
/// @name Linker section boundary markers
/// @brief Magic constants placed at section boundaries by the linker script.
///        Each 8-byte value encodes an ASCII tag (e.g. "TXT_STRT").
extern const uint64_t _m_text_start;
extern const uint64_t _m_text_end;
extern const uint64_t _m_rodata_start;
extern const uint64_t _m_rodata_end;
extern const uint64_t _m_data_start;
extern const uint64_t _m_data_end;
extern const uint64_t _m_bss_start;
extern const uint64_t _m_bss_end;
extern const uint64_t _m_stack_before;
extern const uint64_t _m_stack_after;

extern "C" {
/// @brief Start address of the .text section (linker symbol).
extern uint64_t _text_start[];
/// @brief End address of the .text section (linker symbol).
extern uint64_t _text_end[];
/// @brief Expected CRC-32 value for the .text section (linker symbol).
extern uint64_t _expected_code_crc[];
}
// NOLINTEND(bugprone-dynamic-static-initializers)

/// @brief Verify that all linker section markers contain their expected magic
/// values.
///        Panics on any mismatch with the specific marker name.
void check_section_markers();
/// @brief Reset the CRC computation state for the .text section integrity
/// check.
void reset_crc_state();
/// @brief Process one chunk (4 KiB) of the code CRC scan.
/// @return true when the CRC scan is complete.
bool crc_process_chunk();
/// @brief Idle task main — periodically verifies section markers and runs code
/// CRC.
void idle_task_main();
/// @brief AP idle task main (issue #25 C1) — reclamation + halt only.
void ap_idle_main();

} // namespace integrity
} // namespace kernel