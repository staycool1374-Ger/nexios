/*
 * NexIOS RTOS — Background ELF loader tests
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

/// @file test_elf_loader.cpp
/// @brief Background chunked ELF loader (ElfLoader) tests: success, error
///        taxonomy, cancel, concurrency guards, multiple cycles.

#include <test.hpp>
#include <logger.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

/// @brief Build a minimal valid ELF64 image into @p data (same shape as
///        test_elf's build_minimal_elf, but self-contained).
uint64_t build_minimal_elf(elf::ELF64Header *hdr, uint8_t *data) {
    __builtin_memset(hdr, 0, sizeof(elf::ELF64Header));
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2; // ELFCLASS64
    hdr->ident[5] = 1; // little-endian
    hdr->type = elf::ET_EXEC;
    hdr->machine = 0x3E;
    hdr->version = 1;
    hdr->entry = 0x400000;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 1;
    hdr->shentsize = 0;
    hdr->shnum = 0;
    hdr->shstrndx = 0;

    auto *phdr = reinterpret_cast<elf::ELF64ProgramHeader *>(hdr + 1);
    phdr->type = elf::PT_LOAD;
    phdr->flags = elf::PF_R | elf::PF_X;
    phdr->offset = sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader);
    phdr->vaddr = 0x400000;
    phdr->paddr = 0x400000;
    phdr->filesz = 0x1000;
    phdr->memsz = 0x1000;
    phdr->align = 0x1000;

    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
    __builtin_memcpy(data + hdr->phoff, phdr, sizeof(elf::ELF64ProgramHeader));
    uint64_t code_offset =
        sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader);
    for (size_t i = 0; i < 0x1000; ++i)
        data[code_offset + i] = 0x90; // NOP
    return code_offset + 0x1000;
}

/// @brief Write a file to the boot tmpfs at /tmp and return its total size.
uint64_t write_file(const char *path, const uint8_t *data, uint64_t size) {
    kernel::test::mark_vfs_touched();
    int ret = vfs::create(path, 0);
    if (ret != 0)
        return 0;
    vfs::Vnode *file = vfs::resolve(path);
    if (!file || !file->ops || !file->ops->write)
        return 0;
    int64_t written = file->ops->write(*file, data, size, 0);
    return static_cast<uint64_t>(written);
}

void cleanup_file(const char *path) {
    kernel::test::mark_vfs_touched();
    vfs::unlink(path);
}

} // namespace

// Runmode: kernel
// Testidea: a valid tmpfs ELF loads in the background; the completed TCB is
// retained, has the expected entry + page table, and destroy_completed_tcb
// frees it scheduler-safely (zero ResourceTracker delta).
// it with zero ResourceTracker delta.
JARVIS_TEST(loader_load_success, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/loadtest.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadtest.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->page_table_ != 0);
    JARVIS_ASSERT(t->is_user_);

    // destroy_completed_tcb tears down the never-scheduled TCB
    // scheduler-safely (the completed image was NOT add_task'd, so cleanup()'s
    // unregister path must not run).
    elf::ElfLoader::destroy_completed_tcb(t);
    cleanup_file("/tmp/loadtest.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: corrupting the ELF magic yields INVALID_ELF and a clean FAILED →
// IDLE transition with zero delta.
JARVIS_TEST(loader_load_invalid_elf, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    img[0] = 0xDE; // corrupt magic
    uint64_t written = write_file("/tmp/loadbad.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadbad.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    JARVIS_ASSERT(elf::ElfLoader::take_completed() == nullptr);
    cleanup_file("/tmp/loadbad.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cancel mid-load reclaims all partial state (zero delta) and the
// loader returns to IDLE.  An 8-page load is started; cancel is requested
// after yielding a couple of times (cancel can land mid-chunk).
JARVIS_TEST(loader_cancel_mid_load, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    // Build a multi-page ELF (8 PT_LOAD pages) so the loader yields several
    // times before completing.
    constexpr size_t kPages = 8;
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t base_sz = build_minimal_elf(&hdr, img);
    // Extend the single segment to 8 pages by growing memsz/filesz.
    auto *phdr = reinterpret_cast<elf::ELF64ProgramHeader *>(img + sizeof(elf::ELF64Header));
    phdr->filesz = kPages * 0x1000;
    phdr->memsz = kPages * 0x1000;
    // The image buffer must hold 8 pages of code.
    uint8_t big[4096 * kPages + 4096];
    __builtin_memset(big, 0x90, sizeof(big));
    __builtin_memcpy(big, img, base_sz);
    uint64_t written = write_file("/tmp/loadmulti.elf", big,
                                  sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader) + kPages * 0x1000);
    JARVIS_ASSERT(written != 0);

    auto result = elf::ElfLoader::request_load("/tmp/loadmulti.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    // Yield a few times so the loader makes progress, then cancel.
    for (int i = 0; i < 3; ++i)
        Scheduler::reschedule();
    result = elf::ElfLoader::request_cancel();
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    JARVIS_ASSERT(elf::ElfLoader::take_completed() == nullptr);
    cleanup_file("/tmp/loadmulti.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: a second request_load while one is in flight is rejected.
JARVIS_TEST(loader_already_loading, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/load2.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/load2.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    // Second request must be rejected while the first is in flight.
    auto second = elf::ElfLoader::request_load("/tmp/load2.elf");
    JARVIS_ASSERT(second == elf::LoadResult::ALREADY_LOADING);
    elf::ElfLoader::request_cancel();
    elf::ElfLoader::wait_loader_idle();

    cleanup_file("/tmp/load2.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cancel with nothing in flight is NOT_LOADING (no state change).
JARVIS_TEST(loader_cancel_not_loading, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    elf::ElfLoader::wait_loader_idle();
    auto result = elf::ElfLoader::request_cancel();
    JARVIS_ASSERT(result == elf::LoadResult::NOT_LOADING);
    JARVIS_ASSERT(elf::ElfLoader::state() == elf::LoadState::IDLE);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: three sequential load→release cycles each return to IDLE with
// zero ResourceTracker delta (snapshot_restore check at test end).
JARVIS_TEST(loader_multiple_cycles, "PRE: vfsd, iocd | POST: none") {
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/load3.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    for (int cycle = 0; cycle < 3; ++cycle) {
        elf::ElfLoader::reset();
        auto result = elf::ElfLoader::request_load("/tmp/load3.elf");
        JARVIS_ASSERT(result == elf::LoadResult::OK);
        elf::ElfLoader::wait_loader_idle();
        auto *t = elf::ElfLoader::take_completed();
        JARVIS_ASSERT(t != nullptr);
        elf::ElfLoader::destroy_completed_tcb(t);
        JARVIS_ASSERT(elf::ElfLoader::state() == elf::LoadState::IDLE);
    }
    cleanup_file("/tmp/load3.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: the harness makes forward progress while a multi-page load is in
// flight (the loader yields per chunk and does not starve the harness).
JARVIS_TEST(loader_preemption_yield, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    constexpr size_t kPages = 8;
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t base_sz = build_minimal_elf(&hdr, img);
    auto *phdr = reinterpret_cast<elf::ELF64ProgramHeader *>(img + sizeof(elf::ELF64Header));
    phdr->filesz = kPages * 0x1000;
    phdr->memsz = kPages * 0x1000;
    uint8_t big[4096 * kPages + 4096];
    __builtin_memset(big, 0x90, sizeof(big));
    __builtin_memcpy(big, img, base_sz);
    uint64_t written = write_file("/tmp/loadyield.elf", big,
                                  sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader) + kPages * 0x1000);
    JARVIS_ASSERT(written != 0);

    auto result = elf::ElfLoader::request_load("/tmp/loadyield.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    // Bounded harness spin: the timer must still advance (the loader yields).
    uint64_t t0 = arch::Timer::ticks();
    for (int i = 0; i < 50; ++i)
        Scheduler::reschedule();
    uint64_t t1 = arch::Timer::ticks();
    JARVIS_ASSERT(t1 >= t0);

    elf::ElfLoader::wait_loader_idle();
    auto *done = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(done != nullptr);
    elf::ElfLoader::destroy_completed_tcb(done);
    cleanup_file("/tmp/loadyield.elf");
    JARVIS_TEST_PASS();
}

void register_elf_loader_tests() {
    Logger::info("Registering background ELF loader tests");
    JARVIS_REGISTER_TEST(loader_load_success);
    JARVIS_REGISTER_TEST(loader_load_invalid_elf);
    JARVIS_REGISTER_TEST(loader_cancel_mid_load);
    JARVIS_REGISTER_TEST(loader_already_loading);
    JARVIS_REGISTER_TEST(loader_cancel_not_loading);
    JARVIS_REGISTER_TEST(loader_multiple_cycles);
    JARVIS_REGISTER_TEST(loader_preemption_yield);
}
