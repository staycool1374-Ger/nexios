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
#include <kernel/task/task.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/mempool.hpp>
#include <kernel/ipc/buffer_pool.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/arch/io.hpp>
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

static constexpr uint64_t ELF_MAGIC = 0x00000000464C457FULL;

/// @brief Validate an ELF64 header (magic, class, endianness, machine, bounds).
/// @return true if the header is valid and safe to load.
bool validate_header(const ELF64Header *hdr) {
    if (!hdr)
        return false;
    if (reinterpret_cast<const uint32_t *>(hdr->ident)[0] !=
        static_cast<uint32_t>(ELF_MAGIC))
        return false;
    if (hdr->ident[4] != 2)
        return false; // 64-bit
    if (hdr->ident[5] != 1)
        return false; // little-endian
    if (hdr->type != ET_EXEC && hdr->type != ET_DYN)
        return false;
#if defined(CONFIG_ARCH_AARCH64)
    if (hdr->machine != 0xB7)
        return false; // AArch64
#else
    if (hdr->machine != 0x3E)
        return false; // x86_64 (default)
#endif
    if (hdr->version == 0)
        return false;
    if (hdr->phnum > 64)
        return false;
    if (hdr->ehsize < sizeof(ELF64Header))
        return false;
    if (hdr->phentsize < sizeof(ELF64ProgramHeader))
        return false;
    if (hdr->phnum > 0) {
        // Check for arithmetic overflow in program header region end
        uint64_t ph_end =
            hdr->phoff + static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
        if (ph_end < hdr->phoff)
            return false; // overflow
        // Ensure header fits within reasonable image size
        if (ph_end > 1_MiB)
            return false;
    }
    // Verify ident padding bytes are zero
    for (int i = 7; i < 16; ++i) {
        if (hdr->ident[i] != 0)
            return false;
    }
    // Reject files with suspicious entry point
    if (hdr->entry >= USER_SPACE_LIMIT && hdr->entry != 0)
        return false;
    return true;
}

/// @brief Validate a single program header segment.
/// Checks bounds, permissions (W^X), size limits, and overflow safety.
static bool validate_segment(const ELF64ProgramHeader *phdr,
                             uint64_t file_size = 0) {
    if (phdr->type != PT_LOAD)
        return true;
    // Basic size sanity
    if (phdr->filesz > phdr->memsz)
        return false;
    // Overflow checks
    if (phdr->offset + phdr->filesz < phdr->offset)
        return false;
    if (phdr->vaddr + phdr->memsz < phdr->vaddr)
        return false;
    if (phdr->vaddr + phdr->filesz < phdr->vaddr)
        return false;
    // VULN-H2: the segment's file-resident bytes must fit inside the actual
    // file buffer.  Previously only a 4_MiB constant bounded phdr->offset;
    // a crafted ELF could read past the PMM::alloc_contiguous() buffer.
    if (file_size != 0 && phdr->offset + phdr->filesz > file_size)
        return false;
    // Must be within user space
    if (phdr->vaddr >= USER_SPACE_LIMIT)
        return false;
    if (phdr->vaddr + phdr->memsz > USER_SPACE_LIMIT)
        return false;
    // Reject zero-size segments in memory
    if (phdr->memsz == 0)
        return false;
    // Reject segments that are too large (> 64 MiB)
    if (phdr->memsz > 64_MiB)
        return false;
    // Reject segments with suspicious alignment
    if (phdr->align > 1_MiB)
        return false;
    // Validate file offset is within the image (cannot check exact size
    // without file length,
    // but ensure it's reasonable: less than a typical max ELF size)
    if (phdr->offset > 4_MiB)
        return false;
    // W^X enforcement: segment should not be both writable AND executable
    if ((phdr->flags & 2) && (phdr->flags & 1))
        return false;
    return true;
}

/// @brief Round address up to the next page boundary.
static uint64_t page_align_up(uint64_t addr) {
    return (addr + 0xFFF) & ~0xFFFULL;
}

/// @brief Round address down to the previous page boundary.
static uint64_t page_align_down(uint64_t addr) {
    return addr & ~0xFFFULL;
}

static constexpr size_t INITIAL_HEAP_PAGES = 4;
static constexpr size_t INITIAL_HEAP_SIZE =
    INITIAL_HEAP_PAGES * arch::PAGE_SIZE;

/// @brief Count strings in a null-terminated array (argc/envc helper).
static int count_strings(const char *const *arr) {
    if (!arr)
        return 0;
    int n = 0;
    while (arr[n])
        ++n;
    return n;
}

/// @brief Sum the lengths (including null terminators) of all strings in an
/// array.
static uint64_t total_string_len(const char *const *arr) {
    if (!arr)
        return 0;
    uint64_t total = 0;
    for (int i = 0; arr[i]; ++i)
        total += strlen(arr[i]) + 1;
    return total;
}

