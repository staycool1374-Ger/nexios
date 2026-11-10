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

/// @file test_sched_slot_safety.cpp
/// @brief Trap-attribution slot-safety regression tests (issue #513 QE
///        handoff, tests 2+3 of 3): pin the M1-closure validator contract
///        (validate_trap_frame / resolve_* / trip disposition) against the
///        landed fix. Test 1 (live stale-slot U-trap through entry asm) is
///        intentionally absent: the entry-asm save path it exercises is
///        not landed, so a live test would fail by design, not by defect.

#include <test.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler_integrity.hpp>
#include <kernel/arch/hal/context_id.hpp>
#include <kernel/task/scheduler_switch_internal.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/test/task_ptr.hpp>

using namespace kernel;
using kernel::sched_verify::SchedTrapOwner;

namespace {

// Strong test override of the weak trap_owner_hook (issue #513): latches
// the trip arguments and takes the release path without halting, so the
// trip test asserts the outcome instead of halting. Only this TU defines
// it (link would fail on duplicates).
uint64_t g_hook_seen = 0;
uint64_t g_hook_expected = 0;
uint64_t g_hook_trapped = 0;
uint64_t g_hook_site = 0;
uint64_t g_hook_ret = 0;

} // namespace

namespace kernel {
namespace sched_verify {

bool trap_owner_hook(uint64_t expected_id, uint64_t trapped_id,
                     uint64_t site) noexcept {
    g_hook_seen = 1;
    g_hook_expected = expected_id;
    g_hook_trapped = trapped_id;
    g_hook_site = site;
    return g_hook_ret != 0;
}

} // namespace sched_verify
} // namespace kernel

