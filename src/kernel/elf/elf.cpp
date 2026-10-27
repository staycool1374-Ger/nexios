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

/// @file elf.cpp
/// @brief ELF64 loader implementation — header validation, segment loading,
/// stack setup.

#include <kernel/elf/elf.hpp>
#include <kernel/log/dmesg.hpp>
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/cap/mmio.hpp>
#include <kernel/cap/frame_map.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/ipc/pager_registry.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/arch/io.hpp>
#include <kernel/arch/msr.hpp>
#include <kernel/arch/page_table.hpp>
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
#include <kernel/arch/x86_64/hal/pcid.hpp>
#endif
#include <kernel/arch/timer.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/vfs/initrd_fs.hpp>
#include <constants.hpp>
#include <string.hpp>
#include <assert.hpp>
#include <string.hpp>
#include <constants.hpp>

namespace kernel {
namespace elf {

/// @brief Validate an ELF64 header (magic, class, endianness, machine, bounds).
/// @return true if the header is valid and safe to load.
// Compile-time goldens (issue #348-C, dmesg_catalog model): pin the
// validators above at build time.  Runtime callers use them identically.
namespace {

// Minimal valid header (machine selected per arch, like the validator).
static constexpr ELF64Header kElfGoldenGood = [] {
    ELF64Header hdr{};
    hdr.ident[0] = 0x7F;
    hdr.ident[1] = static_cast<uint8_t>('E');
    hdr.ident[2] = static_cast<uint8_t>('L');
    hdr.ident[3] = static_cast<uint8_t>('F');
    hdr.ident[4] = 2; // 64-bit
    hdr.ident[5] = 1; // little-endian
    hdr.type = ET_EXEC;
#if defined(CONFIG_ARCH_AARCH64)
    hdr.machine = 0xB7;
#elif defined(CONFIG_ARCH_RISCV64)
    hdr.machine = 0xF3;
#else
    hdr.machine = 0x3E;
#endif
    hdr.version = 1;
    hdr.entry = 0x400000ULL;
    hdr.phoff = 64;
    hdr.ehsize = sizeof(ELF64Header);
    hdr.phentsize = sizeof(ELF64ProgramHeader);
    hdr.phnum = 1;
    return hdr;
}();

// Valid read-execute segment, valid read-only segment, W+X segment.
static constexpr ELF64ProgramHeader kSegGoldenRx = {
    1, 5, 0, 0x400000ULL, 0x400000ULL, 0x1000ULL, 0x1000ULL, 0x1000ULL};
static constexpr ELF64ProgramHeader kSegGoldenR = {
    1, 4, 0x1000ULL, 0x401000ULL, 0x401000ULL, 0x1000ULL, 0x1000ULL, 0x1000ULL};
static constexpr ELF64ProgramHeader kSegGoldenWX = {
    1, 3, 0, 0x400000ULL, 0x400000ULL, 0x1000ULL, 0x1000ULL, 0x1000ULL};

static_assert(validate_header(&kElfGoldenGood), "golden header must pass");
static_assert(!validate_segment(&kSegGoldenWX, 0x10000ULL),
              "W+X segment must fail");
static_assert(validate_segment(&kSegGoldenRx, 0x10000ULL),
              "R+X segment must pass");
static_assert(validate_segment(&kSegGoldenR, 0x10000ULL),
              "read-only segment must pass");

} // namespace

/// @brief Round address up to the next page boundary.
static uint64_t page_align_up(uint64_t addr) {
    return (addr + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
}

/// @brief Round address down to the previous page boundary.
static uint64_t page_align_down(uint64_t addr) {
    return addr & ~(arch::PAGE_SIZE - 1);
}

static constexpr size_t INITIAL_HEAP_PAGES = 4;
static constexpr size_t INITIAL_HEAP_SIZE =
    INITIAL_HEAP_PAGES * arch::PAGE_SIZE;

/// @brief Count strings in a null-terminated array (argc/envc helper).
/// @note MP-4 (SMAP): called with USER argv/envp on the exec path; the array
///       and every string window are pre-certified mapped by
///       validate_argv_envp, but SMAP faults on PRESENT U/S=1 pages without
///       AC, so each read runs under stac/clac with fault recovery.
static int count_strings(const char *const *arr) {
    if (!arr)
        return 0;
    // Hoisted above the keep-alive edge: the goto below must not cross a
    // non-vacuous initialization (issue #149).
    int str_count{};
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_cnt);
    NEXIOS_FAULT_RECOVERY_KEEP(recover_cnt);
    arch::stac();
    while (arr[str_count])
        ++str_count;
    arch::clac();
    g_user_access_recover_ip = 0;
    return str_count;

recover_cnt:
    arch::clac();
    g_user_access_recover_ip = 0;
    return 0;
}

/// @brief Sum the lengths (including null terminators) of all strings in an
/// array.
static uint64_t total_string_len(const char *const *arr) {
    if (!arr)
        return 0;
    // Hoisted above the keep-alive edge (issue #149, see count_strings).
    uint64_t total{};
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_len);
    NEXIOS_FAULT_RECOVERY_KEEP(recover_len);
    arch::stac();
    for (int i = 0; arr[i]; ++i)
        total += strlen(arr[i]) + 1;
    arch::clac();
    g_user_access_recover_ip = 0;
    return total;

recover_len:
    arch::clac();
    g_user_access_recover_ip = 0;
    return 0;
}

/// @brief Copy all strings from a null-terminated array into a contiguous
/// buffer.
static void copy_strings(uint8_t *dest, const char *const *arr) {
    if (!arr)
        return;
    g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_cpy);
    NEXIOS_FAULT_RECOVERY_KEEP(recover_cpy);
    arch::stac();
    for (int i = 0; arr[i]; ++i) {
        size_t len = strlen(arr[i]) + 1;
        memcpy(dest, arr[i], len);
        dest += len;
    }
    arch::clac();
    g_user_access_recover_ip = 0;
    return;

recover_cpy:
    arch::clac();
    g_user_access_recover_ip = 0;
    return;
}

