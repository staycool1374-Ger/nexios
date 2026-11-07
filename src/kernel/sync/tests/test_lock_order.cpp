#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Acquire two independent SpinLocks in consistent nested order.
// Input: lock A then lock B, unlock B then unlock A.
// Expect: Both locks acquired, no contention, guards unwind correctly.
// Depends: SpinLock, SpinLockGuard
JARVIS_TEST(lock_order_consistent_nesting, "PRE: none | POST: none") {
    sync::SpinLock a;
    sync::SpinLock b;
    {
        SpinLockGuard<sync::SpinLock> g1(a);
        SpinLockGuard<sync::SpinLock> g2(b);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Acquire two independent Mutex objects in consistent order.
// Input: lock M1 then M2.
// Expect: Both locks acquired, no deadlock on independent objects.
// Depends: Mutex
JARVIS_TEST(lock_order_mutex_nesting, "PRE: none | POST: none") {
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
// Testidea: Release locks in reverse order from acquisition.
// Input: lock A, lock B, unlock A, unlock B (reverse order).
// Expect: All locks released, no panic or hang.
// Depends: SpinLock
JARVIS_TEST(lock_order_reverse_unlock, "PRE: none | POST: none") {
    sync::SpinLock a;
    sync::SpinLock b;
    a.lock();
    b.lock();
    a.unlock();
    b.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Three-way nested lock acquisition.
// Input: lock A, B, C in order; unlock C, B, A.
// Expect: All three held concurrently, released cleanly.
// Depends: SpinLock
JARVIS_TEST(lock_order_three_way, "PRE: none | POST: none") {
    sync::SpinLock a;
    sync::SpinLock b;
    sync::SpinLock c;
    a.lock();
    b.lock();
    c.lock();
    c.unlock();
    b.unlock();
    a.unlock();
    JARVIS_TEST_PASS();
}

void register_lock_order_tests() {
    Logger::info("Registering lock order tests");
    JARVIS_REGISTER_TEST(lock_order_consistent_nesting);
    JARVIS_REGISTER_TEST(lock_order_mutex_nesting);
    JARVIS_REGISTER_TEST(lock_order_reverse_unlock);
    JARVIS_REGISTER_TEST(lock_order_three_way);
}
