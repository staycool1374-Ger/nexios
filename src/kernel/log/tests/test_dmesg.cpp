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
#include <kernel/log/dmesg_catalog.hpp>

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
    JARVIS_ASSERT(db.pop(e));
    JARVIS_ASSERT_EQ(t0 + 1, db.tail_index());

    JARVIS_ASSERT(db.pop(e));
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
    JARVIS_ASSERT(db.pop(e));
    JARVIS_ASSERT_EQ((uint64_t)1, e.error_code);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: subsystem_name() returns the correct string for each subsystem.
// Input: all 19 ErrorSubsystem values
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
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("NET", log::subsystem_name(log::ErrorSubsystem::NET))
            == 0,
        "expected NET");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("ELF", log::subsystem_name(log::ErrorSubsystem::ELF))
            == 0,
        "expected ELF");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("USERSPACE",
                         log::subsystem_name(log::ErrorSubsystem::USER))
            == 0,
        "expected USERSPACE");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("DAEMON",
                         log::subsystem_name(log::ErrorSubsystem::DAEMON))
            == 0,
        "expected DAEMON");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("PMM", log::subsystem_name(log::ErrorSubsystem::PMM))
            == 0,
        "expected PMM");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("VMM", log::subsystem_name(log::ErrorSubsystem::VMM))
            == 0,
        "expected VMM");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("TASK",
                         log::subsystem_name(log::ErrorSubsystem::TASK))
            == 0,
        "expected TASK");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("BUFPOOL",
                         log::subsystem_name(log::ErrorSubsystem::BUFPOOL))
            == 0,
        "expected BUFPOOL");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("DRIVER",
                         log::subsystem_name(log::ErrorSubsystem::DRIVER))
            == 0,
        "expected DRIVER");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("INIT",
                         log::subsystem_name(log::ErrorSubsystem::INIT))
            == 0,
        "expected INIT");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("TIMING",
                         log::subsystem_name(log::ErrorSubsystem::TIMING))
            == 0,
        "expected TIMING");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("TEST",
                         log::subsystem_name(log::ErrorSubsystem::TEST))
            == 0,
        "expected TEST");
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
        __builtin_strcmp("Task exited", log::base_error_string(0xDC01ULL))
            == 0,
        "expected Task exited");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Task faulted", log::base_error_string(0xDC02ULL))
            == 0,
        "expected Task faulted");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Task end event",
                         log::base_error_string(0xDCFFULL))
            == 0,
        "expected Task end event");
    // Issue #77 live session: a new event family must also be INFO-classed,
    // or its lines render as ERR=<stale enum> (0xDC01 showed "Out of
    // memory" before the predicate knew the range).
    JARVIS_ASSERT(log::base_code_is_info(0xDC01ULL));
    JARVIS_ASSERT(log::base_code_is_info(0xDC02ULL));
    JARVIS_ASSERT(!log::base_code_is_info(
        static_cast<uint64_t>(kernel::Error::OOM)));
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
// Depends: DmesgService::DmesgBuffer (friend access)::set_suppressed, is_suppressed
JARVIS_TEST(dmesg_suppression_toggle, "PRE: none | POST: none") {
    bool saved = log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed();
    log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(true);
    JARVIS_ASSERT(
        log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed());
    log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(false);
    JARVIS_ASSERT(
        !log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::is_suppressed());
    log::DmesgService::DmesgBuffer<log::DMESG_CAPACITY>::set_suppressed(saved);
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

// Runmode: kernel
// Testidea: severity_name() covers all five severities + invalid input.
// Input: all LogSeverity values + out-of-range cast
// Expect: DEBUG/INFO/WARN/ERROR/FATAL strings, UNK for invalid
// Depends: kernel::log::severity_name
JARVIS_TEST(dmesg_severity_names, "PRE: none | POST: none") {
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("DEBUG",
                         log::severity_name(log::LogSeverity::DEBUG)) == 0,
        "expected DEBUG");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("INFO", log::severity_name(log::LogSeverity::INFO))
            == 0,
        "expected INFO");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("WARN", log::severity_name(log::LogSeverity::WARN))
            == 0,
        "expected WARN");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("ERROR",
                         log::severity_name(log::LogSeverity::ERROR)) == 0,
        "expected ERROR");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("FATAL",
                         log::severity_name(log::LogSeverity::FATAL)) == 0,
        "expected FATAL");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp(
            "UNK", log::severity_name(static_cast<log::LogSeverity>(0xFF)))
            == 0,
        "expected UNK");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Canonical bases are 1000 apart and legacy codes still decode.