/// @brief Load all PT_LOAD segments and allocate user stack + initial heap.
/// @return true on success.
static bool load_segments_and_stack(const ELF64Header *hdr,
                                    const uint8_t *file_data, uint64_t pml4,
                                    uint64_t *out_ustack_phys,
                                    uint64_t file_size = 0) {
    // v0.4.0 MP-2: the highest PT_LOAD end must leave at least one unmapped
    // page before the heap, and the whole user region (heap + stack) must fit
    // below STACK_VADDR.  Absolute vaddr load-bearing: we cannot insert gaps
    // BETWEEN segments, so the red-zone validation is end-to-end.
    uint64_t max_seg_end = 0;

    if (hdr) {
        for (uint16_t i = 0; i < hdr->phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            file_data + hdr->phoff + static_cast<uint64_t>(i) * hdr->phentsize);
        if (!phdr || phdr->type != PT_LOAD)
            continue;
        if (!validate_segment(phdr, file_size))
            return false;

        uint64_t vaddr_base = page_align_down(phdr->vaddr);
        uint64_t vaddr_end = page_align_up(phdr->vaddr + phdr->memsz);
        uint64_t region_size = vaddr_end - vaddr_base;
        size_t num_pages = region_size / arch::PAGE_SIZE;

        if (vaddr_end > max_seg_end)
            max_seg_end = vaddr_end;

        uint64_t seg_phys = PMM::alloc_user_contiguous(num_pages);
        if (!seg_phys)
            return false;
        for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
            if (VMM::map_page_in_pml4_err(
                    vaddr_base + page_idx * arch::PAGE_SIZE,
                    seg_phys + page_idx * arch::PAGE_SIZE, true,
                    pml4) != errors::VMM_ERR_OK) {
                // Mid-load table OOM: the mapped prefix is reclaimed by
                // the caller's free_user_pages teardown; free only the
                // never-mapped suffix here (double-free otherwise).
                for (size_t rest = page_idx; rest < num_pages; ++rest) {
                    PMM::free_page(seg_phys + rest * arch::PAGE_SIZE);
                }
                return false;
            }
        }

        uint64_t offset_in_region = phdr->vaddr - vaddr_base;
        if (phdr->filesz != 0) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + seg_phys +
                                            offset_in_region),
                   file_data + phdr->offset, phdr->filesz);
        }

        if (phdr->memsz > phdr->filesz) {
            uint64_t zero_offset =
                arch::HHDM_OFFSET + seg_phys + offset_in_region + phdr->filesz;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            memset(reinterpret_cast<void *>(zero_offset), 0,
                   phdr->memsz - phdr->filesz);
        }

        // Issue #271 P0: read-only segments map PTE-read-only through
        // the shared primitive (first run included — otherwise the
        // live task holds a writable alias to retained bytes).
        // Content flowed via HHDM (perm-independent); canary install
        // later uses HHDM phys too.
        if (!(phdr->flags & PF_W)) {
            VMM::remap_range_ro_in_pml4(pml4, vaddr_base, region_size,
                                        (phdr->flags & PF_X) != 0);
        }
    }
    } // if (hdr)

    // v0.4.0 MP-2 red-zone validation (honest scope): one unmapped page must
    // separate the highest loaded segment from the heap, and the initial heap
    // must fit entirely below the user stack region (guard page at
    // STACK_VADDR).  Violation → load fails (caller tears down the partial
    // address space).
    if (max_seg_end > mem::HEAP_VADDR) {
        Logger::warn("ELF load rejected: segment end 0x%lx past heap base "
                     "0x%lx",
                     max_seg_end, mem::HEAP_VADDR);
        // Issue #234: loader validation rejects enter the ring.
        kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::ELF,
                                    kernel::log::kDmesgBase_ELF + 16,
                                    kernel::log::LogSeverity::ERROR,
                                    "segment past heap", max_seg_end);
        return false;
    }
    if (mem::HEAP_VADDR < max_seg_end + arch::PAGE_SIZE) {
        Logger::warn("ELF load rejected: no red-zone page between segment end "
                     "0x%lx and heap 0x%lx",
                     max_seg_end, mem::HEAP_VADDR);
        kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::ELF,
                                    kernel::log::kDmesgBase_ELF + 17,
                                    kernel::log::LogSeverity::ERROR,
                                    "no red-zone page", max_seg_end);
        return false;
    }
    if (mem::HEAP_VADDR + INITIAL_HEAP_SIZE > mem::STACK_VADDR) {
        Logger::warn("ELF load rejected: heap+initial size overruns user "
                     "stack base 0x%lx",
                     mem::STACK_VADDR);
        kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::ELF,
                                    kernel::log::kDmesgBase_ELF + 18,
                                    kernel::log::LogSeverity::ERROR,
                                    "heap overruns stack", mem::STACK_VADDR);
        return false;
    }

    size_t ustack_pages = (mem::STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t ustack_phys = PMM::alloc_user_contiguous(ustack_pages);
    if (!ustack_phys)
        return false;

    // Guard page: leave first page unmapped, start mapping at +arch::PAGE_SIZE
    // VULN-H1: user stack is writable-only (never executable).
    for (size_t i = 0; i < ustack_pages; ++i) {
        if (VMM::map_page_in_pml4_err(mem::STACK_VADDR + arch::PAGE_SIZE +
                                          i * arch::PAGE_SIZE,
                                      ustack_phys + i * arch::PAGE_SIZE, true,
                                      pml4, false) != errors::VMM_ERR_OK) {
            for (size_t rest = i; rest < ustack_pages; ++rest) {
                PMM::free_page(ustack_phys + rest * arch::PAGE_SIZE);
            }
            return false;
        }
    }

    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + ustack_phys), 0,
           mem::STACK_SIZE);

    size_t heap_pages = INITIAL_HEAP_PAGES;
    uint64_t heap_phys = PMM::alloc_user_contiguous(heap_pages);
    if (!heap_phys)
        return false;

    // VULN-H1: initial heap is writable-only (never executable).
    for (size_t i = 0; i < heap_pages; ++i) {
        if (VMM::map_page_in_pml4_err(mem::HEAP_VADDR + i * arch::PAGE_SIZE,
                                      heap_phys + i * arch::PAGE_SIZE, true,
                                      pml4,
                                      false) != errors::VMM_ERR_OK) {
            for (size_t rest = i; rest < heap_pages; ++rest) {
                PMM::free_page(heap_phys + rest * arch::PAGE_SIZE);
            }
            return false;
        }
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + heap_phys), 0,
                     INITIAL_HEAP_SIZE);

    if (out_ustack_phys)
        *out_ustack_phys = ustack_phys;
    return true;
}

