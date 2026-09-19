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

/// @file test_libc_verify.cpp
/// @brief Hosted-C Ring 3 verification (issue #75): the picolibc-linked
///        `libc_verify` program runs printf/malloc/scanf via real dispatch;
///        this class asserts its serial markers, exit code and heap bounds.
///        The program image is embedded in the kernel (objcopy, x86_64 only
///        — the initrd is not mounted in test boot so it cannot be
///        resolved there) and staged to tmpfs per test. Stdin is a
///        tmpfs-backed fd 0 (deterministic, no serial/keyboard injection);
///        stdout is observed through UART loopback with LIBC_VERIFY:
///        -prefixed substring markers (harness logs interleave).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/test/test_isolate.hpp>
#include "test_sched_helpers.hpp"
#include <constants.hpp>

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

extern "C" {
extern const uint8_t _binary_libc_verify_img_start[];
extern const uint8_t _binary_libc_verify_img_end[];
}

namespace {

constexpr uint64_t kJoinSpins = 10000000ULL;
constexpr const char *kImagePath = "/tmp/libc_verify.elf";
constexpr const char *kStdinPath = "/tmp/libc_stdin.txt";
constexpr const char *kStdinData = "1234\n";

// Stage the embedded program image to tmpfs; returns byte size or 0.
// Writes in 4 KiB chunks (a single 174 KiB write exceeds tmpfs call
// limits and returns short).
void cleanup_image_file() {
    kernel::test::mark_vfs_touched();
    vfs::unlink(kImagePath);
}

uint64_t write_image_file() {
    kernel::test::mark_vfs_touched();
    size_t size = static_cast<size_t>(_binary_libc_verify_img_end -
                                      _binary_libc_verify_img_start);
    if (size == 0)
        return 0;
    if (vfs::create(kImagePath, 0) != 0)
        return 0;
    vfs::Vnode *file = vfs::resolve(kImagePath);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->write == nullptr) {
        cleanup_image_file();
        return 0;
    }
    size_t off = 0;
    while (off < size) {
        size_t chunk = size - off;
        if (chunk > 4096)
            chunk = 4096;
        int64_t written = file->ops->write(
            *file, _binary_libc_verify_img_start + off, chunk, off);
        if (written <= 0) {
            cleanup_image_file();
            return 0;
        }
        off += static_cast<size_t>(written);
    }
    return (off == size) ? size : 0ULL;
}

void cleanup_stdin_file() {
    kernel::test::mark_vfs_touched();
    vfs::unlink(kStdinPath);
}

uint64_t write_stdin_file() {
    kernel::test::mark_vfs_touched();
    if (vfs::create(kStdinPath, 0) != 0)
        return 0;
    vfs::Vnode *file = vfs::resolve(kStdinPath);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->write == nullptr) {
        cleanup_stdin_file();
        return 0;
    }
    int64_t written = file->ops->write(
        *file, reinterpret_cast<const uint8_t *>(kStdinData), 5, 0);
    if (written != 5) {
        cleanup_stdin_file();
        return 0;
    }
    return 5ULL;
}

bool serial_contains(const char *buf, const char *needle) {
    for (size_t i = 0; buf[i] != '\0'; ++i) {
        size_t j = 0;
        for (; needle[j] != '\0'; ++j) {
            if (buf[i + j] != needle[j])
                break;
        }
        if (needle[j] == '\0')
            return true;
    }
    return false;
}

// Parse `wanted` unsigned decimals after a tag into out; false on miss.
bool parse_brk_line(const char *buf, const char *tag, uint64_t *out,
                    int wanted) {
    size_t n = 0;
    while (buf[n] != '\0' && n < 2048) {
        size_t j = 0;
        while (tag[j] != '\0' && buf[n + j] == tag[j])
            ++j;
        if (tag[j] == '\0') {
            size_t p = n + j;
            for (int k = 0; k < wanted; ++k) {
                while (buf[p] == ' ')
                    ++p;
                if (buf[p] < '0' || buf[p] > '9')
                    return false;
                uint64_t v = 0;
                while (buf[p] >= '0' && buf[p] <= '9') {
                    v = v * 10 + static_cast<uint64_t>(buf[p] - '0');
                    ++p;
                }
                out[k] = v;
            }
            return true;
        }
        ++n;
    }
    return false;
}

struct VerifyOutcome {
    bool joined = false;
    bool clean_exit = false;
    uint64_t program_break = 0;
    char capture[512]{};
};

