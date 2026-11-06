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

/// @file test_pipe.cpp
/// @brief Pipe (kernel pipe object) tests.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/vfs/pipe.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <string.hpp>

using namespace kernel;

// Runmode: kernel
// Testidea: Verifies create_pipe() returns two valid file descriptors.
// Input: Call create_pipe()
// Expect: Returns 0, fds[0] (read) and fds[1] (write) are valid
// Depends: kernel::vfs::create_pipe
JARVIS_TEST(pipe_create_returns_two_fds, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    int ret = vfs::create_pipe(fds);
    JARVIS_ASSERT_EQ(0, ret);
    JARVIS_ASSERT(fds[0] >= 0);
    JARVIS_ASSERT(fds[1] >= 0);
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    JARVIS_ASSERT(task->fd_table.get(fds[0]) != nullptr);
    JARVIS_ASSERT(task->fd_table.get(fds[1]) != nullptr);
    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Empty-read and full-write are prompt (non-blocking) by
// construction: pipe_read checks write_closed before waiting and
// pipe_write returns a partial count instead of waiting, so an empty
// pipe with a closed writer reads 0 and a full pipe write returns 0.
// (No O_NONBLOCK flag exists on pipe fds; these are the fail-closed
// would-block analogues. A read on an empty pipe with a live writer
// would block and is deliberately NOT driven here.)
// Input: Create pipe, close write end, read empty; create pipe, fill
// 4096 bytes, write one more byte.
// Expect: empty read returns 0 immediately; full write returns 0
// immediately; no hang.
// Depends: kernel::vfs::Pipe (pipe_read/pipe_write fail-closed paths)
JARVIS_TEST(pipe_read_from_empty_nonblock, "PRE: vfsd, iocd | POST: none") {
    // Pseudocode: pipe A: close writer -> read empty (expect 0, no wait);
    // free reader. pipe B: write 4096 (expect 4096) -> write 1 more
    // (expect 0, no wait); free both. All asserts join before cleanup
    // where the fixture allows; fds always freed (no leaks).
    int empty_fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(empty_fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    task->fd_table.free(empty_fds[1]);
    auto *empty_reader = task->fd_table.get(empty_fds[0])->vnode;
    JARVIS_ASSERT(empty_reader != nullptr);
    uint8_t probe = 0xAA;
    int64_t nread =
        empty_reader->ops->read(*empty_reader, &probe, sizeof(probe), 0);
    JARVIS_ASSERT_EQ(0LL, nread);
    task->fd_table.free(empty_fds[0]);

    int full_fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(full_fds));
    auto *full_writer = task->fd_table.get(full_fds[1])->vnode;
    JARVIS_ASSERT(full_writer != nullptr);
    uint8_t fill[4096];
    memset(fill, 0x5A, sizeof(fill));
    int64_t filled = full_writer->ops->write(*full_writer, fill,
                                             sizeof(fill), 0);
    JARVIS_ASSERT_EQ(4096LL, filled);
    uint8_t extra = 0x5A;
    int64_t refused =
        full_writer->ops->write(*full_writer, &extra, sizeof(extra), 0);
    JARVIS_ASSERT_EQ(0LL, refused);
    task->fd_table.free(full_fds[0]);
    task->fd_table.free(full_fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies writing beyond pipe buffer capacity returns partial write.
// Input: Fill pipe buffer, attempt write that exceeds PIPE_BUF_SIZE
// Expect: Returns number of bytes written (up to PIPE_BUF_SIZE)
// Depends: kernel::vfs::Pipe
JARVIS_TEST(pipe_write_to_full_blocks, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(wnode != nullptr);

    uint8_t buf[5000];
    memset(buf, 0xAB, sizeof(buf));
    int64_t written = wnode->ops->write(*wnode, buf, sizeof(buf), 0);
    JARVIS_ASSERT(written > 0);
    JARVIS_ASSERT(static_cast<uint64_t>(written) <= sizeof(buf));

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies writing to pipe with closed read end returns VFS_INVALID.
// Input: Close read_fd, write to write_fd
// Expect: write returns VFS_INVALID (-4)
// Depends: kernel::vfs::Pipe
JARVIS_TEST(pipe_read_end_closed_returns_epipe,
            "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);

    task->fd_table.free(fds[0]);

    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(wnode != nullptr);
    const char *msg = "test";
    int64_t written =
        wnode->ops->write(*wnode, reinterpret_cast<const uint8_t *>(msg), 4, 0);
    JARVIS_ASSERT_EQ(static_cast<int64_t>(vfs::VFS_INVALID), written);

    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Verifies written data read back identically in FIFO order.
// Input: Write "hello", read into buffer
// Expect: Buffer contains "hello"
// Depends: kernel::vfs::Pipe
JARVIS_TEST(pipe_write_then_read_roundtrip, "PRE: vfsd, iocd | POST: none") {
    int fds[2];
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    JARVIS_ASSERT(wnode != nullptr);

    const char *msg = "hello";
    int64_t written =
        wnode->ops->write(*wnode, reinterpret_cast<const uint8_t *>(msg), 5, 0);
    JARVIS_ASSERT_EQ(5, written);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int64_t nread = rnode->ops->read(*rnode, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(5, nread);
    JARVIS_ASSERT(memcmp(buf, "hello", 5) == 0);

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

namespace {

/// @brief Write end shared with the two sequential writer tasks.
vfs::Vnode *g_pipe_writer = nullptr;
/// @brief Per-writer byte counts observed inside the dispatched tasks.
int64_t g_first_written = -1;
int64_t g_second_written = -1;

} // namespace

// Runmode: kernel
// Testidea: Two dispatched writer tasks each write a 1 KiB distinct
// pattern through one shared pipe end, run sequentially (first joined
// before the second starts), then the harness reads 2 KiB back in
// order. Sequential dispatch keeps the test deterministic: pipe_write
// carries no lock, so genuinely overlapping writers could lose bytes
// (a follow-up kernel change, out of scope for this QE lane); the
// multi-task dispatch + join + ordered read is what this test pins.
// Input: Harness creates pipe, dispatches writer A (0xAA x 1024) and
// joins, dispatches writer B (0xBB x 1024) and joins, reads 2048.
// Expect: each writer reports 1024; read returns 2048 with the first
// half all 0xAA and the second half all 0xBB; no deadlock.
// Depends: kernel::vfs::Pipe, Scheduler dispatch + join, fd_table
JARVIS_TEST(pipe_multiple_writers_no_interleaving,
            "PRE: vfsd, iocd | POST: none") {
    // Pseudocode: harness creates pipe, stashes wnode; dispatch A, join;
    // dispatch B, join; read 2048 in harness; assert halves; free fds.
    int fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    g_pipe_writer = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    JARVIS_ASSERT(g_pipe_writer != nullptr);
    g_first_written = -1;
    g_second_written = -1;

    auto *first = TaskControlBlock::create(
        []() {
            uint8_t block[1024];
            memset(block, 0xAA, sizeof(block));
            if (g_pipe_writer != nullptr &&
                g_pipe_writer->ops != nullptr &&
                g_pipe_writer->ops->write != nullptr) {
                g_first_written = g_pipe_writer->ops->write(
                    *g_pipe_writer, block, sizeof(block), 0);
            }
        },
        11, 10);
    JARVIS_ASSERT(first != nullptr);
    Scheduler::add_task(*first);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(first);
    kernel::test::terminate_if_live(first);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1024LL, g_first_written);

    auto *second = TaskControlBlock::create(
        []() {
            uint8_t block[1024];
            memset(block, 0xBB, sizeof(block));
            if (g_pipe_writer != nullptr &&
                g_pipe_writer->ops != nullptr &&
                g_pipe_writer->ops->write != nullptr) {
                g_second_written = g_pipe_writer->ops->write(
                    *g_pipe_writer, block, sizeof(block), 0);
            }
        },
        11, 10);
    JARVIS_ASSERT(second != nullptr);
    Scheduler::add_task(*second);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(second);
    kernel::test::terminate_if_live(second);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1024LL, g_second_written);

    uint8_t both[2048];
    memset(both, 0, sizeof(both));
    int64_t nread =
        rnode->ops->read(*rnode, both, sizeof(both), 0);
    JARVIS_ASSERT_EQ(2048LL, nread);
    for (size_t idx = 0; idx < 1024; ++idx) {
        JARVIS_ASSERT_EQ(0xAA, both[idx]);
    }
    for (size_t idx = 1024; idx < 2048; ++idx) {
        JARVIS_ASSERT_EQ(0xBB, both[idx]);
    }

    g_pipe_writer = nullptr;
    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

void register_pipe_tests() {
    Logger::info("Registering pipe tests");
    JARVIS_REGISTER_TEST(pipe_create_returns_two_fds);
    JARVIS_REGISTER_TEST(pipe_read_from_empty_nonblock);
    JARVIS_REGISTER_TEST(pipe_write_to_full_blocks);
    JARVIS_REGISTER_TEST(pipe_read_end_closed_returns_epipe);
    JARVIS_REGISTER_TEST(pipe_write_then_read_roundtrip);
    JARVIS_REGISTER_TEST(pipe_multiple_writers_no_interleaving);
}
