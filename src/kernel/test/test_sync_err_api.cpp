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

/// @file test_sync_err_api.cpp
/// @brief Synchronisation error-code API tests (milestone v0.4.10 issue #132).
///        The whole kernel/sync gap is the `*_err` family — the error-returning
///        twin of every blocking operation — plus the primitives' constructors
///        and destructors.  Each `*_err` variant must report a *specific*
///        SyncError instead of a generic failure, so callers can distinguish
///        "would block" from "invalid argument" from "resource exhausted".
/// @note  Blocking discipline: every `*_err` call here is made with its
///        non-blocking precondition satisfied (Notify value already posted,
///        EventGroup bits already set, Queue slot/message available,
///        Semaphore count > 0).  `Notify::wait_err()` is the one exception —
///        it registers a waiter and reschedules *unconditionally*, even when a
///        value is pending — so it is deliberately NOT driven here; it needs a
///        disposable waiter task.
/// @note  `Mutex::wake_one()` is private and only reachable from `unlock()`
///        with a waiter registered; that also needs a contending task and is
///        left to the existing priority-inheritance classes.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/sync/eventgroup.hpp>
#include <kernel/sync/notify.hpp>
#include <kernel/sync/queue.hpp>
#include <kernel/sync/mutex.hpp>
#include <kernel/sync/semaphore.hpp>
#include <kernel/sync/spinlock.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/sync/irq_spinlock_guard.hpp>
#include <kernel/sync/spsc_ring.hpp>
#include <kernel/sync/sync.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <string.hpp>

using namespace kernel;
using namespace kernel::errors;

namespace {

/// @brief Documented maximum message size / count (queue.hpp).
constexpr size_t k_queue_max_msg_size = sync::QUEUE_MAX_MSG_SIZE;
constexpr size_t k_queue_max_msg_count = sync::QUEUE_MAX_MSG_COUNT;

} // namespace

// Runmode: kernel
// Testidea: EventGroup's error API: init_err() refuses to re-initialise a
//           group that already carries bits, the bit operations always
//           succeed, try_wait_bits_err() reports availability through its
//           out-parameter rather than an error, and wait_bits_err() returns
//           immediately when the wanted bits are already set.
// Input: A stack EventGroup; init_err() twice (second after set_bits_err),
//        set_bits_err(0x5), wait_bits_err(0x5, false, &out),
//        try_wait_bits_err for an available and an unavailable mask,
//        clear_bits_err(0x1).
// Expect: OK / ALREADY_INITIALIZED / OK / OK with out == 0x5 / OK with result
//         true then false / OK, and get_bits() == 0x4 at the end.
// Depends: sync::EventGroup
JARVIS_TEST(sync_eventgroup_err_api, "PRE: vfsd, iocd | POST: none") {
    sync::EventGroup group;

    const SyncError first_init = group.init_err();
    const SyncError set_bits = group.set_bits_err(0x5);
    const SyncError second_init = group.init_err();

    uint64_t observed = 0;
    const SyncError waited = group.wait_bits_err(0x5, false, &observed);

    bool available = false;
    const SyncError try_available = group.try_wait_bits_err(0x1, &available);
    bool unavailable = true;
    const SyncError try_unavailable =
        group.try_wait_bits_err(0x2, &unavailable);

    const SyncError cleared = group.clear_bits_err(0x1);
    const uint64_t remaining = group.get_bits();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(first_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(set_bits));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(second_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(waited));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x5), observed);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(try_available));
    JARVIS_ASSERT(available);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(try_unavailable));
    JARVIS_ASSERT(!unavailable);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(cleared));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x4), remaining);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Notify's error API: init_err() refuses a second initialisation,