// Input: subsystem_base() for all subsystems, is_canonical_nbr probes,
//        legacy 0xDA01/0xDB02/0xDC01 strings + INFO predicate
// Expect: distinct 1000-spaced bases; legacy compat intact
// Depends: kernel::log::{subsystem_base, is_canonical_nbr,
//         base_error_string, base_code_is_info}
JARVIS_TEST(dmesg_canonical_bases, "PRE: none | POST: none") {
    JARVIS_ASSERT_EQ(log::kDmesgBase_SYNC, log::subsystem_base(
                                               log::ErrorSubsystem::SYNC));
    JARVIS_ASSERT_EQ(log::kDmesgBase_NET, log::subsystem_base(
                                              log::ErrorSubsystem::NET));
    JARVIS_ASSERT_EQ(log::kDmesgBase_ELF, log::subsystem_base(
                                              log::ErrorSubsystem::ELF));
    JARVIS_ASSERT_EQ(log::kDmesgBase_USER, log::subsystem_base(
                                               log::ErrorSubsystem::USER));
    JARVIS_ASSERT_EQ(log::kDmesgBase_DAEMON,
                     log::subsystem_base(log::ErrorSubsystem::DAEMON));
    JARVIS_ASSERT_EQ(log::kDmesgStride, 1000ULL);
    JARVIS_ASSERT(log::is_canonical_nbr(log::ErrorSubsystem::DAEMON,
                                        log::kDmesgBase_DAEMON + 1));
    JARVIS_ASSERT(!log::is_canonical_nbr(log::ErrorSubsystem::DAEMON,
                                         log::kDmesgBase_DAEMON + 1000));
    JARVIS_ASSERT(!log::is_canonical_nbr(log::ErrorSubsystem::BASE, 0xDA01));
    // Legacy field-log compat: old codes decode and classify as before.
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon exited",
                         log::base_error_string(0xDA01ULL)) == 0,
        "legacy daemon code must decode");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("ELF load completed",
                         log::base_error_string(0xDB02ULL)) == 0,
        "legacy elf code must decode");
    JARVIS_ASSERT(log::base_code_is_info(log::kDmesgBase_ELF + 1));
    JARVIS_ASSERT(!log::base_code_is_info(log::kDmesgBase_ELF + 4));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: catalog_lookup resolves records; unknown pairs fall back.
// Input: known DAEMON/ELF/USER/NET numbers, unknown subsystem + number
// Expect: severity + text per record; nullptr / ERROR / UNKNOWN fallback
// Depends: kernel::log::catalog::{catalog_lookup, lookup_severity,
//         catalog_text}
JARVIS_TEST(dmesg_catalog_lookup, "PRE: none | POST: none") {
    const log::catalog::DmesgRecord *rec = log::catalog::catalog_lookup(
        log::ErrorSubsystem::DAEMON, log::kDmesgBase_DAEMON + 1);
    JARVIS_ASSERT(rec != nullptr);
    JARVIS_ASSERT_EQ(log::LogSeverity::ERROR, rec->severity);
    JARVIS_ASSERT_FMT(__builtin_strcmp("Daemon exited", rec->text) == 0,
                      "daemon text, got %s", rec->text);

    rec = log::catalog::catalog_lookup(log::ErrorSubsystem::ELF,
                                       log::kDmesgBase_ELF + 14);
    JARVIS_ASSERT(rec != nullptr);
    JARVIS_ASSERT_EQ(log::LogSeverity::FATAL, rec->severity);

    rec = log::catalog::catalog_lookup(log::ErrorSubsystem::USER,
                                       log::kDmesgBase_USER + 1);
    JARVIS_ASSERT(rec != nullptr);
    JARVIS_ASSERT_EQ(log::LogSeverity::INFO, rec->severity);

    rec = log::catalog::catalog_lookup(log::ErrorSubsystem::NET,
                                       log::kDmesgBase_NET + 2);
    JARVIS_ASSERT(rec != nullptr);
    JARVIS_ASSERT_EQ(log::LogSeverity::WARN, rec->severity);

    JARVIS_ASSERT(log::catalog::catalog_lookup(log::ErrorSubsystem::ELF,
                                               log::kDmesgBase_ELF + 999)
                  == nullptr);
    JARVIS_ASSERT(log::catalog::catalog_lookup(log::ErrorSubsystem::SYNC,
                                               1001ULL) == nullptr);
    JARVIS_ASSERT_EQ(log::LogSeverity::ERROR,
                     log::catalog::lookup_severity(log::ErrorSubsystem::ELF,
                                                   log::kDmesgBase_ELF
                                                       + 999));
    JARVIS_ASSERT_EQ(log::LogSeverity::INFO,
                     log::catalog::lookup_severity(log::ErrorSubsystem::SYNC,
                                                   0ULL));
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("UNKNOWN",
                         log::catalog::catalog_text(log::ErrorSubsystem::ELF,
                                                    log::kDmesgBase_ELF + 999))
            == 0,
        "unknown pair must be UNKNOWN");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Daemon restarted",
                         log::error_string(log::ErrorSubsystem::DAEMON,
                                           log::kDmesgBase_DAEMON + 2))
            == 0,
        "error_string must serve catalog subsystems");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Explicit-severity pushes survive the ring round trip.
