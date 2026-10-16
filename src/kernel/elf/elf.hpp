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

/// @file elf.hpp
/// @brief ELF64 binary loader — validate, load, exec, shared-object support.

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/elf/elf_shared.hpp>
#include <kernel/memory/checked_ptr.hpp>

namespace kernel {
namespace elf {

/// @brief ELF64 file header (64-byte packed structure).
struct ELF64Header {
    uint8_t ident[16];  ///< ELF magic + class/endian/version/OSABI/pad.
    uint16_t type;      ///< Object file type (ET_NONE, ET_EXEC, ET_DYN).
    uint16_t machine;   ///< Architecture (e.g. 0x3E for x86_64).
    uint32_t version;   ///< ELF version (must be 1).
    uint64_t entry;     ///< Virtual address of entry point.
    uint64_t phoff;     ///< Program header table offset.
    uint64_t shoff;     ///< Section header table offset.
    uint32_t flags;     ///< Processor-specific flags.
    uint16_t ehsize;    ///< ELF header size (64).
    uint16_t phentsize; ///< Size of one program header entry.
    uint16_t phnum;     ///< Number of program header entries.
    uint16_t shentsize; ///< Size of one section header entry.
    uint16_t shnum;     ///< Number of section header entries.
    uint16_t shstrndx;  ///< Section header string table index.

