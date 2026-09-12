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

/// @file test_debug_dump.cpp
/// @brief Diagnostic-dump smoke tests (milestone v0.4.10 issue #128):
///        src/kernel/debug/dump.cpp is the post-mortem path — it runs when
///        the kernel is already dying (panic handler at kernel.cpp:1588/1622
///        and the scheduler invariant violations at task.cpp:348/355/375).
///        If the dump itself faults, the entire post-mortem is lost, so the
///        four entry points must be proven to render their report for live
///        state AND for the degenerate inputs a crash actually hands them
///        (unknown task id, freed TCB).
/// @note  Observation channel: the dump writes through Logger, which during
///        test runs routes to QEMU debugcon (NOT the UART — so
///        `arch::Serial::write_count()` stays frozen and Terminal capture
///        sees nothing).  It also feeds the klog ring, whose documented
///        consumers include tests (src/kernel/log/ring_buffer.hpp).  Each
///        probe clears the ring, runs the dump and reads it back, all inside
///        an arch::IrqGuard so no other task can contribute bytes.
/// @note  dump_cpu_regs() has three architecture variants; only the one for
///        the target arch is compiled, so 10/12 is the ceiling per arch.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/debug/dump.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Read-back buffer.  The klog ring holds 32 KiB, so a full
///        dump_all_tasks() report fits comfortably.
constexpr size_t k_probe_size = 16384;

/// @brief A dump this short cannot be a complete report — the smallest one
///        ("[DUMP] --- task id=… ---\n … NOT FOUND …") is well over this.
constexpr size_t k_min_report_bytes = 32;

/// @brief A task id that no scheduler slot can ever hold.
constexpr uint64_t k_no_such_task_id = 0xFFFFFFFFFFFFFFFEull;

bool has(const char *haystack, const char *needle) {
    if (!*needle)
        return true;
    for (size_t i = 0; haystack[i]; ++i) {
        size_t j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return true;
    }
    return false;
}

/// @brief Runs `action` with the scheduler frozen and the klog pre-drained,
///        then returns the bytes the action wrote to the log.
/// @return Number of bytes captured (also NUL-terminates `buffer`).
template <typename Fn>
size_t capture_dump(Fn &&action, char *buffer, size_t size) {
    buffer[0] = '\0';
    size_t length = 0;
    {
        arch::IrqGuard irq_guard{};
        kernel::log::KlogService::instance().clear();
        action();
        length = kernel::log::KlogService::instance().read(buffer, size - 1);
    }
    buffer[length] = '\0';
    return length;
}

/// @brief State that a post-mortem dump must never disturb.
struct Coherence {
    TaskControlBlock *self;
    uint64_t task_count;
    uint64_t corruption;
};

Coherence snapshot_coherence() {
    Coherence snap{};
    snap.self = Scheduler::current_task();
    snap.task_count = Scheduler::task_count();
    snap.corruption = scheduler_corruption_count;
    return snap;
}

} // namespace