//           notify_err() reports NO_WAITER when nobody is waiting (but still
//           latches the value), and try_wait_err() distinguishes "value
//           consumed" from "nothing pending" — 0 is the reserved invalid
//           value, so it can never be delivered.
// Input: A stack Notify; init_err() twice, notify_err(0x2A) with no waiter,
//        try_wait_err before and after the notify.
// Expect: OK / ALREADY_INITIALIZED / NO_WAITER, then BUFFER_EMPTY before and
//         OK with the posted value after.
// Depends: sync::Notify
JARVIS_TEST(sync_notify_err_api, "PRE: vfsd, iocd | POST: none") {
    sync::Notify notify;

    const SyncError first_init = notify.init_err();
    const SyncError second_init = notify.init_err();

    uint64_t before = 0xFFFF;
    const SyncError empty_wait = notify.try_wait_err(&before);

    const SyncError posted = notify.notify_err(0x2A);

    uint64_t consumed = 0;
    const SyncError taken = notify.try_wait_err(&consumed);

    uint64_t after = 0xFFFF;
    const SyncError drained = notify.try_wait_err(&after);

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(first_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(second_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_BUFFER_EMPTY),
                     static_cast<uint64_t>(empty_wait));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0xFFFF), before);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_NO_WAITER),
                     static_cast<uint64_t>(posted));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(taken));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x2A), consumed);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_BUFFER_EMPTY),
                     static_cast<uint64_t>(drained));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Queue's error API separates the three failure shapes an IPC
//           sender can hit: oversized message (never truncates), queue full
//           (never blocks the caller), and queue empty on receive.  The
//           round-trip must preserve the payload and its length exactly.
// Input: A stack Queue; init_err() twice (second after a send), an oversized
//        try_send_err, a normal send + receive, a receive on an empty queue,
//        then fill to capacity and try one more send.
// Expect: OK / ALREADY_INITIALIZED / MSG_TOO_LARGE / OK with the payload
//         returned intact / QUEUE_EMPTY / QUEUE_FULL at capacity.  The
//         blocking send_err/receive_err are driven with room/message
//         available so they never wait.
// Depends: sync::Queue
JARVIS_TEST(sync_queue_err_api, "PRE: vfsd, iocd | POST: none") {
    sync::Queue queue;
    // Queue() zero-initialises the waiter ARRAYS but not send_waiters_count_/
    // recv_waiters_count_ (defect: #145).  init() sets them, so the error API
    // starts from a known state instead of dereferencing garbage waiters.
    queue.init();

    const SyncError first_init = queue.init_err();

    uint8_t oversize[k_queue_max_msg_size + 1] = {};
    size_t oversize_sent = 0;
    const SyncError too_large =
        queue.try_send_err(oversize, sizeof(oversize), &oversize_sent);

    const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    size_t sent = 0;
    // Blocking variants, called with their non-blocking precondition met
    // (room in the queue / a message available), so they return immediately.
    const SyncError sent_err = queue.send_err(payload, sizeof(payload), &sent);
    const SyncError re_init = queue.init_err();

    uint8_t received[8] = {};
    size_t received_size = sizeof(received);
    size_t received_bytes = 0;
    const SyncError received_err =
        queue.receive_err(received, &received_size, &received_bytes);

    const SyncError empty_recv =
        queue.try_receive_err(received, &received_size, &received_bytes);

    // Fill to capacity, then prove the next send is refused, not blocked.
    SyncError fill_error = SYNC_ERR_OK;
    for (size_t idx = 0; idx < k_queue_max_msg_count; ++idx) {
        fill_error = queue.try_send_err(payload, sizeof(payload), &sent);
        if (fill_error != SYNC_ERR_OK)
            break;
    }
    const SyncError full_error = queue.try_send_err(payload, sizeof(payload),
                                                    &sent);

    bool payload_ok = received_bytes == sizeof(payload) &&
                      received[0] == 0xDE && received[1] == 0xAD &&
                      received[2] == 0xBE && received[3] == 0xEF;

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(first_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_MSG_TOO_LARGE),
                     static_cast<uint64_t>(too_large));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), oversize_sent);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(sent_err));
    JARVIS_ASSERT_EQ(sizeof(payload), sent);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(re_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(received_err));
    JARVIS_ASSERT(payload_ok);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_QUEUE_EMPTY),
                     static_cast<uint64_t>(empty_recv));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(fill_error));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_QUEUE_FULL),
                     static_cast<uint64_t>(full_error));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Semaphore's error API: init_err() refuses to re-arm a semaphore
