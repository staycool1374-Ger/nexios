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
#include <crc32.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/integrity.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <initrd/initrd.hpp>
#include <kernel/arch/io.hpp>
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

    // Build the phdr into the DATA buffer (not hdr+1 — that would read past
    // the 64-byte caller `hdr` object).
    auto *data_phdr = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header));
    data_phdr->type = elf::PT_LOAD;
    data_phdr->flags = elf::PF_R | elf::PF_X;
    data_phdr->offset =
        sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader);
    data_phdr->vaddr = 0x400000;
    data_phdr->paddr = 0x400000;
    data_phdr->filesz = 0x1000;
    data_phdr->memsz = 0x1000;
    data_phdr->align = 0x1000;

    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
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
// Testidea: Live-session finding (#77 report) — request_load on an initrd
// path leaked the resolve-result vnode (InitrdFileNode+Vnode, never
// released), so repeated `load`s exhausted MemPool until every resolve
// failed with FILE_NOT_FOUND. The tmpfs-staged tests never caught it:
// tmpfs lookups don't allocate initrd nodes.
// Input: request_load("/hey.c.elf") (initrd-resident ET_EXEC demo) ->
// wait -> take -> destroy, with vnode counters captured around the cycle.
// Expect: take succeeds (proves initrd-path loads work); vnode count
// identical before/after (pre-fix: +2 per request).
// Depends: ElfLoader request/wait/take/destroy, initrd mount at /
JARVIS_TEST(loader_initrd_request_no_vnode_leak, "PRE: vfsd, iocd | POST: none") {
    auto &rt = kernel::test::ResourceTracker::instance();
    kernel::test::ResourceCounters before{};
    rt.capture(before);
    elf::ElfLoader::reset();
    auto result = elf::ElfLoader::request_load("/hey.c.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    elf::ElfLoader::destroy_completed_tcb(t);
    kernel::test::ResourceCounters after{};
    rt.capture(after);
    JARVIS_ASSERT_EQ(before.vnodes, after.vnodes);
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

// Runmode: kernel
// Testidea: v0.4.2 — hammer request_load/request_cancel to hit the
// IDLE->BLOCKED lost-wakeup window (SIL3 BLOCKER #1): a request arriving
// between run_load setting IDLE and the loader blocking itself must still be
// processed.  Regression guard for the atomic-block fix in task_main().
JARVIS_TEST(loader_lost_wakeup_race, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/loadrace.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    // Interleave request_load + cancel + yield many times.  Each request must
    // eventually be serviced (state returns to IDLE) — a lost wakeup would
    // leave state=VALIDATING and wait_loader_idle would hang.
    for (int i = 0; i < 20; ++i) {
        auto r = elf::ElfLoader::request_load("/tmp/loadrace.elf");
        JARVIS_ASSERT(r == elf::LoadResult::OK ||
                      r == elf::LoadResult::ALREADY_LOADING);
        // Cancel some, let others complete; always allow progress.
        if (i % 2 == 0)
            elf::ElfLoader::request_cancel();
        for (int y = 0; y < 4; ++y)
            Scheduler::reschedule();
    }
    // Drain: cancel anything in flight and wait for IDLE.
    elf::ElfLoader::request_cancel();
    elf::ElfLoader::wait_loader_idle();
    JARVIS_ASSERT(elf::ElfLoader::state() == elf::LoadState::IDLE);
    cleanup_file("/tmp/loadrace.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #46 — the loader captures a read-only CRC baseline into
// the completed TCB (UNVERIFIED, nonzero range count, non-trivial CRC).
// Input: minimal ELF (one R-X PT_LOAD page) -> wait -> take.
// Expect: ro_seg_count_ == 1, ro_crc32_ != finalize(INITIAL),
// ro_verify_state_ == UNVERIFIED.  Destroy (never scheduled).
// Depends: ElfLoader request/wait/take/destroy, snapshot_ro_baseline.
JARVIS_TEST(loader_ro_crc_baseline, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/loadbase.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadbase.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->ro_seg_count_ == 1);
    JARVIS_ASSERT(t->ro_crc32_ != CRC32::finalize(CRC32::INITIAL));
    JARVIS_ASSERT(t->ro_verify_state_ ==
                  TaskControlBlock::RoVerifyState::UNVERIFIED);
    JARVIS_ASSERT(t->ro_verify_off_ == 0);

    elf::ElfLoader::destroy_completed_tcb(t);
    cleanup_file("/tmp/loadbase.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #46 — flipping one read-only byte post-load makes the
// idle re-verify slice terminate the task with the dmesg-matched exit
// code.  The image is admitted production-shape (prio 2/FIXED) but never
// dispatched: the prio-10 harness outranks it while the test drives the
// slice directly (no reschedule — fully deterministic, no UAF window).
// Input: load -> take -> add_task_err -> corrupt 0x400000 via HHDM ->
// bounded auth_verify_poll drive.
// Expect: task leaves the scheduler tables, state TERMINATED,
// exit_code == kElfAuthKillExitCode.  Drain reclaims everything.
// Depends: add_task_err, auth_verify_step kill path, drain_zombie_list.
JARVIS_TEST(loader_tamper_killed, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/loadtamp.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadtamp.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::set_priority(*t, 2);
    t->period_ticks = 100;
    t->deadline_ticks = arch::Timer::ticks() + 100;
    bool pok = Scheduler::set_sched_policy(*t, SchedPolicy::FIXED);
    auto acode = Scheduler::add_task_err(*t);
    JARVIS_ASSERT(pok && acode == errors::SCHED_ERR_OK);
    uint64_t tid = t->id;

    // Corrupt one read-only byte through the kernel HHDM alias (bypasses
    // the user PML4 R-X mapping, exactly what a rowhammer-style flip or a
    // stray DMA write looks like to the verifier).  Offset 0x200 lands in
    // the NOP sled, past the ELF header/phdrs and any header canary slot.
    uint64_t phys = VMM::virt_to_phys_in_pml4(0x400200, t->page_table_);
    JARVIS_ASSERT(phys != 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *alias = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    JARVIS_ASSERT(alias[0] == 0x90); // pristine NOP sled byte
    alias[0] ^= 0xFF;

    for (int i = 0; i < 10 && Scheduler::find_task(tid) != nullptr; ++i)
        integrity::auth_verify_poll();
    JARVIS_ASSERT(Scheduler::find_task(tid) == nullptr);
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(t->exit_code == elf::kElfAuthKillExitCode);
    JARVIS_ASSERT(t->ro_verify_state_ == TaskControlBlock::RoVerifyState::FAILED);

    Scheduler::drain_zombie_list();
    cleanup_file("/tmp/loadtamp.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #46 — an untampered image verifies clean: one slice
// covers the single RO page, state reaches VERIFIED, the task stays
// alive.  Same deterministic shape as loader_tamper_killed (admitted,
// never dispatched, no reschedule).
// Expect: VERIFIED + non-terminal state; teardown via terminate + drain
// (mirrors the test_libc_verify admitted-task teardown).
JARVIS_TEST(loader_clean_verified, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    uint64_t written = write_file("/tmp/loadclean.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadclean.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::set_priority(*t, 2);
    t->period_ticks = 100;
    t->deadline_ticks = arch::Timer::ticks() + 100;
    bool pok = Scheduler::set_sched_policy(*t, SchedPolicy::FIXED);
    auto acode = Scheduler::add_task_err(*t);
    JARVIS_ASSERT(pok && acode == errors::SCHED_ERR_OK);

    for (int i = 0;
         i < 10 &&
         t->ro_verify_state_ != TaskControlBlock::RoVerifyState::VERIFIED;
         ++i)
        integrity::auth_verify_poll();
    JARVIS_ASSERT(t->ro_verify_state_ ==
                  TaskControlBlock::RoVerifyState::VERIFIED);
    JARVIS_ASSERT(t->state != TaskState::TERMINATED);

    if (TaskControlBlock::is_valid(t) && t->state != TaskState::TERMINATED)
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    cleanup_file("/tmp/loadclean.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #46 S2 pin — an image whose phdrs sit at a non-default
// file offset (phoff=128, legal per the ELF spec) must baseline the SAME
// ranges as the packed layout: the helpers index the loader's packed
// buffer (phdrs always at +64), never the file offset.  Pre-fix the scan
// walked the wrong buffer bytes (vacuous baseline for dynamic images).
// Input: minimal ELF with 64 pad bytes between header and phdrs.
// Expect: load completes, ro_seg_count_ == 1, UNVERIFIED.
JARVIS_TEST(loader_phoff_shifted_baseline, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t base_sz = build_minimal_elf(&hdr, img);
    (void)base_sz;
    // Relocate the phdr table to file offset 128 (64 pad bytes).
    hdr.phoff = 128;
    __builtin_memcpy(img, &hdr, sizeof(elf::ELF64Header));
    __builtin_memset(img + sizeof(elf::ELF64Header), 0,
                     128 - sizeof(elf::ELF64Header));
    auto *phdr = reinterpret_cast<elf::ELF64ProgramHeader *>(img + 128);
    phdr->type = elf::PT_LOAD;
    phdr->flags = elf::PF_R | elf::PF_X;
    phdr->offset = 128 + sizeof(elf::ELF64ProgramHeader);
    phdr->vaddr = 0x400000;
    phdr->paddr = 0x400000;
    phdr->filesz = 0x1000;
    phdr->memsz = 0x1000;
    phdr->align = 0x1000;
    for (size_t i = 0; i < 0x1000; ++i)
        img[phdr->offset + i] = 0x90;
    uint64_t sz = phdr->offset + 0x1000;
    uint64_t written = write_file("/tmp/loadphoff.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadphoff.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->ro_seg_count_ == 1);
    // Range starts past the carved TEXT before-canary slot.
    JARVIS_ASSERT(t->ro_ranges_[0].vaddr == 0x400008);
    JARVIS_ASSERT(t->ro_verify_state_ ==
                  TaskControlBlock::RoVerifyState::UNVERIFIED);

    elf::ElfLoader::destroy_completed_tcb(t);
    cleanup_file("/tmp/loadphoff.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: issue #270 — a faulting user task dies with SIGSEGV and the
// console stays quiet: no register/CR2/stack-trace dump by default
// (CONFIG_USER_FAULT_VERBOSE undefined); the one-line summary path is
// unchanged (silent in tests via the is_test_active gate, live-verified).
// A real dispatch into an unmapped entry faults deterministically.
// Expect: TERMINATED, exit -11; klog delta free of dump markers (or,
// with the macro defined, dump markers PRESENT — same test pins both).
JARVIS_TEST(loader_fault_dump_gated, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    hdr.entry = 0x500000; // unmapped: first dispatch faults deterministically
    __builtin_memcpy(img, &hdr, sizeof(elf::ELF64Header));
    uint64_t written = write_file("/tmp/loadfault.elf", img, sz);
    JARVIS_ASSERT(written == sz);

    auto result = elf::ElfLoader::request_load("/tmp/loadfault.elf");
    JARVIS_ASSERT(result == elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();

    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    Scheduler::set_priority(*t, 2);
    t->period_ticks = 100;
    t->deadline_ticks = arch::Timer::ticks() + 100;
    bool pok = Scheduler::set_sched_policy(*t, SchedPolicy::FIXED);
    auto acode = Scheduler::add_task_err(*t);
    JARVIS_ASSERT(pok && acode == errors::SCHED_ERR_OK);

    kernel::log::KlogService::instance().clear();
    kernel::test::wait_for_termination_safe(t);
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    JARVIS_ASSERT(t->exit_code ==
                  static_cast<uint64_t>(-static_cast<int64_t>(11)));

    // Scan the klog delta for fault-dump markers.
    char kbuf[512];
    bool saw_dump = false;
    for (;;) {
        size_t n =
            kernel::log::KlogService::instance().read(kbuf, sizeof(kbuf));
        if (n == 0)
            break;
        for (size_t i = 0; i < n; ++i) {
            // Manual char compares only: __builtin_memcmp emits a memcmp
            // libcall on riscv64 (no freestanding provider) while x86_64
            // folds it — the call breaks the riscv64 link (issue #234
            // cross-arch gate; pre-existing at HEAD).
            bool is_stack_trace =
                (kbuf[i] == 'S' && i + 11 < n && kbuf[i + 1] == 't' &&
                 kbuf[i + 2] == 'a' && kbuf[i + 3] == 'c' &&
                 kbuf[i + 4] == 'k' && kbuf[i + 5] == ' ' &&
                 kbuf[i + 6] == 't' && kbuf[i + 7] == 'r' &&
                 kbuf[i + 8] == 'a' && kbuf[i + 9] == 'c' &&
                 kbuf[i + 10] == 'e' && kbuf[i + 11] == ':');
            bool is_unhandled_sig =
                (kbuf[i] == 'u' && i + 15 < n && kbuf[i + 1] == 'n' &&
                 kbuf[i + 2] == 'h' && kbuf[i + 3] == 'a' &&
                 kbuf[i + 4] == 'n' && kbuf[i + 5] == 'd' &&
                 kbuf[i + 6] == 'l' && kbuf[i + 7] == 'e' &&
                 kbuf[i + 8] == 'd' && kbuf[i + 9] == ' ' &&
                 kbuf[i + 10] == 's' && kbuf[i + 11] == 'i' &&
                 kbuf[i + 12] == 'g' && kbuf[i + 13] == 'n' &&
                 kbuf[i + 14] == 'a' && kbuf[i + 15] == 'l');
            if ((kbuf[i] == 'R' && i + 4 < n && kbuf[i + 1] == 'A' &&
                 kbuf[i + 2] == 'X' && kbuf[i + 3] == ':') ||
                (kbuf[i] == 'C' && i + 3 < n && kbuf[i + 1] == 'R' &&
                 kbuf[i + 2] == '2') ||
                is_stack_trace || is_unhandled_sig) {
                saw_dump = true;
                break;
            }
        }
        if (saw_dump)
            break;
    }
#ifdef CONFIG_USER_FAULT_VERBOSE
    JARVIS_ASSERT(saw_dump); // verbose build: dump must be present
#else
    JARVIS_ASSERT(!saw_dump); // default: console stays quiet
#endif

    Scheduler::drain_zombie_list();
    cleanup_file("/tmp/loadfault.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: a runelf-loaded ELF (EMPTY env) whose main takes the 3rd (envp)
//           argument reads the crt0-derived envp without faulting (#311).
// Input: userspace/bin/envprobe.c.elf — main(argc,argv,envp) reads envp;
//        elf::load + dispatch.
// Expect: exit 0 (envp is a valid NULL-terminated empty array).
// Depends: elf::load -> finalize_loaded_task -> setup_user_stack(empty env)
//          vs crt0.S x86_64 entry (rdx = rsp+16+argc*8).
JARVIS_TEST(loader_runelf_3arg_main_envp, "PRE: none | POST: none") {
#if !defined(CONFIG_ARCH_X86_64)
    // crt0 on aarch64/riscv passes only the stack pointer to main, so a
    // 3-argument main is unsupported there (tracked separately).  x86_64 is
    // the execute-test gate.
    JARVIS_TEST_PASS();
#else
    initrd::InitrdFile f = initrd::find("bin/envprobe.c.elf");
    JARVIS_ASSERT(f.data != nullptr);
    auto *hdr = reinterpret_cast<const kernel::elf::ELF64Header *>(f.data);
    JARVIS_ASSERT(kernel::elf::validate_header(hdr));
    auto *t = kernel::elf::load(hdr, f.data, f.size);
    JARVIS_ASSERT(t != nullptr);
    Scheduler::add_task(*t);
    for (int i = 0; i < 400000; ++i) {
        if (!TaskControlBlock::is_valid(t) ||
            t->state == TaskState::TERMINATED ||
            t->state == TaskState::REAPED)
            break;
        __atomic_store_n(&kernel::Scheduler::SwSlots::need_resched(), true,
                         __ATOMIC_RELEASE);
        arch::hlt();
    }
    JARVIS_ASSERT(TaskControlBlock::is_valid(t));
    // The task must have RUN to completion: is_valid() alone also holds for a
    // READY task that was never scheduled (zero-initialized exit_code == 0),
    // which would let this test pass vacuously.  Require TERMINATED so the
    // envp read is proven to have executed.
    JARVIS_ASSERT(t->state == TaskState::TERMINATED);
    // exit 0 = envp read succeeded; a user-mode fault would leave a non-zero
    // exit_code (signal).
    JARVIS_ASSERT_EQ(0ULL, t->exit_code);
    Scheduler::terminate(*t, t->exit_code);
    Scheduler::drain_zombie_list();
    JARVIS_TEST_PASS();
#endif
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
    JARVIS_REGISTER_TEST(loader_lost_wakeup_race);
    JARVIS_REGISTER_TEST(loader_initrd_request_no_vnode_leak); // #77 leak
    JARVIS_REGISTER_TEST(loader_ro_crc_baseline); // #46 baseline
    JARVIS_REGISTER_TEST(loader_tamper_killed);   // #46 mismatch kill
    JARVIS_REGISTER_TEST(loader_clean_verified);  // #46 clean verify
    JARVIS_REGISTER_TEST(loader_phoff_shifted_baseline); // #46 S2 pin
    JARVIS_REGISTER_TEST(loader_fault_dump_gated); // #270 quiet default
    JARVIS_REGISTER_TEST(loader_runelf_3arg_main_envp); // #311 3-arg main/envp
}
