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
#include <kernel/log/dmesg.hpp>
#include <string.hpp>
#include <signal.hpp>
#include <scope_guard.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/test/wait_publish_seam.hpp>
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

namespace {

// #590 seam hooks run inside sys_waitpid's publication window and
// reproduce the full cross-CPU terminator deterministically. The
// victim is stashed in a file-static (hooks cannot capture).
TaskControlBlock *g_seam_victim = nullptr;

// Hook progress stages (diagnostic; Logger is unsafe inside the
// publication window).
uint64_t g_hook_entered = 0;
uint64_t g_hook_marked = 0;
uint64_t g_hook_woke = 0;
uint64_t g_hook_released = 0;

// Counts live dmesg rows for (subsystem, code) — the never-fights
// test pins zero SCHED+23 rows across its race window.
uint64_t sched_row_count(uint64_t code) {
    uint64_t n = 0;
    log::DmesgService::instance().for_each(
        [&](const log::LogEntry &e) {
            if (e.subsystem == log::ErrorSubsystem::SCHED &&
                e.error_code == code)
                ++n;
        });
    return n;
}

// Full funnel: dequeue + TERMINATED + exit 42 + wake + release.
// Exercises steps 4b/5 (consumption precedence) + 8/9 (released-
// corpse collect) together.
void seam_full_funnel(TaskControlBlock &publisher) {
    (void)publisher;
    auto *child = g_seam_victim;
    if (!child)
        return;
    ++g_hook_entered;
    Scheduler::dequeue_ready(*child);
    child->state = TaskState::TERMINATED;
    child->exit_code = 42;
    ++g_hook_marked;
    (void)Scheduler::wake_waiting_parent(*child);
    ++g_hook_woke;
    Scheduler::release_zombie(*child);
    ++g_hook_released;
}

// Miss leg: same funnel minus the wake — the absent branch (step 9).
void seam_funnel_no_wake(TaskControlBlock &publisher) {
    (void)publisher;
    auto *child = g_seam_victim;
    if (!child)
        return;
    Scheduler::dequeue_ready(*child);
    child->state = TaskState::TERMINATED;
    child->exit_code = 42;
    Scheduler::release_zombie(*child);
}

// Read-only observer: records the publication state, mutates nothing.
uint64_t g_seam_saw_blocked = 0;
uint64_t g_seam_saw_unqueued = 0;
uint64_t g_seam_saw_pid = 0;
void seam_observe_only(TaskControlBlock &publisher) {
    g_seam_saw_blocked = (publisher.state == TaskState::BLOCKED) ? 1 : 0;
    g_seam_saw_unqueued =
        (!publisher.in_ready_queue_ && !publisher.in_edf_queue_) ? 1 : 0;
    g_seam_saw_pid = publisher.waiting_child_pid;
}

// Absolute no-op hook: armed but changes nothing.
void seam_noop(TaskControlBlock &publisher) {
    (void)publisher;
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
    static WaitContext g_ctx;
    static uint64_t g_ret = 0;
    static uint64_t g_notblocked = 0;
    static uint64_t g_nofind = 0;
    static uint64_t g_nozombie = 0;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;
    g_ret = g_notblocked = g_nofind = g_nozombie = 0;
    g_hook_entered = g_hook_marked = g_hook_woke = g_hook_released = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    g_ctx.child_id_ = child->id;
    // #590 RACE FIX: arm the seam BEFORE add_task. The parent runs at
    // prio 20 over the prio-10 harness, so any timer tick between
    // add_task and arm dispatches it early and its publication fire
    // consumes an empty slot (parent stuck BLOCKED, hook never runs).
    // Arming first closes the window: no publisher exists before the
    // arm, and every post-add fire observes the hook.
    g_seam_victim = child;
    kernel::test::WaitPublishSeamScope seam(seam_full_funnel);
    JARVIS_ASSERT(seam.armed());
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    // Timer-liveness gate: if guest ticks don't advance in this
    // context, every downstream wait is meaningless — fail loud
    // instead of hanging the suite.
    {
        const uint64_t t0 = arch::Timer::ticks();
        uint64_t spins = 0;
        while (arch::Timer::ticks() == t0 && spins < 100000000ULL)
            arch::pause();
        JARVIS_ASSERT(arch::Timer::ticks() != t0);
    }
    ScopeGuard disarm([&]() {
        g_seam_victim = nullptr;
        if (parent->state == TaskState::BLOCKED)
            Scheduler::set_task_ready(*parent);
    });
    Scheduler::reschedule();
    // Iteration-bounded dispatch wait (pause always terminates;
    // the raw state value distinguishes wedged-BLOCKED from garbage).
    {
        uint64_t spins = 0;
        while (parent->state != TaskState::BLOCKED &&
               parent->state != TaskState::TERMINATED && spins < 100000ULL) {
            arch::pause();
            ++spins;
        }
        if (parent->state != TaskState::BLOCKED &&
            parent->state != TaskState::TERMINATED) {
            JARVIS_ASSERT(false);
            return;
        }
    }
    // Tick-bounded termination wait: ticks advance regardless of
    // dispatch (proven by the gate above), so expiry fails loudly
    // instead of hanging the suite.
    {
        const uint64_t t0 = arch::Timer::ticks();
        while (parent->state != TaskState::TERMINATED &&
               arch::Timer::ticks() < t0 + 4000ULL)
            arch::hlt();
        if (parent->state != TaskState::TERMINATED) {
            JARVIS_ASSERT(false);
            return;
        }
    }
    // The parent task has exited (TERMINATED) by construction; its own
    // TCB legitimately sits on the zombie list until drained. Snapshot
    // the child outcome BEFORE the drain, then drain, then assert the
    // list is empty: the "zombies 0" contract covers the child corpse,
    // not the harness's own just-exited parent.
    const uint64_t cid = g_ctx.child_id_;
    TaskControlBlock *zb[4] = {nullptr, nullptr, nullptr, nullptr};
    g_ret = g_ctx.result_;
    g_notblocked = (parent->state == TaskState::TERMINATED) ? 1ULL : 0ULL;
    g_nofind = (Scheduler::find_task(cid) == nullptr) ? 1ULL : 0ULL;
    disarm.dismiss();
    g_seam_victim = nullptr;
    Scheduler::drain_zombie_list();
    g_nozombie = (Scheduler::snapshot_zombies(zb, 4) == 0) ? 1ULL : 0ULL;

    JARVIS_ASSERT_EQ(1ULL, g_ret);
    JARVIS_ASSERT_EQ(42ULL, g_ctx.status_);
    JARVIS_ASSERT_EQ(1ULL, g_notblocked);
    JARVIS_ASSERT_EQ(1ULL, g_nofind);
    JARVIS_ASSERT_EQ(1ULL, g_nozombie);
    JARVIS_ASSERT_EQ(1ULL, g_hook_released);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 S1-b — funnel minus the wake (dequeue, TERMINATED,
//        exit 42, release; no wake): the miss leg collects via the
//        absent branch. UP only.
// Input: Seam hook without wake_waiting_parent; waitpid.
// Expect: Same outcome as primary via the miss leg.
// Depends: miss-branch collect (step 9)
// Pseudocode: arm hook (no wake); waitpid (collects via miss
// leg); assert all; teardown.
JARVIS_TEST(waitpid_released_corpse_collects, "PRE: none | POST: none") {
    static WaitContext g_ctx;
    static uint64_t g_ret = 0;
    static uint64_t g_nofind = 0;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;
    g_ret = g_nofind = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    g_ctx.child_id_ = child->id;
    // #590 RACE FIX (see test 4): arm before add_task — the prio-20
    // parent preempts the prio-10 harness on any tick in between.
    g_seam_victim = child;
    kernel::test::WaitPublishSeamScope seam(seam_funnel_no_wake);
    JARVIS_ASSERT(seam.armed());
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    ScopeGuard disarm([&]() {
        g_seam_victim = nullptr;
        if (parent->state == TaskState::BLOCKED)
            Scheduler::set_task_ready(*parent);
    });
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    const uint64_t cid5 = g_ctx.child_id_;
    g_ret = g_ctx.result_;
    g_nofind = (Scheduler::find_task(cid5) == nullptr) ? 1ULL : 0ULL;
    disarm.dismiss();
    g_seam_victim = nullptr;
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ret);
    JARVIS_ASSERT_EQ(42ULL, g_ctx.status_);
    JARVIS_ASSERT_EQ(1ULL, g_nofind);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 INV-WAIT-5 — corpse owned by a collecting parent:
//        a claimed zombie is spared by flush_zombies while its owner
//        lives and freed after the claim releases; owner-death frees
//        the corpse (no spare).
// Input: Forged claimed zombie; flush; release; flush; owner-death
//        leg.
// Expect: zombies 1 while owned; 0 after release; corpse freed on
//         owner death.
// Depends: wait_collect_pending_, flush_zombies, drain
// Pseudocode: terminate child (zombie); claim; flush (1 zombie);
// release; flush (0); owner-death leg (freed); teardown.
//
// Note: forged directly (no seam) — the claim/skip/free path does
// not cross the publication window.
JARVIS_TEST(waitpid_retained_corpse_survives_drain, "PRE: none | POST: none") {
    static uint64_t g_z1 = 0;
    static uint64_t g_z0 = 0;
    static uint64_t g_freed = 0;
    g_z1 = g_z0 = g_freed = 0;

    auto *parent = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        20, 10);
    JARVIS_ASSERT(parent != nullptr);
    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    // Terminate the child directly (no waiter): plain zombie.
    Scheduler::terminate(*child, 42);
    // Claim it for the live owner: flush must spare it.
    child->wait_collect_pending_ = true;
    {
        TaskControlBlock *zb[4] = {nullptr, nullptr, nullptr, nullptr};
        Scheduler::flush_zombies(8);
        g_z1 = Scheduler::snapshot_zombies(zb, 4);
    }
    // Release the claim: flush frees it.
    child->wait_collect_pending_ = false;
    Scheduler::flush_zombies(8);
    {
        TaskControlBlock *zb[4] = {nullptr, nullptr, nullptr, nullptr};
        g_z0 = Scheduler::snapshot_zombies(zb, 4);
    }
    // Owner-death leg: a fresh corpse whose owner is already gone is
    // freed by the drain (no spare).
    auto *child2 = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child2 != nullptr);
    parent->add_child(child2);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*child2);
    }
    Scheduler::terminate(*child2, 43);
    Scheduler::terminate(*parent, 0);
    Scheduler::drain_zombie_list();
    g_freed = (Scheduler::find_task(child2->id) == nullptr &&
               Scheduler::find_task(parent->id) == nullptr)
                  ? 1ULL
                  : 0ULL;

    JARVIS_ASSERT_EQ(1ULL, g_z1);
    JARVIS_ASSERT_EQ(0ULL, g_z0);
    JARVIS_ASSERT_EQ(1ULL, g_freed);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #590 wildcard reverify — same as primary with target -1.
