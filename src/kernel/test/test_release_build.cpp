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

/// @file test_release_build.cpp
/// @brief Release-build contract tests (issue #303, Phase 6 module 7).
///
///        The v0.5.x bullets describe a release that does not exist
///        (`-O3`, `-DNDEBUG`, stripped). The ACTUAL contract (Makefile:
///        RELEASE_CXXFLAGS = `-g -O2 -fanalyzer`, same LDFLAGS/objects,
///        build type from CXXFLAGS only) is pinned here instead — one
///        test body, correct in both builds via preprocessor branches,
///        registered TF_RELEASE so it executes in the debug AND the
///        release gates. A silent flag change breaks these tests before
///        it can silently change what ships. `make release` compiling +
///        the release selftest passing are Makefile/CI evidence recorded
///        on the tracker, not kernel asserts.

#include <test.hpp>
#include <logger.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Pin the actual per-build toolchain contract: debug builds
// carry CONFIG_DEBUG; release builds carry -O2 (__OPTIMIZE__), keep -g,
// and define neither CONFIG_DEBUG nor NDEBUG.
// Input: Preprocessor state (compiler + Makefile flags).
// Expect: The branch matching the running build holds; the other branch
// is compiled out. Fails on any silent flag change.
// Depends: Makefile DEBUG/RELEASE_CXXFLAGS (issue #303 module 7 reframe)
JARVIS_TEST(release_build_actual_contract, "PRE: none | POST: none") {
#if defined(CONFIG_DEBUG)
    // Debug gate build (-Og -DCONFIG_DEBUG).
#if defined(__OPTIMIZE__)
    bool opt = true;
#else
    bool opt = false;
#endif
    JARVIS_ASSERT_FMT(opt, "debug build lost optimization flags");
    Logger::info("BUILD contract: debug (CONFIG_DEBUG, __OPTIMIZE__)");
#else
    // Release gate build (-g -O2 -fanalyzer, no -DNDEBUG by design).
#if defined(__OPTIMIZE__)
    bool opt = true;
#else
    bool opt = false;
#endif
#if defined(NDEBUG)
    bool nodebug = true;
#else
    bool nodebug = false;
#endif
    JARVIS_ASSERT_FMT(opt, "release build lost -O2 (__OPTIMIZE__ off)");
    JARVIS_ASSERT_FMT(!nodebug,
                      "release build unexpectedly defines NDEBUG");
    Logger::info("BUILD contract: release (-O2, -g kept, no NDEBUG)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Compiler SSP stays off in release too (the MP-3 software
// canaries are the protection model in both builds; an SSP-enabled
// release would fail the link for lack of __stack_chk_guard).
// Input: Preprocessor state.
// Expect: No SSP macro defined (build breaks here otherwise).
// Depends: Makefile -fno-stack-protector (global, both builds)
JARVIS_TEST(release_build_ssp_off_in_both_builds,
            "PRE: none | POST: none") {
#if defined(__SSP__) || defined(__SSP_STRONG__) || defined(__SSP_ALL__)
    JARVIS_ASSERT_FMT(false, "compiler SSP enabled; MP-3 assumed");
#else
#if defined(CONFIG_DEBUG)
    Logger::info("BUILD contract: SSP off (debug)");
#else
    Logger::info("BUILD contract: SSP off (release)");
#endif
    JARVIS_TEST_PASS();
#endif
}

// Runmode: kernel
// Testidea: Registers all release-build contract tests (TF_RELEASE: they
// run in the debug classes AND in the release gate).
// Input: None
// Expect: All release-build tests registered
// Depends: test framework
void register_release_build_tests() {
    Logger::info("Registering release-build tests");
    JARVIS_REGISTER_TEST_FLAGS(release_build_actual_contract,
                               kernel::test::TF_RELEASE);
    JARVIS_REGISTER_TEST_FLAGS(release_build_ssp_off_in_both_builds,
                               kernel::test::TF_RELEASE);
}
