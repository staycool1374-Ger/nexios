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

/// @file test_dmesg.cpp
/// @brief Dmesg (structured kernel log) tests — DmesgBuffer, error strings,
/// subsystem names.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/log/dmesg.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: A freshly constructed DmesgBuffer is empty.
// Input: none
// Expect: empty() returns true, size() returns 0
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_initially_empty, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    JARVIS_ASSERT(db.empty());
    JARVIS_ASSERT_EQ((size_t)0, db.size());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Pushing a single entry makes the buffer non-empty with correct
// size.
// Input: push(ErrorSubsystem::BASE, 0, "test")
// Expect: empty() false, size() == 1
// Depends: kernel::log::DmesgBuffer::push
JARVIS_TEST(dmesg_push_once, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    bool ok = db.push(log::ErrorSubsystem::BASE, 0, "hello dmesg");
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(!db.empty());
    JARVIS_ASSERT_EQ((size_t)1, db.size());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Push an entry then pop it, verifying all fields match.
// Input: push(BASE, 42, "foo", 0xDEAD), pop(entry)
// Expect: entry.subsystem == BASE, entry.error_code == 42,
//         entry.message matches, entry.context == 0xDEAD
// Depends: kernel::log::DmesgBuffer::push, ::pop
JARVIS_TEST(dmesg_push_and_pop, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::BASE, 42, "test message",
            static_cast<uintptr_t>(0xDEAD));
    log::LogEntry entry;
    bool ok = db.pop(entry);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT_EQ((uint64_t)42, entry.error_code);
    JARVIS_ASSERT_EQ(log::ErrorSubsystem::BASE, entry.subsystem);
    JARVIS_ASSERT_EQ(static_cast<uintptr_t>(0xDEAD), entry.context);
    JARVIS_ASSERT(db.empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Multiple pushes followed by pops return entries in FIFO order.
// Input: push three entries with codes 10, 20, 30
// Expect: pop codes in order 10, 20, 30; buffer empty after third pop
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_push_multiple_fifo, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::SYNC, 10, "first");
    db.push(log::ErrorSubsystem::VFS, 20, "second");
    db.push(log::ErrorSubsystem::IPC, 30, "third");
    JARVIS_ASSERT_EQ((size_t)3, db.size());

    log::LogEntry e;
    JARVIS_ASSERT(db.pop(e));
    JARVIS_ASSERT_EQ((uint64_t)10, e.error_code);
    JARVIS_ASSERT(db.pop(e));
    JARVIS_ASSERT_EQ((uint64_t)20, e.error_code);
    JARVIS_ASSERT(db.pop(e));
    JARVIS_ASSERT_EQ((uint64_t)30, e.error_code);
    JARVIS_ASSERT(db.empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Popping from an empty buffer returns false.
// Input: clear(), then pop()
// Expect: pop() returns false
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_pop_empty, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    log::LogEntry e;
    bool ok = db.pop(e);
    JARVIS_ASSERT(!ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Clear empties the buffer even after pushes.
// Input: push two entries, clear(), check empty
// Expect: empty() true, pop() returns false
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_clear, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::SCHED, 1, "before clear");
    db.push(log::ErrorSubsystem::MEMPOOL, 2, "before clear2");
    JARVIS_ASSERT(!db.empty());
    db.clear();
    JARVIS_ASSERT(db.empty());
    log::LogEntry e;
    JARVIS_ASSERT(!db.pop(e));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: for_each visits every entry without removing them.
// Input: push three entries, for_each counts, then pop all
// Expect: for_each callback invoked exactly 3 times; all entries remain after
// Depends: kernel::log::DmesgBuffer::for_each
JARVIS_TEST(dmesg_for_each, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::BASE, 1, "a");
    db.push(log::ErrorSubsystem::BASE, 2, "b");
    db.push(log::ErrorSubsystem::BASE, 3, "c");

    size_t count = 0;
    db.for_each([&count](const log::LogEntry &) { ++count; });
    JARVIS_ASSERT_EQ((size_t)3, count);

    JARVIS_ASSERT_EQ((size_t)3, db.size());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: for_each on an empty buffer does nothing.
// Input: clear(), for_each
// Expect: callback never called
// Depends: kernel::log::DmesgBuffer::for_each
JARVIS_TEST(dmesg_for_each_empty, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    size_t count = 0;
    db.for_each([&count](const log::LogEntry &) { ++count; });
    JARVIS_ASSERT_EQ((size_t)0, count);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: head_index and tail_index advance correctly on push/pop.
// Input: push N, check head == N; pop M, check tail == M
// Expect: head == N, tail == M after N pushes and M pops
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_head_tail_indices, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    size_t h0 = db.head_index();
    size_t t0 = db.tail_index();
    JARVIS_ASSERT_EQ(h0, t0);

    db.push(log::ErrorSubsystem::BASE, 0, "x");
    JARVIS_ASSERT_EQ((h0 + 1) & (log::DMESG_CAPACITY - 1), db.head_index());
    JARVIS_ASSERT_EQ(t0, db.tail_index());

    db.push(log::ErrorSubsystem::BASE, 0, "y");
    JARVIS_ASSERT_EQ((h0 + 2) & (log::DMESG_CAPACITY - 1), db.head_index());

    log::LogEntry e;
    db.pop(e);
    JARVIS_ASSERT_EQ(t0 + 1, db.tail_index());

    db.pop(e);
    JARVIS_ASSERT_EQ(t0 + 2, db.tail_index());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: When the buffer is full (Capacity-1 entries), the next push
// overwrites the oldest entry.
// Input: fill to Capacity-1 (max without overwrite), push one more
// Expect: first pop yields entry with error_code 1 (second push), not 0
// Depends: kernel::log::DmesgBuffer
JARVIS_TEST(dmesg_overflow, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    size_t cap = log::DMESG_CAPACITY;
    size_t max_fill = cap - 1;

    for (size_t i = 0; i < max_fill; ++i)
        db.push(log::ErrorSubsystem::BASE, i, "fill");

    JARVIS_ASSERT_EQ(max_fill, db.size());

    bool overwritten =
        db.push(log::ErrorSubsystem::BASE, 99, "overflow");
    JARVIS_ASSERT(!overwritten);
    JARVIS_ASSERT_EQ(max_fill, db.size());

    log::LogEntry e;
    db.pop(e);
    JARVIS_ASSERT_EQ((uint64_t)1, e.error_code);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: subsystem_name() returns the correct string for each subsystem.
// Input: all 7 ErrorSubsystem values + invalid
// Expect: correct short names
// Depends: kernel::log::subsystem_name
JARVIS_TEST(dmesg_subsystem_names, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("BASE", log::subsystem_name(log::ErrorSubsystem::BASE))
            == 0,
        "expected BASE");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("SYNC", log::subsystem_name(log::ErrorSubsystem::SYNC))
            == 0,
        "expected SYNC");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("VFS", log::subsystem_name(log::ErrorSubsystem::VFS))
            == 0,
        "expected VFS");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("MPOOL",
                         log::subsystem_name(log::ErrorSubsystem::MEMPOOL))
            == 0,
        "expected MPOOL");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("SCHED",
                         log::subsystem_name(log::ErrorSubsystem::SCHED))
            == 0,
        "expected SCHED");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("IPC", log::subsystem_name(log::ErrorSubsystem::IPC))
            == 0,
        "expected IPC");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("SYSCALL",
                         log::subsystem_name(log::ErrorSubsystem::SYSCALL))
            == 0,
        "expected SYSCALL");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: base_error_string() returns correct descriptions for known codes.