/// @brief Allocate and map the user stack (guard page + pages) and the
///        initial heap into @p pml4.  Extracted from load_segments_and_stack
///        for reuse by the background ElfLoader (chunked loader) which maps
///        segments itself and only needs the stack + heap tail.
bool alloc_user_stack_and_heap(uint64_t pml4, uint64_t *out_ustack_phys) {
    return load_segments_and_stack(nullptr, nullptr, pml4, out_ustack_phys);
}

/// @brief Set up argv/envp on the user stack and return the initial RSP.
/// @return Initial user RSP, or 0 if the strings do not fit (VULN-U2).
static uint64_t setup_user_stack(uint64_t ustack_phys, const char *const *argv,
                                 const char *const *envp) {
    int argc = count_strings(argv);

    uint64_t str_total = total_string_len(argv) + total_string_len(envp);
    // VULN-U2: hard reservation check BEFORE any pointer arithmetic.  The
    // user stack is mem::STACK_SIZE bytes; sp walks backward by str_total
    // plus room for the argv/envp pointer arrays + alignment.  Without this
    // check, an attacker-controlled argv/envp total could underflow sp below
    // the stack allocation and corrupt adjacent physical memory.
    constexpr uint64_t kStackReserve = 2048;
    if (str_total + kStackReserve >= mem::STACK_SIZE)
        return 0;
    uint64_t stack_base = arch::HHDM_OFFSET + ustack_phys + mem::STACK_SIZE;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint8_t *stack_top = reinterpret_cast<uint8_t *>(stack_base);

    // Issue #311: reserve the top 8 bytes for the user-stack "after" canary
    // (canary_install_user_segments writes it at
    // STACK_VADDR+PAGE_SIZE+STACK_SIZE-8, i.e. stack_top-8).  The argument
    // layout is built downward from here, so without the reservation the
    // canary would clobber the highest slot — the envp[0] NULL terminator —
    // and a 3-argument main (which reads envp) would walk a bogus pointer.
    uint8_t *sp = stack_top - 8;
    sp -= str_total;
    if (str_total > 0) {
        uint8_t *str_pos = sp;
        copy_strings(str_pos, envp);
        str_pos += total_string_len(envp);
        copy_strings(str_pos, argv);
    }
    // Issue #335: the strings above live at pre-align `sp`; the pointer
    // fill below must address them there, not at the aligned `sp`
    // (which sits up to 15 bytes lower — every stored pointer would
    // otherwise dangle into the pad gap).
    uint8_t *str_base = sp;

    uint64_t align = reinterpret_cast<uint64_t>(sp) & 0xF;
    if (align)
        sp -= (16 - align);


    // Issue #334: string slots must hold USER addresses. `str_pos`
    // below walks the kernel (HHDM) mapping; user code dereferencing a
    // kernel-half address faults. Translate by the same bias as
    // `user_rsp` (null slots stay exactly NULL — runelf empty args
    // byte-identical).
    const uint64_t kern_to_user = mem::STACK_VADDR + arch::PAGE_SIZE +
                                  mem::STACK_SIZE -
                                  reinterpret_cast<uint64_t>(stack_top);
    int envc = count_strings(envp);
    uint64_t *ptr = reinterpret_cast<uint64_t *>(sp);

    for (int i = envc; i >= 0; --i) {
        *--ptr = 0;
    }
    uint64_t *envp_start = ptr;
    if (envp) {
        uint8_t *str_pos = str_base;
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_envp);
        arch::stac();
        for (int i = 0; i < envc; ++i) {
            envp_start[i] =
                reinterpret_cast<uint64_t>(str_pos) + kern_to_user;
            str_pos += strlen(envp[i]) + 1;
        }
        arch::clac();
        g_user_access_recover_ip = 0;
    }

    for (int i = argc; i >= 0; --i) {
        *--ptr = 0;
    }
    uint64_t *argv_start = ptr;
    if (argv) {
        uint8_t *str_pos = str_base + total_string_len(envp);
        g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_argv);
        arch::stac();
        for (int i = 0; i < argc; ++i) {
            argv_start[i] =
                reinterpret_cast<uint64_t>(str_pos) + kern_to_user;
            str_pos += strlen(argv[i]) + 1;
        }
        arch::clac();
        g_user_access_recover_ip = 0;
    }

    *--ptr = static_cast<uint64_t>(argc);

    uint64_t user_rsp = reinterpret_cast<uint64_t>(ptr);
    user_rsp = user_rsp - reinterpret_cast<uint64_t>(stack_top) +
               mem::STACK_VADDR + arch::PAGE_SIZE + mem::STACK_SIZE;

    // Keep-alive edges for recover_envp/recover_argv (issue #149): they live
    // here — not at their setups — because the gotos must not cross the
    // argv/envp initializations above. Any reachable spot before the labels
    // suffices; the edge originates from live code so the blocks survive.
    NEXIOS_FAULT_RECOVERY_KEEP(recover_envp);
    NEXIOS_FAULT_RECOVERY_KEEP(recover_argv);
    return user_rsp;

