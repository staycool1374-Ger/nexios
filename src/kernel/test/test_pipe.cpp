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
#include <kernel/arch/timer.hpp>
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

/// @brief Blocked-read fixtures (woken-reader test): reader blocks on the
///        empty pipe while a live writer waits for BLOCKED then writes.
vfs::Vnode *g_wake_reader_vnode = nullptr;
vfs::Vnode *g_wake_writer_vnode = nullptr;
TaskControlBlock *g_wake_reader_task = nullptr;
int64_t g_wake_nread = -1;
uint64_t g_wake_done = 0;
uint8_t g_wake_buf[64] = {0};

/// @brief Reader entry: starts blocked (empty pipe, live writer); the
///        semaphore wake inside pipe_read (data_avail.wait) must return
///        the bytes the writer pushed.
void pipe_wake_reader_entry() {
    if (g_wake_reader_vnode != nullptr && g_wake_reader_vnode->ops != nullptr &&
        g_wake_reader_vnode->ops->read != nullptr) {
        uint8_t local[64] = {0};
        g_wake_nread = g_wake_reader_vnode->ops->read(
            *g_wake_reader_vnode, local, sizeof(local), 0);
        memcpy(g_wake_buf, local, sizeof(g_wake_buf));
    }
    __atomic_store_n(&g_wake_done, 1ULL, __ATOMIC_RELEASE);
}

/// @brief Writer entry: spins (bounded) until the reader is BLOCKED, then
///        drives the data_avail.post() wake path deterministically.
void pipe_wake_writer_entry() {
    auto *reader = g_wake_reader_task;
    const uint64_t deadline = arch::Timer::ticks() + 500;
    while (reader != nullptr && reader->state != TaskState::BLOCKED &&
           arch::Timer::ticks() < deadline) {
        arch::pause();
    }
    uint8_t msg[5] = {'h', 'e', 'l', 'l', 'o'};
    if (g_wake_writer_vnode != nullptr &&
        g_wake_writer_vnode->ops != nullptr &&
        g_wake_writer_vnode->ops->write != nullptr) {
        g_wake_writer_vnode->ops->write(*g_wake_writer_vnode, msg, 5, 0);
    }
}

/// @brief Spin until the reader task records its result, bounded.
bool await_wake_done(uint64_t deadline) {
    while (__atomic_load_n(&g_wake_done, __ATOMIC_ACQUIRE) == 0 &&
           arch::Timer::ticks() < deadline) {
        arch::pause();
    }
    return __atomic_load_n(&g_wake_done, __ATOMIC_ACQUIRE) == 1;
}

} // namespace

// Runmode: kernel
// Testidea: Issue #474 "without O_NONBLOCK should block until read drains"
// analogue: a read on an EMPTY pipe with a LIVE writer blocks in
// pipe_read's data_avail.wait() (pipe.cpp:81-87) and is woken by a real
// writer task pushing data. This is the only path exercising the
// semaphore wake; nothing in the tree drives it today.
// Input: Reader task (FIXED prio 12) reads the empty pipe and blocks;
// the harness then observes BLOCKED deterministically (no writer exists
// to wake the reader yet) and only afterwards dispatches the writer,
// whose "hello" write posts data_avail and unblocks the reader.
// Expect: harness sees reader BLOCKED; reader wakes and reports 5 bytes
// = "hello"; both tasks join; fds freed; no hang, no leaks.
// Depends: kernel::vfs::Pipe read + data_avail semaphore, Scheduler
// dispatch + join.
// NOTE: Kernel-side finding for the dev lane — pipe_read/pipe_write
// ignore the open-flags argument entirely, so the issue's O_NONBLOCK
// (ERR_WOULD_BLOCK) contract is NOT implemented (O_NONBLOCK=0x800 exists
// in vfs.hpp but pipes never consult it). Not testable at this surface.
JARVIS_TEST(pipe_read_block_woken_by_writer, "PRE: vfsd, iocd | POST: none") {
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);

    int fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    g_wake_reader_vnode = task->fd_table.get(fds[0])->vnode;
    g_wake_writer_vnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(g_wake_reader_vnode != nullptr);
    JARVIS_ASSERT(g_wake_writer_vnode != nullptr);

    g_wake_nread = -1;
    g_wake_done = 0;
    memset(g_wake_buf, 0, sizeof(g_wake_buf));

    // Stage 1: dispatch the reader ALONE. With a live writer end and an
    // empty buffer, read() can only exit via data_avail.wait() -> BLOCKED.
    auto *reader = TaskControlBlock::create(pipe_wake_reader_entry, 12, 10);
    JARVIS_ASSERT(reader != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*reader, SchedPolicy::FIXED));
    g_wake_reader_task = reader;
    Scheduler::add_task(*reader);
    Scheduler::reschedule();

    const uint64_t blocked_deadline = arch::Timer::ticks() + 500;
    while (reader->state != TaskState::BLOCKED &&
           arch::Timer::ticks() < blocked_deadline) {
        arch::pause();
    }
    // Deterministic: no writer exists yet, nothing can wake the reader.
    JARVIS_ASSERT(reader->state == TaskState::BLOCKED);

    // Stage 2: only now dispatch the writer; its write posts the
    // semaphore and drives the wake path.
    auto *writer = TaskControlBlock::create(pipe_wake_writer_entry, 12, 10);
    JARVIS_ASSERT(writer != nullptr);
    JARVIS_ASSERT(Scheduler::set_sched_policy(*writer, SchedPolicy::FIXED));
    Scheduler::add_task(*writer);
    Scheduler::reschedule();

    JARVIS_ASSERT(await_wake_done(arch::Timer::ticks() + 2000));
    kernel::test::wait_for_termination_safe(reader);
    kernel::test::wait_for_termination_safe(writer);
    kernel::test::terminate_if_live(reader);
    kernel::test::terminate_if_live(writer);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(5LL, g_wake_nread);
    JARVIS_ASSERT(memcmp(g_wake_buf, "hello", 5) == 0);

    g_wake_reader_vnode = nullptr;
    g_wake_writer_vnode = nullptr;
    g_wake_reader_task = nullptr;
    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #474 fail-closed ordering — buffered data must still