    ELF64Header() = default;
} __attribute__((packed));

/// @brief ELF64 program header (segment descriptor).
struct ELF64ProgramHeader {
    uint32_t type;   ///< Segment type (PT_NULL, PT_LOAD, PT_DYNAMIC, etc.).
    uint32_t flags;  ///< Segment flags (PF_R, PF_W, PF_X).
    uint64_t offset; ///< Offset in file.
    uint64_t vaddr;  ///< Virtual address to load at.
    uint64_t paddr;  ///< Physical address (usually same as vaddr).
    uint64_t filesz; ///< Size in file.
    uint64_t memsz;  ///< Size in memory (zero-padded if > filesz).
    uint64_t align;  ///< Alignment constraint.
} __attribute__((packed));

/// Program header type constants.
enum : uint8_t {
    PT_NULL = 0,    ///< Unused entry.
    PT_LOAD = 1,    ///< Loadable segment.
    PT_DYNAMIC = 2, ///< Dynamic linking info.
    PT_INTERP = 3,  ///< Interpreter path.
    PT_NOTE = 4,    ///< Auxiliary info.
    PT_PHDR = 6,    ///< Program header table itself.
};

/// Program header flag constants.
enum : uint8_t {
    PF_X = 1, ///< Execute permission.
    PF_W = 2, ///< Write permission.
    PF_R = 4, ///< Read permission.
};

/// ELF header type constants.
enum : uint8_t {
    ET_NONE = 0, ///< No file type.
    ET_EXEC = 2, ///< Executable.
    ET_DYN = 3,  ///< Shared object / PIE.
};

/// @brief ELF magic word (0x7F,'E','L','F' little-endian).
static constexpr uint64_t ELF_MAGIC = 0x00000000464C457FULL;

/// @brief Validate an ELF64 header (magic, class, endianness, version).
/// @return true if the header is valid.
/// @note constexpr so golden images pin the logic at compile time
///       (issue #348-C); runtime callers use it identically.  Defined
///       here (not in elf.cpp) so every TU calling it sees the definition,
///       which constexpr requires.
constexpr bool validate_header(const ELF64Header *hdr);
/// @brief Validate a single ELF program-header segment (bounds, W^X, sizes).
/// @param phdr The program header to validate.
/// @param file_size Total file size in bytes (0 = unvalidated).
/// @return true if the segment is safe to load.
/// @note constexpr + golden asserts in elf.cpp (issue #348-C).  Defined
///       here (not in elf.cpp) so every TU calling it sees the definition,
///       which constexpr requires.
constexpr bool validate_segment(const ELF64ProgramHeader *phdr,
                                uint64_t file_size = 0);

constexpr bool validate_header(const ELF64Header *hdr) {
    if (!hdr)
        return false;
    // Little-endian u32 assembly of ident[0..3] (all supported arches are
    // LE; identical to the old reinterpret_cast read, constexpr-clean).
    const uint32_t magic = static_cast<uint32_t>(hdr->ident[0]) |
                           (static_cast<uint32_t>(hdr->ident[1]) << 8) |
                           (static_cast<uint32_t>(hdr->ident[2]) << 16) |
                           (static_cast<uint32_t>(hdr->ident[3]) << 24);
    if (magic != static_cast<uint32_t>(ELF_MAGIC))
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
#elif defined(CONFIG_ARCH_RISCV64)
    if (hdr->machine != 0xF3)
        return false; // RISC-V (EM_RISCV)
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
constexpr bool validate_segment(const ELF64ProgramHeader *phdr, uint64_t file_size) {
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


/// @brief Bound for the post-relocation baseline scan (issue #46): larger
///        read-only images fail the load closed instead of stalling MAPPING.
static constexpr uint64_t kMaxRoScanBytes = 4 * 1024 * 1024;
/// @brief Exit code for authenticity-kill termination (issue #46): matches
///        the dmesg event that records the cause, so waitpid status and the
///        log line trace to each other.
static constexpr uint64_t kElfAuthKillExitCode = 0xDB0C;

/// @brief Collect the covered read-only ranges (issue #46): every PT_LOAD
///        segment without PF_W, as [vaddr, filesz), validated segments only.
/// @param phdr_buf_off Buffer offset of the phdr table — NOT the file
///        phoff: the loader packs phdrs at sizeof(ELF64Header) regardless
///        of where they sit in the file.
/// @return Range count, or -1 when more than @p max_ranges exist (fail
///         closed — the TCB snapshot cannot hold them).
uint64_t collect_ro_ranges(const uint8_t *phdr_image, uint64_t phdr_buf_off,
                           uint16_t phnum, uint64_t phentsize,
                           uint64_t file_size, TaskControlBlock::RoRange *out,
                           uint64_t max_ranges) noexcept;
/// @brief Locate the TEXT before-canary slot (issue #46): mirrors
///        install_segment_canaries' text_base rule (first PF_X PT_LOAD,
///        page_align_down(vaddr)) so the authenticity coverage can exclude
///        the 8 bytes the canary subsystem owns and checks itself.
/// @return true + slot address when a PF_X segment exists.
bool text_canary_slot(const uint8_t *phdr_image, uint64_t phdr_buf_off,
                      uint16_t phnum, uint64_t phentsize, uint64_t file_size,
                      uint64_t *slot_out) noexcept;
/// @brief Fold one mapped range into @p acc via the target page table.
/// @return false when any page is unmapped (fail closed).
bool crc_user_range(uint64_t pml4, uint64_t vaddr, uint64_t len,
                    uint32_t &acc) noexcept;
/// @brief Capture the post-relocation CRC baseline into @p tcb (issue #46).
///        For static images the scan must equal the incremental chunk-loop
///        CRC (copy-path cross-check); dynamic images adopt the scan.
///        The TEXT before-canary slot (@p canary_slot, 0 = none) is carved
///        out of the scan — the canary subsystem owns and checks it.
/// @return false to fail the load closed (overflow / oversize / unmapped /
///         static cross-check delta).
bool snapshot_ro_baseline(uint64_t pml4, const uint8_t *phdr_image,
                          uint64_t phdr_buf_off, uint16_t phnum,
                          uint64_t phentsize, uint64_t file_size,
                          bool has_dynamic, uint32_t load_crc,
                          uint64_t canary_slot,
                          TaskControlBlock *tcb) noexcept;
/// @brief Load an ELF binary into a new task.
/// @param hdr Pointer to the ELF header.
/// @param data Raw ELF file data.
/// @param file_size Validated size of `data` in bytes (VULN-H2: bounds
///        `phdr->offset+filesz` against the real file, not a constant).
/// @return New TaskControlBlock, or nullptr on failure.
TaskControlBlock *load(const ELF64Header *hdr, const uint8_t *data,
                       uint64_t file_size = 0);

/// @brief Allocate and map the user stack (guard page + pages) and the
///        initial heap into @p pml4.  Extracted from elf::load for reuse by
///        the background ElfLoader (chunked loader).
/// @param pml4 The task PML4 to map into.
/// @param[out] out_ustack_phys Physical base of the user stack.
/// @return true on success.
bool alloc_user_stack_and_heap(uint64_t pml4, uint64_t *out_ustack_phys);

/// @brief Finish a partially-built load: allocate the TCB, kernel stack,
///        adopt @p pml4 as page_table_, wire std fds, setup the user stack
///        frame and install segment canaries.  Extracted from elf::load for
///        reuse by the background ElfLoader.
/// @param hdr Validated ELF header.
/// @param pml4 The built PML4 (user pages already mapped).
/// @param ustack_phys Physical base of the user stack.
/// @param phdr_image Buffer holding the ELF header followed by the full
///        program-header table (for canary installation).
/// @param file_size Total file size in bytes.
/// @return New TaskControlBlock, or nullptr on failure (partial state freed).
TaskControlBlock *finalize_loaded_task(const ELF64Header *hdr, uint64_t pml4,
                                       uint64_t ustack_phys,
                                       const uint8_t *phdr_image,
                                       uint64_t file_size);

/// @brief Shared user-TCB final tail (issue #271 P2 rerun): open_std_fds,
/// user stack/heap fields, setup_user_stack + entry frame, canaries,
/// text/data/bss sizes, TLS guard. Factored from finalize_loaded_task
/// so the rerun builder produces a byte-identical init (a hand-rolled
/// tail diverges and panics in teardown).
/// @return false on failure WITHOUT any teardown (the tail never owns
///         the address space or retention refs — the caller unmaps its
///         own golden pages, drops its builder ref, then runs destroy;
///         never cleanup on the never-added TCB).
bool finalize_user_tcb_tail(TaskControlBlock *tcb, const ELF64Header *hdr,
                            uint64_t ustack_phys, const uint8_t *phdr_image,
                            uint64_t file_size);

/// @brief Replace the current task's address space with an ELF binary (exec).
/// @param regs Register state to update with new entry point and stack.
/// @param file_size Validated size of `data` in bytes (VULN-H2).
/// @return true on success.
bool exec_into_current(const ELF64Header *hdr, const uint8_t *data,
                       const char *const *argv, const char *const *envp,
                       uint64_t *regs, uint64_t file_size = 0);

// NOTE (issue #95): shared-object entry points (load_shared_object,
// resolve_dependencies, apply_relocations) are declared canonically in
// <kernel/elf/elf_shared.hpp> (already included above); they are NOT
// re-declared here.

} // namespace elf
} // namespace kernel