recover_envp:
    arch::clac();
    g_user_access_recover_ip = 0;
    return 0;

recover_argv:
    arch::clac();
    g_user_access_recover_ip = 0;
    return 0;
}

/// @brief Open /dev/tty as stdin/stdout/stderr for a new task.
static void open_std_fds(TaskControlBlock &tcb) {
    vfs::Vnode *tty = vfs::resolve("/dev/tty");
    if (!tty)
        return;
    for (int std_fd = 0; std_fd < 3; ++std_fd) {
        int fd = tcb.fd_table.alloc();
        if (fd == std_fd) {
            tcb.fd_table.fds[fd].vnode = tty;
            tcb.fd_table.fds[fd].offset = 0;
            tcb.fd_table.fds[fd].flags = 0;
            // Issue #268: each fd holds its own reference (FdTable::free
            // drops one per slot); the resolve() reference is released
            // after the loop below.
            vfs::vnode_ref_inc(tty);
            if (tty->ops && tty->ops->open)
                tty->ops->open(*tty, 0);
        }
    }
    vfs::release(tty);
}

static void install_segment_canaries(TaskControlBlock *tcb,
                                     const ELF64Header *hdr,
                                     const uint8_t *file_data,
                                     uint64_t file_size);

TaskControlBlock *load(const ELF64Header *hdr, const uint8_t *file_data,
                       uint64_t file_size) {
    if (!validate_header(hdr))
        return nullptr;

    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4)
        return nullptr;

    // Issue #96: freshly-cloned tables are the merged state by
    // construction — assert convergence before mapping user pages.
    VMM::assert_kernel_half_converged(pml4);

    uint64_t ustack_phys = 0;
    if (!load_segments_and_stack(hdr, file_data, pml4, &ustack_phys,
                                 file_size)) {
        VMM::free_user_pages(pml4);
        PMM::free_page(pml4);
        return nullptr;
    }

    return finalize_loaded_task(hdr, pml4, ustack_phys, file_data, file_size);
}

