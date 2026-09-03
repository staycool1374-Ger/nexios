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

#pragma once

#include <types.hpp>
#include <logger.hpp>

namespace kernel {

// Forward declaration of the scheduler task type, so test.hpp can reference it
// in helper signatures without pulling in scheduler.hpp (the full definition
// is available at call sites that include <kernel/task/scheduler.hpp>).
class TaskControlBlock;

namespace test {

// Local alias so helpers below can write `TaskControlBlock` and resolve to the
// real kernel::TaskControlBlock declared above.
using TaskControlBlock = kernel::TaskControlBlock;

enum TestFlags : uint8_t {
    TF_KERNEL   = 0,
    TF_RELEASE  = 1 << 0,
    TF_USER     = 1 << 1,
    TF_BENCH    = 1 << 2,
};

class TestBase;

struct TestCase {
    const char* suite;
    const char* name;
    void (*func)();
    TestBase* (*factory)();
    uint8_t flags;
};

struct ClassSection {
    const char* name;
    size_t start;
    size_t count;
};

class Registry {
public:
    static void init();
    static void register_test(const TestCase& tc);
    static const TestCase* tests();
    static size_t count();
    static size_t class_count();
    static const ClassSection* class_section(size_t i);

    static void record_failure(const char* file, int line, const char* expr);
    static void record_failure_fmt(const char* file, int line, const char* fmt, ...);
    static void record_success();
    static void record_test(bool passed);

    static size_t passed();
    static size_t failed();
    static size_t total();
    static size_t test_count();
    static size_t test_passed();
    static size_t test_failed();
    static void reset();
    static void clear();

    static void set_expected_count(size_t n);
    static size_t expected_count();

    static void record_class_section(const char* name, size_t start, size_t count);

    // Tracks the name of the test case currently executing.  Used by
    // add_task_named() to tag test-created tasks with their origin so leaks
    // are traceable to a specific test.
    static void set_current_test_name(const char* name);
    static const char* current_test_name();

private:
    static constexpr size_t MAX_TESTS = 1026;
    static constexpr size_t MAX_CLASSES = 64;
    // NOLINTBEGIN(bugprone-dynamic-static-initializers)
    static TestCase tests_[MAX_TESTS];
    static size_t count_;
    static size_t passed_;
    static size_t failed_;
    static size_t test_count_;
    static size_t test_failed_;
    static ClassSection sections_[MAX_CLASSES];
    static size_t class_count_;
    static size_t expected_count_;
    static const char* current_name_;
    // NOLINTEND(bugprone-dynamic-static-initializers)
};

class TestBase {
public:
    TestBase(const char* name) : name_(name) {}
    virtual ~TestBase() = default;

    void execute() {
        Registry::set_current_test_name(name_);
        setUp();
        run();
        tearDown();
    }

    virtual void setUp() {}
    virtual void run() = 0;
    virtual void tearDown() {}

    const char* name() const { return name_; }

protected:
    void fail(const char* file, int line, const char* expr) {
        failed_ = true;
        Registry::record_failure(file, line, expr);
    }
    void pass() {
        Registry::record_success();
    }

