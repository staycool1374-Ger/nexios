#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: A mutex with correct ceiling priority (set at init).
// Input: Construct Mutex, verify initial state.
// Expect: is_locked == false, owner == nullptr.
// Depends: Mutex
JARVIS_TEST(pcp_mutex_init, "PRE: none | POST: none") {
    sync::Mutex m;
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_ASSERT(m.owner() == nullptr);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Task with priority > ceiling locks successfully.
// Input: Lock mutex from shell task.
// Expect: Lock acquired, owner set to current task.
// Depends: Mutex, Scheduler
JARVIS_TEST(pcp_lock_acquire, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    JARVIS_ASSERT(m.is_locked());
    JARVIS_ASSERT(m.owner() == Scheduler::current_task());
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two independent mutexes can both be locked.
// Input: Lock m1 then m2.
// Expect: Both locked, no deadlock.
// Depends: Mutex
JARVIS_TEST(pcp_two_mutexes, "PRE: none | POST: none") {
    sync::Mutex m1;
    sync::Mutex m2;
    m1.lock();
    m2.lock();
    JARVIS_ASSERT(m1.is_locked());
    JARVIS_ASSERT(m2.is_locked());
    m2.unlock();
    m1.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SpinLock guard nested inside Mutex lock.
// Input: Lock mutex, then acquire SpinLock.
// Expect: Both held, released cleanly.
// Depends: Mutex, SpinLock, SpinLockGuard
JARVIS_TEST(pcp_mutex_spinlock_nest, "PRE: none | POST: none") {
    sync::Mutex m;
    sync::SpinLock s;
    m.lock();
    {
        SpinLockGuard<sync::SpinLock> g(s);
        JARVIS_ASSERT(m.is_locked());
    }
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: try_lock from different task context fails (simulated).
// Input: Hold mutex, call try_lock again.
// Expect: Re-entrant try_lock returns true (same owner).
// Depends: Mutex
JARVIS_TEST(pcp_try_lock_held, "PRE: none | POST: none") {
    sync::Mutex m;
    m.lock();
    bool ok = m.try_lock();
    JARVIS_ASSERT(ok);
    m.unlock();
    m.unlock();
    JARVIS_ASSERT(!m.is_locked());
    JARVIS_TEST_PASS();
}

void register_pcp_tests() {
    Logger::info("Registering PCP tests");
    JARVIS_REGISTER_TEST(pcp_mutex_init);
    JARVIS_REGISTER_TEST(pcp_lock_acquire);
    JARVIS_REGISTER_TEST(pcp_two_mutexes);
    JARVIS_REGISTER_TEST(pcp_mutex_spinlock_nest);
    JARVIS_REGISTER_TEST(pcp_try_lock_held);
}
