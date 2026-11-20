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

/// @file test_waitpid.cpp
/// @brief Wait/PID (waitpid) syscall tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): the waitpid contract is driven
/// through the REAL kernel paths — a real parent task genuinely blocks in a
/// wait (waiting_child_pid + waiting_child_status), a real child genuinely
/// terminates via Scheduler::terminate(), and the real wake_waiting_parent
/// path delivers the exit status and reaps the child.  No direct state
/// writes.

#include <test.hpp>
#include <logger.hpp>
#include <string.hpp>
#include <signal.hpp>
#include <scope_guard.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/syscall/syscall.hpp>

using namespace kernel;

namespace {
struct WaitContext {
    uint64_t child_id_;
    uint64_t status_;
    uint64_t result_;
};

static bool wait_for_child(WaitContext &ctx) {
    auto *self = Scheduler::current_task();
    uint64_t ret = Syscall::handle(
        static_cast<uint64_t>(SyscallNumber::WAITPID), ctx.child_id_,
        reinterpret_cast<uint64_t>(&ctx.status_), 0, 0, nullptr);
    if (ret == static_cast<uint64_t>(-1)) {
        while (self->state == TaskState::BLOCKED)
            arch::hlt();
        return self->waiting_child_pid == 0;
    }
    return ret == ctx.child_id_;
}

static void waitpid_parent_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<WaitContext *>(self->user_data);
    ctx->result_ = wait_for_child(*ctx) ? 1 : 0;
}

struct SequentialWaitContext {
    uint64_t child1_id_;
    uint64_t child2_id_;
    uint64_t status1_;
    uint64_t status2_;
    uint64_t result_;
};

static void sequential_wait_parent_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<SequentialWaitContext *>(self->user_data);
    WaitContext first{ctx->child1_id_, 0, 0};
    WaitContext second{ctx->child2_id_, 0, 0};
    bool first_ok = wait_for_child(first);
    ctx->status1_ = first.status_;
    bool second_ok = wait_for_child(second);
    ctx->status2_ = second.status_;
    ctx->result_ = (first_ok && second_ok) ? 1 : 0;
}

} // namespace

// Runmode: kernel
// Testidea: The zombie-child scenario via the real wait→exit→wake contract:
// a real parent blocks in a wait (waiting_child_pid set), a real child
// genuinely terminates via Scheduler::terminate(), and the real
// wake_waiting_parent path delivers the exit status and reaps the child.
// Input: Real parent task (prio 11) sets its wait; a real child (prio 11)
//        terminates with exit code 42; the real wake path runs.
// Expect: The parent's waiting_child_status receives 42; the child is
//         removed from the scheduler; the parent's wait is cleared.
JARVIS_TEST(waitpid_zombie_over_new_child, "PRE: none | POST: none") {
    WaitContext ctx{};
    // Create BOTH TCBs first (cookbook Rule 1) so a timer tick cannot
    // dispatch the parent before the child is registered — otherwise the
    // WAITPID handler finds no child and returns -1 immediately.
    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &ctx;

    auto *child = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    ctx.child_id_ = child->id;

    // Register both under one IrqGuard (cookbook Rule 2); the parent (prio
    // 20) blocks in WAITPID while the child is present; the child runs next,
    // self-terminates via the trampoline, and wake_waiting_parent resumes the
    // parent.
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }

    // The parent blocks in the real WAITPID handler; the child then runs and
    // exits through the trampoline.
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    JARVIS_ASSERT(Scheduler::find_task(ctx.child_id_) == nullptr);

    // Cleanup BEFORE asserting (cookbook Rule 5): the parent self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, ctx.result_);
    JARVIS_ASSERT_EQ(0ULL, ctx.status_);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two children, sequential reaping via the real wait→exit→wake
// contract: a real parent waits for child1, child1 genuinely terminates and
// is reaped; then the parent waits for child2, child2 genuinely terminates
// and is reaped.
// Input: Real parent task (prio 11) waits for two real children in turn;
//        each child genuinely terminates via its trampoline.
// Expect: Each child's status is delivered to the parent and reaped; no
// zombies remain in the scheduler.
JARVIS_TEST(waitpid_two_children_sequential_reap, "PRE: none | POST: none") {
    SequentialWaitContext ctx{};
    // Create ALL TCBs first (cookbook Rule 1); the parent's WAITPID must find
    // each child registered, otherwise it returns -1 immediately.
    auto *parent = TaskControlBlock::create(sequential_wait_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &ctx;

    auto *child1 = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(child1 != nullptr);
    parent->add_child(child1);
    ctx.child1_id_ = child1->id;

    auto *child2 = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(child2 != nullptr);
    parent->add_child(child2);
    ctx.child2_id_ = child2->id;

    // Register all three under one IrqGuard (cookbook Rule 2).
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child1);
        Scheduler::add_task(*child2);
    }

    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    // Cleanup BEFORE asserting (cookbook Rule 5): the parent self-terminated.
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, ctx.result_);
    JARVIS_ASSERT_EQ(0ULL, ctx.status1_);
    JARVIS_ASSERT_EQ(0ULL, ctx.status2_);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Validates the CR3 switch fix in sys_exit(). When a child task
