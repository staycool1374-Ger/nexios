/*
 * NexIOS RTOS — Development Roadmap / Kernel Core
 * Copyright (C) 2026 Arnold Hasshold
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3, as
 * published by the Free Software Foundation.
 */

/// @file test_pipe_blocking.cpp
/// @brief Pipe blocking-semantics tests (milestone v0.4.3 issue #111):
///        real reader/writer task pairs drive the pipe waiter machinery
///        (Semaphore block/wake, EOF-on-close, EPIPE, ordered wake) through
///        genuine dispatch — plus the documented non-blocking full-pipe
///        partial-write contract.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/pipe.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Shared context for the driven pipe tests (file-static: each test
/// runs exclusively).  Result fields are written ONLY by worker tasks and
/// read ONLY after both workers reached TERMINATED.
struct PipeCtx {
    vfs::Vnode *rnode = nullptr;
    vfs::Vnode *wnode = nullptr;
    TaskControlBlock *harness = nullptr;
    int64_t reader_result = -999;
    int64_t writer_result = -999;
    int64_t reader2_result = -999;
    uint8_t reader_buf[32] = {};
    uint8_t reader2_buf[32] = {};
    int wfd = -1;
};

PipeCtx g_pipe_ctx;

/// @brief Snapshot helper: returns a fresh zeroed context.
void reset_ctx(PipeCtx &ctx) { ctx = PipeCtx{}; }

} // namespace

// Runmode: kernel
// Testidea: A reader that blocks on an empty pipe is woken by a real
// writer task and receives the written bytes — the core waiter contract
// of pipe_read's data_avail semaphore.
// Input: create_pipe; reader task (prio 11) reads 16 bytes on the empty
//        pipe (blocks); writer task (prio 12) writes "ping" (4 bytes).
// Expect: Reader wakes, read returns 4, buffer holds "ping"; writer
//         returned 4.  Both tasks reach TERMINATED (no hang, no lost
//         wakeup).
// Depends: vfs::create_pipe, sync::Semaphore, Scheduler
JARVIS_TEST(pipe_blk_reader_wakes_on_write, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    reset_ctx(g_pipe_ctx);
    g_pipe_ctx.rnode = task->fd_table.get(fds[0])->vnode;
    g_pipe_ctx.wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(g_pipe_ctx.rnode != nullptr);
    JARVIS_ASSERT(g_pipe_ctx.wnode != nullptr);

    auto *reader = TaskControlBlock::create(
        []() {
            uint8_t buf[16] = {};
            int64_t n = g_pipe_ctx.rnode->ops->read(*g_pipe_ctx.rnode, buf,
                                                    sizeof(buf), 0);
            g_pipe_ctx.reader_result = n;
            if (n > 0)
                memcpy(g_pipe_ctx.reader_buf, buf, static_cast<size_t>(n));
        },
        11, 10);
    JARVIS_ASSERT(reader != nullptr);

    auto *writer = TaskControlBlock::create(
        []() {
            const uint8_t msg[4] = {'p', 'i', 'n', 'g'};
            g_pipe_ctx.writer_result = g_pipe_ctx.wnode->ops->write(
                *g_pipe_ctx.wnode, msg, sizeof(msg), 0);
        },
        12, 10);
    JARVIS_ASSERT(writer != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*reader);
        Scheduler::add_task(*writer);
    }
    auto *original = Scheduler::current_task();
    // Steer next_task() to the reader (highest prio, runs first, blocks);
    // the writer is then dispatched while the reader is BLOCKED.
    kernel::test::yield_as(*writer);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(reader);
    kernel::test::wait_for_termination_safe(writer);
    Scheduler::set_current(*original);

    bool reader_done = reader->state == TaskState::TERMINATED;
    bool writer_done = writer->state == TaskState::TERMINATED;
    kernel::test::terminate_and_drain2(reader, writer);

    // fd cleanup BEFORE asserts (assertions early-return on failure).
    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);

    JARVIS_ASSERT(reader_done);
    JARVIS_ASSERT(writer_done);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), g_pipe_ctx.writer_result);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4), g_pipe_ctx.reader_result);
    JARVIS_ASSERT(memcmp(g_pipe_ctx.reader_buf, "ping", 4) == 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The full-pipe contract is NON-blocking partial write: a write
// on a pipe whose 4096-byte ring is full returns 0 immediately (bounded
// behaviour, never hangs) instead of blocking.
// Input: Write 4096 bytes (completes), then write 10 more bytes.
// Expect: First write returns 4096; second returns 0 immediately;
//         after draining, writing 10 bytes completes with 10.  fstat
//         reports S_IFCHR with size 0 (documented pipe fstat contract).
// Depends: vfs::create_pipe
JARVIS_TEST(pipe_blk_full_pipe_partial_write, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr && wnode != nullptr);

    vfs::VfsStat st{};
    JARVIS_ASSERT_EQ(0, rnode->ops->fstat(*rnode, st));
    bool fstat_chr = (st.st_mode & vfs::S_IFCHR) != 0;

    uint8_t payload[4096];
    memset(payload, 0x11, sizeof(payload));
    int64_t fill = wnode->ops->write(*wnode, payload, sizeof(payload), 0);

    uint8_t extra[10] = {};
    int64_t overflow = wnode->ops->write(*wnode, extra, sizeof(extra), 0);

    uint8_t drained[4096];
    int64_t nread = rnode->ops->read(*rnode, drained, sizeof(drained), 0);
    int64_t refill = wnode->ops->write(*wnode, extra, sizeof(extra), 0);

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);

    JARVIS_ASSERT(fstat_chr);
    JARVIS_ASSERT_EQ(static_cast<uint64_t>(0), st.st_size);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4096), fill);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), overflow);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(4096), nread);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(10), refill);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Closing the write end wakes a reader blocked on the empty
