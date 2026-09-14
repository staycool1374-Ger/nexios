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
class TaskControlBlock;
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

/// @brief Shared-object loader error codes (issue #95).
enum class ElfError : uint8_t {
    OK = 0,
    INVALID_ELF, ///< Malformed header/phdr/dynamic/reloc data.
    LAYOUT,     ///< Fixed-vaddr overlap (no silent rebasing, §2.2).
    NOT_FOUND,  ///< Soname not discoverable under /lib/.
    NOMEM,      ///< Page alloc, cache-full, or phys-page bound hit.
    DEPTH,      ///< DT_NEEDED recursion deeper than MAX_DEP_DEPTH.
    CYCLE,      ///< Dependency cycle via in-progress set.
    CANCELED,   ///< Loader cancel observed mid-request.
};

/// @brief Maximum phys pages tracked per cached library (64 = 256 KiB).
static constexpr size_t MAX_LIB_PHYS_PAGES = 64;

/// @brief Loaded shared library entry (dedup cache).
struct LoadedLibrary {
    char soname[128];   ///< Library soname (e.g., "libc.so").
    uint64_t load_base; ///< Fixed preferred base load address.
    uint64_t load_size; ///< Total mapped size.
    uint64_t file_size; ///< .so file size at load (hit verification).
    uint64_t phys_pages[MAX_LIB_PHYS_PAGES]; ///< Shared phys, file order.
    uint64_t writable_mask; ///< Bit i set = page i is RW (per-task copy
                             ///< on cache hit; RO pages map shared phys).
    uint32_t num_phys_pages; ///< Valid entries in phys_pages (≤ 64).
    uint32_t refcount;  ///< Reference count for dedup.
    bool in_progress;   ///< True while loading (cycle detection).