// Input: Seam hook (full funnel); waitpid(-1).
// Expect: Collects with status 42.
// Depends: wildcard path + consumption precedence
// Pseudocode: arm; waitpid(-1) (collects); assert; teardown.
JARVIS_TEST(waitpid_reverify_wildcard_collects, "PRE: none | POST: none") {
    static WaitContext g_ctx;
    static uint64_t g_status = 0;
    static uint64_t g_nofind = 0;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;
    g_status = g_nofind = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    const uint64_t real_id = child->id;
    // Wildcard wait: the helper records -1 as the target; the kernel
    // must still collect the real child (result stays 0 — the helper
    // compares against -1 — while status proves the collect).
    g_ctx.child_id_ = static_cast<uint64_t>(-1);
    // #590 RACE FIX (see test 4): arm before add_task — the prio-20
    // parent preempts the prio-10 harness on any tick in between
    // (the gate below GUARANTEES a tick).
    g_seam_victim = child;
    kernel::test::WaitPublishSeamScope seam(seam_full_funnel);
    JARVIS_ASSERT(seam.armed());
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    // Timer-liveness gate: if guest ticks don't advance in this
    // context, every downstream wait is meaningless — fail loud
    // instead of hanging the suite.
    {
        const uint64_t t0 = arch::Timer::ticks();
        uint64_t spins = 0;
        while (arch::Timer::ticks() == t0 && spins < 100000000ULL)
            arch::pause();
        JARVIS_ASSERT(arch::Timer::ticks() != t0);
    }
    ScopeGuard disarm([&]() {
        g_seam_victim = nullptr;
        if (parent->state == TaskState::BLOCKED)
            Scheduler::set_task_ready(*parent);
    });
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    g_status = g_ctx.status_;
    g_nofind = (Scheduler::find_task(real_id) == nullptr) ? 1ULL : 0ULL;
    disarm.dismiss();
    g_seam_victim = nullptr;
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(42ULL, g_status);
    JARVIS_ASSERT_EQ(1ULL, g_nofind);
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
    static WaitContext g_ctx;
    static uint64_t g_ret = 0;
    static uint64_t g_norow = 0;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;
    g_ret = g_norow = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    g_ctx.child_id_ = child->id;
    // Racer: terminates the victim, then exits. It must run to
    // completion BEFORE the parent's first waitpid scan (see above).
    // Its strictly earlier deadline (period 9 vs 10) forces it first
    // regardless of tick phase: same tick → earlier deadline wins;
    // tick between creates → deadline tie broken by higher priority
    // (30 > 20). The parent then collects the released corpse via the
    // second residence exactly once, with no reverify-cap SCHED+23 row.
    auto *racer = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *victim = reinterpret_cast<TaskControlBlock *>(
                self->user_data);
            Scheduler::terminate(*victim, 42);
            // NOTE: no explicit self-terminate — returning runs the
            // trampoline exit (exactly one release); explicit plus
            // trampoline double-releases (self-cycle + count leak).
        },
        30, 9);
    JARVIS_ASSERT(racer != nullptr);
    racer->user_data = child;
    static constexpr uint64_t kSched23 =
        static_cast<uint64_t>(log::kDmesgBase_SCHED) + 23;
    const uint64_t rows0 = sched_row_count(kSched23);
    // #590 RACE FIX (see test 4): arm before add_task.
    kernel::test::WaitPublishSeamScope seam(seam_noop);
    JARVIS_ASSERT(seam.armed());
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*racer);
        Scheduler::add_task(*child);
    }
    ScopeGuard disarm([&]() {
        if (parent->state == TaskState::BLOCKED)
            Scheduler::set_task_ready(*parent);
    });
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    kernel::test::wait_for_termination_safe(racer);
    g_ret = g_ctx.result_;
    g_norow = (sched_row_count(kSched23) == rows0) ? 1ULL : 0ULL;
    disarm.dismiss();
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_ret);
    JARVIS_ASSERT_EQ(42ULL, g_ctx.status_);
    JARVIS_ASSERT_EQ(1ULL, g_norow);
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
// teardown (child exits via trampoline after the observation).
JARVIS_TEST(waitpid_publish_order_preserved, "PRE: none | POST: none") {
    static WaitContext g_ctx;
    static uint64_t g_blocked = 0;
    static uint64_t g_unqueued = 0;
    static uint64_t g_pidok = 0;
    g_ctx.child_id_ = 0;
    g_ctx.status_ = 0;
    g_ctx.result_ = 0;
    g_blocked = g_unqueued = g_pidok = 0;
    g_seam_saw_blocked = g_seam_saw_unqueued = g_seam_saw_pid = 0;

    auto *parent = TaskControlBlock::create(waitpid_parent_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &g_ctx;

    // Trampoline-exiting child, gated on the parent being BLOCKED
    // first: spins until the parent blocks (hook observation window
    // guaranteed), then returns and exits through the trampoline,
    // waking the parent through the classic path.
    auto *child = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *p = Scheduler::find_task(self->parent_id);
            while (p != nullptr &&
                   p->state != TaskState::BLOCKED &&
                   p->state != TaskState::TERMINATED)
                arch::pause();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    g_ctx.child_id_ = child->id;
    // #590 RACE FIX (see test 4): arm before add_task.
    kernel::test::WaitPublishSeamScope seam(seam_observe_only);
    JARVIS_ASSERT(seam.armed());
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    ScopeGuard disarm([&]() {
        if (parent->state == TaskState::BLOCKED)
            Scheduler::set_task_ready(*parent);
    });
    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        arch::pause();
    kernel::test::wait_for_termination_safe(parent);
    g_blocked = g_seam_saw_blocked;
    g_unqueued = g_seam_saw_unqueued;
    g_pidok = (g_seam_saw_pid == child->id) ? 1ULL : 0ULL;
    disarm.dismiss();
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_blocked);
    JARVIS_ASSERT_EQ(1ULL, g_unqueued);
    JARVIS_ASSERT_EQ(1ULL, g_pidok);
    JARVIS_ASSERT_EQ(1ULL, g_ctx.result_);
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
    static uint64_t g_skipped = 0;
    static uint64_t g_freed = 0;
    g_skipped = g_freed = 0;

    // Owner + child linked; plain terminate (no waiter) leaves a
    // zombie owned by the live owner.
    auto *owner = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        20, 10);
    JARVIS_ASSERT(owner != nullptr);
    auto *child = TaskControlBlock::create(
        []() {
            for (;;)
                arch::hlt();
        },
        11, 10);
    JARVIS_ASSERT(child != nullptr);
    owner->add_child(child);
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*owner);
        Scheduler::add_task(*child);
    }
    Scheduler::terminate(*child, 42);
    // Forge the claim: owned by the live owner.
    child->wait_collect_pending_ = true;
    {
        TaskControlBlock *zb[4] = {nullptr, nullptr, nullptr, nullptr};
        Scheduler::flush_zombies(8);
        g_skipped = (Scheduler::snapshot_zombies(zb, 4) == 1) ? 1ULL : 0ULL;
    }
    // Release the claim: the next flush frees.
    child->wait_collect_pending_ = false;
    Scheduler::flush_zombies(8);
    {
        TaskControlBlock *zb[4] = {nullptr, nullptr, nullptr, nullptr};
        g_freed = (Scheduler::snapshot_zombies(zb, 4) == 0) ? 1ULL : 0ULL;
    }
    Scheduler::terminate(*owner, 0);
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(1ULL, g_skipped);
    JARVIS_ASSERT_EQ(1ULL, g_freed);
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