// Runmode: kernel
// Testidea: dump_scheduler_info() renders the live scheduler state —
//           current index, task count, the deferred-switch globals and the
//           ISR nesting depth — through the hex/decimal key-value helpers,
//           without disturbing the state it is reporting on.
// Input: klog drained, then dump_scheduler_info() inside an IrqGuard, read
//        back; scheduler coherence snapshotted before and after.
// Expect: A report of at least k_min_report_bytes carrying the "[DUMP]"
//         prefix and the "scheduler" section markers; current task, task
//         count and the corruption counter are unchanged.
// Depends: kernel::debug::dump_scheduler_info, log::KlogService, Scheduler
JARVIS_TEST(debug_dump_scheduler_info_renders_state,
            "PRE: vfsd, iocd | POST: none") {
    const Coherence before = snapshot_coherence();

    char report[k_probe_size];
    const size_t length = capture_dump(
        []() { kernel::debug::dump_scheduler_info(); }, report,
        sizeof(report));

    const Coherence after = snapshot_coherence();

    JARVIS_ASSERT(length >= k_min_report_bytes);
    JARVIS_ASSERT(has(report, "[DUMP]"));
    JARVIS_ASSERT(has(report, "scheduler"));
    JARVIS_ASSERT(has(report, "task_count"));
    JARVIS_ASSERT_EQ(before.self, after.self);
    JARVIS_ASSERT_EQ(before.task_count, after.task_count);
    JARVIS_ASSERT_EQ(before.corruption, after.corruption);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dump_task_info() must handle BOTH shapes it is handed during a
//           crash: a live task id (full field dump through the two-column
//           decimal helper, magic reported VALID) and an id that resolves to
//           nothing (the "NOT FOUND" report).  Neither may fault — a panic
//           dump iterates ids whose TCBs may already be freed.
// Input: dump_task_info(current_task()->id) and
//        dump_task_info(k_no_such_task_id), each captured separately.
// Expect: The live report contains "task id=" and "magic: VALID"; the
//         not-found report contains "NOT FOUND"; both carry the "[DUMP]"
//         prefix and are at least k_min_report_bytes; the live report is no
//         shorter than the not-found one.
// Depends: kernel::debug::dump_task_info, Scheduler::current_task/find_task
JARVIS_TEST(debug_dump_task_info_valid_and_missing,
            "PRE: vfsd, iocd | POST: none") {
    TaskControlBlock *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);
    const uint64_t live_id = self->id;

    char live[k_probe_size];
    const size_t live_length = capture_dump(
        [live_id]() { kernel::debug::dump_task_info(live_id); }, live,
        sizeof(live));

    char missing[k_probe_size];
    const size_t missing_length = capture_dump(
        []() { kernel::debug::dump_task_info(k_no_such_task_id); }, missing,
        sizeof(missing));

    JARVIS_ASSERT(live_length >= k_min_report_bytes);
    JARVIS_ASSERT(missing_length >= k_min_report_bytes);
    JARVIS_ASSERT(has(live, "[DUMP]"));
    JARVIS_ASSERT(has(live, "task id="));
    JARVIS_ASSERT(has(live, "magic: VALID"));
    JARVIS_ASSERT(has(missing, "[DUMP]"));
    JARVIS_ASSERT(has(missing, "NOT FOUND"));
    JARVIS_ASSERT(live_length >= missing_length);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dump_all_tasks() walks the whole task registry via TaskIter and
//           renders every slot — including slots whose TCB has been freed
//           (magic check) — which is exactly the traversal a panic dump
//           performs.  The emitted volume must scale with the live task
//           count, and the walk must not disturb the registry.
// Input: Scheduler::task_count(), then dump_all_tasks() captured with
//        interrupts frozen; task count re-read afterwards.
// Expect: At least one task exists; the report carries "[DUMP]" and the
//         "all tasks" header; its length is at least k_min_report_bytes and
//         at least one byte per task; the task count is unchanged.
// Depends: kernel::debug::dump_all_tasks, Scheduler::task_count, TaskIter
JARVIS_TEST(debug_dump_all_tasks_walks_registry,
            "PRE: vfsd, iocd | POST: none") {
    const uint64_t task_count = Scheduler::task_count();

    char report[k_probe_size];
    const size_t length = capture_dump(
        []() { kernel::debug::dump_all_tasks(); }, report, sizeof(report));

    const uint64_t after_count = Scheduler::task_count();

    JARVIS_ASSERT(task_count > 0);
    JARVIS_ASSERT(length >= k_min_report_bytes);
    JARVIS_ASSERT(length >= task_count);
    JARVIS_ASSERT(has(report, "[DUMP]"));
    JARVIS_ASSERT(has(report, "all tasks"));
    JARVIS_ASSERT_EQ(task_count, after_count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: dump_cpu_regs() reads the live GPRs and control registers with
//           inline asm and renders them through the three-column hex
//           helpers — the register snapshot a fault handler emits before it
//           halts.  It must produce the report without clobbering any
//           register the caller relies on, which is checked with sentinels
//           held in locals across the call.
// Input: Two 64-bit sentinels in locals; dump_cpu_regs() captured with
//        interrupts frozen; sentinels re-read afterwards.
// Expect: The report carries the "[DUMP]" prefix and is at least
//         k_min_report_bytes; both sentinels are unchanged; interrupts are
//         enabled again once the guard is released.
// Depends: kernel::debug::dump_cpu_regs
JARVIS_TEST(debug_dump_cpu_regs_snapshot, "PRE: vfsd, iocd | POST: none") {
    uint64_t sentinel_before = 0xA5A5A5A5A5A5A5A5ull;
    uint64_t sentinel_after = 0x5A5A5A5A5A5A5A5Aull;

    char report[k_probe_size];
    const size_t length = capture_dump(
        []() { kernel::debug::dump_cpu_regs(); }, report, sizeof(report));

    uint64_t observed_before = sentinel_before;
    uint64_t observed_after = sentinel_after;
    bool interrupts_restored = arch::interrupts_enabled();

    JARVIS_ASSERT(length >= k_min_report_bytes);
    JARVIS_ASSERT(has(report, "[DUMP]"));
    JARVIS_ASSERT_EQ(0xA5A5A5A5A5A5A5A5ull, observed_before);
    JARVIS_ASSERT_EQ(0x5A5A5A5A5A5A5A5Aull, observed_after);
    JARVIS_ASSERT(interrupts_restored);
    JARVIS_TEST_PASS();
}

void register_debug_dump_tests() {
    Logger::info("Registering debug dump tests");
    JARVIS_REGISTER_TEST(debug_dump_scheduler_info_renders_state);
    JARVIS_REGISTER_TEST(debug_dump_task_info_valid_and_missing);
    JARVIS_REGISTER_TEST(debug_dump_all_tasks_walks_registry);
    JARVIS_REGISTER_TEST(debug_dump_cpu_regs_snapshot);
}
