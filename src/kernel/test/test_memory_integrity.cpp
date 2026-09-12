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

/// @file test_memory_integrity.cpp
/// @brief Kernel-integrity tests (milestone v0.4.10 issue #127):
///        src/kernel/memory/integrity.cpp (linker section markers + the
///        incremental kernel-text CRC) was 0/4 — never executed by any test
///        class, because it is normally driven only from the idle task.
/// @note  `idle_task_main()` is deliberately NOT called: it is the idle loop
///        and never returns.  The three callable entry points are driven
///        directly instead.
/// @note  The CRC scan is restartable by design (the idle task processes one
///        chunk per wakeup), so a test can own the scan: reset_crc_state()
///        then crc_process_chunk() until it reports completion.  The final
///        chunk compares the computed CRC against the linker-patched
///        expected value and panics on mismatch — which is exactly the
///        guarantee being asserted.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/memory/integrity.hpp>

using namespace kernel;

namespace {

/// @brief Upper bound on CRC chunks.  The kernel .text section is far below
///        8192 * 4 KiB, so a scan that has not finished by then is a defect
///        (the loop is fully bounded — never an unbounded wait).
constexpr int k_max_chunks = 8192;

} // namespace

// Runmode: kernel
// Testidea: The linker-placed section markers around .text/.rodata/.data/.bss
//           and the boot stack must all still hold their magic constants —
//           a wild write that walks past a section boundary is detected here
//           rather than as an unrelated crash much later.
// Input: check_section_markers() with the live linker symbols.
// Expect: Returns without panicking (markers intact).
// Depends: kernel::integrity::check_section_markers, linker markers
JARVIS_TEST(integrity_section_markers_intact,
            "PRE: vfsd, iocd | POST: none") {
    kernel::integrity::check_section_markers();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The kernel-text CRC is computed incrementally, 4 KiB per call,
//           and the final chunk compares it against the value patched into
//           the image at link time.  A test can own the whole scan because
//           the state is restartable, and the scan must terminate in a
//           bounded number of chunks.
// Input: reset_crc_state(), then crc_process_chunk() in a bounded loop until
//        it reports completion.
// Expect: At least one chunk is processed and the scan completes within
//         k_max_chunks iterations — i.e. the running kernel's .text still
//         matches the CRC baked into the image.
// Depends: kernel::integrity::reset_crc_state, crc_process_chunk
JARVIS_TEST(integrity_crc_scan_completes_and_matches,
            "PRE: vfsd, iocd | POST: none") {
    kernel::integrity::reset_crc_state();

    int chunks = 0;
    bool completed = false;
    for (int step = 0; step < k_max_chunks; ++step) {
        ++chunks;
        if (kernel::integrity::crc_process_chunk()) {
            completed = true;
            break;
        }
    }

    JARVIS_ASSERT(completed);
    JARVIS_ASSERT(chunks > 0);
    JARVIS_ASSERT(chunks < k_max_chunks);
    JARVIS_TEST_PASS();
}

void register_memory_integrity_tests() {
    Logger::info("Registering memory integrity tests");
    JARVIS_REGISTER_TEST(integrity_section_markers_intact);
    JARVIS_REGISTER_TEST(integrity_crc_scan_completes_and_matches);
}