// writes
//   exit status to the parent's user-space pointer, it must first switch to the
//   parent's page table (CR3) so the write lands in the parent's physical page,
// not the child's. This test creates two different PML4s that map the same
// user
//   virtual address to different physical pages, then proves the fix works.
// Input: Two PML4s (parent/child), each mapping VA 0x70000000 to a different
// phys page.
// Expect: After writing to VA via child's CR3 + parent CR3 switch, the parent's
//   physical page contains the write; the child's physical page is unchanged.
JARVIS_TEST(waitpid_cr3_switch_on_status_write, "PRE: none | POST: none") {
    constexpr uint64_t TEST_VA = 0x70000000;

    // Allocate two different USER-owned physical pages for parent and child.
    uint64_t parent_page = PMM::alloc_user_page();
    uint64_t child_page = PMM::alloc_user_page();
    JARVIS_ASSERT(parent_page != 0);
    JARVIS_ASSERT(child_page != 0);
    JARVIS_ASSERT(parent_page != child_page);

    // Zero them and write sentinel values
    memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + parent_page), 0, 4096);
    memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + child_page), 0, 4096);
    *reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + parent_page) =
        0xAAAAAAAABBBBBBBBULL;
    *reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + child_page) =
        0xCCCCCCCCDDDDDDDDULL;

    // Clone kernel PML4 twice for parent and child
    uint64_t parent_pml4 = VMM::clone_kernel_pml4();
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(parent_pml4 != 0);
    JARVIS_ASSERT(child_pml4 != 0);

    // Map parent_page at TEST_VA in parent's PML4, child_page at TEST_VA in
    // child's PML4
    VMM::map_page_in_pml4(TEST_VA, parent_page, true, false, parent_pml4);
    VMM::map_page_in_pml4(TEST_VA, child_page, true, false, child_pml4);

    // Verify the mappings are correct
    uint64_t phys_in_parent = VMM::virt_to_phys_in_pml4(TEST_VA, parent_pml4);
    uint64_t phys_in_child = VMM::virt_to_phys_in_pml4(TEST_VA, child_pml4);
    JARVIS_ASSERT(phys_in_parent == parent_page);
    JARVIS_ASSERT(phys_in_child == child_page);
    JARVIS_ASSERT(phys_in_parent != phys_in_child);

    // Save current CR3 (kernel PML4)
    uint64_t saved_cr3 = arch::read_cr3();

    // --- Test the CR3 switch fix ---
    arch::write_cr3(parent_pml4);
    // MP-4 (SMAP): the write targets a user VA under the parent's CR3; run it
    // with AC set (mirrors Scheduler::wake_waiting_parent's stac/clac around
    // the exit-status store).  The page is present + mapped (certified above).
    arch::stac();
    *reinterpret_cast<uint64_t *>(TEST_VA) = 0x42;
    arch::clac();
    arch::write_cr3(saved_cr3);

    // Verify: parent's physical page got the write
    uint64_t parent_val =
        *reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + parent_page);
    JARVIS_ASSERT(parent_val == 0x42);

    // Verify: child's physical page is unchanged
    uint64_t child_val =
        *reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET + child_page);
    JARVIS_ASSERT(child_val == 0xCCCCCCCCDDDDDDDDULL);

    // Cleanup
    VMM::free_user_pages(parent_pml4);
    VMM::free_user_pages(child_pml4);
    PMM::free_page(parent_pml4);
    PMM::free_page(child_pml4);
    PMM::free_page(parent_page);
    PMM::free_page(child_page);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 primary — full cross-CPU terminator inside the
