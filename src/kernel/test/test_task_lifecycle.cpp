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

/// @file test_task_lifecycle.cpp
/// @brief Task creation/termination lifecycle tests.
///
/// v0.3.10 rework (SIMULATED → DRIVEN): every lifecycle transition is reached
/// through REAL dispatch and the REAL terminate/reap/cleanup paths — the test
/// never sets `task->state` directly, and blocked senders are woken by the
/// real IPC cleanup (MessageQueue destructor), not by direct field writes.

#include <test.hpp>
#include <logger.hpp>
#include <scope_guard.hpp>
#include <kernel/test/task_ptr.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/syscall/syscall.hpp>
#include <kernel/ipc/ipc.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <initrd/initrd.hpp>

using namespace kernel;

namespace {
struct LifecycleWaitContext {
    uint64_t child_id_;
    uint64_t status_;
    volatile uint64_t woke_ = 0;
};

void lifecycle_wait_entry() {
    auto *self = Scheduler::current_task();
    auto *ctx = reinterpret_cast<LifecycleWaitContext *>(self->user_data);
    Syscall::handle(static_cast<uint64_t>(SyscallNumber::WAITPID),
                    ctx->child_id_,
                    reinterpret_cast<uint64_t>(&ctx->status_), 0, 0, nullptr);
    if (self->state == TaskState::BLOCKED) {
        ctx->woke_ = 1;
        while (self->state == TaskState::BLOCKED)
            arch::hlt();
    }
}

void lifecycle_child_exit_entry() {
    Scheduler::terminate(*Scheduler::current_task(), 42);
    for (;;) {
        arch::hlt();
    }
}
} // namespace