    const char* name_;
    bool failed_ = false;
};

/// @brief Creates a task (via the kernel's TaskControlBlock::create) and adds
///        it to the scheduler, tagging its `name` with the calling test case so
///        leaked/orphaned tasks are traceable to their origin.  `tag` (if
///        non-null) is prefixed to the test name (e.g. a role like "worker").
///        Falls back to the raw `create` name when no test is active.
TaskControlBlock *create_named_task(void (*entry)(), uint64_t priority,
                                     uint64_t period_ticks,
                                     const char *tag = nullptr);

/// @brief Adds an already-created task to the scheduler, tagging its `name`
///        with the calling test case (see create_named_task).  Thin wrapper
///        over Scheduler::add_task that records provenance.
void add_task_named(TaskControlBlock &task, const char *tag = nullptr);

void run_all();
void run_safe();
void run_filtered(uint8_t required_flags, bool use_isolation = true);

/// @brief Run all registered non-benchmark tests.
void run_debug();
/// @brief Run all registered benchmark tests (TF_BENCH).
void run_benchmarks();
void run_release();
void run_registered(uint8_t required_flags);
void run_suite(const char* suite_name);
void print_report(uint64_t start_ns, uint64_t end_ns);

void set_kernel_entry_ns();
void set_class_auto_shutdown(bool enabled);

struct TestClass {
    const char* name;
    void (*register_all)();
};

bool register_class(const char* name);
void dump_class_counts();

// ============================================================================
// DRIVEN-TEST COOKBOOK — reference skeletons (documentation only, never called)
//
// Every kernel test must DRIVE the system to a state through real dispatch,
// then TRIGGER a real event (timer tick / ISR / syscall / real terminate),
// then VERIFY the reaction.  These skeletons capture the exact patterns that
// fix the observed parent/child and blocked-sender HANG classes.  A test that
// copies them is deterministic, leak-free, and cannot wedge the scheduler.
//
// MANDATORY RULES (each one stopped a real hang):
//   1. CREATE both TCBs first and set every field/queue BEFORE registering
//      either task with the scheduler.  The timer ISR fires between arbitrary
//      instructions, so a task added early can be dispatched (and terminate)
//      before the harness finishes arranging the scenario.
//   2. REGISTER multiple cooperating tasks under one `arch::IrqGuard` so no
//      timer tick can split the registration.  The higher-priority task that
//      must BLOCK runs first; the lower-priority peer runs after it blocks.
//   3. WAIT on observed task state (`task->state == TaskState::BLOCKED`) with
//      `asm volatile("pause")`.  Never assume ordering.
//   4. RECLAIM self-terminated tasks via `Scheduler::drain_zombie_list()`.
//      `Scheduler::terminate()` only marks a task TERMINATED and moves it to
//      the zombie list — it does NOT free the TCB.  Never follow it with
//      `remove_task()+cleanup()+delete` (double-free on a poisoned block).
//   5. CLEANUP BEFORE ASSERT: drain zombies / terminate stray live peers
//      BEFORE `JARVIS_ASSERT*`.  Assertions `return` on failure, so asserting
//      before cleanup leaks the TCB (observed as `PMM +17, Tasks +1`) or
//      leaves a live task that hangs the whole class.
//   6. External termination of a task blocked in `Semaphore::wait()` is SAFE
//      since v0.3.12: `TaskControlBlock::cleanup()` unlinks the task from the
//      semaphore's waiter list via the `waiting_on_semaphore` back-pointer
//      (v0.3.9 teardown gap closed).  Verify teardown with
//      `semaphore_waiter_teardown_on_terminate` — do not re-add the old
//      workaround (avoiding the scenario instead of testing it).
// ============================================================================

/// @brief Reference skeleton for a REAL parent-WAITPID / child-exit pattern.
///        Blueprint for tests such as scheduler_waitpid_wakes_parent and
///        scheduler_reap_respects_parent_wait.
///
///        create parent + child TCBs (no add_task)
///        parent->add_child(child)                 // sets child->parent_id
///        ctx.child_id_ = child->id; parent->user_data = &ctx
///        {
///            arch::IrqGuard guard;                // register atomically
///            Scheduler::add_task(*parent);        // prio 20 (blocks first)
///            Scheduler::add_task(*child);         // prio 12 (runs after)
///        }
///        Scheduler::reschedule();
///        while (parent->state != BLOCKED && parent->state != TERMINATED)
///            asm volatile("pause");
///        while (parent->state != TERMINATED)
///            asm volatile("pause");               // child exited, parent woke
///        child_removed = find_task(ctx.child_id_) == nullptr;
///        if (!child_removed) Scheduler::terminate(*child, 0);  // cleanup
///        Scheduler::drain_zombie_list();          // reclaim BEFORE assert
///        JARVIS_ASSERT(ctx.woke_ == 1);
///        JARVIS_ASSERT(child_removed);
///        JARVIS_ASSERT_EQ(exit_status, ctx.status_);
inline void __reference_parent_wait_child() {}

/// @brief Reference skeleton for ONE self-terminating dispatched task.
///        Blueprint for task_exit_cleans_all_ipc_objects and lifecycle_*.
///
///        t = TaskControlBlock::create(entry, prio, period);  // no add_task
///        // set page_table_/user_stack_/user_stack_size_/etc BEFORE add_task
///        Scheduler::add_task(*t);
///        Scheduler::reschedule();
///        while (t->state != TERMINATED) asm volatile("pause");
///        Scheduler::drain_zombie_list();          // terminate() alone frees
///                                                 // nothing; drain reclaims
///        JARVIS_ASSERT(...);                      // never deref the freed TCB
inline void __reference_single_terminating_task() {}

/// @brief Reference skeleton for a REAL blocked-sender / receiver-cleanup
///        pattern.  Blueprint for task_exit_wakes_blocked_senders and
///        task_cleanup_frees_msg_queue_with_blocked_senders.
///
///        receiver = create(receiver_entry, 11, 10)     // no add_task
///        fill receiver->msg_queue to IPC_MAX_QUEUE_MSG // BEFORE register
///        sender = create(sender_entry, 12, 10); sender->user_data = &sctx
///        {
///            arch::IrqGuard guard;                // register atomically
///            Scheduler::add_task(*receiver);
///            Scheduler::add_task(*sender);        // sender blocks on full q
///        }
///        Scheduler::reschedule();
///        while (sender->state != BLOCKED) asm volatile("pause");
///        Scheduler::reschedule();                 // dispatch receiver
///        while (receiver->state != TERMINATED) asm volatile("pause");
///        Scheduler::drain_zombie_list();          // cleanup wakes sender
///        while (sender->state != TERMINATED) asm volatile("pause");
///        Scheduler::drain_zombie_list();
///        JARVIS_ASSERT_EQ(0ULL, send_result);     // woken sender fast-fails
inline void __reference_blocked_sender_cleanup() {}

} // namespace test
} // namespace kernel

