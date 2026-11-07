#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/sync_errors.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Nested SpinLockGuard acquires locks in stack order.
// Input: Create three SpinLock objects, guard them in nested scope.
// Expect: All guards acquired and released without deadlock.
// Depends: SpinLock, SpinLockGuard
JARVIS_TEST(lock_validator_nested_acquire, "PRE: none | POST: none") {
    sync::SpinLock a;
    sync::SpinLock b;
    sync::SpinLock c;
    {
        SpinLockGuard<sync::SpinLock> ga(a);
        SpinLockGuard<sync::SpinLock> gb(b);
        SpinLockGuard<sync::SpinLock> gc(c);
        JARVIS_ASSERT(true);
    }
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Independent SpinLock objects do not interfere.
// Input: Lock A, unlock A, lock B, unlock B.
// Expect: Each operation succeeds independently.
// Depends: SpinLock
JARVIS_TEST(lock_validator_independent_locks, "PRE: none | POST: none") {
    sync::SpinLock a;
    sync::SpinLock b;
    a.lock();
    b.lock();
    a.unlock();
    b.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mutex lock_err returns OK on successful acquire.
// Input: lock_err() on unowned mutex.
// Expect: SYNC_ERR_OK.
// Depends: Mutex
JARVIS_TEST(lock_validator_mutex_lock_err_ok, "PRE: none | POST: none") {
    sync::Mutex m;
    auto err = m.lock_err();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::SYNC_ERR_OK),
                     static_cast<uint64_t>(err));
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: unlock_err on unlocked mutex returns NOT_OWNER.
// Input: Call unlock_err on a mutex that was never locked.
// Expect: SYNC_ERR_NOT_OWNER.
// Depends: Mutex
JARVIS_TEST(lock_validator_unlock_not_owner, "PRE: none | POST: none") {
    sync::Mutex m;
    auto err = m.unlock_err();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::SYNC_ERR_NOT_OWNER),
                     static_cast<uint64_t>(err));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: try_lock_err returns OK on unowned mutex.
// Input: try_lock_err().
// Expect: SYNC_ERR_OK, mutex is locked.
// Depends: Mutex
JARVIS_TEST(lock_validator_try_lock_err_ok, "PRE: none | POST: none") {
    sync::Mutex m;
    auto err = m.try_lock_err();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::SYNC_ERR_OK),
                     static_cast<uint64_t>(err));
    JARVIS_ASSERT(m.is_locked());
    m.unlock();
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: init_err on already-initialized mutex returns ALREADY_INITIALIZED.
// Input: Call init_err twice.
// Expect: First returns OK, second returns ALREADY_INITIALIZED.
// Depends: Mutex
JARVIS_TEST(lock_validator_init_err_twice, "PRE: none | POST: none") {
    sync::Mutex m;
    auto e1 = m.init_err();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::SYNC_ERR_OK),
                     static_cast<uint64_t>(e1));
    auto e2 = m.init_err();
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(errors::SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(e2));
    JARVIS_TEST_PASS();
}

void register_lock_validator_tests() {
    Logger::info("Registering lock validator tests");
    JARVIS_REGISTER_TEST(lock_validator_nested_acquire);
    JARVIS_REGISTER_TEST(lock_validator_independent_locks);
    JARVIS_REGISTER_TEST(lock_validator_mutex_lock_err_ok);
    JARVIS_REGISTER_TEST(lock_validator_unlock_not_owner);
    JARVIS_REGISTER_TEST(lock_validator_try_lock_err_ok);
    JARVIS_REGISTER_TEST(lock_validator_init_err_twice);
}