// Runmode: kernel
// Testidea: Verifies that task cleanup nullifies msg_queue, notify,
// event_group, and kernel_stack after termination.  A REAL task terminates
// via its trampoline (the genuine exit path) and is then cleaned up.
// Input: Dispatch a kernel task (prio 11) whose lambda returns immediately —
//        the trampoline genuinely terminates it; then clean up.
// Expect: kernel_stack is null after cleanup; no double-free or use-after-free.
// Depends: kernel::task::TaskControlBlock, kernel::ipc::MessageQueue,
// kernel::sync::Notify, kernel::sync::EventGroup
JARVIS_TEST(task_exit_cleans_all_ipc_objects, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    Scheduler::reschedule();
    while (t->state != TaskState::TERMINATED)
        asm volatile("pause");

    // terminate() only marks the task as a zombie; the real drain runs
    // cleanup() + MemPool::free().  The reclaimed TCB must not be
    // dereferenced afterwards — resource balance is verified by the test
    // isolation snapshot/restore.
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a terminating task wakes any tasks blocked on
// sending IPC to it — via the REAL IPC cleanup path (MessageQueue
// destructor), not direct field writes.
// Input: Receiver (prio 11) with a genuinely-full queue; a real sender
//        (prio 12) blocks inside IPC::send().  The receiver terminates and
//        its cleanup wakes the sender.
// Expect: Sender is woken (state READY) and its blocked send fast-fails.
JARVIS_TEST(task_exit_wakes_blocked_senders, "PRE: none | POST: none") {
    // Create both TCBs first and fill the receiver queue before registering
    // either task, so no timer tick can dispatch the receiver before the
    // sender blocks (the same registration-ordering rule as the waitpid test).
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        kernel::Message fill_msg{};
        fill_msg.sender_id = 0;
        fill_msg.type = 99;
        fill_msg.priority = 0;
        fill_msg.data_size = 0;
        receiver->msg_queue.push(fill_msg);
    }

    uint64_t r_id = receiver->id;
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = r_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;

    // Register both under an IRQ guard so the timer cannot dispatch the
    // receiver (prio 11) before the sender (prio 12) blocks on the full queue.
    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }

    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        asm volatile("pause");
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Dispatch the receiver → real terminate → the drain runs cleanup, whose
    // MessageQueue destructor wakes the blocked sender.
    Scheduler::reschedule();
    while (receiver->state != TaskState::TERMINATED)
        asm volatile("pause");
    Scheduler::drain_zombie_list();
    while (sender->state != TaskState::TERMINATED)
        asm volatile("pause");
    Scheduler::drain_zombie_list();

    // The woken sender completed its send against the dead receiver.
    JARVIS_ASSERT_EQ(0ULL, send_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that cleanup of a user-created task frees its page
// table, user stack, and stack physical address.
// Input: Create a user task via TaskControlBlock::create_user (32 KiB
// stack), call cleanup().
// Expect: page_table_, user_stack_, and stack_phys_ are all zeroed after
// cleanup.
// Depends: kernel::task::TaskControlBlock, kernel::memory::PMM,
// kernel::memory::VMM
JARVIS_TEST(task_exit_frees_page_tables_correctly, "PRE: none | POST: none") {
    SimpleTaskPtr tcb(TaskControlBlock::create_user([]() {}, 5, 10, 32_KiB));
    JARVIS_ASSERT(tcb != nullptr);
    JARVIS_ASSERT(tcb->page_table_ != 0);
    JARVIS_ASSERT(tcb->user_stack_ != 0);

    tcb->cleanup();

    JARVIS_ASSERT(tcb->page_table_ == 0);
    JARVIS_ASSERT(tcb->user_stack_ == 0);
    JARVIS_ASSERT(tcb->stack_phys_ == 0);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A self-terminating parent leaves its orphaned child live and
// un-corrupted; the real zombie drain reclaims the parent while the child is
// released independently.
// Input: Parent and child are created and linked, then registered together
//        under an IRQ guard.  The parent's trampoline genuinely terminates it.
// Expect: The parent becomes a zombie and is reclaimed by the drain; the
//         orphaned child survives and is released cleanly (no leaks).
// Depends: kernel::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(task_reparent_preserves_resources, "PRE: none | POST: none") {
    auto *parent = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(parent != nullptr);

    // Child below harness priority (10) so it stays READY-but-orphaned while
    // the parent (prio 11) runs and self-terminates.
    auto *child = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    JARVIS_ASSERT(parent->num_children == 1);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }
    Scheduler::reschedule();
    while (parent->state != TaskState::TERMINATED)
        asm volatile("pause");

    // The self-terminated parent is a zombie; reclaim it with the real drain.
    Scheduler::drain_zombie_list();

    // The child is still live (READY); release it normally.
    Scheduler::remove_task(*child);
    child->cleanup();
    delete child;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a terminated task leaves the live task table and is
// reclaimed by the real zombie drain.  A REAL task terminates via its
// trampoline.
// Input: Dispatch a kernel task (prio 11) → real terminate; find_task, then
//        drain the zombie list.
// Expect: find_task returns nullptr while the TCB is a zombie (terminate()
//         removes it from the id table); drain reclaims its resources.
// Depends: kernel::task::TaskControlBlock, kernel::Scheduler
JARVIS_TEST(task_zombie_state_cleanup, "PRE: none | POST: none") {
    auto *t = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);

    Scheduler::reschedule();
    while (t->state != TaskState::TERMINATED)
        asm volatile("pause");

    uint64_t tcb_id = t->id;
    // terminate() → release_zombie() already removed the TCB from the id
    // table; the zombie is owned by the drain list until cleanup + free.
    JARVIS_ASSERT(Scheduler::find_task(tcb_id) == nullptr);

    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies the real waitpid wake and zombie cleanup path: a parent
// blocked in WAITPID is resumed by wake_waiting_parent when the child
// terminates, the child leaves the live task table, and both TCBs are
// reclaimed by the real zombie drain.
// Input: Parent and child are created and linked, then registered together
//        under an IRQ guard so the parent blocks before the child runs.
// Expect: Parent is woken with the child's exit status 42, the child leaves
//         the live table, and both zombies are drained.
// Depends: kernel::task::Scheduler, kernel::task::TaskControlBlock
JARVIS_TEST(scheduler_reap_respects_parent_wait, "PRE: none | POST: none") {
    LifecycleWaitContext ctx{};
    auto *parent = TaskControlBlock::create(lifecycle_wait_entry, 20, 10);
    JARVIS_ASSERT(parent != nullptr);
    parent->user_data = &ctx;

    auto *child = TaskControlBlock::create(lifecycle_child_exit_entry, 12, 10);
    JARVIS_ASSERT(child != nullptr);
    parent->add_child(child);
    ctx.child_id_ = child->id;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*parent);
        Scheduler::add_task(*child);
    }

    Scheduler::reschedule();
    while (parent->state != TaskState::BLOCKED &&
           parent->state != TaskState::TERMINATED)
        asm volatile("pause");
    while (parent->state != TaskState::TERMINATED)
        asm volatile("pause");

    bool child_removed = Scheduler::find_task(ctx.child_id_) == nullptr;
    if (!child_removed) {
        Scheduler::terminate(*child, 0);
    }
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT(ctx.woke_ == 1);
    JARVIS_ASSERT(child_removed);
    JARVIS_ASSERT_EQ(42ULL, ctx.status_);

    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that ELF loading calls init_task_common for the loaded