#define JARVIS_TEST(name, ...)                                                \
    void test_##name();                                                        \
    void test_##name()

#define JARVIS_TEST_SUITE(suite, name, ...)                                   \
    void test_##suite##_##name();                                              \
    void test_##suite##_##name()

#define JARVIS_ASSERT(cond)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kernel::test::Registry::record_failure(                            \
                __FILE__, __LINE__, #cond);                                    \
            return;                                                            \
        }                                                                      \
        kernel::test::Registry::record_success();                              \
    } while (0)

#define JARVIS_ASSERT_HEX_EQ(expected, actual)                                \
    do {                                                                       \
        uint64_t _exp = static_cast<uint64_t>(expected);                       \
        uint64_t _act = static_cast<uint64_t>(actual);                         \
        if (_exp != _act) {                                                    \
            kernel::test::Registry::record_failure(                            \
                __FILE__, __LINE__,                                            \
                #actual " == " #expected " (exp=0x" #expected ")");            \
            return;                                                            \
        }                                                                      \
        kernel::test::Registry::record_success();                              \
    } while (0)

#define JARVIS_ASSERT_EQ(expected, actual)                                    \
    do {                                                                       \
        if ((expected) != (actual)) {                                          \
            kernel::test::Registry::record_failure(                            \
                __FILE__, __LINE__,                                            \
                #actual " != " #expected);                                     \
            return;                                                            \
        }                                                                      \
        kernel::test::Registry::record_success();                              \
    } while (0)

#define JARVIS_TEST_PASS()                                                     \
    kernel::test::Registry::record_success()

#define JARVIS_FAIL(fmt, ...)                                                  \
    do {                                                                       \
        kernel::test::Registry::record_failure_fmt(                            \
            __FILE__, __LINE__, fmt, ##__VA_ARGS__);                           \
        return;                                                                \
    } while (0)

#define JARVIS_ASSERT_FMT(cond, fmt, ...)                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kernel::test::Registry::record_failure_fmt(                        \
                __FILE__, __LINE__, fmt, ##__VA_ARGS__);                       \
            return;                                                            \
        }                                                                      \
        kernel::test::Registry::record_success();                              \
    } while (0)

#define JARVIS_REGISTER_TEST_FLAGS(name, flags_)                              \
    do {                                                                       \
        static constexpr kernel::test::TestCase tc = {                         \
            "", #name, test_##name, nullptr, flags_                            \
        };                                                                     \
        kernel::test::Registry::register_test(tc);                             \
    } while (0)

#define JARVIS_REGISTER_TEST(name)                                            \
    JARVIS_REGISTER_TEST_FLAGS(name, kernel::test::TF_KERNEL)

#define JARVIS_REGISTER_RELEASE_TEST(name)                                    \
    JARVIS_REGISTER_TEST_FLAGS(name, kernel::test::TF_RELEASE)

#define JARVIS_REGISTER_TEST_SUITE_FLAGS(suite, name, flags_)                 \
    do {                                                                       \
        static constexpr kernel::test::TestCase tc = {                         \
            #suite, #name, test_##suite##_##name, nullptr, flags_              \
        };                                                                     \
        kernel::test::Registry::register_test(tc);                             \
    } while (0)

#define JARVIS_REGISTER_TEST_SUITE(suite, name)                                \
    JARVIS_REGISTER_TEST_SUITE_FLAGS(suite, name, kernel::test::TF_KERNEL)

#define JARVIS_REGISTER_RELEASE_TEST_SUITE(suite, name)                       \
    JARVIS_REGISTER_TEST_SUITE_FLAGS(suite, name, kernel::test::TF_RELEASE)

// NOLINTBEGIN(bugprone-macro-parentheses)
#ifndef __clang__
#define TEST_CLASS_DIAG_PUSH    _Pragma("GCC diagnostic push")
#define TEST_CLASS_DIAG_IGNORE  _Pragma("GCC diagnostic ignored \"-Wanalyzer-possible-null-dereference\"")
#define TEST_CLASS_DIAG_POP     _Pragma("GCC diagnostic pop")
#else
#define TEST_CLASS_DIAG_PUSH
#define TEST_CLASS_DIAG_IGNORE
#define TEST_CLASS_DIAG_POP
#endif

#define TEST_CLASS(name)                                                      \
    class name : public kernel::test::TestBase {                              \
    public:                                                                   \
        name() : TestBase(#name) {}                                           \
        void run() override;                                                  \
    };                                                                        \
    TEST_CLASS_DIAG_PUSH                                                      \
    TEST_CLASS_DIAG_IGNORE                                                    \
    static kernel::test::TestBase* _factory_##name() { return new name(); }   \
    TEST_CLASS_DIAG_POP                                                       \
    void name::run()
// NOLINTEND(bugprone-macro-parentheses)

#define REGISTER_CLASS_FLAGS(name, flags_)                                    \
    do {                                                                      \
        static constexpr kernel::test::TestCase tc = {                        \
            "", #name, nullptr, _factory_##name, flags_                       \
        };                                                                    \
        kernel::test::Registry::register_test(tc);                            \
    } while (0)

#define REGISTER_CLASS(name)                                                  \
    REGISTER_CLASS_FLAGS(name, kernel::test::TF_KERNEL)

#define REGISTER_RELEASE_CLASS(name)                                          \
    REGISTER_CLASS_FLAGS(name, kernel::test::TF_RELEASE)

#define CT_ASSERT(cond)                                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fail(__FILE__, __LINE__, #cond);                                  \
            return;                                                           \
        }                                                                     \
        pass();                                                               \
    } while (0)