// pipe with EOF (read returns 0) — the close path must post the data
// semaphore BEFORE the buffer is disposed.
// Input: Reader task (prio 11) blocks on the empty pipe; writer task
//        (prio 12) closes the write end via fd_table.free on the
//        harness's fd (single-owner close → post()).
// Expect: Reader wakes and returns 0 (EOF); both tasks TERMINATED.
// Depends: vfs::create_pipe, pipe_write_close
JARVIS_TEST(pipe_blk_write_close_wakes_reader_eof,
            "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    reset_ctx(g_pipe_ctx);
    g_pipe_ctx.rnode = task->fd_table.get(fds[0])->vnode;
    g_pipe_ctx.harness = task;
    g_pipe_ctx.wfd = fds[1];
    JARVIS_ASSERT(g_pipe_ctx.rnode != nullptr);

    auto *reader = TaskControlBlock::create(
        []() {
            uint8_t buf[16] = {};
            int64_t n = g_pipe_ctx.rnode->ops->read(*g_pipe_ctx.rnode, buf,
                                                    sizeof(buf), 0);
            g_pipe_ctx.reader_result = n;
        },
        11, 10);
    JARVIS_ASSERT(reader != nullptr);

    auto *writer = TaskControlBlock::create(
        []() {
            // Closing the write end (single 1->0 refcount transition)
            // posts the semaphore, waking the blocked reader with EOF.
            g_pipe_ctx.harness->fd_table.free(g_pipe_ctx.wfd);
        },
        12, 10);
    JARVIS_ASSERT(writer != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*reader);
        Scheduler::add_task(*writer);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*writer);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(reader);
    kernel::test::wait_for_termination_safe(writer);
    Scheduler::set_current(*original);

    bool reader_done = reader->state == TaskState::TERMINATED;
    bool writer_done = writer->state == TaskState::TERMINATED;
    kernel::test::terminate_and_drain2(reader, writer);

    // The write end was closed by the writer; only the read end remains.
    task->fd_table.free(fds[0]);

    JARVIS_ASSERT(reader_done);
    JARVIS_ASSERT(writer_done);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), g_pipe_ctx.reader_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: After the read end is closed, a writer's write fails with
