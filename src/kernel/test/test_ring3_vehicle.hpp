#pragma once

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

/// @file test_ring3_vehicle.hpp
/// @brief Shared Ring-3 dispatch vehicle (issue #303): stages the embedded
///        picolibc `libc_verify` image + stdin to tmpfs, loads it, repoints
///        fd 0/1, dispatches at prio 11, joins, reads the stdout capture
///        file back, and tears everything down. Byte-for-byte the legacy
///        path of test_libc_verify.cpp (which is intentionally NOT
///        refactored onto this header — non-stub tests are read-only);
///        the only deltas are a caller-sized capture buffer and optional
///        extra stage/cleanup hooks for per-test files.
///        x86_64 only (the image is embedded for x86_64 only).

#include <test.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <kernel/test/test_isolate.hpp>
#include "test_sched_helpers.hpp"
#include <constants.hpp>

#if defined(CONFIG_ARCH_X86_64)

extern "C" {
extern const uint8_t _binary_libc_verify_img_start[];
extern const uint8_t _binary_libc_verify_img_end[];
}

namespace kernel::test::ring3 {

constexpr uint64_t kJoinSpins = 10000000ULL;
constexpr const char *kImagePath = "/tmp/libc_verify.elf";
constexpr const char *kStdinPath = "/tmp/libc_stdin.txt";
constexpr const char *kStdoutPath = "/tmp/libc_stdout.txt";
constexpr const char *kStdinData = "1234\n";

struct Outcome {
    bool joined = false;
    bool clean_exit = false;
    uint64_t program_break = 0;
};

inline void unlink_path(const char *path) {
    kernel::test::mark_vfs_touched();
    vfs::unlink(path);
}

inline uint64_t write_data_file(const char *path, const uint8_t *data,
                                size_t len) {
    kernel::test::mark_vfs_touched();
    if (vfs::create(path, 0) != 0)
        return 0;
    vfs::Vnode *file = vfs::resolve(path);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->write == nullptr) {
        unlink_path(path);
        return 0;
    }
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 4096)
            chunk = 4096;
        int64_t written =
            file->ops->write(*file, data + off, chunk, off);
        if (written <= 0) {
            unlink_path(path);
            return 0;
        }
        off += static_cast<size_t>(written);
    }
    return (off == len) ? len : 0ULL;
}

inline uint64_t write_image_file() {
    size_t size = static_cast<size_t>(_binary_libc_verify_img_end -
                                      _binary_libc_verify_img_start);
    if (size == 0)
        return 0;
    return write_data_file(kImagePath, _binary_libc_verify_img_start,
                           size);
}