/// @brief Copy all strings from a null-terminated array into a contiguous
/// buffer.
static void copy_strings(uint8_t *dest, const char *const *arr) {
    if (!arr)
        return;
    for (int i = 0; arr[i]; ++i) {
        size_t len = strlen(arr[i]) + 1;
        memcpy(dest, arr[i], len);
        dest += len;
    }
}

/// @brief Load all PT_LOAD segments and allocate user stack + initial heap.
/// @return true on success.
static bool load_segments_and_stack(const ELF64Header *hdr,
                                    const uint8_t *file_data, uint64_t pml4,
                                    uint64_t *out_ustack_phys,
                                    uint64_t file_size = 0) {
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

        uint64_t seg_phys = PMM::alloc_user_contiguous(num_pages);
        if (!seg_phys)
            return false;
        for (size_t page_idx = 0; page_idx < num_pages; ++page_idx) {
            VMM::map_page_in_pml4(vaddr_base + page_idx * arch::PAGE_SIZE,
                                  seg_phys + page_idx * arch::PAGE_SIZE, true,
                                  pml4);
        }

        uint64_t offset_in_region = phdr->vaddr - vaddr_base;
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        memcpy(reinterpret_cast<void *>(arch::HHDM_OFFSET + seg_phys +
                                        offset_in_region),
               file_data + phdr->offset, phdr->filesz);

        if (phdr->memsz > phdr->filesz) {
            uint64_t zero_offset =
                arch::HHDM_OFFSET + seg_phys + offset_in_region + phdr->filesz;
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            memset(reinterpret_cast<void *>(zero_offset), 0,
                   phdr->memsz - phdr->filesz);
        }
    }

    size_t ustack_pages = (mem::STACK_SIZE + 4095) / arch::PAGE_SIZE;
    uint64_t ustack_phys = PMM::alloc_user_contiguous(ustack_pages);
    if (!ustack_phys)
        return false;

    // Guard page: leave first page unmapped, start mapping at +arch::PAGE_SIZE
    // VULN-H1: user stack is writable-only (never executable).
    for (size_t i = 0; i < ustack_pages; ++i) {
        VMM::map_page_in_pml4(mem::STACK_VADDR + arch::PAGE_SIZE +
                                  i * arch::PAGE_SIZE,
                              ustack_phys + i * arch::PAGE_SIZE, true, false,
                              pml4);
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
        VMM::map_page_in_pml4(mem::HEAP_VADDR + i * arch::PAGE_SIZE,
                              heap_phys + i * arch::PAGE_SIZE, true, false,
                              pml4);
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    __builtin_memset(reinterpret_cast<void *>(arch::HHDM_OFFSET + heap_phys), 0,
                     INITIAL_HEAP_SIZE);

    if (out_ustack_phys)
        *out_ustack_phys = ustack_phys;
    return true;
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

    uint8_t *sp = stack_top;
    sp -= str_total;
    if (str_total > 0) {
        uint8_t *str_pos = sp;
        copy_strings(str_pos, envp);
        str_pos += total_string_len(envp);
        copy_strings(str_pos, argv);
    }

    uint64_t align = reinterpret_cast<uint64_t>(sp) & 0xF;
    if (align)
        sp -= (16 - align);

    int envc = count_strings(envp);
    uint64_t *ptr = reinterpret_cast<uint64_t *>(sp);

    for (int i = envc; i >= 0; --i) {
        *--ptr = 0;
    }
    uint64_t *envp_start = ptr;
    if (envp) {
        uint8_t *str_pos = sp;
        for (int i = 0; i < envc; ++i) {
            envp_start[i] = reinterpret_cast<uint64_t>(str_pos);
            str_pos += strlen(envp[i]) + 1;
        }
    }

    for (int i = argc; i >= 0; --i) {
        *--ptr = 0;
    }
    uint64_t *argv_start = ptr;
    if (argv) {
        uint8_t *str_pos = sp + total_string_len(envp);
        for (int i = 0; i < argc; ++i) {
            argv_start[i] = reinterpret_cast<uint64_t>(str_pos);
            str_pos += strlen(argv[i]) + 1;
        }
    }

    *--ptr = static_cast<uint64_t>(argc);

    uint64_t user_rsp = reinterpret_cast<uint64_t>(ptr);
    user_rsp = user_rsp - reinterpret_cast<uint64_t>(stack_top) +
               mem::STACK_VADDR + arch::PAGE_SIZE + mem::STACK_SIZE;

    return user_rsp;
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
            if (tty->ops && tty->ops->open)
                tty->ops->open(*tty, 0);
        }
    }
}

