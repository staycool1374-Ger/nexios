/// @file test_o1_scheduler.cpp
/// @brief O(1) scheduler tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/nexios_config.h>
#include <kernel/task/task.hpp>
#include <kernel/task/priority_map.hpp>
#include <kernel/task/task_queue.hpp>
#include <kernel/task/ready_queue_manager.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/test/task_ptr.hpp>
#include <scope_guard.hpp>

using namespace kernel;

// ---------------------------------------------------------------------------
// PriorityMap tests
// ---------------------------------------------------------------------------

JARVIS_TEST(o1_priority_map_set_get, "PRE: none | POST: none") {
    PriorityMap pm;
    JARVIS_ASSERT(pm.empty());
    pm.set(5);
    JARVIS_ASSERT(pm.is_set(5));
    JARVIS_ASSERT_EQ(5ULL, pm.get_highest_priority());
    pm.clear(5);
    JARVIS_ASSERT(!pm.is_set(5));
    JARVIS_ASSERT(pm.empty());
    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_priority_map_multiple, "PRE: none | POST: none") {
    PriorityMap pm;
    pm.set(10);
    pm.set(5);
    pm.set(127);
    pm.set(0);
    JARVIS_ASSERT_EQ(127ULL, pm.get_highest_priority());
    pm.clear(127);
    JARVIS_ASSERT_EQ(10ULL, pm.get_highest_priority());
    pm.clear(10);
    JARVIS_ASSERT_EQ(5ULL, pm.get_highest_priority());
    pm.clear(5);
    JARVIS_ASSERT_EQ(0ULL, pm.get_highest_priority());
    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_priority_map_boundary, "PRE: none | POST: none") {
    PriorityMap pm;
    pm.set(0);
    JARVIS_ASSERT_EQ(0ULL, pm.get_highest_priority());
    pm.clear(0);
    pm.set(63);
    JARVIS_ASSERT_EQ(63ULL, pm.get_highest_priority());
    pm.set(64);
    JARVIS_ASSERT_EQ(64ULL, pm.get_highest_priority());
    pm.set(127);
    JARVIS_ASSERT_EQ(127ULL, pm.get_highest_priority());
    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_priority_map_clear_nonexistent, "PRE: none | POST: none") {
    PriorityMap pm;
    pm.clear(42);
    JARVIS_ASSERT(pm.empty());
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// TaskQueue tests
// ---------------------------------------------------------------------------

JARVIS_TEST(o1_task_queue_single, "PRE: none | POST: none") {
    auto *tcb = TaskControlBlock::create([]() {}, 5, 20);
    JARVIS_ASSERT(tcb != nullptr);
    TaskQueue q;
    JARVIS_ASSERT(q.empty());

    q.push_back(*tcb);
    JARVIS_ASSERT(!q.empty());
    JARVIS_ASSERT_EQ(1ULL, q.count());

    auto *popped = q.pop_front();
    JARVIS_ASSERT(popped == tcb);
    JARVIS_ASSERT(q.empty());

    tcb->state = TaskState::TERMINATED;
    tcb->cleanup();
    delete tcb;
    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_task_queue_multiple, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create([]() {}, 5, 20);
    auto *t2 = TaskControlBlock::create([]() {}, 6, 20);
    auto *t3 = TaskControlBlock::create([]() {}, 7, 20);
    JARVIS_ASSERT(t1 && t2 && t3);

    TaskQueue q;
    q.push_back(*t1);
    q.push_back(*t2);
    q.push_back(*t3);
    JARVIS_ASSERT_EQ(3ULL, q.count());

    JARVIS_ASSERT(q.pop_front() == t1);
    JARVIS_ASSERT(q.pop_front() == t2);
    JARVIS_ASSERT(q.pop_front() == t3);
    JARVIS_ASSERT(q.empty());

    t1->state = TaskState::TERMINATED;
    t1->cleanup();
    delete t1;
    t2->state = TaskState::TERMINATED;
    t2->cleanup();
    delete t2;
    t3->state = TaskState::TERMINATED;
    t3->cleanup();
    delete t3;
    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_task_queue_remove_middle, "PRE: none | POST: none") {
    auto *t1 = TaskControlBlock::create([]() {}, 5, 20);
    auto *t2 = TaskControlBlock::create([]() {}, 6, 20);
    auto *t3 = TaskControlBlock::create([]() {}, 7, 20);
    JARVIS_ASSERT(t1 && t2 && t3);

    TaskQueue q;
    q.push_back(*t1);
    q.push_back(*t2);
    q.push_back(*t3);

    q.remove(*t2);
    JARVIS_ASSERT_EQ(2ULL, q.count());
    JARVIS_ASSERT(q.pop_front() == t1);
    JARVIS_ASSERT(q.pop_front() == t3);
    JARVIS_ASSERT(q.empty());

    t1->state = TaskState::TERMINATED;
    t1->cleanup();
    delete t1;
    t2->state = TaskState::TERMINATED;
    t2->cleanup();
    delete t2;
    t3->state = TaskState::TERMINATED;
    t3->cleanup();
    delete t3;
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// ReadyQueueManager tests
// ---------------------------------------------------------------------------

JARVIS_TEST(o1_ready_queue_single, "PRE: none | POST: none") {
    SimpleTaskPtr tcb(TaskControlBlock::create([]() {}, 5, 20));
    JARVIS_ASSERT(tcb != nullptr);
    ReadyQueueManager rqm;

    rqm.enqueue(*tcb, 5);
    JARVIS_ASSERT(rqm.has_ready());

    auto *popped = rqm.dequeue_highest();
    JARVIS_ASSERT(popped == tcb.get());
    JARVIS_ASSERT(!rqm.has_ready());

    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_ready_queue_highest_priority, "PRE: none | POST: none") {
    SimpleTaskPtr low(TaskControlBlock::create([]() {}, 5, 20));
    SimpleTaskPtr mid(TaskControlBlock::create([]() {}, 10, 20));
    SimpleTaskPtr high(TaskControlBlock::create([]() {}, 15, 20));
    JARVIS_ASSERT(low && mid && high);

    ReadyQueueManager rqm;
    rqm.enqueue(*low, 5);
    rqm.enqueue(*mid, 10);
    rqm.enqueue(*high, 15);

    JARVIS_ASSERT(rqm.dequeue_highest() == high.get());
    JARVIS_ASSERT(rqm.dequeue_highest() == mid.get());
    JARVIS_ASSERT(rqm.dequeue_highest() == low.get());
    JARVIS_ASSERT(!rqm.has_ready());

    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_ready_queue_remove, "PRE: none | POST: none") {
    SimpleTaskPtr t1(TaskControlBlock::create([]() {}, 5, 20));
    SimpleTaskPtr t2(TaskControlBlock::create([]() {}, 10, 20));
    JARVIS_ASSERT(t1 && t2);

    ReadyQueueManager rqm;
    rqm.enqueue(*t1, 5);
    rqm.enqueue(*t2, 10);

    rqm.remove(*t2, 10);
    JARVIS_ASSERT(rqm.dequeue_highest() == t1.get());
    JARVIS_ASSERT(!rqm.has_ready());

    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_ready_queue_128_levels, "PRE: none | POST: none") {
    ReadyQueueManager rqm;
    SimpleTaskPtr tasks[CONFIG_MAX_TASKS];
    uint64_t created = 0;

    for (uint64_t p = 0; p < CONFIG_MAX_TASKS; ++p) {
        tasks[p].reset(TaskControlBlock::create([]() {}, 1, 20));
        if (!tasks[p])
            break;
        rqm.enqueue(*tasks[p], p);
        ++created;
    }
    JARVIS_ASSERT(created > 0);

    for (uint64_t p = created - 1;; --p) {
        auto *t = rqm.dequeue_highest();
        JARVIS_ASSERT(t != nullptr);
        if (p == 0)
            break;
    }
    JARVIS_ASSERT(!rqm.has_ready());

    for (uint64_t p = 0; p < created; ++p) {
        tasks[p]->state = TaskState::TERMINATED;
        tasks[p]->cleanup();
    }
    JARVIS_TEST_PASS();
}

// ---------------------------------------------------------------------------
// Scheduler integration tests (O(1) next_task)
// ---------------------------------------------------------------------------

JARVIS_TEST(o1_scheduler_dequeues_highest, "PRE: none | POST: none") {
    SimpleTaskPtr low(TaskControlBlock::create([]() {}, 5, 20));
    SimpleTaskPtr high(TaskControlBlock::create([]() {}, 15, 20));
    JARVIS_ASSERT(low && high);

    // Register AND select under one IrqGuard (cookbook Rule 2): a timer tick
    // between add_task and next_task would dispatch the prio-15 empty-lambda
    // task (self-terminates), so next_task() then returns idle.
    arch::IrqGuard guard;
    Scheduler::add_task(*low);
    Scheduler::add_task(*high);

    auto *next = Scheduler::next_task();
    // next_task should return the highest priority task (15)
    JARVIS_ASSERT(next != nullptr);
    JARVIS_ASSERT(next->priority == 15);

    JARVIS_TEST_PASS();
}

JARVIS_TEST(o1_scheduler_add_remove_ready_queue, "PRE: none | POST: none") {
    SimpleTaskPtr t1(TaskControlBlock::create([]() {}, 10, 20));
    JARVIS_ASSERT(t1);

    // Register + select atomically: without the guard a tick can dispatch the
    // empty-lambda task between add_task and next_task (returns idle).
    arch::IrqGuard guard;
    Scheduler::add_task(*t1);
    // t1 was enqueued by add_task
    auto *next = Scheduler::next_task();
    JARVIS_ASSERT(next != nullptr);
    // The O(1) queue must never return a task with priority BELOW the
    // freshly enqueued t1 — but a concurrent SYSTEM task (daemons, the
    // background elf-loader at prio 15) may legitimately outrank t1 in the
    // full-suite environment, so equality to 10 is NOT invariant here
    // (issue #4 all-run: next_task() correctly returned prio-15 elf-load).
    if (next->priority < 10ULL) {
        Logger::info("o1_scheduler_add_remove_ready_queue: next_task() returned id=%u prio=%llu name=%s (BELOW expected prio=10)",
                     next->id, next->priority, next->name);
        Logger::info("  t1 id=%u prio=%llu state=%u in_rq=%u magic=0x%llx",
                     t1->id, t1->priority, static_cast<unsigned>(t1->state),
                     t1->in_ready_queue_ ? 1u : 0u, t1->magic);
    }
    JARVIS_ASSERT(next->priority >= 10ULL);

    JARVIS_TEST_PASS();
}

void register_o1_scheduler_tests() {
    Logger::info("Registering O(1) scheduler tests");
    JARVIS_REGISTER_TEST(o1_priority_map_set_get);
    JARVIS_REGISTER_TEST(o1_priority_map_multiple);
    JARVIS_REGISTER_TEST(o1_priority_map_boundary);
    JARVIS_REGISTER_TEST(o1_priority_map_clear_nonexistent);
    JARVIS_REGISTER_TEST(o1_task_queue_single);
    JARVIS_REGISTER_TEST(o1_task_queue_multiple);
    JARVIS_REGISTER_TEST(o1_task_queue_remove_middle);
    JARVIS_REGISTER_TEST(o1_ready_queue_single);
    JARVIS_REGISTER_TEST(o1_ready_queue_highest_priority);
    JARVIS_REGISTER_TEST(o1_ready_queue_remove);
    JARVIS_REGISTER_TEST(o1_ready_queue_128_levels);
    JARVIS_REGISTER_TEST(o1_scheduler_dequeues_highest);
    JARVIS_REGISTER_TEST(o1_scheduler_add_remove_ready_queue);
}