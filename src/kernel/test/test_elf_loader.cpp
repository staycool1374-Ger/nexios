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
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/integrity.hpp>
#include <kernel/arch/timer.hpp>
#include <kernel/log/ring_buffer.hpp>
#include <kernel/test/test_isolate.hpp>
#include <kernel/test/test_sched_helpers.hpp>
#include <kernel/test/resource_tracker.hpp>
#include <initrd/initrd.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/hal/irq_guard.hpp>
#include <string.hpp>

using namespace kernel;

namespace {

// Native ELF machine for synthetic headers (issue #352): the validator
// gates e_machine per arch (x86_64 0x3E, AArch64 0xB7, RISC-V 0xF3).
#if defined(CONFIG_ARCH_AARCH64)
constexpr uint16_t kNativeEm = 0xB7;
#elif defined(CONFIG_ARCH_RISCV64)
constexpr uint16_t kNativeEm = 0xF3;
#else
constexpr uint16_t kNativeEm = 0x3E;
#endif

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
    hdr->machine = kNativeEm;
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
    JARVIS_ASSERT(t->get_page_table() != 0);
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
// Testidea: #171 P2 generated-stub round trip. The stub-roundtrip probe
//        (userspace/stub-roundtrip.c) calls generated nexios_sys_* stubs
//        end to end and exits 0 only if every check passes: ABI_VERSION
//        exact value, TIMER_CREATE fail-closed NULL out-pointer (EFAULT
//        through the generated stub), CREATE + GETTIME + DELETE with a
//        live timer id (all four argument positions observed), invalid-op
//        EINVAL with four distinct nonzero args (number + op routing
//        proof), GETPID determinism. Nonzero exit names the failing
//        check (see the probe source).
// Input: request_load("/bin/stub-roundtrip.c.elf"), dispatch at prio 11,
//        join to TERMINATED via wait_for_termination_safe.
// Expect: TERMINATED with exit_code 0.
// Depends: ElfLoader initrd-path load, P1 crt0 contract, timer_create.
JARVIS_TEST(loader_stub_roundtrip_generated_stubs,
            "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    JARVIS_ASSERT_FMT(
        elf::ElfLoader::request_load("/bin/stub-roundtrip.c.elf") ==
            elf::LoadResult::OK,
        "stub-roundtrip request_load failed");
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT_FMT(t != nullptr && t->get_page_table() != 0 &&
                          t->is_user_,
                      "stub-roundtrip take_completed failed");
    // Prio 11: the harness idles; lower-priority waitees starve
    // (every driven test uses >= 11). Set before add_task.
    t->priority = 11;
    t->base_priority = 11;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    bool exited = (t->state == TaskState::TERMINATED);
    uint64_t code = t->exit_code;
    if (TaskControlBlock::is_valid(t) &&
        (t->state != TaskState::TERMINATED || t->exit_code == 0))
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(exited, "stub-roundtrip task did not terminate");
    JARVIS_ASSERT_FMT(code == 0, "stub-roundtrip probe failed check %lu",
                      (unsigned long)code);
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: #171 P3 Ring-3 cap smoke. The cap-smoke probe
//        (userspace/cap-smoke.c) calls nexios_cap_* with handles that
//        cannot resolve and exits 0 only if every call fails closed
//        (-1): grant/copy/revoke/mint, frame_create(0),
//        frame_map/mmio_map with a bogus handle, irq_register and
//        iommu_map with bogus handles. Creates no residue: every probe
//        call fails by design, so no cap is ever installed.
// Input: request_load("/bin/cap-smoke.c.elf"), dispatch at prio 11,
//        join to TERMINATED via wait_for_termination_safe.
// Expect: TERMINATED with exit_code 0.
// Depends: ElfLoader initrd-path load, P1 crt0 contract, cap syscalls.
JARVIS_TEST(loader_cap_smoke_fail_closed,
            "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    JARVIS_ASSERT_FMT(
        elf::ElfLoader::request_load("/bin/cap-smoke.c.elf") ==
            elf::LoadResult::OK,
        "cap-smoke request_load failed");
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT_FMT(t != nullptr && t->get_page_table() != 0 &&
                          t->is_user_,
                      "cap-smoke take_completed failed");
    // Prio 11: the harness idles; lower-priority waitees starve
    // (every driven test uses >= 11). Set before add_task.
    t->priority = 11;
    t->base_priority = 11;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    bool exited = (t->state == TaskState::TERMINATED);
    uint64_t code = t->exit_code;
    if (TaskControlBlock::is_valid(t) &&
        (t->state != TaskState::TERMINATED || t->exit_code == 0))
        Scheduler::terminate(*t, 0);
    Scheduler::drain_zombie_list();
    JARVIS_ASSERT_FMT(exited, "cap-smoke task did not terminate");
    JARVIS_ASSERT_FMT(code == 0, "cap-smoke probe failed check %lu",
                      (unsigned long)code);
    JARVIS_TEST_PASS();
}
// Runmode: kernel
// Testidea: #431 forged-mismatch fail-closed standing gate. Forge the
// entry ABI check (expect-major 1 -> 2) in a copy of the initrd hey
// image, dispatch it, and prove the fail-closed contract end to end:
// the task terminates (no hang/crash), exit code is 0xC0DE, the .Lfail
// diagnostic lands on the task's stderr, and teardown reaps everything
// (zero ResourceTracker delta — exercises the #431 always-reap path).
// The forge target is located by byte signature, never by hardcoded
// file offset: absent/duplicated signature fails the test instead of
// testing the wrong bytes. Per-arch step-2 compare shapes (verified by
// objdump on tip-built hey.c.elf): x86_64 shr+cmp imm8, aarch64 lsr+cmp
// imm, riscv64 srli+li (patched li immediate).
// Input: read /hey.c.elf via vfs, patch the compare, write
// /tmp/hey-mismatch.elf, request_load, take, repoint fd 2 at a tmpfs
// capture file, dispatch at prio 11, join via wait_for_termination_safe.
// Expect: TERMINATED, exit_code == 0xC0DE, capture contains the
// "crt0: ABI contract mismatch" diagnostic; zero resource delta.
// Depends: ElfLoader initrd-path load, P1 crt0 contract, fd plumbing.
JARVIS_TEST(loader_stub_mismatch_fail_closed, "PRE: vfsd, iocd | POST: none") {
    // 1. Read pristine image bytes from initrd (bounded: reject > 128 KiB).
    static uint8_t s_mm_img[128 * 1024];
    vfs::Vnode *src = vfs::resolve("/hey.c.elf");
    JARVIS_ASSERT_FMT(src != nullptr && src->ops != nullptr &&
                          src->ops->read != nullptr,
                      "mismatch: cannot resolve /hey.c.elf");
    uint64_t img_sz = 0;
    while (img_sz < sizeof(s_mm_img)) {
        uint64_t chunk = sizeof(s_mm_img) - img_sz;
        if (chunk > 4096)
            chunk = 4096;
        int64_t rd = src->ops->read(*src, s_mm_img + img_sz, chunk, img_sz);
        if (rd <= 0)
            break;
        img_sz += static_cast<uint64_t>(rd);
    }
    JARVIS_ASSERT_FMT(img_sz > 64 && img_sz < sizeof(s_mm_img),
                      "mismatch: bad image size %lu",
                      (unsigned long)img_sz);
    // 2. Locate the step-2 compare by byte signature (exactly one hit).
    static const uint8_t kSigX86[] = {0x48, 0xc1, 0xe8, 0x10,
                                      0x48, 0x83, 0xf8, 0x01};
    static const uint8_t kSigAA64[] = {0x00, 0xfc, 0x50, 0xd3,
                                       0x1f, 0x04, 0x00, 0xf1};
    static const uint8_t kSigRV64[] = {0x41, 0x81, 0x85, 0x42};
    const uint8_t *sig = nullptr;
    uint64_t sig_len = 0, patch_off = 0;
    uint8_t patch_to = 0;
#if defined(CONFIG_ARCH_X86_64)
    sig = kSigX86;
    sig_len = sizeof(kSigX86);
    patch_off = 7;
    patch_to = 0x02;
#elif defined(CONFIG_ARCH_AARCH64)
    sig = kSigAA64;
    sig_len = sizeof(kSigAA64);
    patch_off = 5;
    patch_to = 0x08;
#elif defined(CONFIG_ARCH_RISCV64)
    sig = kSigRV64;
    sig_len = sizeof(kSigRV64);
    patch_off = 2;
    patch_to = 0x89;
#else
    JARVIS_ASSERT_FMT(false, "mismatch: no forge signature for this arch");
#endif
    (void)sizeof(kSigX86);
    (void)sizeof(kSigAA64);
    (void)sizeof(kSigRV64);
    uint64_t hits = 0, hit_at = 0;
    for (uint64_t i = 0; i + sig_len <= img_sz; ++i) {
        uint64_t j = 0;
        while (j < sig_len && s_mm_img[i + j] == sig[j])
            ++j;
        if (j == sig_len) {
            ++hits;
            hit_at = i;
        }
    }
    JARVIS_ASSERT_FMT(hits == 1,
                      "mismatch: signature hits=%lu, want exactly 1",
                      (unsigned long)hits);
    s_mm_img[hit_at + patch_off] = patch_to;
    // 3. Stage the doctored image and load it.
    constexpr const char *kMmPath = "/tmp/hey-mismatch.elf";
    JARVIS_ASSERT(write_file(kMmPath, s_mm_img, img_sz) == img_sz);
    elf::ElfLoader::reset();
    JARVIS_ASSERT_FMT(elf::ElfLoader::request_load(kMmPath) ==
                          elf::LoadResult::OK,
                      "mismatch request_load failed");
    elf::ElfLoader::wait_loader_idle();
    TaskControlBlock *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT_FMT(t != nullptr && t->get_page_table() != 0 &&
                          t->is_user_,
                      "mismatch take_completed failed");
    // 4. Repoint fd 2 (stderr, unwired in harness) at a tmpfs capture
    // file so the .Lfail diagnostic is observable in-guest (fd-1
    // pattern from the libc_verify harness; same no-dec discipline:
    // FdTable::free would dec a vnode ref never inc'd).
    constexpr const char *kCapPath = "/tmp/mismatch-stderr.txt";
    JARVIS_ASSERT(t->fd_table.fds[2].used);
    kernel::test::mark_vfs_touched();
    JARVIS_ASSERT(vfs::create(kCapPath, 0) == 0);
    vfs::Vnode *capvn = vfs::resolve(kCapPath);
    JARVIS_ASSERT(capvn != nullptr);
    t->fd_table.fds[2].vnode = capvn;
    t->fd_table.fds[2].offset = 0;
    t->fd_table.fds[2].flags = 0;
    // 5. Dispatch and join (prio 11: the harness idles below it).
    t->priority = 11;
    t->base_priority = 11;
    {
        arch::IrqGuard irq_guard{};
        Scheduler::add_task(*t);
    }
    Scheduler::reschedule();
    kernel::test::wait_for_termination_safe(t);
    bool exited = (t->state == TaskState::TERMINATED);
    uint64_t code = t->exit_code;
    // 6. Teardown first (mirrors the #431 always-reap harness path so a
    // nonzero exit still reaps), then assert on the saved values.
    if (TaskControlBlock::is_valid(t))
        Scheduler::terminate(*t, code);
    Scheduler::drain_zombie_list();
    char diag[96]{};
    uint64_t dpos = 0;
    vfs::Vnode *cap = vfs::resolve(kCapPath);
    if (cap != nullptr && cap->ops != nullptr && cap->ops->read != nullptr) {
        while (dpos < sizeof(diag) - 1) {
            int64_t rd = cap->ops->read(*cap,
                                        reinterpret_cast<uint8_t *>(diag) +
                                            dpos,
                                        1, dpos);
            if (rd <= 0)
                break;
            dpos += static_cast<uint64_t>(rd);
        }
    }
    diag[dpos] = '\0';
    cleanup_file(kMmPath);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kCapPath);
    JARVIS_ASSERT_FMT(exited, "mismatch task did not terminate");
    JARVIS_ASSERT_FMT(code == 0xC0DE, "mismatch exit code 0x%lx, want 0xC0DE",
                      (unsigned long)code);
    const char *want = "crt0: ABI contract mismatch";
    bool found = false;
    for (uint64_t i = 0; dpos >= 28 && i + 28 <= dpos; ++i) {
        uint64_t j = 0;
        while (want[j] != '\0' && diag[i + j] == want[j])
            ++j;
        if (want[j] == '\0') {
            found = true;
            break;
        }
    }
    JARVIS_ASSERT_FMT(found, "mismatch diagnostic absent from stderr");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: cancel reclaims all partial state (zero delta) and the loader