/// @brief Finish a partially-built load: allocate the TCB, kernel stack,
///        adopt @p pml4 as page_table_, wire std fds, setup the user stack
///        frame and install segment canaries.  Extracted from elf::load for
///        reuse by the background ElfLoader (chunked loader).
/// @param hdr Validated ELF header.
/// @param pml4 The built PML4 (user pages already mapped).
/// @param ustack_phys Physical base of the user stack.
/// @param phdr_image Buffer holding the ELF header followed by the full
///        program-header table (for canary installation).
/// @param file_size Total file size in bytes.
/// @return New TaskControlBlock, or nullptr on failure (partial state freed).
///
/// @brief Binding bootstrapping rule (issue #171 P4): no loader magic.
///        A freshly finalized task owns NO capabilities: cspace_ stays
///        null (lazy ensure_cspace on the first cap syscall) and the
///        loader passes only argc/argv/envp on the user stack plus the
///        entry point in the trap frame — no versioned launch struct
///        (reserved-absent), no pre-seeded handles. Whatever a driver
///        needs, its creator must grant/copy/mint into its CSpace before
///        first dispatch (caller-relative handles). Exec preserves the
///        rule by inheritance: exec_into_current keeps cspace_ untouched
///        (mappings/FDs/TLS reset, caps kept). Any behavior change here
///        requires a plan amendment and re-audit first (P4 scope: comment
///        + seeding test only).
TaskControlBlock *finalize_loaded_task(const ELF64Header *hdr, uint64_t pml4,
                                       uint64_t ustack_phys,
                                       const uint8_t *phdr_image,
                                       uint64_t file_size) {
    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb)
        return nullptr;
    memset(tcb, 0, sizeof(TaskControlBlock));
    tcb->iopb_slot_ = TaskControlBlock::IOPB_SLOT_NONE;
    tcb->tls_base_ = 0; // issue #74: fresh image starts with no TLS
    // Issue #41: watchdog disarmed, never-armed generation (see
    // TaskControlBlock::create).
    tcb->wdog_armed = false;
    tcb->wdog_period_ticks = 0;
    tcb->wdog_last_kick_tick = 0;
    tcb->wdog_expiry_tick = 0;
    tcb->wdog_gen = 0;
    tcb->magic = TaskControlBlock::TCB_MAGIC;
    tcb->id = kernel::Scheduler::alloc_id();
    tcb->state = TaskState::READY;
    tcb->priority = 2;
    tcb->period_ticks = 0;
    tcb->base_priority = 2;
    tcb->executed_ticks = 0;
    tcb->exec_ns_total = 0;
    tcb->exec_period_ns = 0;
    tcb->exec_window_start_tick = arch::Timer::ticks(); // issue #331
    tcb->exec_stamp_ns = 0;
    tcb->wcet_observed_ns = 0; // issue #283 (see TaskControlBlock::create)
    tcb->mem_alloc_ops_ = 0;   // issue #284 (see TaskControlBlock::create)
    tcb->mem_free_ops_ = 0;
    tcb->mem_prev_outstanding_ = 0;
    tcb->mem_prev_used_ = 0;
    tcb->mem_leak_streak_ = 0;
    tcb->sched_policy = SchedPolicy::AUTO; // issue #19 (AUTO=0; explicit)
    tcb->edf_exempt = false;
    tcb->edf_next_ = nullptr;
    tcb->edf_prev_ = nullptr;
    tcb->in_edf_queue_ = false;
    tcb->ready_deferred_ = false;
    tcb->remaining_ticks = 0;
    tcb->deadline_ticks = arch::Timer::ticks();
    tcb->clear_page_table();
    tcb->stack_phys_ = 0;
    tcb->kernel_stack = nullptr;
    tcb->kernel_stack_top = 0;
    tcb->user_stack_ = 0;
    tcb->user_stack_size_ = 0;
    tcb->program_break_start = 0;
    tcb->program_break = 0;
    tcb->buf_list_head = -1;
    tcb->fpu_used = false;
    tcb->sporadic_server = nullptr;

    init_task_common(*tcb);

    size_t kstack_pages =
        (TaskControlBlock::STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t kstack_phys = PMM::alloc_contiguous(kstack_pages);
    if (!kstack_phys) {
        tcb->cleanup();
        delete tcb;
        return nullptr;
    }
    tcb->stack_phys_ = kstack_phys;
    // #434 P2: route through the kslot window (guard page below the
    // stack) via the shared funnel — HHDM fallback only on exhaustion.
    TaskControlBlock::provision_kstack(tcb, kstack_phys,
                                       TaskControlBlock::STACK_SIZE);

    tcb->install_page_table(pml4);
    tcb->is_user_ = true;
#if defined(CONFIG_ARCH_X86_64) && CONFIG_PCID
    // Issue #156: eager PCID assignment (see task.cpp birth sites).
    tcb->pcid_ = arch::pcid_alloc();
#endif

    if (!finalize_user_tcb_tail(tcb, hdr, ustack_phys, phdr_image,
                                file_size)) {
        tcb->cleanup();
        delete tcb;
        return nullptr;
    }

    // (text/data/bss sizes, canaries, entry frame now live in
    // finalize_user_tcb_tail above.)
    return tcb;
}

