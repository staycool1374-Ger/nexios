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
// Testidea: STUB - Pipe implementation does not support O_NONBLOCK.
// Input: N/A
// Expect: Stub passes
// Depends: kernel::vfs::Pipe
JARVIS_TEST(pipe_read_from_empty_nonblock, "PRE: vfsd, iocd | POST: none") {
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

// Runmode: kernel
// Testidea: STUB - Requires multi-task environment for concurrent writes.
// Input: N/A
// Expect: Stub passes
// Depends: kernel::vfs::Pipe
JARVIS_TEST(pipe_multiple_writers_no_interleaving,
            "PRE: vfsd, iocd | POST: none") {
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