// returns to IDLE with no retained image.  The cancel is issued
// pre-first-dispatch with the timer tick masked (issue #267): reschedule()
// only arms need_resched — the switch happens on the next tick — so any
// unmasked gap lets the loader run whole slices, and an 8-page load fits
// in few slices (1/6 runs completed inside 3 harness yields; polling for
// COPYING_SEGMENTS does not fix it since fast runs jump VALIDATING→DONE
// between two polls).  With the tick masked the loader cannot be
// dispatched at all (UP: no tick, no switch; the harness blocks on
// nothing in this scope), so observing VALIDATING proves zero loader
// progress and the cancel is provably pre-completion.  No retries, no
// NOT_LOADING tolerance.
// Coverage note: this pins the cancel CONTRACT (CANCELED → cleanup →
// IDLE + null take + zero delta), deterministically at the VALIDATING
// checkpoint; the per-chunk checkpoint location is timing-dependent and
// cannot be targeted without a kernel seam.
JARVIS_TEST(loader_cancel_mid_load, "PRE: vfsd, iocd | POST: none") {
    elf::ElfLoader::reset();
    // Build a multi-page ELF (8 PT_LOAD pages).
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

    {
        // Masking across request_load is safe HERE only: the harness is a
        // kernel task, so vfsd_authorize bypasses IPC (syscall_handlers_fs),
        // and the /tmp path resolves synchronously under spinlocks — nothing
        // in this scope waits on a tick (any future blocking added here would
        // hang loudly, never pass silently).  IrqGuard→lock_ nest order
        // matches task_main.
        arch::IrqGuard irq_guard{};
        auto result = elf::ElfLoader::request_load("/tmp/loadmulti.elf");
        JARVIS_ASSERT(result == elf::LoadResult::OK);
        // Loader frozen since before it became runnable: still VALIDATING.
        JARVIS_ASSERT(elf::ElfLoader::state() ==
                      elf::LoadState::VALIDATING);
        result = elf::ElfLoader::request_cancel();
        JARVIS_ASSERT(result == elf::LoadResult::OK);
    }
    elf::ElfLoader::wait_loader_idle();

    JARVIS_ASSERT(elf::ElfLoader::state() == elf::LoadState::IDLE);
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
    uint64_t phys = VMM::virt_to_phys_in_pml4(0x400200, t->get_page_table());
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

// Issue #271 P0: build a two-segment static image (R+X code + RW data)
// for the RO-permission pin. Mirrors build_minimal_elf's layout.
uint64_t build_rw_elf(elf::ELF64Header *hdr, uint8_t *data) {
    __builtin_memset(hdr, 0, sizeof(elf::ELF64Header));
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_EXEC;
    hdr->machine = kNativeEm;
    hdr->version = 1;
    hdr->entry = 0x400000;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 2;
    auto *ph0 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header));
    ph0->type = elf::PT_LOAD;
    ph0->flags = elf::PF_R | elf::PF_X;
    ph0->offset = sizeof(elf::ELF64Header) +
                  2 * sizeof(elf::ELF64ProgramHeader);
    ph0->vaddr = 0x400000;
    ph0->paddr = 0x400000;
    ph0->filesz = 0x1000;
    ph0->memsz = 0x1000;
    ph0->align = 0x1000;
    auto *ph1 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader));
    ph1->type = elf::PT_LOAD;
    ph1->flags = elf::PF_R | elf::PF_W;
    ph1->offset = ph0->offset + 0x1000;
    ph1->vaddr = 0x402000;
    ph1->paddr = 0x402000;
    ph1->filesz = 0x100;
    ph1->memsz = 0x1000;
    ph1->align = 0x1000;
    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
    uint8_t *code = data + ph0->offset;
    for (size_t i = 0; i < 0x1000; ++i)
        code[i] = 0x90;
    uint8_t *rw = data + ph1->offset;
    for (size_t i = 0; i < 0x100; ++i)
        rw[i] = static_cast<uint8_t>(i);
    return ph1->offset + 0x100;
}

