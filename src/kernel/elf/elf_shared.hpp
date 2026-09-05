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

/// @file elf_shared.hpp
/// @brief Shared-object / dynamic linking structures (issue #95).
///        DT_NEEDED resolution, GOT/PLT fixups, relocation handling.

#pragma once

#include <types.hpp>
#include <kernel/sync/spinlock.hpp>

// Forward declarations for types defined in elf.hpp
namespace kernel {
namespace elf {
struct ELF64Header;
struct ELF64ProgramHeader;
} // namespace elf
} // namespace kernel

namespace kernel {
namespace elf {

/// @brief Dynamic section entry (DT_* tags).
struct ELF64Dynamic {
    int64_t d_tag;  ///< Dynamic entry type (DT_*).
    uint64_t d_val; ///< Value (or address for DT_* where d_ptr).
} __attribute__((packed));

/// @brief Dynamic entry type constants.
enum : int64_t {
    DT_NULL = 0,         ///< Marks end of dynamic array.
    DT_NEEDED = 1,       ///< Name of needed library (offset in .dynstr).
    DT_PLTRELSZ = 2,     ///< Size of PLT relocations.
    DT_PLTGOT = 3,       ///< Address of PLT GOT.
    DT_HASH = 4,         ///< Symbol hash table address.
    DT_STRTAB = 5,       ///< Address of string table (.dynstr).
    DT_SYMTAB = 6,       ///< Address of symbol table (.dynsym).
    DT_RELA = 7,         ///< Address of RELA relocations.
    DT_RELASZ = 8,       ///< Size of RELA table.
    DT_RELAENT = 9,      ///< Size of one RELA entry.
    DT_STRSZ = 10,       ///< Size of string table.
    DT_SYMENT = 11,      ///< Size of symbol entry.
    DT_INIT = 12,        ///< Address of init function.
    DT_FINI = 13,        ///< Address of fini function.
    DT_SONAME = 14,      ///< Library soname (offset in .dynstr).
    DT_RPATH = 15,       ///< Library search path (deprecated).
    DT_SYMBOLIC = 16,    ///< Symbolic resolution required.
    DT_REL = 17,         ///< Address of REL relocations.
    DT_RELSZ = 18,       ///< Size of REL table.
    DT_RELENT = 19,      ///< Size of one REL entry.
    DT_PLTREL = 20,      ///< Type of PLT relocations (REL/RELA).
    DT_DEBUG = 21,       ///< Debugging interface.
    DT_TEXTREL = 22,     ///< Text relocations exist.
    DT_JMPREL = 23,      ///< Address of PLT relocations.
    DT_BIND_NOW = 24,    ///< Process all relocations now.
    DT_INIT_ARRAY = 25,  ///< Address of init array.
    DT_FINI_ARRAY = 26,  ///< Address of fini array.
    DT_INIT_ARRAYSZ = 27,///< Size of init array.
    DT_FINI_ARRAYSZ = 28,///< Size of fini array.
    DT_RUNPATH = 29,     ///< Library search path.
    DT_FLAGS = 30,       ///< Flags.
    DT_ENCODING = 32,    ///< Encoding range start.
    DT_PREINIT_ARRAY = 32,///< Address of preinit array.
    DT_PREINIT_ARRAYSZ = 33,///< Size of preinit array.
};

/// @brief Symbol table entry (ELF64).
struct ELF64Symbol {
    uint32_t st_name;  ///< Symbol name (index into .dynstr).
    uint8_t  st_info;  ///< Binding and type.
    uint8_t  st_other; ///< Visibility.
    uint16_t st_shndx; ///< Section index.
    uint64_t st_value; ///< Symbol value (address).
    uint64_t st_size;  ///< Symbol size.
} __attribute__((packed));

/// @brief Relocation entry with addend (RELA).
struct ELF64Rela {
    uint64_t r_offset; ///< Address to relocate.
    uint64_t r_info;   ///< Symbol index + relocation type.
    int64_t  r_addend; ///< Addend.
} __attribute__((packed));

/// @brief Relocation entry without addend (REL).
struct ELF64Rel {
    uint64_t r_offset; ///< Address to relocate.
    uint64_t r_info;   ///< Symbol index + relocation type.
} __attribute__((packed));

/// @brief Loaded shared library entry (dedup cache).
struct LoadedLibrary {
    char soname[128];   ///< Library soname (e.g., "libc.so").
    uint64_t load_base; ///< Base load address.
    uint64_t load_size; ///< Total mapped size.
    uint32_t refcount;  ///< Reference count for dedup.
    bool in_progress;   ///< True while loading (cycle detection).