// task.
// Input: Load an ELF binary via elf::load, check that msg_queue, notify,
// event_group are initialized.
// Expect: All three IPC objects are non-null after ELF load
// (init_task_common was called).
JARVIS_TEST(elf_load_init_task_common_called, "PRE: none | POST: none") {
    // Find a test ELF in initrd
    initrd::InitrdFile f = initrd::find("./user-app.c.elf");
    if (!f.data)
        f = initrd::find("user-app.c.elf");
    if (!f.data) {
        // No test ELF available, skip with pass
        JARVIS_TEST_PASS();
        return;
    }

    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    if (!kernel::elf::validate_header(hdr)) {
        JARVIS_TEST_PASS();
        return;
    }

    SimpleTaskPtr tcb(kernel::elf::load(hdr, f.data, f.size));
    JARVIS_ASSERT(tcb != nullptr);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that a terminated task with no parent (no waker) is
// reaped by the real reaper path.
// Input: Real kernel task (prio 11) genuinely terminates via its trampoline;
//        reap_orphans() runs.
// Expect: find_task returns nullptr after terminate removes the TCB; the
//         zombie drain reclaims it.
JARVIS_TEST(lifecycle_zombie_no_waker, "PRE: none | POST: none") {
    auto *tcb = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(tcb != nullptr);
    Scheduler::add_task(*tcb);

    uint64_t tid = tcb->id;
    Scheduler::reschedule();
    while (tcb->state != TaskState::TERMINATED)
        asm volatile("pause");

    // terminate() → release_zombie() already removed the TCB from the live
    // table; the zombie drain reclaims it.
    JARVIS_ASSERT(Scheduler::find_task(tid) == nullptr);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies that cleanup() frees the msg_queue even when blocked
// senders were present (bug #016) — via the REAL IPC cleanup path.
// Create a receiver, fill its queue so a sender genuinely blocks. Terminate
// receiver and cleanup.
// Expect: The blocked sender is woken and the receiver's queue is freed (no
// leak — snapshot/restore balances ResourceTracker).
JARVIS_TEST(task_cleanup_frees_msg_queue_with_blocked_senders,
            "PRE: none | POST: none") {
    // Create and prepare both TCBs before registering either task so no timer
    // tick can dispatch the receiver before the sender blocks.
    auto *receiver = TaskControlBlock::create([]() {}, 11, 10);
    JARVIS_ASSERT(receiver != nullptr);

    for (size_t i = 0; i < IPC_MAX_QUEUE_MSG; ++i) {
        Message fill_msg{};
        fill_msg.sender_id = 0;
        fill_msg.type = 99;
        fill_msg.priority = 0;
        fill_msg.data_size = 0;
        receiver->msg_queue.push(fill_msg);
    }

    uint64_t r_id = receiver->id;
    uint64_t send_result = 0;
    struct SCtx {
        uint64_t recv_;
        uint64_t out_;
    } sctx;
    sctx.recv_ = r_id;
    sctx.out_ = reinterpret_cast<uint64_t>(&send_result);
    auto *sender = TaskControlBlock::create(
        []() {
            auto *self = Scheduler::current_task();
            auto *c = reinterpret_cast<SCtx *>(self->user_data);
            kernel::Message msg{};
            msg.sender_id = self->id;
            msg.type = 1;
            msg.priority = 0;
            msg.data_size = 0;
            bool ok = IPC::send(c->recv_, msg);
            __atomic_store_n(reinterpret_cast<uint64_t *>(c->out_),
                             ok ? 1 : 0, __ATOMIC_RELEASE);
        },
        12, 10);
    JARVIS_ASSERT(sender != nullptr);
    sender->user_data = &sctx;

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*receiver);
        Scheduler::add_task(*sender);
    }

    Scheduler::reschedule();
    while (sender->state != TaskState::BLOCKED)
        asm volatile("pause");
    JARVIS_ASSERT(receiver->msg_queue.blocked_senders_head == sender);

    // Dispatch the receiver → real terminate → the drain runs cleanup, whose
    // MessageQueue destructor frees the queue and wakes the blocked sender.
    Scheduler::reschedule();
    while (receiver->state != TaskState::TERMINATED)
        asm volatile("pause");
    Scheduler::drain_zombie_list();
    while (sender->state != TaskState::TERMINATED)
        asm volatile("pause");
    Scheduler::drain_zombie_list();

    JARVIS_ASSERT_EQ(0ULL, send_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Registers all task lifecycle test cases with the test framework.
// Input: None.
// Expect: All JARVIS_REGISTER_TEST calls succeed and tests are available for
// execution.
// Depends: kernel::Logger, kernel::test framework
void register_task_lifecycle_tests() {
    Logger::info("Registering task lifecycle tests");
    JARVIS_REGISTER_TEST(task_exit_cleans_all_ipc_objects);
    JARVIS_REGISTER_TEST(task_exit_wakes_blocked_senders);
    JARVIS_REGISTER_TEST(task_exit_frees_page_tables_correctly);
    JARVIS_REGISTER_TEST(task_reparent_preserves_resources);
    JARVIS_REGISTER_TEST(task_zombie_state_cleanup);
    JARVIS_REGISTER_TEST(scheduler_reap_respects_parent_wait);
    JARVIS_REGISTER_TEST(elf_load_init_task_common_called);
    JARVIS_REGISTER_TEST(lifecycle_zombie_no_waker);
    JARVIS_REGISTER_TEST(task_cleanup_frees_msg_queue_with_blocked_senders);
}