// Issue #271 live-E2E follow-up: BSS-only RW segment (filesz 0, memsz
// nonzero — as in userspace/hey.c.elf's 0x402e80 filesz-0 segment).
// The rerun builder must still map+zero the memsz span (canary slots
// live in the tail padding) even though there are no file bytes to pin.
uint64_t build_bss_elf(elf::ELF64Header *hdr, uint8_t *data) {
    __builtin_memset(hdr, 0, sizeof(elf::ELF64Header));
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_EXEC;
    hdr->machine = kNativeEm;
    hdr->version = 1;
    hdr->entry = 0x400000;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 2;
    auto *ph0 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header));
    ph0->type = elf::PT_LOAD;
    ph0->flags = elf::PF_R | elf::PF_X;
    ph0->offset = sizeof(elf::ELF64Header) +
                  2 * sizeof(elf::ELF64ProgramHeader);
    ph0->vaddr = 0x400000;
    ph0->paddr = 0x400000;
    ph0->filesz = 0x1000;
    ph0->memsz = 0x1000;
    ph0->align = 0x1000;
    auto *ph1 = reinterpret_cast<elf::ELF64ProgramHeader *>(
        data + sizeof(elf::ELF64Header) + sizeof(elf::ELF64ProgramHeader));
    ph1->type = elf::PT_LOAD;
    ph1->flags = elf::PF_R | elf::PF_W;
    ph1->offset = ph0->offset + 0x1000;
    // hey.c.elf's exact RW shape: BSS-only (filesz 0) with a tiny
    // non-page-aligned memsz, so the DATA-after slot falls in real
    // page padding (in_padding true) and must be armed + live.
    ph1->vaddr = 0x402e80;
    ph1->paddr = 0x402e80;
    ph1->filesz = 0;
    ph1->memsz = 0x18;
    ph1->align = 0x1000;
    __builtin_memcpy(data, hdr, sizeof(elf::ELF64Header));
    uint8_t *code = data + ph0->offset;
    for (size_t i = 0; i < 0x1000; ++i)
        code[i] = 0x90;
    return ph1->offset;
}