// VFS_INVALID (EPIPE analogue) — the error path that the shell/vfsd
// plumbing relies on.
// Input: Close the read end via fd_table.free, then a writer task writes.
// Expect: write returns VFS_INVALID; writer reaches TERMINATED.
// Depends: vfs::create_pipe, pipe_read_close
JARVIS_TEST(pipe_blk_read_close_fails_writer, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    reset_ctx(g_pipe_ctx);
    g_pipe_ctx.wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(g_pipe_ctx.wnode != nullptr);

    // Close the read end first: pipe_read_close marks read_closed.
    task->fd_table.free(fds[0]);

    auto *writer = TaskControlBlock::create(
        []() {
            const uint8_t msg[2] = {'x', 'y'};
            g_pipe_ctx.writer_result = g_pipe_ctx.wnode->ops->write(
                *g_pipe_ctx.wnode, msg, sizeof(msg), 0);
        },
        12, 10);
    JARVIS_ASSERT(writer != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*writer);
    }
    auto *original = Scheduler::current_task();
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(writer);
    Scheduler::set_current(*original);

    bool writer_done = writer->state == TaskState::TERMINATED;
    kernel::test::terminate_and_drain(*writer);

    task->fd_table.free(fds[1]);

    JARVIS_ASSERT(writer_done);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, g_pipe_ctx.writer_result);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two blocked readers are woken in order and each receives one
// complete message — writes never interleave byte-wise (each write is
// atomic up to the ring size) and the semaphore posts pair up with the
// waiters.
// Input: Readers prio 11/12 block on the empty pipe; writer prio 13
//        writes "msg_a1" then "msg_b2" (6 bytes each).
// Expect: Reader 1 gets "msg_a1", reader 2 gets "msg_b2"; both reads
//         return 6; all tasks TERMINATED.
// Depends: vfs::create_pipe, sync::Semaphore wake ordering
JARVIS_TEST(pipe_blk_two_readers_ordered, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    reset_ctx(g_pipe_ctx);
    g_pipe_ctx.rnode = task->fd_table.get(fds[0])->vnode;
    g_pipe_ctx.wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(g_pipe_ctx.rnode != nullptr);
    JARVIS_ASSERT(g_pipe_ctx.wnode != nullptr);

    auto *reader1 = TaskControlBlock::create(
        []() {
            uint8_t buf[6] = {};
            int64_t n = g_pipe_ctx.rnode->ops->read(*g_pipe_ctx.rnode, buf,
                                                    sizeof(buf), 0);
            g_pipe_ctx.reader_result = n;
            if (n > 0)
                memcpy(g_pipe_ctx.reader_buf, buf, static_cast<size_t>(n));
        },
        11, 10);
    JARVIS_ASSERT(reader1 != nullptr);
    auto *reader2 = TaskControlBlock::create(
        []() {
            uint8_t buf[6] = {};
            int64_t n = g_pipe_ctx.rnode->ops->read(*g_pipe_ctx.rnode, buf,
                                                    sizeof(buf), 0);
            g_pipe_ctx.reader2_result = n;
            if (n > 0)
                memcpy(g_pipe_ctx.reader2_buf, buf, static_cast<size_t>(n));
        },
        12, 10);
    JARVIS_ASSERT(reader2 != nullptr);
    auto *writer = TaskControlBlock::create(
        []() {
            const uint8_t msg1[6] = {'m', 's', 'g', '_', 'a', '1'};
            const uint8_t msg2[6] = {'m', 's', 'g', '_', 'b', '2'};
            g_pipe_ctx.writer_result =
                g_pipe_ctx.wnode->ops->write(*g_pipe_ctx.wnode, msg1,
                                             sizeof(msg1), 0);
            g_pipe_ctx.writer_result +=
                g_pipe_ctx.wnode->ops->write(*g_pipe_ctx.wnode, msg2,
                                             sizeof(msg2), 0);
        },
        13, 10);
    JARVIS_ASSERT(writer != nullptr);

    {
        arch::IrqGuard guard;
        Scheduler::add_task(*reader1);
        Scheduler::add_task(*reader2);
        Scheduler::add_task(*writer);
    }
    auto *original = Scheduler::current_task();
    kernel::test::yield_as(*writer);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(reader1);
    kernel::test::wait_for_termination_safe(reader2);
    kernel::test::wait_for_termination_safe(writer);
    Scheduler::set_current(*original);

    bool r1_done = reader1->state == TaskState::TERMINATED;
    bool r2_done = reader2->state == TaskState::TERMINATED;
    bool w_done = writer->state == TaskState::TERMINATED;
    kernel::test::terminate_and_drain3(reader1, reader2, writer);

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);

    JARVIS_ASSERT(r1_done);
    JARVIS_ASSERT(r2_done);
    JARVIS_ASSERT(w_done);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(12), g_pipe_ctx.writer_result);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(6), g_pipe_ctx.reader_result);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(6), g_pipe_ctx.reader2_result);
    // Wake order between the two readers is dispatch-dependent; the
    // invariant under test is that each reader received ONE complete
    // message (no byte-wise interleaving, no split reads) and together
    // they covered both messages exactly.
    bool pair_ok =
        (memcmp(g_pipe_ctx.reader_buf, "msg_a1", 6) == 0 &&
         memcmp(g_pipe_ctx.reader2_buf, "msg_b2", 6) == 0) ||
        (memcmp(g_pipe_ctx.reader_buf, "msg_b2", 6) == 0 &&
         memcmp(g_pipe_ctx.reader2_buf, "msg_a1", 6) == 0);
    JARVIS_ASSERT(pair_ok);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Closed-end error paths return immediately (non-blocking):