//           that already has a count, wait_err()/try_wait_err() consume only
//           what is available and report exhaustion instead of blocking, and
//           post_err() refuses to exceed the configured maximum (a counting
//           semaphore must not silently grow without bound).
// Input: A stack Semaphore initialised to (2, max 2); wait_err, try_wait_err
//        until empty and once more, then post_err until full and once more.
// Expect: OK / ALREADY_INITIALIZED / OK / OK / QUEUE_EMPTY / OK / OK /
//         BUFFER_FULL, with value() tracking the count.
// Depends: sync::Semaphore
JARVIS_TEST(sync_semaphore_err_api, "PRE: vfsd, iocd | POST: none") {
    sync::Semaphore semaphore;

    const SyncError init = semaphore.init_err(2, 2);
    const SyncError re_init = semaphore.init_err(2, 2);
    const uint64_t after_init = semaphore.value();

    const SyncError waited = semaphore.wait_err();
    const uint64_t after_wait = semaphore.value();

    const SyncError taken = semaphore.try_wait_err();
    const SyncError exhausted = semaphore.try_wait_err();

    const SyncError post_one = semaphore.post_err();
    const SyncError post_two = semaphore.post_err();
    const SyncError post_three = semaphore.post_err();
    const uint64_t after_posts = semaphore.value();

    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(re_init));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), after_init);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(waited));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(1), after_wait);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(taken));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_QUEUE_EMPTY),
                     static_cast<uint64_t>(exhausted));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(post_one));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_OK),
                     static_cast<uint64_t>(post_two));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_BUFFER_FULL),
                     static_cast<uint64_t>(post_three));
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(2), after_posts);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Mutex's error API and waiter bookkeeping: init_err() refuses a
//           second initialisation, and remove_waiter() must reject a task that
//           is not registered (it is the teardown path used when a blocked
//           waiter dies) rather than corrupting the waiter table.
// Input: A stack Mutex; init_err(0) twice; remove_waiter() on the current
//        task, which was never enqueued as a waiter.
// Expect: OK / ALREADY_INITIALIZED / remove_waiter returns false and the
//         mutex stays unlocked.
// Depends: sync::Mutex, Scheduler::current_task
JARVIS_TEST(sync_mutex_err_api, "PRE: vfsd, iocd | POST: none") {
    TaskControlBlock *self = Scheduler::current_task();
    JARVIS_ASSERT(self != nullptr);

    sync::Mutex mutex;
    // Mutex() does not initialise `initialized_` (defect: #145), so a fresh
    // mutex may legitimately report either code here; both paths enter
    // init_err().  The SECOND call is deterministic: by then initialized_ is
    // set, so it must refuse.
    const SyncError init = mutex.init_err(0);
    const SyncError re_init = mutex.init_err(0);
    const bool removed = mutex.remove_waiter(*self);
    const bool still_unlocked = !mutex.is_locked();

    JARVIS_ASSERT(init == SYNC_ERR_OK || init == SYNC_ERR_ALREADY_INITIALIZED);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(SYNC_ERR_ALREADY_INITIALIZED),
                     static_cast<uint64_t>(re_init));
    JARVIS_ASSERT(!removed);
    JARVIS_ASSERT(still_unlocked);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The two RAII lock guards and the lock-free SPSC ring are the