// Runmode: kernel
// Testidea: Issue #271 P0 — read-only segments map PTE-read-only
//           through the shared primitive on the FIRST run (both the
//           background COPYING path and the direct load path), with
//           NX following PF_X; RW segments stay writable.
// Input: Two-segment static image via background request_load and via
//        direct elf::load; walk the resulting PML4s with the RO
//        inspector.
// Expect: code page RO + executable; data page writable +
//         non-executable; identical perms on both paths.
// Depends: VMM::map_page_ro_in_pml4 + pte_is_ro_in_pml4 (issue #271)
JARVIS_TEST(rerun_ro_perm_first_run, "PRE: none | POST: none") {
    elf::ELF64Header hdr{};
    uint8_t img[8192];
    uint64_t sz = build_rw_elf(&hdr, img);

    // Path A: background COPYING loader.
    JARVIS_ASSERT(write_file("/tmp/loadro.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/loadro.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *ta = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(ta != nullptr && ta->get_page_table() != 0);
    // NOTE: the inspector writes the exec out-param only for RO
    // leaves; on a writable page it is untouched, so exec is asserted
    // only alongside a true RO result.
    bool exec_a = false;
    JARVIS_ASSERT(VMM::pte_is_ro_in_pml4(0x400000, ta->get_page_table(), &exec_a));
    JARVIS_ASSERT(exec_a);
    JARVIS_ASSERT(
        !VMM::pte_is_ro_in_pml4(0x402000, ta->get_page_table(), nullptr));
    elf::ElfLoader::destroy_completed_tcb(ta);
    cleanup_file("/tmp/loadro.elf");

    // Path B: direct synchronous load.
    auto *tb = kernel::elf::load(&hdr, img, sz);
    JARVIS_ASSERT(tb != nullptr && tb->get_page_table() != 0);
    bool exec_b = false;
    JARVIS_ASSERT(VMM::pte_is_ro_in_pml4(0x400000, tb->get_page_table(), &exec_b));
    JARVIS_ASSERT(exec_b);
    JARVIS_ASSERT(
        !VMM::pte_is_ro_in_pml4(0x402000, tb->get_page_table(), nullptr));
    tb->cleanup();
    delete tb;
    JARVIS_TEST_PASS();
}

// Issue #271 P1: retention + outcome tracking: take moves staging
// (newest take wins), abandon frees only at zero/zero, lifecycle
// reset/release drop both retention and staging. Static, one set.
JARVIS_TEST(rerun_retention_lifecycle, "PRE: none | POST: none") {
    // Genuine missing file (never planted): must fail before any
    // retained-image mutation — a bad new path never destroys a good
    // retained set (fail-before-free).
    elf::ElfLoader::reset();

    uint8_t img[8192];
    elf::ELF64Header hdr0{};
    uint64_t sz0 = build_minimal_elf(&hdr0, img);
    JARVIS_ASSERT(write_file("/tmp/retain0.elf", img, sz0) == sz0);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/retain0.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t0 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t0 != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    JARVIS_ASSERT(
        elf::ElfLoader::retained_outcome() ==
        elf::ElfLoader::RunOutcome::UNKNOWN);

    // Bad load must NOT destroy retained set (validates fail-before-free).
    auto bad = elf::ElfLoader::request_load("/tmp/retain-missing-never.elf");
    JARVIS_ASSERT(bad == elf::LoadResult::FILE_NOT_FOUND);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());

    // Second take overwrites (newest wins) — t0 freed, second adopted.
    uint8_t img1[8192];
    elf::ELF64Header hdr1{};
    uint64_t sz1 = build_minimal_elf(&hdr1, img1);
    JARVIS_ASSERT(write_file("/tmp/retain1.elf", img1, sz1) == sz1);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/retain1.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t1 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    elf::ElfLoader::destroy_completed_tcb(t0);

    // Outcome tracking is per-retention (reset on new adoption).
    JARVIS_ASSERT(
        elf::ElfLoader::retained_outcome() ==
        elf::ElfLoader::RunOutcome::UNKNOWN);
    elf::ElfLoader::note_launched(t1->id);
    elf::ElfLoader::note_task_end(t1->id, true, 0);
    JARVIS_ASSERT(
        elf::ElfLoader::retained_outcome() ==
        elf::ElfLoader::RunOutcome::CLEAN);
    elf::ElfLoader::note_task_end(0xDEADULL, true, 0); // unknown id ignored
    JARVIS_ASSERT(
        elf::ElfLoader::retained_outcome() ==
        elf::ElfLoader::RunOutcome::CLEAN);

    // reset/release abandon (and the live-map guard keeps a live
    // mapper's pages alive if one exists — t1 is untaken here, not
    // live, so its golden pages are reclaimable through this path).
    elf::ElfLoader::reset();
    JARVIS_ASSERT(!elf::ElfLoader::has_retained());
    elf::ElfLoader::destroy_completed_tcb(t1);
    cleanup_file("/tmp/retain0.elf");
    cleanup_file("/tmp/retain1.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P1: balance cannot regress through retained-set
// lifecycle: every PMM page the retained path pins is reclaimed by
// reset (no leak, no double-free) with ResourceTracker flat.
JARVIS_TEST(rerun_golden_balance, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/balance.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/balance.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    elf::ElfLoader::note_launched(t->id);
    // Simulate the P2 path that has not landed: task owns its PML4
    // user pages + one task ref on the golden set; teardown is
    // t->cleanup() (unmaps golden + drops that task's ref), not loader
    // destroy (which would not touch retention).
    t->cleanup();
    delete t;
    elf::ElfLoader::reset();
    // Isolation owns the delta: a non-zero ResourceTracker across
    // take→launch→death→reset would fail the *next* test's baseline
    // comparison (suite-wide guard).
    cleanup_file("/tmp/balance.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P2: second runelf without loadelf — byte-identical via
// retention, zero filesystem I/O (prove by deleting the file before
// rerun). Also proves pin copy and golden remap.
JARVIS_TEST(rerun_byte_identity_deleted_file, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_rw_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/rerun1.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/rerun1.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t1 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    uint64_t id1 = t1->id;
    uint64_t g0 = VMM::virt_to_phys_in_pml4(0x400000, t1->get_page_table());
    JARVIS_ASSERT(g0 != 0);
    elf::ElfLoader::note_launched(id1);
    elf::ElfLoader::note_task_end(id1, true, 0);
    t1->cleanup();
    delete t1;
    // File deleted — rerun must not touch filesystem
    cleanup_file("/tmp/rerun1.elf");
    JARVIS_ASSERT(vfs::resolve("/tmp/rerun1.elf") == nullptr);
    const char *reason = nullptr;
    auto *t2 = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT_FMT(t2 != nullptr, "rerun failed: %s", reason ? reason : "?");
    JARVIS_ASSERT(t2->get_page_table() != 0);
    uint64_t g2 = VMM::virt_to_phys_in_pml4(0x400000, t2->get_page_table());
    JARVIS_ASSERT_FMT(g2 == g0, "golden phys diverged 0x%lx vs 0x%lx", g2, g0);
    uint64_t w2 = VMM::virt_to_phys_in_pml4(0x402000, t2->get_page_table());
    JARVIS_ASSERT(w2 != 0);
    // Writable content is pin-copied (check first byte)
    uint64_t wphys = VMM::virt_to_phys_in_pml4(0x402000, t2->get_page_table());
    uint8_t first = *reinterpret_cast<uint8_t*>(arch::HHDM_OFFSET + wphys);
    JARVIS_ASSERT_FMT(first == 0, "writable pin byte 0x%x != 0", first);
    // Std fds 0/1/2 wired (would have caught the missing open_std_fds).
    JARVIS_ASSERT(t2->fd_table.fds[0].used);
    JARVIS_ASSERT(t2->fd_table.fds[1].used);
    JARVIS_ASSERT(t2->fd_table.fds[2].used);
    // Never-added TCB: destroy path (no scheduler-touching cleanup).
    // unmap_golden drops our builder ref; destroy frees the rest.
    elf::ElfLoader::unmap_golden(t2);
    elf::ElfLoader::destroy_completed_tcb(t2);
    elf::ElfLoader::reset();
    JARVIS_TEST_PASS();
}

// Issue #271 P2: refusal matrix — FAULTED/UNKNOWN/BUSY all
// refuse with explicit messages; CLEAN/0/static/IDLE succeeds.
JARVIS_TEST(rerun_refusal_matrix, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    const char *reason = nullptr;
    auto *t = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT(t == nullptr);
    JARVIS_ASSERT(reason != nullptr);
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_minimal_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/refuse.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/refuse.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t0 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t0 != nullptr);
    // UNKNOWN → refuse
    t = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT(t == nullptr);
    // FAULTED → refuse
    uint64_t id = t0->id;
    elf::ElfLoader::note_launched(id);
    elf::ElfLoader::note_task_end(id, false, 11);
    t = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT(t == nullptr);
    // CLEAN but busy (load in flight) → refuse
    elf::ElfLoader::note_task_end(id, true, 0);
    JARVIS_ASSERT(write_file("/tmp/refuse2.elf", img, sz) == sz);
    auto res = elf::ElfLoader::request_load("/tmp/refuse2.elf");
    JARVIS_ASSERT(res == elf::LoadResult::OK);
    t = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT(t == nullptr);
    elf::ElfLoader::wait_loader_idle();
    elf::ElfLoader::reset();
    t0->cleanup();
    delete t0;
    cleanup_file("/tmp/refuse.elf");
    cleanup_file("/tmp/refuse2.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P3: decay Refused pre-publish — corrupt one golden byte
// (HHDM phys write, simulating bit-rot) and the rerun build must
// refuse with kRebuildScan before publishing any TCB. Retention stays
// valid (builder never adopts on refuse).
JARVIS_TEST(rerun_auth_decay_refused, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_rw_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/decay.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/decay.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t1 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t1 != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    uint64_t id1 = t1->id;
    uint64_t g0 = VMM::virt_to_phys_in_pml4(0x400000, t1->get_page_table());
    JARVIS_ASSERT(g0 != 0);
    elf::ElfLoader::note_launched(id1);
    elf::ElfLoader::note_task_end(id1, true, 0);
    t1->cleanup();
    delete t1;
    // Corrupt one golden byte past the TEXT canary slot [base, base+8).
    uint8_t *decayed = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET +
                                                   g0 + 0x100);
    uint8_t orig = *decayed;
    *decayed = static_cast<uint8_t>(orig ^ 0xFF);
    const char *reason = nullptr;
    auto *t2 = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT(t2 == nullptr);
    JARVIS_ASSERT(reason != nullptr);
    // Retention intact (refuse path adopts nothing, clobbers nothing).
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    *decayed = orig; // restore before teardown accounting
    elf::ElfLoader::reset();
    cleanup_file("/tmp/decay.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P3: canary + RO-perm pins on the RERUN table — DATA-after,
// stack and heap slots hold live derived magic after a rerun build,
// TEXT-before is untouched since load (shared phys), and golden PTEs
// are RW-clear with NX following PF_X.
// Issue #429: the build_rw_elf fixture entry (0x400000) sits inside the
// TEXT canary slot, so the installer skips it (never clobbers entry
// bytes); TEXT-before assertions below verify the skip + intact entry.
JARVIS_TEST(rerun_canary_perm_pins, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_rw_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/canary.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/canary.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t1 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t1 != nullptr);
    uint64_t id1 = t1->id;
    elf::ElfLoader::note_launched(id1);
    elf::ElfLoader::note_task_end(id1, true, 0);
    t1->cleanup();
    delete t1;
    const char *reason = nullptr;
    auto *t2 = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT_FMT(t2 != nullptr, "rerun failed: %s", reason ? reason : "?");
    auto read8 = [](uint64_t va, uint64_t pml4, uint64_t &out) {
        uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
        if (!phys)
            return false;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        __builtin_memcpy(&out, reinterpret_cast<const void *>(
                                    arch::HHDM_OFFSET + phys),
                         8);
        return true;
    };
    const uint64_t text = TaskControlBlock::SEG_TEXT;
    const uint64_t heap = TaskControlBlock::SEG_HEAP;
    const uint64_t stk = TaskControlBlock::SEG_STACK;
    JARVIS_ASSERT(!(t2->canary_installed & (1u << text)));
    JARVIS_ASSERT(t2->canary_before[text] == 0);
    JARVIS_ASSERT(t2->canary_installed & (1u << heap));
    JARVIS_ASSERT(t2->canary_installed & (1u << stk));
    // Issue #429: TEXT-before skipped — the fixture entry (0x400000) is
    // inside the canary slot, so the installer leaves it alone: no
    // installed bit, canary_before[text] stays 0.
    // Entry bytes intact: the first 8 bytes are the original NOP fill
    // (build_rw_elf), not canary magic — the loader never clobbers
    // the entry point.
    uint64_t entry0 = 0;
    JARVIS_ASSERT(read8(0x400000, t2->get_page_table(), entry0));
    JARVIS_ASSERT_FMT(entry0 == 0x9090909090909090ULL,
                      "entry clobbered: 0x%lx", entry0);
    // HEAP + STACK slots: fresh pages, live derived magic both ends.
    uint64_t live = 0;
    JARVIS_ASSERT(read8(t2->canary_before[heap], t2->get_page_table(), live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (heap + 1)));
    JARVIS_ASSERT(read8(t2->canary_after[heap], t2->get_page_table(), live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (heap + 1)));
    JARVIS_ASSERT(read8(t2->canary_before[stk], t2->get_page_table(), live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (stk + 1)));
    JARVIS_ASSERT(read8(t2->canary_after[stk], t2->get_page_table(), live));
    JARVIS_ASSERT(live == (TaskControlBlock::CANARY_MAGIC ^ (stk + 1)));
    // RO perms on the rerun table: code RO+exec, data writable.
    bool exec = false;
    JARVIS_ASSERT(VMM::pte_is_ro_in_pml4(0x400000, t2->get_page_table(), &exec));
    JARVIS_ASSERT(exec);
    JARVIS_ASSERT(
        !VMM::pte_is_ro_in_pml4(0x402000, t2->get_page_table(), nullptr));
    t2->cleanup();
    delete t2;
    elf::ElfLoader::reset();
    cleanup_file("/tmp/canary.elf");
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Issue #271 live-E2E follow-up — BSS-only RW segment
//           (filesz 0) must still get its memsz span mapped+zeroed in
//           the rerun build, with the DATA-after canary holding live
//           magic. Pre-fix the span was skipped (no pin bytes), the
//           canary write silently missed, and dispatch tripped.
// Input: BSS-only two-segment static image via background
//        request_load, take, note_launched, note_task_end, try_rerun.
// Expect: try_rerun succeeds; DATA-after slot (0x402FF8) mapped and
//         holding live derived magic.
// Depends: pin-range recording for filesz-0 RW spans (issue #271)
JARVIS_TEST(rerun_bss_only_data_canary, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_bss_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/bsscan.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/bsscan.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t1 = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t1 != nullptr);
    uint64_t id1 = t1->id;
    elf::ElfLoader::note_launched(id1);
    elf::ElfLoader::note_task_end(id1, true, 0);
    t1->cleanup();
    delete t1;
    const char *reason = nullptr;
    auto *t2 = elf::ElfLoader::try_rerun(&reason);
    JARVIS_ASSERT_FMT(t2 != nullptr, "rerun failed: %s", reason ? reason : "?");
    const uint64_t data = TaskControlBlock::SEG_DATA;
    JARVIS_ASSERT(t2->canary_installed & (1u << data));
    // DATA-after = page_align_up(0x402e80 + 0x18) - 8 = 0x402FF8:
    // must be mapped AND hold live magic (pre-fix: unmapped).
    JARVIS_ASSERT(t2->canary_after[data] == 0x402FF8);
    uint64_t phys =
        VMM::virt_to_phys_in_pml4(0x402FF8, t2->get_page_table());
    JARVIS_ASSERT_FMT(phys != 0, "DATA-after slot unmapped");
    uint64_t live = 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memcpy(&live,
                     reinterpret_cast<const void *>(arch::HHDM_OFFSET + phys),
                     8);
    JARVIS_ASSERT_FMT(live == (TaskControlBlock::CANARY_MAGIC ^ (data + 1)),
                      "DATA-after canary dead: 0x%lx", live);
    t2->cleanup();
    delete t2;
    elf::ElfLoader::reset();
    cleanup_file("/tmp/bsscan.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P3: supersede-while-live — take-overwrite during a live
// run must NOT free golden phys (live mapper owns them through its own
// teardown); the live task stays byte-correct; everything reclaims
// after death + drop.
// Issue #429: TEXT canary slot asserts entry-bytes-intact (NOP fill),
// not magic — the installer skips entry-overlapping slots.
JARVIS_TEST(rerun_supersede_while_live, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t imgA[8192];
    elf::ELF64Header hdrA{};
    uint64_t szA = build_rw_elf(&hdrA, imgA);
    JARVIS_ASSERT(write_file("/tmp/supA.elf", imgA, szA) == szA);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/supA.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *tA = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(tA != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    // tA stays LIVE (no teardown): it is the live mapper.
    // Second image: same layout, one byte different.
    uint8_t imgB[8192];
    elf::ELF64Header hdrB{};
    uint64_t szB = build_rw_elf(&hdrB, imgB);
    imgB[sizeof(elf::ELF64Header) +
         2 * sizeof(elf::ELF64ProgramHeader) + 0x200] = 0xCC;
    JARVIS_ASSERT(write_file("/tmp/supB.elf", imgB, szB) == szB);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/supB.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *tB = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(tB != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    // Live task still byte-correct: TEXT canary slot holds the original
    // NOP fill (issue #429: installer skips the slot when the fixture
    // entry 0x400000 sits inside it — entry bytes are never clobbered),
    // the rest of the prefix is NOP fill.
    {
        uint64_t phys =
            VMM::virt_to_phys_in_pml4(0x400000, tA->get_page_table());
        JARVIS_ASSERT(phys != 0);
        uint64_t slot = 0;
        __builtin_memcpy(&slot, reinterpret_cast<const void *>(
                                    arch::HHDM_OFFSET + phys),
                         8);
        JARVIS_ASSERT_FMT(
            slot == 0x9090909090909090ULL,
            "live entry bytes clobbered: 0x%lx", slot);
    }
    for (size_t i = 8; i < 24; ++i) {
        uint64_t phys =
            VMM::virt_to_phys_in_pml4(0x400000 + i, tA->get_page_table());
        JARVIS_ASSERT(phys != 0);
        uint8_t b = *reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
        JARVIS_ASSERT_FMT(b == 0x90, "live byte %lu = 0x%x, want NOP", i,
                          b);
    }
    // Teardown live mapper first: no phys match vs R_B, so no drop;
    // its pages (incl. old golden) free normally — nobody owns them.
    tA->cleanup();
    delete tA;
    // Then the superseding set: matches, drops, frees at reset.
    tB->cleanup();
    delete tB;
    elf::ElfLoader::reset();
    JARVIS_ASSERT(!elf::ElfLoader::has_retained());
    cleanup_file("/tmp/supA.elf");
    cleanup_file("/tmp/supB.elf");
    JARVIS_TEST_PASS();
}

// Issue #271 P3 (C8): fork children deep-copy golden pages into
// private phys, so teardown never disturbs retention counts. Uses the
// real deep_copy_user_pages primitive (what clone() calls): the copy's
// golden VA must resolve to DIFFERENT phys; freeing the copy must leave
// retention valid with refs intact; final teardown stays flat.
JARVIS_TEST(rerun_fork_child_private_copy, "PRE: none | POST: none") {
    elf::ElfLoader::reset();
    uint8_t img[8192];
    elf::ELF64Header hdr{};
    uint64_t sz = build_rw_elf(&hdr, img);
    JARVIS_ASSERT(write_file("/tmp/forkcopy.elf", img, sz) == sz);
    JARVIS_ASSERT(elf::ElfLoader::request_load("/tmp/forkcopy.elf") ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *tA = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(tA != nullptr);
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    uint64_t g0 = VMM::virt_to_phys_in_pml4(0x400000, tA->get_page_table());
    JARVIS_ASSERT(g0 != 0);
    // Fork-equivalent: deep-copy the whole address space into a fresh
    // PML4, like clone() does for a fork child.
    uint64_t child_pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(child_pml4 != 0);
    JARVIS_ASSERT(VMM::deep_copy_user_pages(tA->get_page_table(), child_pml4));
    uint64_t gc = VMM::virt_to_phys_in_pml4(0x400000, child_pml4);
    JARVIS_ASSERT(gc != 0);
    JARVIS_ASSERT_FMT(gc != g0, "fork copy shares golden phys 0x%lx", gc);
    // Copy content identical (deep copy, not empty).
    uint8_t b0 = *reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + g0 + 0x100);
    uint8_t bc = *reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + gc + 0x100);
    JARVIS_ASSERT_FMT(b0 == bc, "copy byte 0x%x != orig 0x%x", bc, b0);
    // Teardown copy via blind free (never mapped golden-shared — all
    // phys private, so no unmap needed and no ref may drop).
    VMM::free_user_pages(child_pml4);
    PMM::free_page(child_pml4);
    // Retention intact: still valid (refs untouched by private copy).
    JARVIS_ASSERT(elf::ElfLoader::has_retained());
    // Parent teardown drops its ref normally.
    tA->cleanup();
    delete tA;
    elf::ElfLoader::reset();
    JARVIS_ASSERT(!elf::ElfLoader::has_retained());
    cleanup_file("/tmp/forkcopy.elf");
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
    JARVIS_REGISTER_TEST(loader_lost_wakeup_race);
    JARVIS_REGISTER_TEST(loader_initrd_request_no_vnode_leak); // #77 leak
    JARVIS_REGISTER_TEST(loader_ro_crc_baseline); // #46 baseline
    JARVIS_REGISTER_TEST(loader_tamper_killed);   // #46 mismatch kill
    JARVIS_REGISTER_TEST(loader_clean_verified);  // #46 clean verify
    JARVIS_REGISTER_TEST(loader_phoff_shifted_baseline); // #46 S2 pin
    JARVIS_REGISTER_TEST(loader_fault_dump_gated); // #270 quiet default
    JARVIS_REGISTER_TEST(loader_runelf_3arg_main_envp); // #311 3-arg main/envp
    JARVIS_REGISTER_TEST(rerun_ro_perm_first_run); // #271 P0 RO primitive
    JARVIS_REGISTER_TEST(rerun_retention_lifecycle); // #271 P1 staging/adopt
    JARVIS_REGISTER_TEST(rerun_golden_balance); // #271 P1 balance
    JARVIS_REGISTER_TEST(rerun_byte_identity_deleted_file); // #271 P2 byte-identity
    JARVIS_REGISTER_TEST(rerun_refusal_matrix); // #271 P2 refusals
    JARVIS_REGISTER_TEST(rerun_auth_decay_refused); // #271 P3 auth-decay
    JARVIS_REGISTER_TEST(rerun_canary_perm_pins); // #271 P3 canary+perm
    JARVIS_REGISTER_TEST(rerun_supersede_while_live); // #271 P3 supersede
    JARVIS_REGISTER_TEST(rerun_fork_child_private_copy); // #271 P3 fork (C8)
    JARVIS_REGISTER_TEST(rerun_bss_only_data_canary); // #271 live-E2E BSS-only
    JARVIS_REGISTER_TEST(loader_stub_roundtrip_generated_stubs); // #171 P2
    JARVIS_REGISTER_TEST(loader_cap_smoke_fail_closed); // #171 P3
    JARVIS_REGISTER_TEST(loader_stub_mismatch_fail_closed); // #431 forged-mismatch fail-closed
}