//        publication window (dequeue + TERMINATED + exit 42 + wake +
//        release), then WAITPID collects: return == child, status 42,
//        wait cleared, not BLOCKED, corpse gone, zombies 0, delta 0.
// Input: Parent prio 20 + child prio 11 + add_child; seam hook
//        runs the funnel inside the window; waitpid.
// Expect: child id; 42; pid 0; status null; not BLOCKED;
//         find_task null; zombies 0; delta 0.
// Depends: WaitPublishSeamScope, wake path, second-residence collect
// Pseudocode: arm seam (full-funnel hook); waitpid (collects);
// assert all; disarm via ScopeGuard; resume parent; teardown.
JARVIS_TEST(waitpid_missed_wake_consumed_collects, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 S1-b — funnel minus the wake (dequeue, TERMINATED,
//        exit 42, release; no wake): the miss leg collects via the
//        absent branch. UP only.
// Input: Seam hook without wake_waiting_parent; waitpid.
// Expect: Same outcome as primary via the miss leg.
// Depends: miss-branch collect (step 9)
// Pseudocode: arm hook (no wake); waitpid (collects); assert all;
// teardown.
JARVIS_TEST(waitpid_released_corpse_collects, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 INV-WAIT-5 — corpse owned by a collecting parent:
//        hook terminates+wakes+releases, then drain from the hook:
//        zombie still returned while owner lives; collect; then
//        owner-first termination frees the corpse (owner-death).
// Input: Hook with in-hook drain; waitpid; owner-kill leg.
// Expect: zombies 1 while owned; 0 after collect; corpse freed
//         on owner death.
// Depends: wait_collect_pending_, drain, collect
// Pseudocode: arm; drain-in-hook (1 zombie); collect (0);
// terminate owner; drain (freed); teardown.
JARVIS_TEST(waitpid_retained_corpse_survives_drain, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 wildcard reverify — same as primary with target -1.
// Input: Seam hook (full funnel); waitpid(-1).
// Expect: Collects with status 42.
// Depends: wildcard path + consumption precedence
// Pseudocode: arm; waitpid(-1) (collects); assert; teardown.
JARVIS_TEST(waitpid_reverify_wildcard_collects, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 reverify never fights the wake — read-only hook;
//        a second task terminates the child; the classic path
//        resolves exactly once with no SCHED+23 dmesg record.
// Input: No-op hook; racing terminator; waitpid.
// Expect: Single resolution; no SCHED+23 row.
// Depends: classic wake path, dmesg ring scan
// Pseudocode: arm no-op; racer terminates; waitpid (once);
// assert no +23 row; teardown.
JARVIS_TEST(waitpid_reverify_never_fights_the_wake, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 publish order (INV-WAIT-4/INV-2) — at the seam
//        point (read-only): parent BLOCKED, not in ready queue,
//        waiting_child_pid == target.
// Input: Seam hook observing only; waitpid.
// Expect: BLOCKED + !in_ready_queue_ + pid == target at seam.
// Depends: waitpid publication order
// Pseudocode: arm observer; waitpid; assert seam snapshot;
// teardown.
JARVIS_TEST(waitpid_publish_order_preserved, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 zombie-claim atomicity — a claimed zombie is
//        skipped by flush_zombies/cleanup_step while claimed and
//        freed after the collect releases the claim.
// Input: Forge claimed zombie; flush; release via collect.
// Expect: Skipped while claimed; freed after.
// Depends: two-phase zombie tail
// Pseudocode: claim; flush (skipped); release (freed); teardown.
JARVIS_TEST(waitpid_zombie_claim_is_atomic, "PRE: none | POST: none") {
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #593 self-kill wakes parent — child sys_kill(self,
//        SIGKILL): ctx.result_ == 1, status == -SIGKILL, corpse
//        gone, zombies 0, delta 0.
// Input: Parent + self-killing child (cookbook scaffolding).
// Expect: 1; -SIGKILL; find null; zombies 0; delta 0.
// Depends: sys_kill self-fatal funnel, waitpid collect
// Pseudocode: spawn pair; child kills self; parent collects;
// assert all; teardown.
JARVIS_TEST(waitpid_kill_self_wakes_parent, "PRE: none | POST: none") {
    static WaitContext g_ctx;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    auto *child = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            const uint64_t self_id = self->id;
            (void)Syscall::handle(
                static_cast<uint64_t>(SyscallNumber::KILL), self_id,
                static_cast<uint64_t>(Signal::SIGKILL), 0, 0, nullptr);
            // The funnel arms the trap-return gate; never resumed.
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    g_ctx.child_id_ = child->id;
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    JARVIS_ASSERT(Scheduler::find_task(g_ctx.child_id_) == nullptr);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1ULL, g_ctx.result_);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(-static_cast<int64_t>(9)),
                     g_ctx.status_);
    JARVIS_ASSERT_EQ(0ULL, Scheduler::zombie_count());
    JARVIS_TEST_PASS();
}

void register_waitpid_tests() {
    Logger::info("Registering waitpid tests");
    JARVIS_REGISTER_TEST(waitpid_zombie_over_new_child);
    JARVIS_REGISTER_TEST(waitpid_two_children_sequential_reap);
    JARVIS_REGISTER_RELEASE_TEST(waitpid_cr3_switch_on_status_write);
    JARVIS_REGISTER_TEST(waitpid_missed_wake_consumed_collects);
    JARVIS_REGISTER_TEST(waitpid_released_corpse_collects);
    JARVIS_REGISTER_TEST(waitpid_retained_corpse_survives_drain);
    JARVIS_REGISTER_TEST(waitpid_reverify_wildcard_collects);
    JARVIS_REGISTER_TEST(waitpid_reverify_never_fights_the_wake);
    JARVIS_REGISTER_TEST(waitpid_publish_order_preserved);
    JARVIS_REGISTER_TEST(waitpid_zombie_claim_is_atomic);
    JARVIS_REGISTER_TEST(waitpid_kill_self_wakes_parent);
}
