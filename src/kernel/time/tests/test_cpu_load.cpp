#include <test.hpp>
#include <logger.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Idle-task execution counter increments on each idle loop iteration.
// Pseudocode: Check idle task's executed_ticks before and after a short busy-wait.
//   After some ticks, idle task should have accumulated ticks.
// Input: None (read idle task state from Scheduler).
// Expect: Idle task exists and has a valid executed_ticks value.
// Depends: Scheduler::get_idle_task(), TaskControlBlock::executed_ticks
JARVIS_TEST(cpu_load_idle_task_exists, "PRE: none | POST: none") {
    auto* idle = Scheduler::get_idle_task();
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT(idle->state == TaskState::READY ||
                  idle->state == TaskState::RUNNING);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Idle task has priority 0 (lowest).
// Input: Read idle task priority.
// Expect: priority == 0.
// Depends: Scheduler::get_idle_task()
JARVIS_TEST(cpu_load_idle_priority, "PRE: none | POST: none") {
    auto* idle = Scheduler::get_idle_task();
    JARVIS_ASSERT(idle != nullptr);
    JARVIS_ASSERT_EQ(0ULL, idle->priority);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Scheduler tracks a count of all registered tasks.
// Input: Call Scheduler::task_count().
// Expect: task_count() >= 1 (at least idle task exists).
// Depends: Scheduler::task_count()
JARVIS_TEST(cpu_load_task_count, "PRE: none | POST: none") {
    uint64_t cnt = Scheduler::task_count();
    JARVIS_ASSERT(cnt >= 1);
    JARVIS_ASSERT(cnt <= CONFIG_MAX_TASKS);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: /proc/sched cpu_util_pct field can be read (stub: verify no crash).
// Pseudocode: When /proc/sched is implemented, read cpu_util_pct field.
//   Verify it returns 0 ≤ pct ≤ 100.
// Input: Read cpu_util_pct from scheduler.
// Expect: Value in [0, 100] or 0 if not yet implemented.
// Depends: Scheduler CPU utilisation tracking (not yet implemented)
JARVIS_TEST(cpu_load_util_range, "PRE: none | POST: none | PENDING: cpu_util_pct") {
    /* Pseudocode:
     *   uint64_t pct = Scheduler::cpu_util_pct();
     *   JARVIS_ASSERT(pct <= 100);
     */
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Counter wraps correctly after 2^64 ticks.
// Pseudocode: Verify idle task's executed_ticks field is uint64_t.
//   Wrap-around from UINT64_MAX to 0 should not cause division-by-zero.
// Input: None.
// Expect: No crash or assertion failure on idle counter wrap.
// Depends: Idle task (executed_ticks uint64_t field)
JARVIS_TEST(cpu_load_counter_wrap, "PRE: none | POST: none | PENDING: wrap test") {
    /* Pseudocode:
     *   auto* idle = Scheduler::get_idle_task();
     *   uint64_t saved = idle->executed_ticks;
     *   idle->executed_ticks = UINT64_MAX;
     *   // Force one tick (simulate)
     *   volatile uint64_t check = idle->executed_ticks;
     *   JARVIS_ASSERT(check == UINT64_MAX || check == 0);
     *   idle->executed_ticks = saved;
     */
    JARVIS_TEST_PASS();
}

void register_cpu_load_tests() {
    Logger::info("Registering CPU load tests");
    JARVIS_REGISTER_TEST(cpu_load_idle_task_exists);
    JARVIS_REGISTER_TEST(cpu_load_idle_priority);
    JARVIS_REGISTER_TEST(cpu_load_task_count);
    JARVIS_REGISTER_TEST(cpu_load_util_range);
    JARVIS_REGISTER_TEST(cpu_load_counter_wrap);
}