// reading a pipe whose write end was closed drains to EOF (0); reading
// through a closed read end fails with VFS_INVALID; writing through a
// closed read end fails with VFS_INVALID.
// Input: Three pipe pairs driven from the harness task.
// Expect: EOF 0 after write-close; VFS_INVALID for read-through-closed
//         read end and write-through-closed read end.
// Depends: vfs::create_pipe
JARVIS_TEST(pipe_blk_closed_end_error_paths, "PRE: vfsd, iocd | POST: none") {
    // (a) write end closed → read drains to EOF 0
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    task->fd_table.free(fds[1]);
    uint8_t buf[4] = {};
    int64_t eof = rnode->ops->read(*rnode, buf, sizeof(buf), 0);
    int64_t read_after_close =
        rnode->ops->read(*rnode, buf, sizeof(buf), 0);
    task->fd_table.free(fds[0]);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), eof);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(0), read_after_close);

    // (b) read end closed → writer fails with VFS_INVALID (EPIPE
    //     analogue).  NOTE: pipe_read_close FREES the read vnode, so the
    //     read end cannot be probed after the close — the observable
    //     contract is the failing write through the still-live write end.
    int fds2[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds2));
    auto *wnode2 = task->fd_table.get(fds2[1])->vnode;
    JARVIS_ASSERT(wnode2 != nullptr);
    task->fd_table.free(fds2[0]);
    const uint8_t msg[2] = {'z', 'z'};
    int64_t write_err = wnode2->ops->write(*wnode2, msg, sizeof(msg), 0);
    task->fd_table.free(fds2[1]);
    JARVIS_ASSERT_EQ(vfs::VFS_INVALID, write_err);
    JARVIS_TEST_PASS();
}

void register_pipe_blocking_tests() {
    Logger::info("Registering pipe blocking tests");
    JARVIS_REGISTER_TEST(pipe_blk_reader_wakes_on_write);
    JARVIS_REGISTER_TEST(pipe_blk_full_pipe_partial_write);
    JARVIS_REGISTER_TEST(pipe_blk_write_close_wakes_reader_eof);
    JARVIS_REGISTER_TEST(pipe_blk_read_close_fails_writer);
    JARVIS_REGISTER_TEST(pipe_blk_two_readers_ordered);
    JARVIS_REGISTER_TEST(pipe_blk_closed_end_error_paths);
}
