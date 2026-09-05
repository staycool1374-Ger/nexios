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

#pragma once

#include <types.hpp>
#include <kernel/task/task.hpp>
#include <kernel/elf/elf_shared.hpp>

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

/// @brief Validate an ELF64 header (magic, class, endianness, version).
/// @return true if the header is valid.
bool validate_header(const ELF64Header *hdr);
/// @brief Validate a single ELF program-header segment (bounds, W^X, sizes).
/// @param phdr The program header to validate.
/// @param file_size Total file size in bytes (0 = unvalidated).
/// @return true if the segment is safe to load.
bool validate_segment(const ELF64ProgramHeader *phdr,
                      uint64_t file_size = 0);
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

/// @brief Replace the current task's address space with an ELF binary (exec).
/// @param regs Register state to update with new entry point and stack.
/// @param file_size Validated size of `data` in bytes (VULN-H2).
/// @return true on success.
bool exec_into_current(const ELF64Header *hdr, const uint8_t *data,
                       const char *const *argv, const char *const *envp,
                       uint64_t *regs, uint64_t file_size = 0);

/// @brief Load a shared object (.so) into the current task's address space.
///        Returns the load base address, or 0 on failure.
/// @param soname The shared object name (e.g. "libc.so").
/// @param pml4 The task's PML4 to map the library into.
/// @param[out] out_load_base Base address where the library was mapped.
/// @return true on success.
bool load_shared_object(const char *soname, uint64_t pml4,
                        uint64_t *out_load_base);

/// @brief Resolve DT_NEEDED dependencies for an ELF binary, loading each
///        required shared object recursively.
/// @param hdr The main executable's ELF header (must have PT_DYNAMIC).
/// @param file_data Raw file data of the main executable.
/// @param file_size Size of the file data.
/// @param pml4 The task PML4 to map libraries into.
/// @param load_base Base load address of the main executable.
/// @return true on success, false on failure (missing lib, cycle, etc.).
bool resolve_dependencies(const ELF64Header *hdr, const uint8_t *file_data,
                          uint64_t file_size, uint64_t pml4,
                          uint64_t load_base);

/// @brief Apply relocations for a loaded shared object.
/// @param phdr The PT_LOAD program header for the library.
/// @param dyn The PT_DYNAMIC program header for the library.
/// @param load_base Base address where the library was mapped.
/// @param pml4 The task PML4.
/// @param soname Library soname (for error reporting).
/// @return true on success.
bool apply_relocations(const ELF64ProgramHeader *phdr,
                       const ELF64ProgramHeader *dyn,
                       uint64_t load_base, uint64_t pml4,
                       const char *soname);

} // namespace elf
} // namespace kernel