inline bool serial_contains(const char *buf, const char *needle) {
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
// (Whitespace-tolerant: skips spaces AND newlines between numbers so
// multi-line marker blocks parse.)
inline bool parse_tag_line(const char *buf, const char *tag, uint64_t *out,
                           int wanted, size_t cap) {
    size_t n = 0;
    while (buf[n] != '\0' && n < cap) {
        size_t j = 0;
        while (tag[j] != '\0' && buf[n + j] == tag[j])
            ++j;
        if (tag[j] == '\0') {
            size_t p = n + j;
            for (int k = 0; k < wanted; ++k) {
                while ((buf[p] == ' ' || buf[p] == '\n') && p < cap)
                    ++p;
                if (p >= cap || buf[p] < '0' || buf[p] > '9')
                    return false;
                uint64_t v = 0;
                while (p < cap && buf[p] >= '0' && buf[p] <= '9') {
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

// Run the image; stage_extra (optional) stages per-test files after the
// standard three and before load; cleanup_extra unlinks them after the
// capture is read. Asserts happen after teardown (allocation-free).
inline Outcome run(void (*stage_extra)() = nullptr,
                   void (*cleanup_extra)() = nullptr, char *capture = nullptr,
                   size_t capture_size = 0) {
    Outcome out{};
    elf::ElfLoader::reset();
    if (write_image_file() == 0)
        return out;
    if (write_data_file(kStdinPath,
                        reinterpret_cast<const uint8_t *>(kStdinData),
                        5) != 5) {
        unlink_path(kImagePath);
        return out;
    }
    kernel::test::mark_vfs_touched();
    if (vfs::create(kStdoutPath, 0) != 0) {
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        return out;
    }
    if (stage_extra != nullptr)
        stage_extra();
    if (elf::ElfLoader::request_load(kImagePath) != elf::LoadResult::OK) {
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    if (t != nullptr && t->get_page_table() != 0) {
        uint64_t p_text =
            VMM::virt_to_phys_in_pml4(0x400000, t->get_page_table());
        uint64_t p_stack = VMM::virt_to_phys_in_pml4(
            mem::STACK_VADDR + 4096, t->get_page_table());
        if (p_text == 0 || p_stack == 0) {
            elf::ElfLoader::destroy_completed_tcb(t);
            unlink_path(kImagePath);
            unlink_path(kStdinPath);
            unlink_path(kStdoutPath);
            if (cleanup_extra != nullptr)
                cleanup_extra();
            return out;
        }
    }
    if (t == nullptr || t->get_page_table() == 0 || !t->is_user_) {
        if (t != nullptr)
            elf::ElfLoader::destroy_completed_tcb(t);
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    // Repoint fd 0 at the tmpfs stdin file (direct slot overwrite:
    // FdTable::free decs a vnode ref never inc'd — destroy_completed_tcb
    // no-dec discipline).
    if (!t->fd_table.fds[0].used) {
        elf::ElfLoader::destroy_completed_tcb(t);
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    vfs::Vnode *stdin_vnode = vfs::resolve(kStdinPath);
    if (stdin_vnode == nullptr) {
        elf::ElfLoader::destroy_completed_tcb(t);
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    t->fd_table.fds[0].vnode = stdin_vnode;
    t->fd_table.fds[0].offset = 0;
    t->fd_table.fds[0].flags = 0;

    // Repoint stdout at the tmpfs capture file (same discipline).
    if (!t->fd_table.fds[1].used) {
        elf::ElfLoader::destroy_completed_tcb(t);
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    vfs::Vnode *stdout_vnode = vfs::resolve(kStdoutPath);
    if (stdout_vnode == nullptr) {
        elf::ElfLoader::destroy_completed_tcb(t);
        unlink_path(kImagePath);
        unlink_path(kStdinPath);
        unlink_path(kStdoutPath);
        if (cleanup_extra != nullptr)
            cleanup_extra();
        return out;
    }
    t->fd_table.fds[1].vnode = stdout_vnode;
    t->fd_table.fds[1].offset = 0;
    t->fd_table.fds[1].flags = 0;

    // Prio 11: the harness (prio 10) would starve a lower-priority waitee
    // after the first dispatch demotes it to READY. Set before add_task.
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
    // Read back the capture BEFORE assert (allocation-free teardown rule).
    if (capture != nullptr && capture_size > 1) {
        vfs::Vnode *cap = vfs::resolve(kStdoutPath);
        size_t pos = 0;
        if (cap != nullptr && cap->ops != nullptr &&
            cap->ops->read != nullptr) {
            while (pos < capture_size - 1) {
                int64_t rd = cap->ops->read(
                    *cap, reinterpret_cast<uint8_t *>(capture + pos), 1,
                    pos);
                if (rd <= 0)
                    break;
                pos += static_cast<size_t>(rd);
            }
        }
        capture[pos] = '\0';
    }
    if (TaskControlBlock::is_valid(t) &&
        (t->state != TaskState::TERMINATED || t->exit_code == 0))
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    unlink_path(kImagePath);
    unlink_path(kStdinPath);
    unlink_path(kStdoutPath);
    if (cleanup_extra != nullptr)
        cleanup_extra();
    out.joined = clean;
    out.clean_exit = clean;
    (void)exited;
    return out;
}

} // namespace kernel::test::ring3

namespace kernel::test::ring3 {

// Read a whole tmpfs file into buf (up to size-1 bytes, NUL-terminated).
// Returns bytes read, or -1 on any failure. Marks VFS touched.
inline int64_t read_file(const char *path, char *buf, size_t size) {
    if (size == 0)
        return -1;
    kernel::test::mark_vfs_touched();
    vfs::Vnode *file = vfs::resolve(path);
    if (file == nullptr || file->ops == nullptr ||
        file->ops->read == nullptr)
        return -1;
    size_t pos = 0;
    while (pos < size - 1) {
        int64_t rd =
            file->ops->read(*file, reinterpret_cast<uint8_t *>(buf + pos),
                            size - 1 - pos, pos);
        if (rd <= 0)
            break;
        pos += static_cast<size_t>(rd);
    }
    buf[pos] = '\0';
    return static_cast<int64_t>(pos);
}

} // namespace kernel::test::ring3

#endif // CONFIG_ARCH_X86_64
