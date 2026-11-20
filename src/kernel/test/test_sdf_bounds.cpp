/*
 * NexIOS RTOS — SDF Compose Bounds (M4/F10)
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

/// @file test_sdf_bounds.cpp
/// @brief F10 SDF bound checks (issue #563): sdf::sdf_check_bounds()
///        negatives with specific SdfError returns, plus a positive
///        sweep over all 13 real rows.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/boot_sdf.hpp>
#include <kernel/task/sdf_errors.hpp>
#include <kernel/nexios_config.h>
#include <kernel/test/resource_tracker.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: F10 sdf bounds — the reachable negatives (null specs ->
//        BOUND_EXCEEDED; count max_caps+1 -> ROW_TOO_WIDE, which the
//        check runs first) plus a boundary sweep over all 13 real
//        rows via sdf_role_bounds (max_caps passes, max_caps+1
//        fails).
// Input: Crafted (role, specs, count) tuples + all real rows.
// Expect: BOUND_EXCEEDED / ROW_TOO_WIDE per site; boundary holds
//         on all 13 rows.
// Depends: sdf_check_bounds, sdf_role_bounds, SdfError
//
// Note: OBJECTS_EXCEEDED / CSPACE_EXCEEDED / the two quota
// BOUND_EXCEEDs are unreachable with the baked values: any count
// above object_budget 32 already exceeds max_caps 8 (the
// ROW_TOO_WIDE check runs first), any count above CSLOT 64
// likewise, and the row untyped_quota/death_watches are 0 by
// construction. Same defense-in-depth class as the F4 DEAD/IDLE
// legs. (Was blocked on the boot_sdf testability gap; dev seam
// 1bd83c74d compiles the pure helpers unconditionally. A full
// sdf_row sweep stays ungated-out with the PD tables — the
// boundary sweep here pins the same rows through the lifted
// helpers instead.)
// Pseudocode: null-specs (BOUND_EXCEEDED); count 9 (ROW_TOO_WIDE);
// boundary sweep roles 0..12 (max_caps ok, +1 rejected);
// teardown (none).
JARVIS_TEST(sdf_bounds_reject_over_demand, "PRE: none | POST: none") {
    static uint64_t g_null = 0;
    static uint64_t g_wide = 0;
    static uint64_t g_rows_ok = 0;
    g_null = g_wide = g_rows_ok = 0;

    errors::SdfError err = errors::SDF_ERR_OK;
    // Null specs with nonzero count: BOUND_EXCEEDED.
    const bool null_ok = sdf::sdf_check_bounds(
        sdf::PdRole::SERIAL, nullptr, 1, err);
    g_null = (!null_ok && err == errors::SDF_ERR_BOUND_EXCEEDED) ? 1ULL
                                                                 : 0ULL;
    // Count 9 exceeds max_caps 8: ROW_TOO_WIDE (check runs before the
    // budget/cspace checks, so wider counts land here too).
    sdf::CapSpec wide[9] = {};
    err = errors::SDF_ERR_OK;
    const bool wide_ok = sdf::sdf_check_bounds(
        sdf::PdRole::SERIAL, wide, 9, err);
    g_wide = (!wide_ok && err == errors::SDF_ERR_ROW_TOO_WIDE) ? 1ULL
                                                               : 0ULL;
    // Boundary sweep: every real row admits exactly max_caps and
    // rejects max_caps+1.
    uint64_t ok_rows = 0;
    for (uint64_t r = 0; r < 13; ++r) {
        const sdf::RowBounds row =
            sdf::sdf_role_bounds(static_cast<sdf::PdRole>(r));
        sdf::CapSpec specs[sdf::kMaxCapsPerRole] = {};
        errors::SdfError serr = errors::SDF_ERR_OK;
        const bool at_ok = sdf::sdf_check_bounds(
            static_cast<sdf::PdRole>(r), specs, row.max_caps, serr);
        errors::SdfError serr2 = errors::SDF_ERR_OK;
        const bool over_ok = sdf::sdf_check_bounds(
            static_cast<sdf::PdRole>(r), specs,
            static_cast<uint64_t>(row.max_caps) + 1, serr2);
        if (at_ok && serr == errors::SDF_ERR_OK && !over_ok &&
            serr2 == errors::SDF_ERR_ROW_TOO_WIDE) {
            ++ok_rows;
        }
    }
    g_rows_ok = ok_rows;

    JARVIS_ASSERT_EQ(1ULL, g_null);
    JARVIS_ASSERT_EQ(1ULL, g_wide);
    JARVIS_ASSERT_EQ(13ULL, g_rows_ok);
    JARVIS_TEST_PASS();
}

void register_sdf_bounds_tests() {
    Logger::info("Registering sdf_bounds tests");
    JARVIS_REGISTER_TEST(sdf_bounds_reject_over_demand);
}