bool finalize_user_tcb_tail(TaskControlBlock *tcb, const ELF64Header *hdr,
                            uint64_t ustack_phys, const uint8_t *phdr_image,
                            uint64_t file_size) {
    if (!tcb || !hdr || !phdr_image)
        return false;
    // Issue #271 P2: the rerun builder shares this tail, so std fds are
    // wired here (single funnel) — first-run TCBs get byte-identical init.
    open_std_fds(*tcb);
    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = mem::STACK_SIZE;
    tcb->program_break_start = mem::HEAP_VADDR;
    tcb->program_break = mem::HEAP_VADDR + INITIAL_HEAP_SIZE;

    uint64_t user_rsp = setup_user_stack(ustack_phys, nullptr, nullptr);
    if (user_rsp == 0)
        return false;

#if defined(CONFIG_ARCH_X86_64)
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = arch::SEG_USER_DATA;
    *--stack = user_rsp;
    *--stack = arch::RFLAGS_DEFAULT;
    *--stack = arch::SEG_USER_CODE;
    *--stack = hdr->entry;
    *--stack = 0;
    *--stack = 0;
    for (int j = 0; j < 15; ++j)
        *--stack = 0;
    tcb->context.rsp = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_AARCH64)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = 0;          // padding
    *--stack = 0;          // padding
    *--stack = 0x0;        // SPSR_EL1: M[4:0]=EL0t (AArch64 EL0) — bit 4 set
                           // would eret into AArch32 EL0, not EL0t.
    *--stack = hdr->entry; // ELR_EL1
    *--stack = user_rsp;   // SP_EL0
    for (int j = 0; j < 31; ++j)
        *--stack = 0;
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    // Trap frame matching syscall_entry.S save area (SAVE_SIZE 296 = 37
    // qwords: [0..30]=X1-X31, [31]=SEPC, [32]=SSTATUS, [33]=SCAUSE,
    // [34]=STVAL, [35]=RET, [36]=pad). Issue #206 M2: the old 21-push frame
    // under-ran the save area and set sstatus 0x100 (SPP=1 S-mode).
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    stack -= 37;
    __builtin_memset(stack, 0, 37 * 8);
    stack[1] = user_rsp;              // OFF_SP (X2)
    stack[31] = hdr->entry;           // OFF_SEPC
    // SSTATUS: SPIE=1, SPP=0 (U-mode) + FS=11b (issue #353: loaded
    // images may use FP from the first instruction; sret restores FS
    // from this slot).
    stack[32] = (1ULL << 5) | (3ULL << 13);
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

    // v0.4.0 MP-3: arm segment-boundary canaries (TEXT/DATA/STACK/HEAP +
    // kernel-stack) after the address space is fully mapped.
    install_segment_canaries(tcb, hdr, phdr_image, file_size);

    // Capture user-image segment sizes (text/data/bss) for the `tasks`
    // memory report.  Summed from the PT_LOAD program headers.
    {
        uint64_t text = 0, data = 0, bss = 0;
        for (uint16_t i = 0; i < hdr->phnum; ++i) {
            auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
                phdr_image + hdr->phoff +
                static_cast<uint64_t>(i) * hdr->phentsize);
            if (!phdr || phdr->type != PT_LOAD)
                continue;
            if (!validate_segment(phdr, file_size))
                continue;
            const uint64_t memsz = phdr->memsz;
            const uint64_t filesz = phdr->filesz;
            if (phdr->flags & PF_X)
                text += memsz;
            else if (phdr->flags & PF_W)
                data += memsz;
            if (memsz > filesz)
                bss += memsz - filesz;
        }
        tcb->text_size_ = text;
        tcb->data_size_ = data;
        tcb->bss_size_ = bss;
    }

    // Issue #77 handoff contract: a completed user image carries no TLS yet.
    // The dispatcher publishes tls_base_ to FS_BASE on user entry, so a
    // stale nonzero base would corrupt the new task's thread-local state
    // silently — fail stop here, not at first dispatch.  (When user TLS
    // lands, this relaxes to the TLS-template contract.)
    ENSURE(tcb->tls_base_ == 0);
    return true;
}

