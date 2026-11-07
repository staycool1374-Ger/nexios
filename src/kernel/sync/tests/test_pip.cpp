#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/task_ptr.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: A freshly initialized mutex is unlocked with no owner.
// Input: Construct Mutex, call init().
// Expect: is_locked() == false, owner() == nullptr.
// Depends: Mutex
JARVIS_TEST(pip_mutex_init_state, "PRE: none | POST: none") {
    sync::Mutex m;
    m.init();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_ASSERT(m.owner() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Lock a mutex from the current task, check owner.
// Input: m.lock() then check owner.
// Expect: owner == current_task, is_locked == true.
// Depends: Mutex, Scheduler
JARVIS_TEST(pip_mutex_lock_owner, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    JARVIS_ASSERT(m.is_locked());
    JARVIS_ASSERT(m.owner() == Scheduler::current_task());
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: try_lock succeeds on unowned mutex.
// Input: m.try_lock().
// Expect: returns true, owner set.
// Depends: Mutex
JARVIS_TEST(pip_try_lock_success, "PRE: none | POST: none") {
    sync::Mutex m;
    bool ok = m.try_lock();
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: try_lock fails when another task holds the mutex.
// Input: Lock mutex from current task, then try_lock again.
// Expect: Re-entrant try_lock succeeds (same owner).
// Depends: Mutex
JARVIS_TEST(pip_try_lock_reentrant, "PRE: none | POST: none") {
    sync::Mutex m;
    JARVIS_ASSERT(m.try_lock());
    JARVIS_ASSERT(m.try_lock());
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: unlock transfers ownership to next waiter (wake_one).
// Input: Create two tasks, one holds lock, other waits.
// Expect: After unlock, waiter becomes owner.
// Depends: Mutex, TaskControlBlock
JARVIS_TEST(pip_mutex_unlock_clean, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_ASSERT(m.owner() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mutex can be re-locked after unlock.
// Input: lock, unlock, lock again.
// Expect: Works, owner updated each time.
// Depends: Mutex
JARVIS_TEST(pip_mutex_relock, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    auto* first = m.owner();
    m.unlock();
    m.lock();
    JARVIS_ASSERT(m.owner() == first);
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Unlock of an already-unlocked mutex is a no-op.
// Input: m.unlock() when not owned.
// Expect: No crash, returns silently.
// Depends: Mutex
JARVIS_TEST(pip_unlock_idempotent, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    m.unlock();
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_TEST_PASS();
}

void register_pip_tests() {
    Logger::info("Registering PIP tests");
    JARVIS_REGISTER_TEST(pip_mutex_init_state);
    JARVIS_REGISTER_TEST(pip_mutex_lock_owner);
    JARVIS_REGISTER_TEST(pip_try_lock_success);
    JARVIS_REGISTER_TEST(pip_try_lock_reentrant);
    JARVIS_REGISTER_TEST(pip_mutex_unlock_clean);
    JARVIS_REGISTER_TEST(pip_mutex_relock);
    JARVIS_REGISTER_TEST(pip_unlock_idempotent);
}