TaskControlBlock *load(const ELF64Header *hdr, const uint8_t *file_data,
                       uint64_t file_size) {
    if (!validate_header(hdr))
        return nullptr;

    auto *tcb = static_cast<TaskControlBlock *>(
        MemPool::alloc(sizeof(TaskControlBlock)));
    if (!tcb)
        return nullptr;
    memset(tcb, 0, sizeof(TaskControlBlock));
    tcb->magic = TaskControlBlock::TCB_MAGIC;
    tcb->id = kernel::Scheduler::alloc_id();
    tcb->state = TaskState::READY;
    tcb->priority = 2;
    tcb->period_ticks = 0;
    tcb->base_priority = 2;
    tcb->executed_ticks = 0;
    tcb->remaining_ticks = 0;
    tcb->deadline_ticks = arch::Timer::ticks();
    tcb->page_table_ = 0;
    tcb->page_table_shared_ = false;
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
    uint64_t kstack_virt = arch::HHDM_OFFSET + kstack_phys;
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    tcb->kernel_stack = reinterpret_cast<uint8_t *>(kstack_virt);
    tcb->kernel_stack_top = kstack_virt + TaskControlBlock::STACK_SIZE;

    uint64_t pml4 = VMM::clone_kernel_pml4();
    if (!pml4) {
        tcb->cleanup();
        delete tcb;
        return nullptr;
    }
    tcb->page_table_ = pml4;

    uint64_t ustack_phys = 0;
    if (!load_segments_and_stack(hdr, file_data, pml4, &ustack_phys,
                                 file_size)) {
        tcb->cleanup();
        delete tcb;
        return nullptr;
    }

    open_std_fds(*tcb);

    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = mem::STACK_SIZE;
    tcb->program_break_start = mem::HEAP_VADDR;
    tcb->program_break = mem::HEAP_VADDR + INITIAL_HEAP_SIZE;

    uint64_t user_rsp = setup_user_stack(ustack_phys, nullptr, nullptr);
    if (user_rsp == 0) {
        tcb->cleanup();
        delete tcb;
        return nullptr;
    }

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
    *--stack = 0x10;       // SPSR_EL1: EL0t
    *--stack = hdr->entry; // ELR_EL1
    *--stack = user_rsp;   // SP_EL0
    for (int j = 0; j < 31; ++j)
        *--stack = 0;
    tcb->context.sp_el0 = reinterpret_cast<uint64_t>(stack);
#elif defined(CONFIG_ARCH_RISCV64)
    uint64_t *stack = reinterpret_cast<uint64_t *>(tcb->kernel_stack_top);
    *--stack = 0;          // padding
    *--stack = hdr->entry; // sepc
    *--stack = user_rsp;   // sp (user stack)
    *--stack = 0x100;      // sstatus: SPP=0 (U-mode), SPIE=1
    *--stack = 0;          // stvec
    for (int j = 0; j < 12; ++j)
        *--stack = 0; // s0-s11
    *--stack = 0;     // tp
    *--stack = 0;     // gp
    *--stack = 0;     // sp (init)
    *--stack = 0;     // ra
    tcb->context.sp = reinterpret_cast<uint64_t>(stack);
#endif

    return tcb;
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

    uint64_t old_pml4 = tcb->page_table_;
    bool old_shared = tcb->page_table_shared_;
    tcb->page_table_ = new_pml4;
    tcb->page_table_shared_ = false;
    tcb->user_stack_ = ustack_phys;
    tcb->user_stack_size_ = mem::STACK_SIZE;
    tcb->program_break_start = mem::HEAP_VADDR;
    tcb->program_break = mem::HEAP_VADDR + INITIAL_HEAP_SIZE;

    {
        vfs::Vnode *tty = vfs::resolve("/dev/tty");
        if (tty && tcb->fd_table.get(0) && tcb->fd_table.get(0)->vnode == tty) {
            // keep std fds
        } else {
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
                        if (tty2->ops && tty2->ops->open)
                            tty2->ops->open(*tty2, 0);
                    }
                }
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
    regs[0] = 0;           // X0 = 0
    regs[17] = hdr->entry; // ELR_EL1 (index 17)
    regs[19] = 0x10;       // SPSR_EL1: EL0t (index 19)
    regs[20] = user_rsp;   // SP_EL0 (index 20)
#elif defined(CONFIG_ARCH_RISCV64)
    (void)regs;
    (void)user_rsp;
    (void)hdr;
#endif

    if (arch::read_cr3() != new_pml4) {
        arch::write_cr3(new_pml4);
    }

    if (old_pml4 && old_pml4 != VMM::get_kernel_pml4()) {
        if (!old_shared) {
            VMM::free_user_pages(old_pml4);
            PMM::free_page(old_pml4);
        }
    }

    return true;
}

} // namespace elf
} // namespace kernel