namespace {
// Stdout capture file (replaces UART loopback: bursts exceed the 16 B
// RX FIFO and overrun mid-burst at tick granularity — undrainable.
// A tmpfs file is byte-exact and deterministic).
constexpr const char *kStdoutPath = "/tmp/libc_stdout.txt";
} // namespace

// Stage image + stdin, load, repoint fd 0, dispatch to EXIT with a
// bounded join, capture serial markers, tear down. Returns observable
// state; asserts happen after teardown (allocation-free).
VerifyOutcome run_verify_program() {
    VerifyOutcome out{};
    elf::ElfLoader::reset();
    if (write_image_file() == 0)
        return out;
    if (write_stdin_file() != 5) {
        cleanup_image_file();
        return out;
    }
    if (elf::ElfLoader::request_load(kImagePath) != elf::LoadResult::OK) {
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    // Mapping sanity: entry and first-stack pages must translate in
    // the task PML4 (catches loader mapping failures early; the data
    // page address is link-layout-specific and intentionally not
    // pinned here).
    if (t != nullptr && t->page_table_ != 0) {
        uint64_t p_text =
            VMM::virt_to_phys_in_pml4(0x400000, t->page_table_);
        uint64_t p_stack = VMM::virt_to_phys_in_pml4(
            mem::STACK_VADDR + 4096, t->page_table_);
        if (p_text == 0 || p_stack == 0) {
            elf::ElfLoader::destroy_completed_tcb(t);
            cleanup_image_file();
            cleanup_stdin_file();
            return out;
        }
    }
    if (t == nullptr || t->page_table_ == 0 || !t->is_user_) {
        if (t != nullptr)
            elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    // Repoint fd 0 at the tmpfs stdin file (elf finalize opened tty).
    // Direct slot overwrite (no free/alloc: FdTable::free decs a vnode
    // ref never inc'd — destroy_completed_tcb no-dec discipline).
    if (!t->fd_table.fds[0].used) {
        elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    vfs::Vnode *stdin_vnode = vfs::resolve(kStdinPath);
    if (stdin_vnode == nullptr) {
        elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    t->fd_table.fds[0].vnode = stdin_vnode;
    t->fd_table.fds[0].offset = 0;
    t->fd_table.fds[0].flags = 0;

    // Repoint stdout at a tmpfs capture file (byte-exact; UART
    // loopback overruns on bursts > 16 B and is undrainable mid-burst).
    // Direct slot overwrite (same no-dec discipline as fd 0 above).
    if (!t->fd_table.fds[1].used) {
        elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    kernel::test::mark_vfs_touched();
    if (vfs::create(kStdoutPath, 0) != 0) {
        elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        return out;
    }
    vfs::Vnode *stdout_vnode = vfs::resolve(kStdoutPath);
    if (stdout_vnode == nullptr) {
        elf::ElfLoader::destroy_completed_tcb(t);
        cleanup_image_file();
        cleanup_stdin_file();
        kernel::test::mark_vfs_touched();
        vfs::unlink(kStdoutPath);
        return out;
    }
    t->fd_table.fds[1].vnode = stdout_vnode;
    t->fd_table.fds[1].offset = 0;
    t->fd_table.fds[1].flags = 0;

    // The loader assigns prio 2, but the harness (prio 10) would starve a
    // lower-priority waitee forever after the first dispatch demotes the
    // harness to READY (every driven test uses >= 11 for this reason).
    // Set before add_task (not yet queued: no re-bucket needed).
    t->priority = 11;
    t->base_priority = 11;

    {
        arch::IrqGuard ig{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    for (uint64_t i = 0;
         i < kJoinSpins && t->state != TaskState::TERMINATED; ++i)
        arch::pause();
    bool exited = (t->state == TaskState::TERMINATED);
    bool clean = exited && (t->exit_code == 0);
    out.program_break = t->program_break;
    // Read back the stdout capture file, then tear down (BEFORE assert:
    // test_syscall.cpp:651-655 rule).
    {
        vfs::Vnode *cap = vfs::resolve(kStdoutPath);
        size_t pos = 0;
        if (cap != nullptr && cap->ops != nullptr &&
            cap->ops->read != nullptr) {
            while (pos < sizeof(out.capture) - 1) {
                int64_t rd = cap->ops->read(
                    *cap, reinterpret_cast<uint8_t *>(out.capture + pos),
                    1, pos);
                if (rd <= 0)
                    break;
                pos += static_cast<size_t>(rd);
            }
        }
        out.capture[pos] = '\0';
    }
    if (TaskControlBlock::is_valid(t) &&
        (t->state != TaskState::TERMINATED || t->exit_code == 0))
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    cleanup_image_file();
    cleanup_stdin_file();
    kernel::test::mark_vfs_touched();
    vfs::unlink(kStdoutPath);
    out.joined = clean;
    out.clean_exit = clean;
    (void)exited;
    return out;
}

} // namespace

// Runmode: kernel
// Testidea: printf reaches a file through the WRITE path in Ring 3.
// Input: Dispatch the verify image with fd 1 wired to a tmpfs file.
// Expect: Joined clean; file contains LIBC_VERIFY: hello.
// Depends: picolibc printf + _write→WRITE stubs (issues #71/#73)
JARVIS_TEST(libc_verify_printf_write, "PRE: vfsd | POST: none") {
    VerifyOutcome o = run_verify_program();
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(serial_contains(o.capture, "LIBC_VERIFY: hello"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: malloc grows the break via _sbrk→BRK.
// Input: Same dispatch; parse the brk marker line.
// Expect: Joined clean; b1 > b0 (heap grew on first malloc).
// Depends: _sbrk/BRK + program_break (issues #71/#73, MP-2.2)
JARVIS_TEST(libc_verify_malloc_growth, "PRE: vfsd | POST: none") {
    VerifyOutcome o = run_verify_program();
    JARVIS_ASSERT(o.joined);
    uint64_t b[4] = {};
    JARVIS_ASSERT(parse_brk_line(o.capture, "LIBC_VERIFY: brk", b, 4));
    JARVIS_ASSERT(b[1] > b[0]);
    JARVIS_ASSERT(o.program_break < mem::STACK_VADDR);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: free returns the block to the free list (reuse, not regrow).
// Input: Same dispatch; parse brk + ptr marker lines.
// Expect: Joined clean; b3 == b2 (no regrowth) and p2 == p1 (same block).
// Depends: picolibc malloc free-list over _sbrk (issue #75)
JARVIS_TEST(libc_verify_malloc_reuse, "PRE: vfsd | POST: none") {
    VerifyOutcome o = run_verify_program();
    JARVIS_ASSERT(o.joined);
    uint64_t b[4] = {};
    uint64_t p[4] = {};
    JARVIS_ASSERT(parse_brk_line(o.capture, "LIBC_VERIFY: brk", b, 4));
    JARVIS_ASSERT(parse_brk_line(o.capture, "LIBC_VERIFY: ptr", p, 2));
    JARVIS_ASSERT(b[3] == b[2]);
    JARVIS_ASSERT(p[1] == p[0]);
    JARVIS_ASSERT(p[0] != 0);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: scanf serves fd 0 through _read→SYS_READ from the tmpfs file.
// Input: Same dispatch with "1234\n" staged as stdin.
// Expect: Joined clean; capture contains LIBC_VERIFY: scanf 1234.
// Depends: _read→READ + fd repointing (issues #71/#75)
JARVIS_TEST(libc_verify_scanf_read, "PRE: vfsd | POST: none") {
    VerifyOutcome o = run_verify_program();
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(serial_contains(o.capture, "LIBC_VERIFY: scanf 1234"));
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: The program runs to clean EXIT(0) with zero resource delta.
// Input: Same dispatch; observe exit code (fault-death also yields
//        TERMINATED but nonzero — the code makes vacuous passes
//        impossible, test_syscall.cpp:628-632 rule).
// Expect: TERMINATED with exit_code 0; harness snapshot_restore delta 0.
// Depends: _exit→EXIT path (issues #71/#73)
JARVIS_TEST(libc_verify_exit_clean_zero_delta, "PRE: vfsd | POST: none") {
    VerifyOutcome o = run_verify_program();
    JARVIS_ASSERT(o.joined);
    JARVIS_ASSERT(o.clean_exit);
    JARVIS_TEST_PASS();
}

#endif // CONFIG_ARCH_X86_64

void register_libc_verify_tests() {
    Logger::info("Registering libc_verify tests");
#if defined(CONFIG_ARCH_X86_64)
    JARVIS_REGISTER_TEST(libc_verify_printf_write);
    JARVIS_REGISTER_TEST(libc_verify_malloc_growth);
    JARVIS_REGISTER_TEST(libc_verify_malloc_reuse);
    JARVIS_REGISTER_TEST(libc_verify_scanf_read);
    JARVIS_REGISTER_TEST(libc_verify_exit_clean_zero_delta);
#endif
}