// Input: kernel::Error::OK, OOM, INVALID_ARG, daemon event codes
// Expect: matching human-readable strings
// Depends: kernel::log::base_error_string
JARVIS_TEST(dmesg_base_error_strings, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("OK", log::base_error_string(
                                   static_cast<uint64_t>(kernel::Error::OK)))
            == 0,
        "expected OK");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Out of memory",
            log::base_error_string(static_cast<uint64_t>(kernel::Error::OOM)))
            == 0,
        "expected Out of memory");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Invalid argument",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::INVALID_ARG)))
            == 0,
        "expected Invalid argument");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Not found",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::NOT_FOUND)))
            == 0,
        "expected Not found");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Already exists",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::ALREADY_EXISTS)))
            == 0,
        "expected Already exists");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Timeout",
                         log::base_error_string(
                             static_cast<uint64_t>(kernel::Error::TIMEOUT)))
            == 0,
        "expected Timeout");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Busy", log::base_error_string(
                                     static_cast<uint64_t>(kernel::Error::BUSY)))
            == 0,
        "expected Busy");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Not implemented",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::NOT_IMPLEMENTED)))
            == 0,
        "expected Not implemented");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "I/O error",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::IO_ERROR)))
            == 0,
        "expected I/O error");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "Corrupted",
            log::base_error_string(
                static_cast<uint64_t>(kernel::Error::CORRUPTED)))
            == 0,
        "expected Corrupted");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon exited",
                         log::base_error_string(0xDA01ULL))
            == 0,
        "expected Daemon exited");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon restarted",
                         log::base_error_string(0xDA02ULL))
            == 0,
        "expected Daemon restarted");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon ensured",
                         log::base_error_string(0xDA03ULL))
            == 0,
        "expected Daemon ensured");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon terminated",
                         log::base_error_string(0xDA04ULL))
            == 0,
        "expected Daemon terminated");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon restarting",
                         log::base_error_string(0xDA05ULL))
            == 0,
        "expected Daemon restarting");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon up",
                         log::base_error_string(0xDA06ULL))
            == 0,
        "expected Daemon up");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon event",
                         log::base_error_string(0xDAFFULL))
            == 0,
        "expected Daemon event");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Unknown base error",
                         log::base_error_string(9999ULL))
            == 0,
        "expected Unknown base error");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: error_string() dispatches to the correct subsystem's error