    LoadedLibrary()
        : load_base(0), load_size(0), refcount(0), in_progress(false) {
        soname[0] = '\0';
    }
};

/// @brief Maximum number of loaded shared libraries (cache size).
static constexpr size_t MAX_LOADED_LIBS = 8;

/// @brief Maximum DT_NEEDED recursion depth.
static constexpr size_t MAX_DEP_DEPTH = 8;

/// @brief Shared library cache (singleton).
class SharedLibCache {
public:
    static SharedLibCache &instance() {
        static SharedLibCache instance;
        return instance;
    }

    /// @brief Find a loaded library by soname.
    /// @return Pointer to LoadedLibrary, or nullptr if not loaded.
    LoadedLibrary *find(const char *soname);

    /// @brief Register a newly loaded library.
    /// @return Pointer to the new entry, or nullptr if full.
    LoadedLibrary *register_lib(const char *soname, uint64_t load_base, uint64_t load_size);

    /// @brief Increment refcount for a soname.
    void acquire(const char *soname);

    /// @brief Decrement refcount; unload if zero and not in_progress.
    void release(const char *soname);

    /// @brief Clear in-progress flag (called on load failure).
    void clear_in_progress(const char *soname);

private:
    LoadedLibrary libs_[MAX_LOADED_LIBS];
    kernel::sync::SpinLock lock_;
};

/// @brief Shared library state machine (for background loader extension).
enum class SharedLibState : uint8_t {
    IDLE = 0,
    LOADING_DEPS,   ///< Recursively loading DT_NEEDED libraries.
    RELOCATING,     ///< Applying relocations.
    DONE,
    FAILED,
    CANCELED,
};

/// @brief Load a shared object (.so) into the current task's address space.
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

/// @brief Extract soname from a DT_SONAME dynamic entry.
/// @param dyn Dynamic array.
/// @param strtab String table address.
/// @return soname string (offset in strtab), or nullptr.
const char *get_soname(const ELF64Dynamic *dyn, const char *strtab);

/// @brief Extract DT_NEEDED names from dynamic section.
/// @param dyn Dynamic array.
/// @param strtab String table address.
/// @param needed Array to fill (size MAX_DEP_DEPTH).
/// @return Number of dependencies found.
size_t get_needed(const ELF64Dynamic *dyn, const char *strtab,
                  const char **needed);

/// @brief Read dynamic section for a loaded library.
/// @param phdr PT_DYNAMIC program header.
/// @param load_base Library load base.
/// @param[out] out_dyn Dynamic array pointer.
/// @param[out] out_strtab String table address.
/// @return true on success.
bool read_dynamic_section(const ELF64ProgramHeader *phdr,
                          uint64_t load_base,
                          const ELF64Dynamic **out_dyn,
                          const char **out_strtab);

/// @brief Relocation type extraction.
inline uint64_t rela_type(uint64_t r_info) {
    return r_info & 0xFFFFFFFF;
}
inline uint64_t rela_sym(uint64_t r_info) {
    return r_info >> 32;
}

/// @brief x86_64 relocation types (subset).
enum : uint64_t {
    R_X86_64_NONE = 0,
    R_X86_64_64 = 1,       ///< 64-bit absolute.
    R_X86_64_PC32 = 2,     ///< PC-relative 32-bit.
    R_X86_64_GOT32 = 3,    ///< 32-bit GOT entry.
    R_X86_64_PLT32 = 4,    ///< 32-bit PLT address.
    R_X86_64_COPY = 5,     ///< Copy data.
    R_X86_64_GLOB_DAT = 6, ///< Global data.
    R_X86_64_JUMP_SLOT = 7,///< PLT entry.
    R_X86_64_RELATIVE = 8, ///< Relative adjustment.
    R_X86_64_GOTPCREL = 9, ///< GOT relative.
    R_X86_64_32 = 10,      ///< 32-bit absolute.
    R_X86_64_32S = 11,     ///< 32-bit signed.
    R_X86_64_16 = 12,      ///< 16-bit absolute.
    R_X86_64_PC16 = 13,    ///< 16-bit PC-relative.
    R_X86_64_8 = 14,       ///< 8-bit absolute.
    R_X86_64_PC8 = 15,     ///< 8-bit PC-relative.
    R_X86_64_DTPMOD64 = 16,///< TLS module.
    R_X86_64_DTPOFF64 = 17,///< TLS offset.
    R_X86_64_TPOFF64 = 18, ///< TLS offset.
};

} // namespace elf
} // namespace kernel