/// @brief Install v0.4.0 MP-3 segment-boundary canaries for a freshly loaded
///        ELF address space: TEXT = first R E PT_LOAD (before at the region
///        base — the consumed ELF header — and after at the mapped page
///        padding), DATA = last RW PT_LOAD after-canary at mapped-page
///        padding only.  A canary is only written where the slot bytes are
///        NOT part of the segment payload (page padding), so no loaded data
///        is corrupted.  Also arms the stack/heap + kernel-stack canaries.
static void install_segment_canaries(TaskControlBlock *tcb,
                                     const ELF64Header *hdr,
                                     const uint8_t *file_data,
                                     uint64_t file_size) {
    const uint64_t text = TaskControlBlock::SEG_TEXT;
    const uint64_t data = TaskControlBlock::SEG_DATA;
    uint64_t text_base = 0, text_after = 0, data_after = 0;

    for (uint16_t i = 0; i < hdr->phnum; ++i) {
        auto *phdr = reinterpret_cast<const ELF64ProgramHeader *>(
            file_data + hdr->phoff +
            static_cast<uint64_t>(i) * hdr->phentsize);
        if (!phdr || phdr->type != PT_LOAD)
            continue;
        if (!validate_segment(phdr, file_size))
            continue;

        const uint64_t region_end = page_align_up(phdr->vaddr + phdr->memsz);
        const uint64_t after_va = region_end - 8;
        const bool in_padding = after_va >= phdr->vaddr + phdr->memsz;

        if ((phdr->flags & PF_X) && !text_base) {
            text_base = page_align_down(phdr->vaddr);
            text_after = in_padding ? after_va : 0;
        } else if (phdr->flags & PF_W) {
            data_after = in_padding ? after_va : 0;
        }
    }

    if (text_base) {
        // Issue #429: never clobber the entry bytes. The canary slot is
        // the first 8 bytes of the first X segment; when the ELF entry
        // point lives there (the normal case: _start at the segment base),
        // writing the canary destroys the entry and the task faults on
        // dispatch (observed: UDF at entry, no sys_exit, unreaped leak).
        // Skip the install in that case: the verify loop skips zero
        // addresses, so an uninstalled canary is fail-closed (none
        // claimed, none checked). canary_before[text] stays 0.
        if (hdr->entry >= text_base && hdr->entry < text_base + 8) {
            tcb->canary_before[text] = 0;
        } else {
            tcb->canary_before[text] = text_base;
            canary_write_at(text_base,
                            TaskControlBlock::CANARY_MAGIC ^ (text + 1),
                            tcb->get_page_table());
            tcb->canary_installed |= (1u << text);
        }
    }
    if (text_after) {
        tcb->canary_after[text] = text_after;
        canary_write_at(text_after,
                        TaskControlBlock::CANARY_MAGIC ^ (text + 1),
                        tcb->get_page_table());
    }
    if (data_after) {
        tcb->canary_after[data] = data_after;
        canary_write_at(data_after,
                        TaskControlBlock::CANARY_MAGIC ^ (data + 1),
                        tcb->get_page_table());
        tcb->canary_installed |= (1u << data);
    }

    canary_install_user_segments(tcb);
    canary_install_kernel_stack(tcb);
}