// strings.
// Input: known codes for each subsystem
// Expect: correct error descriptions
// Depends: kernel::log::error_string
JARVIS_TEST(dmesg_error_string_dispatch, "PRE: none | POST: none") {
    const char *s = log::error_string(log::ErrorSubsystem::BASE, 0);
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s",
                      s);

    s = log::error_string(log::ErrorSubsystem::SYNC,
                          static_cast<uint64_t>(errors::SYNC_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(log::ErrorSubsystem::VFS,
                          static_cast<uint64_t>(errors::VFS_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(log::ErrorSubsystem::MEMPOOL,
                          static_cast<uint64_t>(errors::MEMPOOL_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(log::ErrorSubsystem::SCHED,
                          static_cast<uint64_t>(errors::SCHED_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(
        log::ErrorSubsystem::IPC,
        static_cast<uint64_t>(errors::IPC_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(
        log::ErrorSubsystem::SYSCALL,
        static_cast<uint64_t>(errors::SYS_ERR_OK));
    JARVIS_ASSERT_FMT(__builtin_strcmp("OK", s) == 0, "expected OK, got %s", s);

    s = log::error_string(static_cast<log::ErrorSubsystem>(0xFF), 0);
    JARVIS_ASSERT_FMT(__builtin_strcmp("UNKNOWN", s) == 0,
                      "expected UNKNOWN, got %s", s);

    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: is_suppressed() and set_suppressed() correctly toggle push
// suppression.
// Input: save, set_suppressed(true/false), is_suppressed()
// Expect: is_suppressed reflects the last set value
// Depends: kernel::log::DmesgBuffer::set_suppressed, is_suppressed
JARVIS_TEST(dmesg_suppression_toggle, "PRE: none | POST: none") {
    bool saved = log::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed();
    log::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(true);
    JARVIS_ASSERT(
        log::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed());
    log::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(false);
    JARVIS_ASSERT(
        !log::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed());
    log::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(saved);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: push records the current tick count and task ID in the entry.
// Input: push one entry, pop it, verify timestamp > 0 and task_id matches
// current
// Expect: timestamp > 0, task_id == Scheduler::current_task()->id
// Depends: kernel::log::DmesgBuffer, kernel::Scheduler
JARVIS_TEST(dmesg_timestamp_and_task_id, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::BASE, 0, "ts-check");
    log::LogEntry entry;
    JARVIS_ASSERT(db.pop(entry));
    JARVIS_ASSERT_FMT(entry.timestamp > 0, "timestamp should be > 0, got %lu",
                      entry.timestamp);
    auto *current = Scheduler::current_task();
    if (current) {
        JARVIS_ASSERT_EQ(current->id, entry.task_id);
    }
    JARVIS_TEST_PASS();
}

void register_dmesg_tests() {
    Logger::info("Registering DMESG tests");

    JARVIS_REGISTER_TEST(dmesg_initially_empty);
    JARVIS_REGISTER_TEST(dmesg_push_once);
    JARVIS_REGISTER_TEST(dmesg_push_and_pop);
    JARVIS_REGISTER_TEST(dmesg_push_multiple_fifo);
    JARVIS_REGISTER_TEST(dmesg_pop_empty);
    JARVIS_REGISTER_TEST(dmesg_clear);
    JARVIS_REGISTER_TEST(dmesg_for_each);
    JARVIS_REGISTER_TEST(dmesg_for_each_empty);
    JARVIS_REGISTER_TEST(dmesg_head_tail_indices);
    JARVIS_REGISTER_TEST(dmesg_overflow);
    JARVIS_REGISTER_TEST(dmesg_subsystem_names);
    JARVIS_REGISTER_TEST(dmesg_base_error_strings);
    JARVIS_REGISTER_TEST(dmesg_error_string_dispatch);
    JARVIS_REGISTER_TEST(dmesg_suppression_toggle);
    JARVIS_REGISTER_TEST(dmesg_timestamp_and_task_id);
}
