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
    (1ULL << 0) ^ (1ULL << 1) ^ (1ULL << 2) ^
    (1ULL << 3) ^ (1ULL << 4) ^ (1ULL << 5);

static void test_path_tracer_chain_math() {
    path_reset();
    path_step(1ULL << 0);
    path_step(1ULL << 1);
    path_step(1ULL << 2);
    JARVIS_ASSERT(path_validate((1ULL << 0) ^ (1ULL << 1) ^ (1ULL << 2)));
    JARVIS_ASSERT(!path_validate((1ULL << 0) ^ (1ULL << 1)));
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
        k_full_test_path ^ static_cast<uint64_t>(to_underlying(TracerTestSteps::Commit));
    JARVIS_ASSERT(path_validate(without_commit));
    path_reset();
}

static void test_path_tracer_duplicate_step_detected() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Enter);
    JARVIS_ASSERT(!path_validate(static_cast<uint64_t>(to_underlying(TracerTestSteps::Enter))));
    JARVIS_ASSERT(path_validate(0));
    path_reset();
}

static void test_path_tracer_checkpoint_resume() {
    path_reset();
    path_step(TracerTestSteps::Enter);
    path_step(TracerTestSteps::Validate);
    const uint64_t handoff = path_checkpoint();
    // === <Break>: preemptible window, chain untouched ===
    JARVIS_ASSERT(path_resume_validate(handoff, 0));
    path_step(TracerTestSteps::Commit);
    path_step(TracerTestSteps::Publish);
    const uint64_t resumed_bits =
        static_cast<uint64_t>(to_underlying(TracerTestSteps::Commit)) ^
        static_cast<uint64_t>(to_underlying(TracerTestSteps::Publish));
    JARVIS_ASSERT(path_resume_validate(handoff, resumed_bits));
    JARVIS_ASSERT(!path_resume_validate(handoff,
        static_cast<uint64_t>(to_underlying(TracerTestSteps::Commit))));
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
    JARVIS_REGISTER_TEST(path_tracer_checkpoint_resume);
    JARVIS_REGISTER_TEST(path_tracer_scoped_enum_full_path);
    JARVIS_REGISTER_TEST(path_tracer_reset_clears_chain);
}