    LoadedLibrary()
        : load_base(0), load_size(0), file_size(0), writable_mask(0),
          num_phys_pages(0), refcount(0), in_progress(false) {
        soname[0] = '\0';
        for (size_t i = 0; i < MAX_LIB_PHYS_PAGES; ++i)
            phys_pages[i] = 0;
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

    /// @brief Reserve a slot for a soname being loaded (cycle detection).
    ///        Returns the existing slot when present (caller checks
    ///        in_progress: true = cycle, false = cache hit), otherwise a
    ///        fresh slot with in_progress=true and refcount=0, or nullptr
    ///        when the cache is full.  out_created reports a fresh slot.
    LoadedLibrary *reserve(const char *soname, bool *out_created);

    /// @brief Decrement refcount; unload if zero and not in_progress.
    void release(const char *soname);

    /// @brief Clear in-progress flag (called on load failure).
    void clear_in_progress(const char *soname);

private:
    LoadedLibrary libs_[MAX_LOADED_LIBS];
    kernel::sync::SpinLock lock_;
};

/// @brief Parsed PT_DYNAMIC view (all pointers into the file image,
///        bounds-checked at parse time; issue #95).
struct DynView {
    const ELF64Dynamic *dyn; ///< Dynamic array (file image).
    uint64_t dyn_count;      ///< Entry count (capped).
    const char *strtab;      ///< .dynstr (file image).
    uint64_t strsz;          ///< .dynstr size.
    const ELF64Symbol *symtab; ///< .dynsym (file image).
    uint64_t sym_count;      ///< Symbol count (filesize-bounded).
    uint64_t syment;         ///< Symbol entry size (must match).
    const ELF64Rela *rela;   ///< DT_RELA table (file image, nullable).
    uint64_t relasz;         ///< DT_RELA size.
    uint64_t relaent;        ///< DT_RELA entry size.
    const ELF64Rela *jmprel; ///< DT_JMPREL table (file image, nullable).
    uint64_t pltrelsz;       ///< DT_JMPREL size.
    uint64_t pltrel_type;    ///< DT_PLTREL (RELA expected).
};

/// @brief Maximum .so file size accepted for buffering (4 MiB).
static constexpr uint64_t MAX_SO_FILE_SIZE = 4ULL * 1024 * 1024;

/// @brief Maximum total bytes mapped per resolve request (64 MiB).
static constexpr uint64_t MAX_TOTAL_MAPPED = 64ULL * 1024 * 1024;

/// @brief Library search directory for non-absolute sonames.
static constexpr const char *LIB_SEARCH_DIR = "/lib/";

/// @brief Per-request dependency-resolution context (issue #95).
///        Single instance per top-level resolve; passed by pointer
///        through the bounded recursion (no global resolve state).
///        Image buffers are retained until the post-order relocate pass
///        finishes (closure symtab/strtab reads), then released.
struct DepResolveContext {
    uint64_t pml4;        ///< Target task PML4 (phys).
    uint64_t exec_base;   ///< Main exec load base (overlap checks).
    uint64_t exec_size;   ///< Main exec mapped size.
    uint64_t mapped_bytes;///< Running total for the rlimit bound.
    uint64_t budget_pages;///< Page budget (0 = unlimited).
    uint64_t generation;  ///< Loader generation for cancel checks
                           ///< (ElfLoader::generation(); 0 matches only
                           ///< when no load ever ran — tests set it).
    const char *acquired[MAX_DEP_DEPTH]; ///< Cache soname ptrs (stable:
                                          ///< static slots, refcount > 0).
    uint64_t acquired_count;
    const uint8_t *images[MAX_DEP_DEPTH + 1]; ///< Retained file images
                                               ///< ([0] = exec).
    uint64_t image_sizes[MAX_DEP_DEPTH + 1];
    uint64_t image_bases[MAX_DEP_DEPTH + 1]; ///< Runtime base per image.
    DynView image_views[MAX_DEP_DEPTH + 1]; ///< Parsed dynamic per image.
    uint64_t image_count;
    uint64_t image_phys[MAX_DEP_DEPTH + 1]; ///< PMM buffer base per image
                                             ///< (0 = caller-owned, e.g.
                                             ///< the exec buffer).
    uint64_t image_npages[MAX_DEP_DEPTH + 1];
    ElfError last_error;  ///< Terminal error of a failed resolve.
    char fail_chain[256]; ///< Soname chain for dmesg on failure.
};

/// @brief Zero-initialise a resolve context for a new request.
/// @param ctx Context to initialise (all arrays cleared, counts zero).
/// @param pml4 Target task PML4 (phys).
/// @param generation Loader generation for cancel checks.
inline void init_resolve_context(DepResolveContext *ctx, uint64_t pml4,
                                 uint64_t generation) {
    if (!ctx) {
        return;
    }
    __builtin_memset(ctx, 0, sizeof(DepResolveContext));
    ctx->pml4 = pml4;
    ctx->generation = generation;
}

/// @brief Free PMM image buffers retained in a context (loader-owned
///        entries only; caller-owned images[0] with image_phys 0 kept).
/// @param ctx Context whose buffers to release (counts zeroed after).
void free_resolve_images(DepResolveContext *ctx);

/// @brief Unmap one acquired lib's shared RO pages from a pml4.
///        Used by request-fail teardowns (loader + tests) before
///        releasing: the blind free_user_pages that follows must not
///        see (and double-free) cache-owned phys.
/// @param pml4 Target pml4 being torn down.
/// @param ctx Request context holding the acquired list.
/// @param idx Index into ctx->acquired (no-op when out of range or the
///        slot vanished).
void unmap_acquired_ro(uint64_t pml4, const DepResolveContext *ctx,
                       uint64_t idx);

/// @brief Release a task's acquired shared libs at teardown (issue #95):
///        unmap each lib's shared RO pages from the task's pml4 first
///        (the cache owns shared phys by refcount; blind per-pml4
///        teardown would double-free), then release refcounts and clear
///        the task list.  Runs before page-table teardown in TCB::cleanup
///        and destroy_completed_tcb paths.
/// @param tcb Task whose lib list to drain (pml4 = tcb->page_table_).
void release_task_libs(TaskControlBlock *tcb);

/// @brief Shared library state machine (for background loader extension).
enum class SharedLibState : uint8_t {
    IDLE = 0,
    LOADING_DEPS,   ///< Recursively loading DT_NEEDED libraries.
    RELOCATING,     ///< Applying relocations.
    DONE,
    FAILED,
    CANCELED,
};

/// @brief Load a shared object (.so) into a task address space.
/// @param soname Library soname (e.g. "libc.so"; absolute path used
///        verbatim, otherwise resolved under /lib/).
/// @param ctx Request context (pml4, budget, acquired list, fail chain).
/// @param depth Current recursion depth (0 = top-level exec deps).
/// @param[out] out_load_base Fixed base address the library was mapped at.
/// @return true on success (cache hit or fresh load + recurse + reloc
///         of the lib's own subtree deferred to the post-order pass).
bool load_shared_object(const char *soname, DepResolveContext *ctx,
                        uint64_t depth, uint64_t *out_load_base);

/// @brief Resolve DT_NEEDED dependencies for a file image, loading each
///        required shared object recursively (Stage B, issue #95).
/// @param hdr Image ELF header (ET_EXEC or ET_DYN, PT_DYNAMIC optional —
///        absent means static: success with no action).
/// @param file_data Full file image in memory.
/// @param file_size Image size (all parsing bounds-checked against it).
/// @param ctx Request context (pre-filled: pml4, exec range, budget).
/// @param depth Current recursion depth.
/// @param load_base Runtime base of THIS image (fixed-address model).
/// @return true on success, false on failure (fail_chain describes it).
bool resolve_dependencies(const ELF64Header *hdr, const uint8_t *file_data,
                          uint64_t file_size, DepResolveContext *ctx,
                          uint64_t depth, uint64_t load_base);

/// @brief Apply eager relocations for one loaded image (Stage C).
/// @param hdr Image ELF header.
/// @param file_data Full file image in memory.
/// @param file_size Image size.
/// @param load_base Runtime base of this image.
/// @param ctx Request context (closure = exec + acquired libs, with
///        retained image buffers for symtab/strtab reads).
/// @param soname Image soname (for error reporting).
/// @return true on success (RELATIVE + GLOB_DAT/JUMP_SLOT/64 only;
///         TEXTREL and other types fail closed).
bool apply_relocations(const ELF64Header *hdr, const uint8_t *file_data,
                       uint64_t file_size, uint64_t load_base,
                       DepResolveContext *ctx, const char *soname);

/// @brief Post-order relocate pass over a resolved closure (Stage C).
///        Applies apply_relocations to every image in ctx (exec first).
/// @param ctx Resolved request context.
/// @return true when every image relocates cleanly.
bool relocate_closure(DepResolveContext *ctx);

/// @brief Extract soname from a parsed dynamic view (DT_SONAME).
/// @return Pointer into the view's strtab, or nullptr if absent.
const char *get_soname(const DynView *view);

/// @brief Extract DT_NEEDED names from a parsed dynamic view.
/// @param needed Output array (capacity MAX_DEP_DEPTH).
/// @return Number of dependencies found (capped).
size_t get_needed(const DynView *view, const char **needed);

/// @brief Parse and validate the PT_DYNAMIC section of a file image.
/// @param hdr Image ELF header.
/// @param file_data Full file image in memory.
/// @param file_size Image size (every pointer bounds-checked against it).
/// @param[out] out_view Parsed view (pointers into file_data).
/// @return true when PT_DYNAMIC exists and parses; false when absent
///         (static image — not an error) or malformed (INVALID_ELF).
bool read_dynamic_section(const ELF64Header *hdr, const uint8_t *file_data,
                          uint64_t file_size, DynView *out_view);

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