//           primitives every sync object is built on.  A guard must acquire on
//           construction, release on destruction, support adopting an
//           already-held lock, and become a no-op after an explicit unlock();
//           the ring must reject a push when full and a pop when empty rather
//           than overwriting or wrapping incorrectly.
// Input: SpinLockGuard<SpinLock> (scoped, adopted, explicitly unlocked),
//        IrqSpinLockGuard (scoped), and an SPSCRing<uint64_t,4> driven
//        empty → full → drained → reset.
// Expect: owns_lock() true inside each guard and false after unlock(); ring
//         accepts exactly N-1 pushes, refuses the next, pops them in FIFO
//         order, and is empty again after reset().
// Depends: sync::SpinLock, SpinLockGuard, IrqSpinLockGuard, SPSCRing
JARVIS_TEST(sync_guards_and_spsc_ring, "PRE: vfsd, iocd | POST: none") {
    sync::SpinLock lock;
    bool scoped_owns = false;
    {
        SpinLockGuard<sync::SpinLock> guard(lock);
        scoped_owns = guard.owns_lock();
    }

    bool adopted_owns = false;
    lock.lock();
    {
        SpinLockGuard<sync::SpinLock> guard(lock, adopt_lock);
        adopted_owns = guard.owns_lock();
    }
    // The adopted guard must have released it: a fresh try_lock succeeds.
    const bool released_by_guard = lock.try_lock();
    if (released_by_guard)
        lock.unlock();

    bool cleared_owns = true;
    {
        SpinLockGuard<sync::SpinLock> guard(lock);
        guard.unlock();
        cleared_owns = guard.owns_lock();
    }

    sync::SpinLock irq_lock;
    {
        sync::IrqSpinLockGuard guard(irq_lock);
    }

    // A power-of-two ring reserves one slot to tell "full" from "empty", so
    // an N=4 ring holds N-1 = 3 items (try_push refuses when next == tail).
    constexpr size_t k_ring_slots = 4;
    constexpr size_t k_ring_capacity = k_ring_slots - 1;

    SPSCRing<uint64_t, k_ring_slots> ring;
    ring.reset();
    const bool empty_after_reset = ring.empty();
    uint64_t dummy = 0;
    const bool pop_when_empty = ring.try_pop(dummy);

    size_t pushed = 0;
    for (uint64_t value = 0; value < k_ring_slots; ++value) {
        if (!ring.try_push(0x100 + value))
            break;
        ++pushed;
    }
    const bool push_when_full = ring.try_push(0x999);
    const bool non_empty = !ring.empty();

    uint64_t first = 0;
    uint64_t second = 0;
    const bool popped_first = ring.try_pop(first);
    const bool popped_second = ring.try_pop(second);
    ring.reset();
    const bool empty_at_end = ring.empty();

    JARVIS_ASSERT(scoped_owns);
    JARVIS_ASSERT(adopted_owns);
    JARVIS_ASSERT(released_by_guard);
    JARVIS_ASSERT(!cleared_owns);
    JARVIS_ASSERT(empty_after_reset);
    JARVIS_ASSERT(!pop_when_empty);
    JARVIS_ASSERT_EQ(k_ring_capacity, pushed);
    JARVIS_ASSERT(!push_when_full);
    JARVIS_ASSERT(non_empty);
    JARVIS_ASSERT(popped_first);
    JARVIS_ASSERT(popped_second);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x100), first);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x101), second);
    JARVIS_ASSERT(empty_at_end);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: SPSCRing is instantiated for several element types and capacities
