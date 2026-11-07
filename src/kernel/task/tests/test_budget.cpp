#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verify remaining_ticks equals period_ticks at task creation.
// Input: Create tasks with various period values.
// Expect: remaining_ticks == period_ticks for each.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_initial_remaining, "PRE: none | POST: none") {
    auto* t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(10ULL, t->period_ticks);
    JARVIS_ASSERT_EQ(10ULL, t->remaining_ticks);
    t->cleanup();
    delete t;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verify executed_ticks starts at 0 for a new task.
// Input: Create a task and check executed_ticks.
// Expect: executed_ticks == 0.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_executed_ticks_initial, "PRE: none | POST: none") {
    auto* t = TaskControlBlock::create([]() {}, 5, 10);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, t->executed_ticks);
    t->cleanup();
    delete t;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Create a task with period 0 — edge case.
// Input: period_ticks = 0.
// Expect: Task created, remaining_ticks == 0, does not crash.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_zero_period, "PRE: none | POST: none") {
    auto* t = TaskControlBlock::create([]() {}, 5, 0);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(0ULL, t->remaining_ticks);
    JARVIS_ASSERT_EQ(0ULL, t->period_ticks);
    t->cleanup();
    delete t;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Create a task with large period value.
// Input: period_ticks = UINT64_MAX - 1.
// Expect: Task created, remaining_ticks matches.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_large_period, "PRE: none | POST: none") {
    auto* t = TaskControlBlock::create([]() {}, 5, UINT64_MAX - 1);
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT_EQ(UINT64_MAX - 1, t->remaining_ticks);
    t->cleanup();
    delete t;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple tasks with different periods.
// Input: Create three tasks with periods 5, 10, 20.
// Expect: Each task has correct remaining_ticks.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_varying_periods, "PRE: none | POST: none") {
    auto* a = TaskControlBlock::create([]() {}, 5, 5);
    auto* b = TaskControlBlock::create([]() {}, 5, 10);
    auto* c = TaskControlBlock::create([]() {}, 5, 20);
    JARVIS_ASSERT(a != nullptr && b != nullptr && c != nullptr);
    JARVIS_ASSERT_EQ(5ULL, a->remaining_ticks);
    JARVIS_ASSERT_EQ(10ULL, b->remaining_ticks);
    JARVIS_ASSERT_EQ(20ULL, c->remaining_ticks);
    a->cleanup(); delete a;
    b->cleanup(); delete b;
    c->cleanup(); delete c;
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Different priority tasks have period independent of priority.
// Input: Create tasks with same period but different priorities.
// Expect: remaining_ticks depends only on period_ticks.
// Depends: TaskControlBlock::create
JARVIS_TEST(budget_priority_independent, "PRE: none | POST: none") {
    auto* low = TaskControlBlock::create([]() {}, 1, 10);
    auto* high = TaskControlBlock::create([]() {}, 127, 10);
    JARVIS_ASSERT(low != nullptr && high != nullptr);
    JARVIS_ASSERT_EQ(10ULL, low->remaining_ticks);
    JARVIS_ASSERT_EQ(10ULL, high->remaining_ticks);
    low->cleanup(); delete low;
    high->cleanup(); delete high;
    JARVIS_TEST_PASS();
}

void register_budget_tests() {
    Logger::info("Registering budget tests");
    JARVIS_REGISTER_TEST(budget_initial_remaining);
    JARVIS_REGISTER_TEST(budget_executed_ticks_initial);
    JARVIS_REGISTER_TEST(budget_zero_period);
    JARVIS_REGISTER_TEST(budget_large_period);
    JARVIS_REGISTER_TEST(budget_varying_periods);
    JARVIS_REGISTER_TEST(budget_priority_independent);
}