// read back after the WRITE end is closed; only a fully-drained
// write-closed pipe reads 0 (pipe.cpp:79-80 pre-empts the wait loop).
// Input: Write "hello", close write end, then read twice.
// Expect: first read returns the 5 buffered bytes; second read returns
// 0; read end still valid (freed by the harness).
// Depends: kernel::vfs::Pipe read + write_close ordering.
JARVIS_TEST(pipe_read_returns_buffered_after_writer_close,
            "PRE: vfsd, iocd | POST: none") {
    int fds[2] = {-1, -1};
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
    JARVIS_ASSERT_EQ(5LL, written);

    // Close the WRITE end; buffered bytes must remain readable.
    task->fd_table.free(fds[1]);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    int64_t nread = rnode->ops->read(*rnode, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(5LL, nread);
    JARVIS_ASSERT(memcmp(buf, "hello", 5) == 0);

    int64_t eof = rnode->ops->read(*rnode, buf, sizeof(buf), 0);
    JARVIS_ASSERT_EQ(0LL, eof);

    task->fd_table.free(fds[0]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #474 atomic-boundary analogue — the 4096-byte ring
// cursor must wrap without reordering: read a prefix to advance
// read_pos, write a distinct pattern into the freed window, then drain
// everything and verify byte order across the wrap (old tail before new
// head, no torn bytes).
// Input: Fill 4096x0x5A; read 1024; write 1024x0xCC; read 4096.
// Expect: drain returns 4096; first 3072 bytes are 0x5A (old tail), last
// 1024 are 0xCC (new head); harness write returned exactly 1024.
// Depends: kernel::vfs::Pipe ring-buffer cursor arithmetic.
JARVIS_TEST(pipe_ring_wrap_byte_order, "PRE: vfsd, iocd | POST: none") {
    int fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    JARVIS_ASSERT(wnode != nullptr);

    uint8_t fill[4096];
    memset(fill, 0x5A, sizeof(fill));
    JARVIS_ASSERT_EQ(4096LL, wnode->ops->write(*wnode, fill, sizeof(fill), 0));

    uint8_t skip[1024];
    memset(skip, 0, sizeof(skip));
    JARVIS_ASSERT_EQ(1024LL, rnode->ops->read(*rnode, skip, sizeof(skip), 0));

    uint8_t refill[1024];
    memset(refill, 0xCC, sizeof(refill));
    JARVIS_ASSERT_EQ(
        1024LL, wnode->ops->write(*wnode, refill, sizeof(refill), 0));

    uint8_t drained[4096];
    memset(drained, 0, sizeof(drained));
    JARVIS_ASSERT_EQ(
        4096LL, rnode->ops->read(*rnode, drained, sizeof(drained), 0));
    for (size_t idx = 0; idx < 3072; ++idx) {
        JARVIS_ASSERT_EQ(0x5A, drained[idx]);
    }
    for (size_t idx = 3072; idx < 4096; ++idx) {
        JARVIS_ASSERT_EQ(0xCC, drained[idx]);
    }

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #474 O_NONBLOCK contract at the ops layer: an end
// opened with O_NONBLOCK fails closed with -EAGAIN (-11) instead of
// parking — empty read with a live writer, and write to a full pipe.
// NOTE (audit): -11 is observable ONLY here — sys_read/sys_write
// collapse negatives, and there is no fcntl, so open-time-only.
// Input: Create pipe; open read end O_NONBLOCK, read empty (live
// writer); fill 4096; open write end O_NONBLOCK, write 1 byte.
// Expect: both return -11; no hang; default blocking shape untouched.
// Depends: pipe_open O_NONBLOCK, pipe_read/write -EAGAIN paths
JARVIS_TEST(pipe_o_nonblock_eagain, "PRE: vfsd, iocd | POST: none") {
    int fds[2] = {-1, -1};
    JARVIS_ASSERT_EQ(0, vfs::create_pipe(fds));
    auto *task = Scheduler::current_task();
    JARVIS_ASSERT(task != nullptr);
    auto *rnode = task->fd_table.get(fds[0])->vnode;
    auto *wnode = task->fd_table.get(fds[1])->vnode;
    JARVIS_ASSERT(rnode != nullptr);
    JARVIS_ASSERT(wnode != nullptr);

    // Read end non-blocking: empty pipe + live writer fails closed.
    JARVIS_ASSERT_EQ(0, rnode->ops->open(
                            *rnode, static_cast<uint64_t>(
                                        vfs::O_NONBLOCK)));
    uint8_t probe = 0xAA;
    JARVIS_ASSERT_EQ(-11, rnode->ops->read(*rnode, &probe, sizeof(probe),
                                           0));

    // Write end non-blocking: full pipe fails closed.
    JARVIS_ASSERT_EQ(0, wnode->ops->open(
                            *wnode, static_cast<uint64_t>(
                                        vfs::O_NONBLOCK)));
    uint8_t fill[4096];
    memset(fill, 0x5A, sizeof(fill));
    JARVIS_ASSERT_EQ(4096LL, wnode->ops->write(*wnode, fill,
                                               sizeof(fill), 0));
    uint8_t extra = 0x5A;
    JARVIS_ASSERT_EQ(-11, wnode->ops->write(*wnode, &extra,
                                            sizeof(extra), 0));

    task->fd_table.free(fds[0]);
    task->fd_table.free(fds[1]);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Two writer tasks dispatched CONCURRENTLY (no sequential pin)
// each push a 1 KiB distinct pattern through one shared pipe end; the
// write-side mutex makes each write atomic, so the 2 KiB drain is two
// contiguous uniform runs in either order (no torn bytes).
// Input: Harness creates pipe, dispatches A (0xAA) + B (0xBB) together,
// joins both, reads 2048.
// Expect: each writer reports 1024; drain is 1024-run AA + 1024-run BB
// in either order; no deadlock.
// Depends: pipe_write mutex exclusion, Scheduler dispatch + join
JARVIS_TEST(pipe_multiple_writers_no_interleaving,
            "PRE: vfsd, iocd | POST: none") {
    // Pseudocode: harness creates pipe, stashes wnode; dispatch A and B
    // together; join both; read 2048 in harness; assert two uniform
    // runs (either order); free fds.
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
    // Concurrent dispatch: both writers live before either joins. Each
    // 1 KiB write fits the empty 4 KiB ring, so neither blocks and the
    // mutex serializes the two copies (UP proves outcome).
    Scheduler::add_task(*first);
    Scheduler::add_task(*second);
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(first);
    kernel::test::wait_for_termination_safe(second);
    kernel::test::terminate_if_live(first);
    kernel::test::terminate_if_live(second);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_EQ(1024LL, g_first_written);
    JARVIS_ASSERT_EQ(1024LL, g_second_written);

    uint8_t both[2048];
    memset(both, 0, sizeof(both));
    int64_t nread =
        rnode->ops->read(*rnode, both, sizeof(both), 0);
    JARVIS_ASSERT_EQ(2048LL, nread);
    // Either order, but each half must be one uniform run (write
    // atomicity: no torn bytes inside a single write).
    bool aa_first = true;
    for (size_t idx = 0; idx < 1024; ++idx) {
        if (both[idx] != 0xAA) {
            aa_first = false;
            break;
        }
    }
    if (aa_first) {
        for (size_t idx = 1024; idx < 2048; ++idx) {
            JARVIS_ASSERT_EQ(0xBB, both[idx]);
        }
    } else {
        for (size_t idx = 0; idx < 1024; ++idx) {
            JARVIS_ASSERT_EQ(0xBB, both[idx]);
        }
        for (size_t idx = 1024; idx < 2048; ++idx) {
            JARVIS_ASSERT_EQ(0xAA, both[idx]);
        }
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
    JARVIS_REGISTER_TEST(pipe_read_block_woken_by_writer);
    JARVIS_REGISTER_TEST(pipe_read_returns_buffered_after_writer_close);
    JARVIS_REGISTER_TEST(pipe_ring_wrap_byte_order);
    JARVIS_REGISTER_TEST(pipe_o_nonblock_eagain);
    JARVIS_REGISTER_TEST(pipe_multiple_writers_no_interleaving);
}
