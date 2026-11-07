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

/// @file test_path_tracer.cpp
/// @brief PathTracer core-primer tests (issue #347 Slice A): chain math,
///        missing/duplicate detection, checkpoint/resume, scoped-enum
///        steps. Purely computational — no kernel state touched beyond the
///        tracer's own per-CPU slot, reset before and after every test.

#include <test.hpp>
#include <kernel/core/path_tracer.hpp>

using namespace kernel;

namespace {

/// @brief Scoped-enum step registry (the #348-D1 pattern Slice B–D copy).
enum class TracerTestSteps : uint64_t {
    Enter   = 1ULL << 0,
    Validate = 1ULL << 1,
    Commit  = 1ULL << 2,
    Publish = 1ULL << 3,
    Arm     = 1ULL << 4,
    Retire  = 1ULL << 5,
};

constexpr uint64_t k_full_test_path =
    path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate,
                  TracerTestSteps::Commit, TracerTestSteps::Publish,
                  TracerTestSteps::Arm, TracerTestSteps::Retire);
constexpr uint64_t k_first_three =
    path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate,
                  TracerTestSteps::Commit);

static void test_path_tracer_chain_math() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    path_step(TracerTestSteps::Commit);
    JARVIS_ASSERT(path_validate(k_first_three));
    JARVIS_ASSERT(!path_validate(
        path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate)));
    path_reset();
}

static void test_path_tracer_missing_step_detected() {
    // The missing-e() shape: all steps except Commit are recorded.
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    // Commit skipped.
    path_step(TracerTestSteps::Publish);
    path_step(TracerTestSteps::Arm);
    path_step(TracerTestSteps::Retire);
    JARVIS_ASSERT(!path_validate(k_full_test_path));
    const uint64_t without_commit =
        path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate,
                      TracerTestSteps::Publish, TracerTestSteps::Arm,
                      TracerTestSteps::Retire);
    JARVIS_ASSERT(path_validate(without_commit));
    path_reset();
}

static void test_path_tracer_duplicate_step_detected() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Enter);
    // Two steps do not fold to the single-step value, and not to zero.
    JARVIS_ASSERT(!path_validate(path_expected(TracerTestSteps::Enter)));
    JARVIS_ASSERT(!path_validate(0));
    path_reset();
}

static void test_path_tracer_reorder_detected() {
    // The order-sensitivity contract (#362): a permutation of two distinct
    // steps must NOT validate against the in-order value. Under the old
    // XOR fold these were equal (a^b == b^a).
    const uint64_t in_order =
        path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate);
    const uint64_t swapped =
        path_expected(TracerTestSteps::Validate, TracerTestSteps::Enter);
    JARVIS_ASSERT(in_order != swapped);

    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    JARVIS_ASSERT(path_validate(in_order));
    JARVIS_ASSERT(!path_validate(swapped));
    path_reset();

    // Reordered full path: same step SET, different order → mismatch.
    path_reset();
    path_step(TracerTestSteps::Validate);
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Commit);
    path_step(TracerTestSteps::Publish);
    path_step(TracerTestSteps::Arm);
    path_step(TracerTestSteps::Retire);
    JARVIS_ASSERT(!path_validate(k_full_test_path));
    path_reset();
}

static void test_path_tracer_checkpoint_resume() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    const uint64_t handoff = path_checkpoint();
    // === <Break>: preemptible window, chain untouched ===
    JARVIS_ASSERT(path_resume_validate(handoff));
    path_step(TracerTestSteps::Commit);
    path_step(TracerTestSteps::Publish);
    // After stepping, the chain no longer equals the handoff.
    JARVIS_ASSERT(!path_resume_validate(handoff));
    JARVIS_ASSERT(path_validate(
        path_expected(TracerTestSteps::Enter, TracerTestSteps::Validate,
                      TracerTestSteps::Commit, TracerTestSteps::Publish)));
    path_reset();
}

static void test_path_tracer_scoped_enum_full_path() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    path_step(TracerTestSteps::Commit);
    path_step(TracerTestSteps::Publish);
    path_step(TracerTestSteps::Arm);
    path_step(TracerTestSteps::Retire);
    JARVIS_ASSERT(path_validate(k_full_test_path));
    path_reset();
}

static void test_path_tracer_reset_clears_chain() {
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Arm);
    path_reset();
    JARVIS_ASSERT(path_validate(0));
    JARVIS_ASSERT(!path_validate(k_full_test_path));
}

} // namespace

void register_path_tracer_tests() {
    JARVIS_REGISTER_TEST(path_tracer_chain_math);
    JARVIS_REGISTER_TEST(path_tracer_missing_step_detected);
    JARVIS_REGISTER_TEST(path_tracer_duplicate_step_detected);
    JARVIS_REGISTER_TEST(path_tracer_reorder_detected);
    JARVIS_REGISTER_TEST(path_tracer_checkpoint_resume);
    JARVIS_REGISTER_TEST(path_tracer_scoped_enum_full_path);
    JARVIS_REGISTER_TEST(path_tracer_reset_clears_chain);
}