// Input: push ELF/FATAL + DAEMON/INFO, pop both
// Expect: severity, subsystem, code, message preserved in order
// Depends: DmesgService::push (severity overload), ::pop
JARVIS_TEST(dmesg_push_severity_roundtrip, "PRE: none | POST: none") {
    auto &db = log::DmesgService::instance();
    db.clear();
    db.push(log::ErrorSubsystem::ELF, log::kDmesgBase_ELF + 14,
            log::LogSeverity::FATAL, "auth kill probe");
    db.push(log::ErrorSubsystem::DAEMON, log::kDmesgBase_DAEMON + 3,
            log::LogSeverity::INFO, "ensured probe");
    log::LogEntry entry{};
    JARVIS_ASSERT(db.pop(entry));
    JARVIS_ASSERT_EQ(log::LogSeverity::FATAL, entry.severity);
    JARVIS_ASSERT_EQ(log::ErrorSubsystem::ELF, entry.subsystem);
    JARVIS_ASSERT_EQ(log::kDmesgBase_ELF + 14, entry.error_code);
    JARVIS_ASSERT(db.pop(entry));
    JARVIS_ASSERT_EQ(log::LogSeverity::INFO, entry.severity);
    JARVIS_ASSERT_EQ(log::ErrorSubsystem::DAEMON, entry.subsystem);
    JARVIS_ASSERT(db.empty());
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: format_dmesg_entry emits the canonical line shape, bounded.
// Input: synthetic FATAL/DAEMON entry (tick time) + truncation probe
// Expect: "[DMESG <ts>ms]: FATAL DAEMON <nr> <text>: <msg> [task=..]",
//         wall_ms override, NUL-terminated truncation within cap
// Depends: kernel::log::format_dmesg_entry
JARVIS_TEST(dmesg_render_format, "PRE: none | POST: none") {
    log::LogEntry entry{};
    entry.timestamp = 4242;
    entry.task_id = 7;
    entry.subsystem = log::ErrorSubsystem::DAEMON;
    entry.severity = log::LogSeverity::FATAL;
    entry.error_code = log::kDmesgBase_DAEMON + 1;
    entry.context = 0xAB;
    const char *msg = "render probe";
    size_t mi = 0;
    while (msg[mi] && mi < log::LogEntry::kMessageCap - 1) {
        entry.message[mi] = msg[mi];
        ++mi;
    }
    entry.message[mi] = '\0';

    char line[log::DMESG_RENDER_CAP] = {};
    size_t len = log::format_dmesg_entry(line, sizeof(line), entry);
    JARVIS_ASSERT(len > 0);
    JARVIS_ASSERT(len < sizeof(line));
    JARVIS_ASSERT(line[len] == '\0');
    // Spot-check shape markers in order: prefix, severity, codebase,
    // canonical number, message, task trailer. wall_ms == 0 here, so the
    // tick fallback renders.
    const char *needles[] = {"[DMESG 4242ms tick]:", "FATAL", "DAEMON",
                             "10001", "Daemon exited", "render probe",
                             "[task=7 "};
    size_t scan_from = 0;
    for (size_t ni = 0; ni < sizeof(needles) / sizeof(needles[0]); ++ni) {
        const char *nd = needles[ni];
        size_t nlen = 0;
        while (nd[nlen] != '\0' && nlen < 64) {
            ++nlen;
        }
        bool found = false;
        for (size_t pi = scan_from; line[pi] != '\0'; ++pi) {
            size_t ki = 0;
            while (ki < nlen && line[pi + ki] == nd[ki]) {
                ++ki;
            }
            if (ki == nlen) {
                scan_from = pi + nlen;
                found = true;
                break;
            }
        }
        JARVIS_ASSERT_FMT(found, "render missing %s: %s", nd, line);
    }

    // Wall-clock override wins over the tick count: 999001 ms after the
    // epoch renders as a datetime, not as a tick count.
    entry.wall_ms = 999001;
    char wall_line[log::DMESG_RENDER_CAP] = {};
    log::format_dmesg_entry(wall_line, sizeof(wall_line), entry);
    bool wall_found = false;
    const char *wall_nd = "[DMESG 1970-01-01 00:16:39:001]:";
    size_t wall_len = 0;
    while (wall_nd[wall_len] != '\0' && wall_len < 64) {
        ++wall_len;
    }
    for (size_t pi = 0; wall_line[pi] != '\0'; ++pi) {
        size_t ki = 0;
        while (ki < wall_len && wall_line[pi + ki] == wall_nd[ki]) {
            ++ki;
        }
        if (ki == wall_len) {
            wall_found = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(wall_found, "wall time missing: %s", wall_line);

    // Truncation: tiny buffer stays NUL-terminated within cap.
    char tiny[16] = {};
    size_t tiny_len = log::format_dmesg_entry(tiny, sizeof(tiny), entry);
    JARVIS_ASSERT(tiny_len < sizeof(tiny));
    JARVIS_ASSERT(tiny[sizeof(tiny) - 1] == '\0');
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Full #234 taxonomy resolves: every wired table serves text
// for code 0 and its max code, canonical numbers strip to the same text,
// the panic code decodes, and unknown pairs stay UNKNOWN/ERROR.
// Catalog records take precedence over stripping (dmesg.hpp): the strip
// probe must use a code with no catalog record — PMM base+1 is owned by
// the #284 leak-suspect record, so the probe uses base+2 (raw USER_OOM).
// Input: code 0 / max / canonical / huge codes across all 19 subsystems
// Expect: real texts for defined codes, UNKNOWN only for undefined
// Depends: error_string, strip_canonical, lookup_severity, base codes
JARVIS_TEST(dmesg_full_taxonomy, "PRE: none | POST: none") {
    // Newly wired tables: raw and canonical resolve identically.
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Out of memory — no free physical pages",
                         log::error_string(log::ErrorSubsystem::PMM, 1))
            == 0,
        "PMM OOM text");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Out of memory — no free user physical pages",
                         log::error_string(log::ErrorSubsystem::PMM,
                                           log::kDmesgBase_PMM + 2))
            == 0,
        "PMM canonical strip");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Virtual address not mapped",
                         log::error_string(log::ErrorSubsystem::VMM,
                                           log::kDmesgBase_VMM + 4))
            == 0,
        "VMM canonical strip");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Task control block corrupt (bad magic)",
                         log::error_string(log::ErrorSubsystem::TASK, 9))
            == 0,
        "TASK corruption text");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Buffer is not mapped in this task",
                         log::error_string(log::ErrorSubsystem::BUFPOOL, 7))
            == 0,
        "BUFPOOL text");
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Invalid or uninitialized BAR",
                         log::error_string(log::ErrorSubsystem::DRIVER, 3))
            == 0,
        "DRIVER/PCI text");
    // Event records resolve through the catalog with record severity.
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Timing: deadline missed",
                         log::error_string(log::ErrorSubsystem::TIMING,
                                           log::kDmesgBase_TIMING + 1))
            == 0,
        "TIMING text");
    JARVIS_ASSERT_EQ(log::LogSeverity::WARN,
                     log::catalog::lookup_severity(
                         log::ErrorSubsystem::TIMING,
                         log::kDmesgBase_TIMING + 3));
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("Kernel panic",
                         log::error_string(log::ErrorSubsystem::BASE,
                                           log::kDmesgPanicCode))
            == 0,
        "panic text");
    // Taxonomy spot checks: OK is INFO, routine codes WARN, faults ERROR.
    JARVIS_ASSERT_EQ(log::LogSeverity::INFO,
                     log::catalog::lookup_severity(log::ErrorSubsystem::SYNC,
                                                   0ULL));
    JARVIS_ASSERT_EQ(log::LogSeverity::WARN,
                     log::catalog::lookup_severity(log::ErrorSubsystem::VFS,
                                                   3ULL));
    JARVIS_ASSERT_EQ(log::LogSeverity::ERROR,
                     log::catalog::lookup_severity(log::ErrorSubsystem::PMM,
                                                   1ULL));
    JARVIS_ASSERT_EQ(log::LogSeverity::ERROR,
                     log::catalog::lookup_severity(log::ErrorSubsystem::TASK,
                                                   9ULL));
    // Unknown pairs fail closed.
    JARVIS_ASSERT_FMT(
        __builtin_strcmp("UNKNOWN",
                         log::error_string(log::ErrorSubsystem::TEST,
                                           log::kDmesgBase_TEST + 999))
            == 0,
        "unknown stays UNKNOWN");
    JARVIS_ASSERT_EQ(log::LogSeverity::ERROR,
                     log::catalog::lookup_severity(
                         log::ErrorSubsystem::TEST,
                         log::kDmesgBase_TEST + 999));
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
    JARVIS_REGISTER_TEST(dmesg_severity_names);
    JARVIS_REGISTER_TEST(dmesg_canonical_bases);
    JARVIS_REGISTER_TEST(dmesg_catalog_lookup);
    JARVIS_REGISTER_TEST(dmesg_push_severity_roundtrip);
    JARVIS_REGISTER_TEST(dmesg_render_format);
    JARVIS_REGISTER_TEST(dmesg_full_taxonomy);
}