// Runmode: kernel
// Testidea: Trap-frame attribution contract (issue #513 test 2): the
// validator must blame the frame owner resolved from live state, never
// the published current. Hostile, victim, harness, and user fixtures
// pin every validate_trap_frame branch reachable from a kernel test.
// Input: synthetic frame addresses (bogus, victim kstack mid, live SP,
// user fixture kstack mid + self-computed root tag), with user
// impersonation for the fast path.
// Expect: bogus -> no owner, foreign, unresolved-consistent; victim ->
// owned by victim, foreign, victim untouched; live SP -> owned by the
// running task, foreign (kernel current); user tag resolves to the
// fixture; impersonated fast path attributes the fixture yet stays
// foreign (hart runs kernel tables in-test).
// Depends: sched_verify validator, create_test_task/create_user
// fixtures (BLOCKED, TaskPtr teardown), set_current_task impersonation
// with restore. The foreign==false positive control is unreachable
// in-test (requires the hart on the fixture's tables); the resolve_*
// unit asserts cover the matching logic instead.
JARVIS_TEST(slot_trap_frame_contract, "PRE: none | POST: none") {
    auto *cur0 = Scheduler::current_task();
    JARVIS_ASSERT(cur0 != nullptr &&
                  cur0->magic == TaskControlBlock::TCB_MAGIC);
    auto *harness = Scheduler::get_harness_task();
    JARVIS_ASSERT(harness != nullptr &&
                  harness->magic == TaskControlBlock::TCB_MAGIC);

    // Hostile address: no owner, foreign, unresolved-consistent.
    {
        SchedTrapOwner own =
            sched_verify::validate_trap_frame(0x1, "qe-bogus");
        JARVIS_ASSERT(own.frame_owner == nullptr);
        JARVIS_ASSERT(own.foreign);
        JARVIS_ASSERT(own.unresolved == (own.trapped == nullptr));
        JARVIS_ASSERT(sched_verify::resolve_stack_owner(0x1) == nullptr);
        JARVIS_ASSERT(sched_verify::resolve_task_by_root(0x1234) ==
                      nullptr);
    }

    // Victim kernel task: owned, foreign (a kernel current can never
    // own a U-trap), victim untouched by the validation.
    TaskPtr victim = create_test_task(5, 10);
    JARVIS_ASSERT(victim.get() != nullptr);
    const uint64_t vbase =
        reinterpret_cast<uint64_t>(victim->kernel_stack);
    const uint64_t vmid =
        vbase + (victim->kernel_stack_top - vbase) / 2;
    {
        SchedTrapOwner own =
            sched_verify::validate_trap_frame(vmid, "qe-victim");
        JARVIS_ASSERT(own.frame_owner == victim.get());
        JARVIS_ASSERT(own.foreign);
    }
    JARVIS_ASSERT(victim->magic == TaskControlBlock::TCB_MAGIC);
    JARVIS_ASSERT(victim->state == TaskState::BLOCKED);
    JARVIS_ASSERT(victim->exit_code == 0);

    // Live SP: the harness runs on its own kstack (not the linker boot
    // stack), so the running task owns the live frame address. Fast
    // path attributes current; kernel current stays foreign by contract.
    {
        const uint64_t sp = sched_switch::current_sp();
        JARVIS_ASSERT(sp != 0);
        JARVIS_ASSERT(!sched_verify::is_boot_stack_rsp(sp));
        JARVIS_ASSERT(sched_verify::resolve_stack_owner(sp) == cur0);
        JARVIS_ASSERT(!cur0->is_user_);
        SchedTrapOwner own =
            sched_verify::validate_trap_frame(sp, "qe-self");
        JARVIS_ASSERT(own.frame_owner == cur0);
        JARVIS_ASSERT(own.foreign);
        JARVIS_ASSERT(own.unresolved == (own.trapped == nullptr));
    }

    // User fixture: self-computed root tag resolves; kstack mid owned.
    TaskControlBlock *raw =
        TaskControlBlock::create_user([]() {}, 10, 10, 32_KiB);
    JARVIS_ASSERT(raw != nullptr);
    JARVIS_ASSERT(raw->is_user_);
    JARVIS_ASSERT(raw->get_page_table() != 0);
    Scheduler::register_task(*raw);
    raw->state = TaskState::BLOCKED;
    TaskPtr user(raw);
    const uint64_t utag = arch::context_id_tag(raw->get_page_table(),
                                               raw->context_id());
    JARVIS_ASSERT(sched_verify::resolve_task_by_root(utag) == raw);
    const uint64_t ubase = reinterpret_cast<uint64_t>(raw->kernel_stack);
    const uint64_t umid =
        ubase + (raw->kernel_stack_top - ubase) / 2;
    JARVIS_ASSERT(sched_verify::resolve_stack_owner(umid) == raw);

    // Impersonated fast path: frame in current's kstack attributes the
    // fixture, yet stays foreign — the hart still runs kernel tables.
    Scheduler::set_current_task(raw);
    {
        SchedTrapOwner own =
            sched_verify::validate_trap_frame(umid, "qe-fast");
        JARVIS_ASSERT(own.frame_owner == raw);
        JARVIS_ASSERT(own.foreign);
        JARVIS_ASSERT(own.unresolved == (own.trapped == nullptr));
    }
    Scheduler::set_current_task(cur0);
    JARVIS_ASSERT(Scheduler::current_task() == cur0);
    JARVIS_ASSERT(raw->magic == TaskControlBlock::TCB_MAGIC);
    JARVIS_ASSERT(raw->state == TaskState::BLOCKED);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Trap-owner trip disposition (issue #513 test 3): with the
// test hook armed, a trip on a live user fixture must PROCEED and repair
// current to the trapped task; a trip with no attributable task must
// PARK with current untouched. The DEBUG halt path is deliberately not
// exercised (it halts the guest by design); the hook is the test seam
// for both builds.
// Input: synthetic SchedTrapOwner values + hook override (latch args).
// Expect: PROCEED + current==fixture + hook saw (harness id, fixture
// id, site); PARK + current==harness for the null-trapped trip.
// Depends: trap_owner_trip/repair, TaskPtr user fixture, current
// save/restore. Release shares the hook path (no halt when hooked).
JARVIS_TEST(slot_trap_trip_hook_repair, "PRE: none | POST: none") {
    auto *cur0 = Scheduler::current_task();
    JARVIS_ASSERT(cur0 != nullptr &&
                  cur0->magic == TaskControlBlock::TCB_MAGIC);
    TaskControlBlock *raw =
        TaskControlBlock::create_user([]() {}, 10, 10, 32_KiB);
    JARVIS_ASSERT(raw != nullptr && raw->is_user_);
    Scheduler::register_task(*raw);
    raw->state = TaskState::BLOCKED;
    TaskPtr user(raw);
    const uint64_t ubase = reinterpret_cast<uint64_t>(raw->kernel_stack);
    const uint64_t umid =
        ubase + (raw->kernel_stack_top - ubase) / 2;
    constexpr uint64_t kSite = 0x513;

    // PROCEED: hook armed, live trapped task -> repair current.
    g_hook_seen = 0;
    g_hook_ret = 1;
    {
        SchedTrapOwner own{raw, raw, true, false};
        sched_verify::TrapOwnerDisposition disp =
            sched_verify::trap_owner_trip(own, 0x1000, 12, 0x1000, umid,
                                          kSite);
        JARVIS_ASSERT(disp == sched_verify::TrapOwnerDisposition::PROCEED);
        JARVIS_ASSERT(g_hook_seen == 1);
        JARVIS_ASSERT(g_hook_expected == cur0->id);
        JARVIS_ASSERT(g_hook_trapped == raw->id);
        JARVIS_ASSERT(g_hook_site == kSite);
        JARVIS_ASSERT(Scheduler::current_task() == raw);
    }
    Scheduler::set_current_task(cur0);
    JARVIS_ASSERT(Scheduler::current_task() == cur0);

    // PARK: nothing attributable -> current untouched.
    g_hook_seen = 0;
    g_hook_ret = 1;
    {
        SchedTrapOwner own{nullptr, raw, true, true};
        sched_verify::TrapOwnerDisposition disp =
            sched_verify::trap_owner_trip(own, 0x2000, 12, 0x2000, umid,
                                          kSite);
        JARVIS_ASSERT(disp == sched_verify::TrapOwnerDisposition::PARK);
        JARVIS_ASSERT(g_hook_seen == 1);
        JARVIS_ASSERT(Scheduler::current_task() == cur0);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all slot-safety tests with the test framework
// Input: None
// Expect: All slot-safety tests are registered (no direct assertion,
// logging only)
// Depends: Test framework
void register_sched_slot_safety_tests() {
    Logger::info("Registering slot-safety tests");
    JARVIS_REGISTER_TEST(slot_trap_frame_contract);
    JARVIS_REGISTER_TEST(slot_trap_trip_hook_repair);
}