bool exec_into_current(const ELF64Header *hdr, const uint8_t *data,
                       const char *const *argv, const char *const *envp,
                       uint64_t *regs, uint64_t file_size) {
    if (!validate_header(hdr))
        return false;

    auto *tcb = Scheduler::current_task();
    if (!tcb)
        return false;

    uint64_t new_pml4 = VMM::clone_kernel_pml4();
    if (!new_pml4)
        return false;
    // Issue #96: structural audit on the exec image (see load() above).
    VMM::assert_kernel_half_converged(new_pml4);

    uint64_t ustack_phys = 0;
    if (!load_segments_and_stack(hdr, data, new_pml4, &ustack_phys,
                                 file_size)) {
        VMM::free_user_pages(new_pml4);
        PMM::free_page(new_pml4);
        return false;
    }

    uint64_t user_rsp = setup_user_stack(ustack_phys, argv, envp);
    if (user_rsp == 0) {
        // VULN-U2: argv/envp total exceeds the stack reservation — abort the
        // exec and release the freshly-cloned address space before it leaks.
        VMM::free_user_pages(new_pml4);
        PMM::free_page(new_pml4);
        return false;
    }

    // Free zero-copy buffers mapped into the OLD page table before swapping it
    // out
    BufferPool::unmap_all(*tcb);
    // Issue #8: drain the task's user MMIO mappings from the OLD page table
    // before it is freed - a stored slot pml4 must never dangle past exec
    // (a later unmap/revoke/cleanup would write PTE entries into freed
    // page-table memory).
    cap::MmioUserMap::drain_task(*tcb);
    cap::FrameUserMap::drain_task(*tcb);
    // Issue #107: drain the pager registry before the old PML4 is freed so a
    // pager-mapped ledger PTE cannot outlive the frames it references.
    kernel::ipc::PagerRegistry::drain_task(*tcb);

    uint64_t old_pml4 = tcb->get_page_table();
    // Issue #92 canary relocation: the scheduler now samples user-task segment
    // canaries at every context switch and on a bounded tick cadence
    // (canary_check_in_scheduler_hooks).  Between the page_table_ swap below
    // and install_segment_canaries() at the tail of this function, the TCB's
    // canary_before/canary_after/canary_installed still describe the OLD image
    // while page_table_ is the NEW pml4 — a tick/context-switch sample landing
    // in this window would walk stale canary VAs against the new pml4 and
    // false-trip (spurious production panic / test-mode latch).  Clear
    // canary_installed so the verify loop is a no-op until the new canaries are
    // armed below (install_segment_canaries re-ORs the bits).
    tcb->canary_installed = 0;
    tcb->swap_page_table(new_pml4);
    tcb->is_user_ = true;
    // Issue #74: exec reuses the live TCB — the old image's TLS base must not
    // survive into the new image. Reset the field and clear the live register
    // (the new image calls TLS_SET itself per the §13 crt0 contract).
    tcb->tls_base_ = 0;
#if defined(CONFIG_ARCH_X86_64)
    arch::wrmsr(arch::MSR_FS_BASE, 0);
#elif defined(CONFIG_ARCH_AARCH64)
    arch::write_tpidr_el0(0);
#elif defined(CONFIG_ARCH_RISCV64)
    arch::write_tp(0);
#endif
#if defined(CONFIG_ARCH_X86_64)
    // Issue #157 H3: the TCB keeps its PCID across the table swap, but
    // the OLD image's entries cached under it must die with the old
    // table (tagged switches no longer flush).  Purge the context now —
    // the new image cannot have cached anything yet.
    arch::tlb_purge_context(tcb->pcid_);
#endif
    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = mem::STACK_SIZE;
    tcb->program_break_start = mem::HEAP_VADDR;
    tcb->program_break = mem::HEAP_VADDR + INITIAL_HEAP_SIZE;

    {
        vfs::Vnode *tty = vfs::resolve("/dev/tty");
        bool keep = (tty && tcb->fd_table.get(0) &&
                     tcb->fd_table.get(0)->vnode == tty);
        vfs::release(tty); // issue #268: probe only, not retained
        if (!keep) {
            for (int std_fd = 0; std_fd < 3; ++std_fd) {
                if (tcb->fd_table.get(std_fd)) {
                    tcb->fd_table.free(std_fd);
                }
            }
            vfs::Vnode *tty2 = vfs::resolve("/dev/tty");
            if (tty2) {
                for (int std_fd = 0; std_fd < 3; ++std_fd) {
                    int fd = tcb->fd_table.alloc();
                    if (fd == std_fd) {
                        tcb->fd_table.fds[fd].vnode = tty2;
                        tcb->fd_table.fds[fd].offset = 0;
                        tcb->fd_table.fds[fd].flags = 0;
                        // Issue #268: one reference per fd (see
                        // open_std_fds); drop the resolve() reference after.
                        vfs::vnode_ref_inc(tty2);
                        if (tty2->ops && tty2->ops->open)
                            tty2->ops->open(*tty2, 0);
                    }
                }
                vfs::release(tty2);
            }
        }
    }

#if defined(CONFIG_ARCH_X86_64)
    regs[17] = hdr->entry;
    regs[18] = arch::SEG_USER_CODE;
    regs[19] = arch::RFLAGS_DEFAULT;
    regs[20] = user_rsp;
    regs[21] = arch::SEG_USER_DATA;
    regs[0] = 0;
#elif defined(CONFIG_ARCH_AARCH64)
    regs[0] = 0;                 // X0 = 0
    regs[32] = hdr->entry;       // ELR_EL1 (issue #215: was regs[17]/x17)
    regs[33] = 0x0;              // SPSR_EL1: M[4:0]=EL0t (was regs[19])
    regs[31] = user_rsp;         // SP_EL0 (was regs[20])
#elif defined(CONFIG_ARCH_RISCV64)
    // Issue #206 M2: rewrite the live trap frame slots (syscall_entry.S
    // OFF_*: SP=idx1, SEPC=idx31, SSTATUS=idx32, A0=idx9).
    regs[31] = hdr->entry;  // SEPC
    // SSTATUS: SPIE=1, SPP=0 (U-mode) + FS=11b (issue #353: the exec'd
    // image may use FP from its first instruction, and sret restores FS
    // from this slot — FS=0 would fault it immediately).
    regs[32] = (1ULL << 5) | (3ULL << 13);
    regs[1] = user_rsp;     // OFF_SP
    regs[9] = 0;            // A0 = 0 (child/exec return value)
#endif

    if (arch::read_cr3() != new_pml4) {
        arch::write_cr3(new_pml4);
    }

    // v0.4.0 MP-3: arm segment-boundary canaries in the new address space.
    install_segment_canaries(tcb, hdr, data, file_size);

    if (old_pml4 && old_pml4 != VMM::get_kernel_pml4()) {
        VMM::free_user_pages(old_pml4);
        PMM::free_page(old_pml4);
    }

    return true;
}

} // namespace elf
} // namespace kernel