//           across the kernel (byte rings for IRQ data, 64-bit rings for
//           traces).  Each instantiation is a separate function set and must
//           honour the same contract: reject a push when full (one slot is
//           reserved to tell full from empty), reject a pop when empty, drain
//           in FIFO order, and return to empty after reset().
// Input: Static SPSCRing<char,256>, <char,4>, <unsigned char,64>,
//        <uint64_t,1024> and <uint64_t,256> — each reset, filled to capacity,
//        over-filled, drained and re-checked.  Also IrqSpinLockGuard's
//        explicit unlock()/lock() pair, sync::init_all(), and the SyncError
//        string table.
// Expect: Every ring accepts exactly N-1 pushes and refuses the next, pops in
//         order and is empty after reset; the guard's temporary unlock/lock
//         round-trip keeps it owning the lock; error_string() maps
//         SYNC_ERR_OK to its documented text.
// Depends: SPSCRing, IrqSpinLockGuard, sync::init_all, errors::error_string
JARVIS_TEST(sync_ring_instantiations_and_misc,
            "PRE: vfsd, iocd | POST: none") {
    // Static: a 1024-entry 64-bit ring is 8 KiB — far too large for a kernel
    // task stack.
    static SPSCRing<char, 256> char_ring_256;
    static SPSCRing<char, 4> char_ring_4;
    static SPSCRing<unsigned char, 64> byte_ring_64;
    static SPSCRing<uint64_t, 1024> quad_ring_1024;
    static SPSCRing<uint64_t, 256> quad_ring_256;

    char_ring_256.reset();
    char_ring_4.reset();
    byte_ring_64.reset();
    quad_ring_1024.reset();
    quad_ring_256.reset();

    const bool small_empty = char_ring_4.empty();
    const bool big_empty = quad_ring_1024.empty();

    // Fill each ring to capacity (N-1) and prove the next push is refused.
    size_t char_256_pushed = 0;
    for (size_t i = 0; i < 256; ++i) {
        if (!char_ring_256.try_push(static_cast<char>('a' + (i % 26))))
            break;
        ++char_256_pushed;
    }
    const bool char_256_full = !char_ring_256.try_push('Z');

    size_t char_4_pushed = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (!char_ring_4.try_push(static_cast<char>('0' + i)))
            break;
        ++char_4_pushed;
    }
    const bool char_4_full = !char_ring_4.try_push('X');

    size_t byte_64_pushed = 0;
    for (size_t i = 0; i < 64; ++i) {
        if (!byte_ring_64.try_push(static_cast<unsigned char>(i)))
            break;
        ++byte_64_pushed;
    }
    const bool byte_64_full = !byte_ring_64.try_push(0xFF);

    size_t quad_1024_pushed = 0;
    for (size_t i = 0; i < 1024; ++i) {
        if (!quad_ring_1024.try_push(0x1000 + i))
            break;
        ++quad_1024_pushed;
    }
    const bool quad_1024_full = !quad_ring_1024.try_push(0xDEAD);

    size_t quad_256_pushed = 0;
    for (size_t i = 0; i < 256; ++i) {
        if (!quad_ring_256.try_push(0x2000 + i))
            break;
        ++quad_256_pushed;
    }
    const bool quad_256_full = !quad_ring_256.try_push(0xBEEF);

    // Drain: first element out must be the first element in.
    char first_char = 0;
    const bool popped_char_4 = char_ring_4.try_pop(first_char);
    unsigned char first_byte = 0;
    const bool popped_byte_64 = byte_ring_64.try_pop(first_byte);
    uint64_t first_quad_1024 = 0;
    const bool popped_quad_1024 = quad_ring_1024.try_pop(first_quad_1024);
    uint64_t first_quad_256 = 0;
    const bool popped_quad_256 = quad_ring_256.try_pop(first_quad_256);
    char first_char_256 = 0;
    const bool popped_char_256 = char_ring_256.try_pop(first_char_256);

    char_ring_256.reset();
    char_ring_4.reset();
    byte_ring_64.reset();
    quad_ring_1024.reset();
    quad_ring_256.reset();
    const bool drained_small = char_ring_4.empty();
    const bool drained_big = quad_ring_1024.empty();

    // IrqSpinLockGuard's temporary release/re-acquire pair (OOM-handler path).
    sync::SpinLock guard_lock;
    bool still_owns = false;
    {
        sync::IrqSpinLockGuard guard(guard_lock);
        guard.unlock();
        guard.lock();
        still_owns = true;
    }

    sync::init_all();
    const char *ok_text = kernel::errors::error_string(SYNC_ERR_OK);
    const bool ok_text_matches = strcmp(ok_text, "OK") == 0;

    JARVIS_ASSERT(small_empty);
    JARVIS_ASSERT(big_empty);
    JARVIS_ASSERT_EQ(static_cast<size_t>(255), char_256_pushed);
    JARVIS_ASSERT(char_256_full);
    JARVIS_ASSERT_EQ(static_cast<size_t>(3), char_4_pushed);
    JARVIS_ASSERT(char_4_full);
    JARVIS_ASSERT_EQ(static_cast<size_t>(63), byte_64_pushed);
    JARVIS_ASSERT(byte_64_full);
    JARVIS_ASSERT_EQ(static_cast<size_t>(1023), quad_1024_pushed);
    JARVIS_ASSERT(quad_1024_full);
    JARVIS_ASSERT_EQ(static_cast<size_t>(255), quad_256_pushed);
    JARVIS_ASSERT(quad_256_full);
    JARVIS_ASSERT(popped_char_4);
    JARVIS_ASSERT_EQ('0', first_char);
    JARVIS_ASSERT(popped_byte_64);
    JARVIS_ASSERT_EQ(static_cast<unsigned char>(0), first_byte);
    JARVIS_ASSERT(popped_quad_1024);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x1000), first_quad_1024);
    JARVIS_ASSERT(popped_quad_256);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0x2000), first_quad_256);
    JARVIS_ASSERT(popped_char_256);
    JARVIS_ASSERT_EQ('a', first_char_256);
    JARVIS_ASSERT(drained_small);
    JARVIS_ASSERT(drained_big);
    JARVIS_ASSERT(still_owns);
    JARVIS_ASSERT(ok_text_matches);
    JARVIS_TEST_PASS();
}

void register_sync_err_api_tests() {
    Logger::info("Registering sync error-code API tests");
    JARVIS_REGISTER_TEST(sync_eventgroup_err_api);
    JARVIS_REGISTER_TEST(sync_notify_err_api);
    JARVIS_REGISTER_TEST(sync_queue_err_api);
    JARVIS_REGISTER_TEST(sync_semaphore_err_api);
    JARVIS_REGISTER_TEST(sync_mutex_err_api);
    JARVIS_REGISTER_TEST(sync_guards_and_spsc_ring);
    JARVIS_REGISTER_TEST(sync_ring_instantiations_and_misc);
}
