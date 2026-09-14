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

/// @file elf_shared.cpp
/// @brief Shared-object / dynamic linking implementation (issue #95).
///        DT_NEEDED resolution, GOT/PLT fixups, relocation handling.

#include <kernel/elf/elf_shared.hpp>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/task/task.hpp>
#include <kernel/sync/spinlock_guard.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/checked_ptr.hpp>
#include <kernel/vfs/vfs.hpp>
#include <constants.hpp>
#include <logger.hpp>
#include <string.hpp>

namespace kernel {
namespace elf {

LoadedLibrary *SharedLibCache::find(const char *soname) {
    if (!soname) {
        return nullptr;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] != '\0' &&
            strcmp(libs_[i].soname, soname) == 0) {
            return &libs_[i];
        }
    }
    return nullptr;
}

LoadedLibrary *SharedLibCache::register_lib(const char *soname,
                                            uint64_t load_base,
                                            uint64_t load_size) {
    if (!soname) {
        return nullptr;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] == '\0' && !libs_[i].in_progress) {
            strncpy(libs_[i].soname, soname, sizeof(libs_[i].soname) - 1);
            libs_[i].soname[sizeof(libs_[i].soname) - 1] = '\0';
            libs_[i].load_base = load_base;
            libs_[i].load_size = load_size;
            libs_[i].refcount = 1;
            libs_[i].in_progress = false;
            return &libs_[i];
        }
    }
    return nullptr;
}

void SharedLibCache::acquire(const char *soname) {
    if (!soname) {
        return;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] != '\0' &&
            strcmp(libs_[i].soname, soname) == 0) {
            ++libs_[i].refcount;
            return;
        }
    }
}

void SharedLibCache::release(const char *soname) {
    if (!soname) {
        return;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] != '\0' &&
            strcmp(libs_[i].soname, soname) == 0) {
            if (libs_[i].in_progress) {
                return;
            }
            // Last owner out frees the shared RO phys (RW copies are
            // per-task and die with their own pml4 teardowns).  Every
            // releaser unmaps shared RO from its own pml4 FIRST (see
            // release_task_libs and the loader fail path), so at
            // refcount 0 no mapping references these pages.
            if (libs_[i].refcount == 1) {
                uint32_t n = libs_[i].num_phys_pages;
                if (n > MAX_LIB_PHYS_PAGES) {
                    n = MAX_LIB_PHYS_PAGES;
                }
                for (uint32_t p = 0; p < n; ++p) {
                    if ((libs_[i].writable_mask & (1ULL << p)) == 0) {
                        PMM::free_page(libs_[i].phys_pages[p]);
                    }
                }
            }
            if (libs_[i].refcount > 0) {
                --libs_[i].refcount;
            }
            if (libs_[i].refcount == 0) {
                libs_[i].soname[0] = '\0';
                libs_[i].load_base = 0;
                libs_[i].load_size = 0;
                libs_[i].file_size = 0;
                libs_[i].writable_mask = 0;
                libs_[i].num_phys_pages = 0;
                for (size_t q = 0; q < MAX_LIB_PHYS_PAGES; ++q) {
                    libs_[i].phys_pages[q] = 0;
                }
            }
            return;
        }
    }
}

void SharedLibCache::clear_in_progress(const char *soname) {
    if (!soname) {
        return;
    }
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] != '\0' &&
            strcmp(libs_[i].soname, soname) == 0) {
            libs_[i].in_progress = false;
            return;
        }
    }
}

// ---- Stage B/C pipeline (issue #95) ----
namespace {

// Bounded string length (the global strlen is unbounded — never use it
// on untrusted image bytes).
uint64_t strnlen_capped(const char *s, uint64_t max) {
    uint64_t n = 0;
    while (n < max && s[n] != '\0') {
        ++n;
    }
    return n;
}

// Fail-closed overlap: overflow on either end counts as overlap.
bool ranges_overlap(uint64_t a_base, uint64_t a_size, uint64_t b_base,
                    uint64_t b_size) {
    if (a_size == 0 || b_size == 0) {
        return false;
    }
    uint64_t a_end = a_base + a_size;
    uint64_t b_end = b_base + b_size;
    if (a_end < a_base || b_end < b_base) {
        return true;
    }
    return a_base < b_end && b_base < a_end;
}

// Append "->soname" to the fail chain (bounded, always NUL-terminated).
void chain_append(DepResolveContext *ctx, const char *soname) {
    uint64_t len = strnlen_capped(ctx->fail_chain,
                                   sizeof(ctx->fail_chain));
    const char *arrow = "->";
    for (uint64_t i = 0; arrow[i] != '\0'; ++i) {
        if (len + 1 >= sizeof(ctx->fail_chain)) {
            return;
        }
        ctx->fail_chain[len++] = arrow[i];
    }
    for (uint64_t i = 0; soname[i] != '\0'; ++i) {
        if (len + 1 >= sizeof(ctx->fail_chain)) {
            break;
        }
        ctx->fail_chain[len++] = soname[i];
    }
    ctx->fail_chain[len] = '\0';
}

// Count DT_NEEDED entries (get_needed caps silently; over-cap must fail).
uint64_t count_needed(const DynView *view) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < view->dyn_count; ++i) {
        if (view->dyn[i].d_tag == DT_NEEDED) {
            ++n;
        }
    }
    return n;
}

// Write 8 bytes to a user VA mapped in a foreign pml4: validate the VA
// range, walk the target tables (unmapped = fail), then write through
// the HHDM alias.  CheckedPtr itself only covers the current address
// space, so this is the cross-AS equivalent (validated + bounded).
// NOLINTNEXTLINE(performance-no-int-to-ptr)
bool target_write_u64(uint64_t pml4, uint64_t va, uint64_t value) {
    if (va >= kernel::USER_SPACE_LIMIT) {
        return false;
    }
    if ((va & 7U) != 0) {
        return false;
    }
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
    if (phys == 0) {
        return false;
    }
    uint64_t alias = arch::HHDM_OFFSET + (phys & ~0xFFFULL) + (va & 0xFFFULL);
    *reinterpret_cast<uint64_t *>(alias) = value;
    return true;
}

// Free a PMM page run allocated for a file image buffer.
void free_image_pages(uint64_t phys, uint64_t npages) {
    for (uint64_t i = 0; i < npages; ++i) {
        PMM::free_page(phys + i * arch::PAGE_SIZE);
    }
}

// Release one request's retained image (cache slot bookkeeping is the
// caller's job: clear_in_progress + release on failure paths).
void drop_image(DepResolveContext *ctx, uint64_t idx) {
    if (ctx->image_phys[idx] != 0) {
        free_image_pages(ctx->image_phys[idx], ctx->image_npages[idx]);
        ctx->image_phys[idx] = 0;
        ctx->image_npages[idx] = 0;
    }
    ctx->images[idx] = nullptr;
    ctx->image_sizes[idx] = 0;
}

} // namespace

void free_resolve_images(DepResolveContext *ctx) {
    if (!ctx) {
        return;
    }
    for (uint64_t i = 0; i < ctx->image_count; ++i) {
        drop_image(ctx, i);
    }
    ctx->image_count = 0;
}

void unmap_acquired_ro(uint64_t pml4, const DepResolveContext *ctx,
                       uint64_t idx) {
    if (!ctx || idx >= ctx->acquired_count) {
        return;
    }
    LoadedLibrary *slot =
        SharedLibCache::instance().find(ctx->acquired[idx]);
    if (!slot) {
        return;
    }
    uint64_t n = slot->num_phys_pages;
    if (n > MAX_LIB_PHYS_PAGES) {
        n = MAX_LIB_PHYS_PAGES;
    }
    for (uint64_t i = 0; i < n; ++i) {
        if ((slot->writable_mask & (1ULL << i)) == 0) {
            VMM::unmap_page_in_pml4(slot->load_base + i * arch::PAGE_SIZE,
                                    pml4);
        }
    }
}

void release_task_libs(TaskControlBlock *tcb) {    if (!tcb || tcb->needed_lib_count == 0) {
        return;
    }
    if (tcb->needed_lib_count > TaskControlBlock::kMaxTaskLibs) {
        return; // Corrupt count: fail closed, leak refcounts loudly below.
    }
    for (uint64_t i = 0; i < tcb->needed_lib_count; ++i) {
        LoadedLibrary *slot =
            SharedLibCache::instance().find(tcb->needed_libs[i]);
        if (slot) {
            uint32_t n = slot->num_phys_pages;
            if (n > MAX_LIB_PHYS_PAGES) {
                n = MAX_LIB_PHYS_PAGES;
            }
            for (uint64_t p = 0; p < n; ++p) {
                if ((slot->writable_mask & (1ULL << p)) == 0) {
                    VMM::unmap_page_in_pml4(
                        slot->load_base + p * arch::PAGE_SIZE,
                        tcb->page_table_);
                }
            }
            SharedLibCache::instance().release(tcb->needed_libs[i]);
        }
        tcb->needed_libs[i][0] = '\0';
    }
    tcb->needed_lib_count = 0;
}

LoadedLibrary *SharedLibCache::reserve(const char *soname,
                                       bool *out_created) {
    if (!soname || !out_created) {
        return nullptr;
    }
    *out_created = false;
    SpinLockGuard<sync::SpinLock> guard(lock_);
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] != '\0' &&
            strcmp(libs_[i].soname, soname) == 0) {
            return &libs_[i];
        }
    }
    for (size_t i = 0; i < MAX_LOADED_LIBS; ++i) {
        if (libs_[i].soname[0] == '\0' && !libs_[i].in_progress) {
            strncpy(libs_[i].soname, soname, sizeof(libs_[i].soname) - 1);
            libs_[i].soname[sizeof(libs_[i].soname) - 1] = '\0';
            libs_[i].load_base = 0;
            libs_[i].load_size = 0;
            libs_[i].num_phys_pages = 0;
            libs_[i].refcount = 0;
            libs_[i].in_progress = true;
            *out_created = true;
            return &libs_[i];
        }
    }
    return nullptr;
}

// Forward declarations for the mutually recursive Stage-B core.
static ElfError load_one(const char *soname, DepResolveContext *ctx,
                         uint64_t depth, uint64_t *out_base);
static ElfError resolve_image(const ELF64Header *hdr, const uint8_t *image,
                              uint64_t size, DepResolveContext *ctx,
                              uint64_t depth, uint64_t load_base);
static ElfError load_fresh(const char *name, LoadedLibrary *slot,
                           DepResolveContext *ctx, uint64_t depth,
                           uint64_t *out_base);
static ElfError map_hit(LoadedLibrary *slot, DepResolveContext *ctx,
                        const char *name, uint64_t *out_base);
static bool page_file_bytes(const ELF64Header *hdr, const uint8_t *image,
                            uint64_t size, uint64_t va, uint64_t *out_off,
                            uint64_t *out_len, uint64_t *out_dst);

namespace {

// Overflow-safe range check: [off, off+len) must lie within [0, size).
bool in_image(uint64_t off, uint64_t len, uint64_t size) {
    if (len > size) {
        return false;
    }
    if (off > size - len) {
        return false;
    }
    return true;
}

// First program header of a type (phdr table itself bounds-checked).
const ELF64ProgramHeader *find_phdr(const ELF64Header *hdr,
                                    const uint8_t *image, uint64_t size,
                                    uint32_t type) {
    if (hdr->phnum > 64) {
        return nullptr;
    }
    if (hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return nullptr;
    }
    uint64_t table = static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
    if (!in_image(hdr->phoff, table, size)) {
        return nullptr;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type == type) {
            return p;
        }
    }
    return nullptr;
}

} // namespace

// (va_to_offset and the dynamic-section readers follow the pipeline
// bodies below; they only use in_image/find_phdr via internal calls.)
static bool apply_view(const DynView *view, const uint8_t *image,
                       uint64_t size, uint64_t load_base,
                       DepResolveContext *ctx, const char *soname,
                       const ELF64Header *hdr);

// Read a whole vnode into a contiguous PMM run (size pre-bounded by the
// caller).  Returns base phys, 0 on failure (OOM/read/cancel).
static uint64_t read_image_pages(vfs::Vnode *vn, uint64_t size,
                                 uint64_t *out_npages,
                                 DepResolveContext *ctx) {
    uint64_t npages = (size + arch::PAGE_SIZE - 1) / arch::PAGE_SIZE;
    if (npages == 0 || npages > MAX_SO_FILE_SIZE / arch::PAGE_SIZE) {
        return 0;
    }
    uint64_t base = PMM::alloc_contiguous(npages);
    if (base == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < npages; ++i) {
        uint64_t want = arch::PAGE_SIZE;
        if (i + 1 == npages) {
            want = size - i * arch::PAGE_SIZE;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + base +
                                                i * arch::PAGE_SIZE);
        int64_t r = vn->ops->read(*vn, dst, want, i * arch::PAGE_SIZE);
        if (r != static_cast<int64_t>(want)) {
            free_image_pages(base, npages);
            return 0;
        }
        if (ElfLoader::cancel_pending(ctx->generation)) {
            free_image_pages(base, npages);
            return 0;
        }
        Scheduler::reschedule();
    }
    *out_npages = npages;
    return base;
}

// Final executable flag of the PT_LOAD covering va (for hit-path maps).
static bool page_executable(const ELF64Header *hdr, const uint8_t *image,
                            uint64_t size, uint64_t va, bool *out_exec) {
    if (hdr->phnum > 64 || hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return false;
    }
    uint64_t table = static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
    if (!in_image(hdr->phoff, table, size)) {
        return false;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) {
            continue;
        }
        uint64_t start = p->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t end = (p->vaddr + p->memsz + arch::PAGE_SIZE - 1) &
                       ~(arch::PAGE_SIZE - 1);
        if (va >= start && va < end) {
            *out_exec = (p->flags & PF_X) != 0;
            return true;
        }
    }
    return false;
}

// Cache-hit path: overlap + budget checks, map shared RO pages and
// fresh RW copies (content re-read from the file), acquire, record.
static ElfError map_hit(LoadedLibrary *slot, DepResolveContext *ctx,
                        const char *name, uint64_t *out_base) {
    if (ranges_overlap(ctx->exec_base, ctx->exec_size, slot->load_base,
                       slot->load_size)) {
        return ElfError::LAYOUT;
    }
    for (uint64_t i = 0; i < ctx->acquired_count; ++i) {
        LoadedLibrary *s =
            SharedLibCache::instance().find(ctx->acquired[i]);
        if (s && s != slot &&
            ranges_overlap(s->load_base, s->load_size, slot->load_base,
                           slot->load_size)) {
            return ElfError::LAYOUT;
        }
    }
    if (ctx->budget_pages != 0 &&
        ctx->mapped_bytes + slot->load_size >
            ctx->budget_pages * arch::PAGE_SIZE) {
        return ElfError::NOMEM;
    }
    if (ctx->mapped_bytes + slot->load_size > MAX_TOTAL_MAPPED) {
        return ElfError::NOMEM;
    }
    // Re-read the file for RW page content (RO pages map shared phys).
    char path[128];
    if (name[0] == '/') {
        strncpy(path, name, sizeof(path) - 1);
    } else {
        strncpy(path, LIB_SEARCH_DIR, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        size_t d = strnlen_capped(path, sizeof(path));
        size_t n = strnlen_capped(name, sizeof(path));
        if (d + n + 1 > sizeof(path)) {
            return ElfError::NOT_FOUND;
        }
        __builtin_memcpy(path + d, name, n + 1);
    }
    path[sizeof(path) - 1] = '\0';
    vfs::Vnode *vn = vfs::resolve(path);
    if (!vn || !vn->ops || !vn->ops->read || vn->size != slot->file_size) {
        return ElfError::NOT_FOUND; // Missing or changed under us.
    }
    uint64_t npages = 0;
    uint64_t buf = read_image_pages(vn, vn->size, &npages, ctx);
    if (buf == 0) {
        return ElfError::NOMEM;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint8_t *image =
        reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + buf);
    const auto *hdr = reinterpret_cast<const ELF64Header *>(image);
    ElfError err = ElfError::OK;
    uint64_t va = slot->load_base;
    uint64_t npg = slot->num_phys_pages;
    if (npg > MAX_LIB_PHYS_PAGES) {
        free_image_pages(buf, npages);
        return ElfError::INVALID_ELF; // Corrupt slot: fail closed.
    }
    for (uint64_t i = 0; i < npg; ++i) {
        uint64_t phys = 0;
        if ((slot->writable_mask & (1ULL << i)) == 0) {
            phys = slot->phys_pages[i]; // Share read-only page.
        } else {
            phys = PMM::alloc_user_page();
            if (phys == 0) {
                err = ElfError::NOMEM;
                break;
            }
            // RW content via the segment map (fixed-layout invariant:
            // same VA range as the original mapping).
            uint64_t foff = 0;
            uint64_t flen = 0;
            uint64_t doff = 0;
            if (!page_file_bytes(hdr, image, vn->size, va, &foff, &flen,
                                 &doff)) {
                PMM::free_page(phys);
                err = ElfError::INVALID_ELF;
                break;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *dst =
                reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            __builtin_memset(dst, 0, arch::PAGE_SIZE);
            if (flen > 0) {
                __builtin_memcpy(dst + doff, image + foff, flen);
            }
        }
        // Final flags from the image's own PT_LOAD covering va.
        bool exec = false;
        if (!page_executable(hdr, image, vn->size, va, &exec)) {
            if ((slot->writable_mask & (1ULL << i)) != 0) {
                PMM::free_page(phys);
            }
            err = ElfError::INVALID_ELF;
            break;
        }
        VMM::map_page_in_pml4(va, phys, true, exec, ctx->pml4);
        if (VMM::virt_to_phys_in_pml4(va, ctx->pml4) != phys) {
            if ((slot->writable_mask & (1ULL << i)) != 0) {
                PMM::free_page(phys);
            }
            err = ElfError::NOMEM;
            break;
        }
        va += arch::PAGE_SIZE;
        if (ElfLoader::cancel_pending(ctx->generation)) {
            err = ElfError::CANCELED;
            break;
        }
        Scheduler::reschedule();
    }
    if (err != ElfError::OK) {
        free_image_pages(buf, npages);
        return err;
    }
    // Relocate the fresh RW copies for this task (same values: fixed
    // addresses make the pass idempotent).  Registration precedes the
    // reloc pass (mirrors load_fresh): symbol lookup needs this image
    // in the closure.
    DynView view{};
    __builtin_memset(&view, 0, sizeof(view));
    read_dynamic_section(hdr, image, vn->size, &view);
    if (ctx->acquired_count >= MAX_LOADED_LIBS ||
        ctx->image_count >= MAX_DEP_DEPTH + 1) {
        free_image_pages(buf, npages);
        SharedLibCache::instance().release(name);
        return ElfError::NOMEM;
    }
    SharedLibCache::instance().acquire(name);
    ctx->acquired[ctx->acquired_count++] = slot->soname;
    ctx->images[ctx->image_count] = image;
    ctx->image_sizes[ctx->image_count] = vn->size;
    ctx->image_bases[ctx->image_count] = slot->load_base;
    ctx->image_views[ctx->image_count] = view;
    ctx->image_phys[ctx->image_count] = buf; // Retained for closure.
    ctx->image_npages[ctx->image_count] = npages;
    ++ctx->image_count;
    if (view.dyn &&
        !apply_view(&view, image, vn->size, slot->load_base, ctx, name,
                    hdr)) {
        // Undo this call's registration (entries are last): the caller
        // treats a bare error as "nothing recorded", and the loader
        // cleanup would otherwise release twice.  The retained buffer
        // goes with the popped entry: free it here.
        --ctx->image_count;
        --ctx->acquired_count;
        free_image_pages(buf, npages);
        SharedLibCache::instance().release(name);
        return ElfError::INVALID_ELF;
    }
    ctx->mapped_bytes += slot->load_size;
    *out_base = slot->load_base;
    return ElfError::OK;
}

// File coverage of one VA page: overlap of [va, va+4K) with the
// file-backed prefix of the PT_LOAD covering va.  validate_segment (run
// by the caller beforehand) proved the segment arithmetic overflow-free
// and inside the image.
static bool page_file_bytes(const ELF64Header *hdr, const uint8_t *image,
                            uint64_t size, uint64_t va, uint64_t *out_off,
                            uint64_t *out_len, uint64_t *out_dst) {
    if (hdr->phnum > 64 || hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return false;
    }
    uint64_t table = static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
    if (!in_image(hdr->phoff, table, size)) {
        return false;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) {
            continue;
        }
        uint64_t start = p->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t end = (p->vaddr + p->memsz + arch::PAGE_SIZE - 1) &
                       ~(arch::PAGE_SIZE - 1);
        if (va < start || va >= end) {
            continue;
        }
        uint64_t lo = va < p->vaddr ? p->vaddr : va;
        uint64_t body_end = p->vaddr + p->filesz;
        uint64_t page_end = va + arch::PAGE_SIZE;
        *out_off = 0;
        *out_len = 0;
        *out_dst = lo > va ? lo - va : 0;
        if (lo < page_end && lo < body_end) {
            uint64_t hi = page_end < body_end ? page_end : body_end;
            *out_off = p->offset + (lo - p->vaddr);
            *out_len = hi - lo;
        }
        return true;
    }
    return false;
}

// Compute the fixed load range of an image (min PT_LOAD vaddr, total).
// Returns false on overflow or empty range.
static bool image_range(const ELF64Header *hdr, const uint8_t *image,
                        uint64_t size, uint64_t *out_base,
                        uint64_t *out_total) {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool any = false;
    if (hdr->phnum > 64 || hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return false;
    }
    uint64_t table = static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
    if (table > size || hdr->phoff > size - table) {
        return false;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) {
            continue;
        }
        uint64_t start = p->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t end = p->vaddr + p->memsz;
        if (end < p->vaddr) {
            return false;
        }
        end = (end + arch::PAGE_SIZE - 1) & ~(arch::PAGE_SIZE - 1);
        if (end < p->vaddr) {
            return false;
        }
        if (!any || start < lo) {
            lo = start;
        }
        if (!any || end > hi) {
            hi = end;
        }
        any = true;
    }
    if (!any || hi <= lo) {
        return false;
    }
    *out_base = lo;
    *out_total = hi - lo;
    return true;
}

// Map one image page (fresh phys) with final flags; file_len bytes go
// at dst_off, the rest is zeroed (bss / unaligned head).
static ElfError map_one_page(DepResolveContext *ctx, uint64_t va,
                             const uint8_t *image, uint64_t file_off,
                             uint64_t file_len, uint64_t dst_off,
                             bool executable, uint64_t *out_phys) {
    uint64_t phys = PMM::alloc_user_page();
    if (phys == 0) {
        return ElfError::NOMEM;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *dst = reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
    __builtin_memset(dst, 0, arch::PAGE_SIZE);
    if (file_len > 0) {
        __builtin_memcpy(dst + dst_off, image + file_off, file_len);
    }
    VMM::map_page_in_pml4(va, phys, true, executable, ctx->pml4);
    if (VMM::virt_to_phys_in_pml4(va, ctx->pml4) != phys) {
        PMM::free_page(phys); // Silent table-alloc failure: fail closed.
        return ElfError::NOMEM;
    }
    *out_phys = phys;
    return ElfError::OK;
}

struct MapOut {
    uint64_t phys[MAX_LIB_PHYS_PAGES];
    uint64_t writable;
    uint64_t count;
};

// Map all PT_LOADs of an image in VA order (= cache file-page order).
static ElfError map_image(const ELF64Header *hdr, const uint8_t *image,
                          uint64_t size, DepResolveContext *ctx,
                          MapOut *out) {
    out->count = 0;
    out->writable = 0;
    if (hdr->phnum > 64 || hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return ElfError::INVALID_ELF;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != PT_LOAD || p->memsz == 0) {
            continue;
        }
        if (!validate_segment(p, size)) {
            return ElfError::INVALID_ELF;
        }
        bool exec = (p->flags & PF_X) != 0;
        bool write = (p->flags & PF_W) != 0;
        uint64_t start = p->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t end = (p->vaddr + p->memsz + arch::PAGE_SIZE - 1) &
                       ~(arch::PAGE_SIZE - 1);
        for (uint64_t va = start; va < end; va += arch::PAGE_SIZE) {
            if (out->count >= MAX_LIB_PHYS_PAGES) {
                return ElfError::NOMEM; // Beyond the share cap.
            }
            // File coverage via the shared segment-map helper.
            uint64_t file_off = 0;
            uint64_t file_len = 0;
            uint64_t dst_off = 0;
            if (!page_file_bytes(hdr, image, size, va, &file_off,
                                 &file_len, &dst_off)) {
                return ElfError::INVALID_ELF;
            }
            uint64_t phys = 0;
            ElfError e = map_one_page(ctx, va, image, file_off, file_len,
                                      dst_off, exec, &phys);
            if (e != ElfError::OK) {
                return e;
            }
            out->phys[out->count] = phys;
            if (write) {
                out->writable |= 1ULL << out->count;
            }
            ++out->count;
            if (ElfLoader::cancel_pending(ctx->generation)) {
                return ElfError::CANCELED;
            }
            Scheduler::reschedule();
        }
    }
    return ElfError::OK;
}

bool load_shared_object(const char *soname, DepResolveContext *ctx,
                        uint64_t depth, uint64_t *out_load_base) {
    if (!soname || soname[0] == '\0' || !ctx || !out_load_base) {
        return false;
    }
    if (depth > MAX_DEP_DEPTH) {
        chain_append(ctx, soname);
        return false;
    }
    if (ElfLoader::cancel_pending(ctx->generation)) {
        return false;
    }
    // Bounded local copy (dynstr already NUL-validated; belt-and-braces).
    char name[128];
    strncpy(name, soname, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    bool created = false;
    LoadedLibrary *slot = SharedLibCache::instance().reserve(name, &created);
    if (!slot) {
        chain_append(ctx, name);
        return false;
    }
    if (!created) {
        if (slot->in_progress) {
            chain_append(ctx, name); // Cycle: ancestor still loading it.
            return false;
        }
        if (load_one(name, ctx, depth, out_load_base) != ElfError::OK) {
            // map_hit appends its own chain on terminal failure.
            return false;
        }
        return true;
    }
    if (load_fresh(name, slot, ctx, depth, out_load_base) != ElfError::OK) {
        chain_append(ctx, name);
        SharedLibCache::instance().clear_in_progress(name);
        SharedLibCache::instance().release(name);
        return false;
    }
    return true;
}

// Fresh load of a reserved slot: discover, buffer, validate, overlap +
// budget checks, map, fill slot, recurse into its own deps.
static ElfError load_fresh(const char *name, LoadedLibrary *slot,
                           DepResolveContext *ctx, uint64_t depth,
                           uint64_t *out_base) {
    char path[128];
    if (name[0] == '/') {
        strncpy(path, name, sizeof(path) - 1);
    } else {
        strncpy(path, LIB_SEARCH_DIR, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        size_t d = strnlen_capped(path, sizeof(path));
        size_t n = strnlen_capped(name, sizeof(path));
        if (d + n + 1 > sizeof(path)) {
            return ElfError::NOT_FOUND;
        }
        __builtin_memcpy(path + d, name, n + 1);
    }
    path[sizeof(path) - 1] = '\0';
    vfs::Vnode *vn = vfs::resolve(path);
    if (!vn || !vn->ops || !vn->ops->read) {
        return ElfError::NOT_FOUND;
    }
    uint64_t size = vn->size;
    if (size == 0 || size > MAX_SO_FILE_SIZE) {
        return ElfError::NOMEM;
    }
    uint64_t npages = 0;
    uint64_t buf = read_image_pages(vn, size, &npages, ctx);
    if (buf == 0) {
        return ElfError::NOMEM;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const uint8_t *image =
        reinterpret_cast<const uint8_t *>(arch::HHDM_OFFSET + buf);
    const auto *hdr = reinterpret_cast<const ELF64Header *>(image);
    ElfError err = ElfError::INVALID_ELF;
    if (validate_header(hdr) && hdr->type == ET_DYN) {
        uint64_t base = 0;
        uint64_t total = 0;
        if (image_range(hdr, image, size, &base, &total) &&
            !ranges_overlap(ctx->exec_base, ctx->exec_size, base, total) &&
            !ranges_overlap(mem::STACK_VADDR, mem::STACK_SIZE, base,
                            total) &&
            !ranges_overlap(mem::HEAP_VADDR, mem::HEAP_SIZE, base, total)) {
            bool clash = false;
            for (uint64_t i = 0; i < ctx->acquired_count; ++i) {
                LoadedLibrary *s =
                    SharedLibCache::instance().find(ctx->acquired[i]);
                if (s && ranges_overlap(s->load_base, s->load_size, base,
                                        total)) {
                    clash = true;
                    break;
                }
            }
            if (!clash &&
                (ctx->budget_pages == 0 ||
                 ctx->mapped_bytes + total <=
                     ctx->budget_pages * arch::PAGE_SIZE) &&
                ctx->mapped_bytes + total <= MAX_TOTAL_MAPPED) {
                MapOut mapped{};
                err = map_image(hdr, image, size, ctx, &mapped);
                if (err == ElfError::OK) {
                    DynView lib_view{};
                    __builtin_memset(&lib_view, 0, sizeof(lib_view));
                    read_dynamic_section(hdr, image, size, &lib_view);
                    slot->load_base = base;
                    slot->load_size = total;
                    slot->file_size = size;
                    slot->num_phys_pages = static_cast<uint32_t>(
                        mapped.count);
                    slot->writable_mask = mapped.writable;
                    for (uint64_t i = 0; i < mapped.count; ++i) {
                        slot->phys_pages[i] = mapped.phys[i];
                    }
                    slot->refcount = 1;
                    // NOTE: in_progress stays true through dep recursion
                    // below (cycle detection); cleared on success here,
                    // or via clear_in_progress+release on failure.
                    if (ctx->acquired_count < MAX_LOADED_LIBS &&
                        ctx->image_count < MAX_DEP_DEPTH + 1) {
                        ctx->acquired[ctx->acquired_count++] =
                            slot->soname;
                        ctx->images[ctx->image_count] = image;
                        ctx->image_sizes[ctx->image_count] = size;
                        ctx->image_bases[ctx->image_count] = base;
                        ctx->image_views[ctx->image_count] = lib_view;
                        ctx->image_phys[ctx->image_count] = buf;
                        ctx->image_npages[ctx->image_count] = npages;
                        ++ctx->image_count;
                        ctx->mapped_bytes += total;
                        *out_base = base;
                        buf = 0; // Retained: owned by ctx now.
                        err = resolve_image(
                            hdr, image, size, ctx, depth, base);
                        if (err == ElfError::OK) {
                            slot->in_progress = false; // Fully loaded.
                        }
                    } else {
                        err = ElfError::NOMEM;
                    }
                }
            } else if (!clash) {
                err = ElfError::NOMEM;
            } else {
                err = ElfError::LAYOUT;
            }
        } else {
            err = ElfError::LAYOUT;
        }
    }
    if (buf != 0) {
        free_image_pages(buf, npages); // Not retained: failure path.
    }
    return err;
}

// Single-soname driver shared by the public entry and the recursion.
static ElfError load_one(const char *soname, DepResolveContext *ctx,
                         uint64_t depth, uint64_t *out_base) {
    if (!soname || soname[0] == '\0' || !ctx || !out_base) {
        return ElfError::INVALID_ELF;
    }
    if (depth > MAX_DEP_DEPTH) {
        return ElfError::DEPTH;
    }
    if (ElfLoader::cancel_pending(ctx->generation)) {
        return ElfError::CANCELED;
    }
    char name[128];
    strncpy(name, soname, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    bool created = false;
    LoadedLibrary *slot = SharedLibCache::instance().reserve(name, &created);
    if (!slot) {
        return ElfError::NOMEM;
    }
    if (!created) {
        if (slot->in_progress) {
            return ElfError::CYCLE;
        }
        return map_hit(slot, ctx, name, out_base);
    }
    ElfError err = load_fresh(name, slot, ctx, depth, out_base);
    if (err != ElfError::OK) {
        SharedLibCache::instance().clear_in_progress(name);
        SharedLibCache::instance().release(name);
    }
    return err;
}

static ElfError resolve_image(const ELF64Header *hdr, const uint8_t *image,
                              uint64_t size, DepResolveContext *ctx,
                              uint64_t depth, uint64_t load_base) {
    if (!hdr || !image || !ctx || size == 0) {
        return ElfError::INVALID_ELF;
    }
    DynView view{};
    if (!read_dynamic_section(hdr, image, size, &view)) {
        // Static image (no PT_DYNAMIC) resolves trivially.
        return find_phdr(hdr, image, size, PT_DYNAMIC) == nullptr
                   ? ElfError::OK
                   : ElfError::INVALID_ELF;
    }
    if (count_needed(&view) > MAX_DEP_DEPTH) {
        return ElfError::NOMEM;
    }
    const char *needed[MAX_DEP_DEPTH];
    size_t n = get_needed(&view, needed);
    for (size_t i = 0; i < n; ++i) {
        uint64_t mark = strnlen_capped(ctx->fail_chain,
                                       sizeof(ctx->fail_chain));
        chain_append(ctx, needed[i]);
        uint64_t base = 0;
        ElfError err = load_one(needed[i], ctx, depth + 1, &base);
        (void)base;
        if (err != ElfError::OK) {
            ctx->last_error = err;
            return err;
        }
        ctx->fail_chain[mark] = '\0'; // Pop on success: exact failure path.
    }
    // NOTE: image registration in ctx.images[] is the CALLER's job
    // (public resolve registers the exec, load_fresh registers each lib)
    // so each image appears exactly once for the post-order reloc pass.
    (void)load_base;
    return ElfError::OK;
}

bool resolve_dependencies(const ELF64Header *hdr, const uint8_t *file_data,
                          uint64_t file_size, DepResolveContext *ctx,
                          uint64_t depth, uint64_t load_base) {
    if (!hdr || !file_data || !ctx || file_size == 0) {
        return false;
    }
    if (depth > MAX_DEP_DEPTH) {
        return false;
    }
    // Register the exec image for the closure (images[0]); libs append
    // their own entries in load_fresh/map_hit in acquired order, so
    // images[j+1] always pairs with acquired[j] for dmesg labels.
    DynView exec_view{};
    __builtin_memset(&exec_view, 0, sizeof(exec_view));
    read_dynamic_section(hdr, file_data, file_size, &exec_view);
    if (ctx->image_count >= MAX_DEP_DEPTH + 1) {
        return false;
    }
    ctx->images[ctx->image_count] = file_data;
    ctx->image_sizes[ctx->image_count] = file_size;
    ctx->image_bases[ctx->image_count] = load_base;
    ctx->image_views[ctx->image_count] = exec_view;
    ctx->image_phys[ctx->image_count] = 0; // Caller-owned (exec buffer).
    ctx->image_npages[ctx->image_count] = 0;
    ++ctx->image_count;
    ElfError err = resolve_image(hdr, file_data, file_size, ctx, depth,
                                 load_base);
    if (err != ElfError::OK) {
        ctx->last_error = err;
        return false;
    }
    // Post-order relocate pass over the full closure (top level only;
    // nested resolves leave relocation to the outermost call so each
    // image is relocated exactly once).
    if (depth == 0) {
        return relocate_closure(ctx);
    }
    return true;
}

bool relocate_closure(DepResolveContext *ctx) {
    if (!ctx) {
        return false;
    }
    for (uint64_t i = 0; i < ctx->image_count; ++i) {
        const auto *ihdr =
            reinterpret_cast<const ELF64Header *>(ctx->images[i]);
        const char *who = (i == 0) ? "exec" : ctx->acquired[i - 1];
        if (!apply_relocations(ihdr, ctx->images[i], ctx->image_sizes[i],
                               ctx->image_bases[i], ctx, who)) {
            return false;
        }
    }
    return true;
}

// Look up one image's table for a GLOBAL/WEAK definition.
// Returns 1 = defined (value set), 0 = not here, -1 = corrupt entry.
static int64_t lookup_one(DepResolveContext *ctx, uint64_t idx,
                          const char *name, uint64_t *out_value) {
    const DynView *view = &ctx->image_views[idx];
    if (!view->dyn || !view->symtab) {
        return 0;
    }
    // Defined-symbol values are link-time VAs: add the defining image's
    // load bias (mapped − linked; 0 for fixed-address images, nonzero
    // for base-0 links).  Adding the mapped base unconditionally would
    // double-count based links (observed 0xA00100).
    const auto *hdr =
        reinterpret_cast<const ELF64Header *>(ctx->images[idx]);
    uint64_t link_base = 0;
    uint64_t link_total = 0;
    if (!image_range(hdr, ctx->images[idx], ctx->image_sizes[idx],
                     &link_base, &link_total)) {
        return -1;
    }
    uint64_t mapped = ctx->image_bases[idx];
    if (mapped < link_base) {
        return -1;
    }
    uint64_t bias = mapped - link_base;
    for (uint64_t s = 0; s < view->sym_count; ++s) {
        const ELF64Symbol *sym = &view->symtab[s];
        uint64_t bind = (sym->st_info >> 4) & 0xF;
        if (bind != 1 && bind != 2) {
            continue; // LOCAL or odd: not interposable.
        }
        if (sym->st_name >= view->strsz) {
            continue;
        }
        const char *sym_name = view->strtab + sym->st_name;
        if (strnlen_capped(sym_name, view->strsz - sym->st_name) >=
            view->strsz - sym->st_name) {
            continue;
        }
        if (strcmp(sym_name, name) != 0) {
            continue;
        }
        if (sym->st_shndx == 0) {
            if (bind == 2) {
                *out_value = 0; // WEAK undefined: resolves zero.
                return 1;
            }
            continue; // UNDEF: keep searching.
        }
        if (sym->st_value > ~0ULL - bias) {
            return -1;
        }
        *out_value = bias + sym->st_value;
        return 1;
    }
    return 0;
}

// Look up a symbol closure-wide: own image first, then closure order.
// Returns false when undefined everywhere (caller fails closed), except
// WEAK-undefined which resolves to 0.
static bool lookup_symbol(DepResolveContext *ctx, uint64_t self_idx,
                          const char *name, uint64_t *out_value) {
    if (self_idx < ctx->image_count) {
        int64_t r = lookup_one(ctx, self_idx, name, out_value);
        if (r != 0) {
            return r > 0;
        }
    }
    for (uint64_t k = 0; k < ctx->image_count; ++k) {
        if (k == self_idx) {
            continue;
        }
        int64_t r = lookup_one(ctx, k, name, out_value);
        if (r != 0) {
            return r > 0;
        }
    }
    return false;
}

// Apply one RELA table (DT_RELA or DT_JMPREL, both RELA-form).
// Fixed-address model (§2.2): images link at their load VAs, so the
// load bias (mapped base − link base) is normally 0; base-0 (PIE-style)
// images still relocate via the bias.  r_offset/d_ptr are link-time VAs.
static bool apply_rela_table(const ELF64Rela *table, uint64_t count,
                             const ELF64Header *hdr, const uint8_t *image,
                             uint64_t size, uint64_t load_base,
                             DepResolveContext *ctx, uint64_t self_idx,
                             const char *soname) {
    uint64_t link_base = 0;
    uint64_t link_total = 0;
    if (!image_range(hdr, image, size, &link_base, &link_total)) {
        return false;
    }
    if (load_base < link_base) {
        return false;
    }
    uint64_t bias = load_base - link_base;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t type = rela_type(table[i].r_info);
        uint64_t sym = rela_sym(table[i].r_info);
        if (table[i].r_offset > ~0ULL - bias) {
            return false;
        }
        uint64_t target = table[i].r_offset + bias;
        uint64_t value = 0;
        if (type == R_X86_64_RELATIVE) {
            if (sym != 0) {
                return false;
            }
            if (table[i].r_addend < 0 ||
                static_cast<uint64_t>(table[i].r_addend) > ~0ULL - bias) {
                return false;
            }
            value = bias + static_cast<uint64_t>(table[i].r_addend);
        } else if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT ||
                   type == R_X86_64_64) {
            const DynView *self = &ctx->image_views[self_idx];
            if (!self->dyn || !self->symtab || sym >= self->sym_count) {
                return false;
            }
            const ELF64Symbol *s = &self->symtab[sym];
            if (s->st_name >= self->strsz) {
                return false;
            }
            const char *name = self->strtab + s->st_name;
            if (strnlen_capped(name, self->strsz - s->st_name) >=
                self->strsz - s->st_name) {
                return false;
            }
            if (!lookup_symbol(ctx, self_idx, name, &value)) {
                return false; // Undefined symbol: fail closed.
            }
        } else {
            return false; // Outside the eager subset (COPY/PC32/...).
        }
        if (!target_write_u64(ctx->pml4, target, value)) {
            return false; // Unmapped/out-of-range target.
        }
        (void)soname;
    }
    return true;
}

static bool apply_view(const DynView *view, const uint8_t *image,
                       uint64_t size, uint64_t load_base,
                       DepResolveContext *ctx, const char *soname,
                       const ELF64Header *hdr) {
    (void)image;
    (void)size;
    if (!view || !view->dyn || !ctx || !soname || !hdr) {
        kernel::Logger::warn("elf_shared: apply_view null argument");
        return false;
    }
    // This image must be registered in the closure for symbol lookup.
    uint64_t self_idx = 0;
    bool found = false;
    for (uint64_t k = 0; k < ctx->image_count; ++k) {
        if (ctx->image_bases[k] == load_base) {
            self_idx = k;
            found = true;
            break;
        }
    }
    if (!found) {
        kernel::Logger::warn("elf_shared: image not in closure base=%lx",
                             load_base);
        return false;
    }
    // Non-empty init arrays need untrusted code execution: reject.
    for (uint64_t i = 0; i < view->dyn_count; ++i) {
        if (view->dyn[i].d_tag == DT_INIT) {
            return false;
        }
        if ((view->dyn[i].d_tag == DT_INIT_ARRAY ||
             view->dyn[i].d_tag == DT_FINI_ARRAY) &&
            (i + 1 >= view->dyn_count ||
             (view->dyn[i + 1].d_tag != DT_INIT_ARRAYSZ &&
              view->dyn[i + 1].d_tag != DT_FINI_ARRAYSZ) ||
             view->dyn[i + 1].d_val != 0)) {
            // Non-empty init/fini array (or array without a proven-zero
            // size successor): fail closed.  Empty arrays pass.
            bool empty = false;
            for (uint64_t j = 0; j < view->dyn_count; ++j) {
                if ((view->dyn[i].d_tag == DT_INIT_ARRAY &&
                     view->dyn[j].d_tag == DT_INIT_ARRAYSZ &&
                     view->dyn[j].d_val == 0) ||
                    (view->dyn[i].d_tag == DT_FINI_ARRAY &&
                     view->dyn[j].d_tag == DT_FINI_ARRAYSZ &&
                     view->dyn[j].d_val == 0)) {
                    empty = true;
                    break;
                }
            }
            if (!empty) {
                return false;
            }
        }
    }
    if (view->rela && view->relasz > 0) {
        uint64_t count = view->relasz / view->relaent;
        if (!apply_rela_table(view->rela, count, hdr, image, size,
                              load_base, ctx, self_idx, soname)) {
            return false;
        }
    }
    if (view->jmprel && view->pltrelsz > 0) {
        uint64_t count = view->pltrelsz / sizeof(ELF64Rela);
        if (!apply_rela_table(view->jmprel, count, hdr, image, size,
                              load_base, ctx, self_idx, soname)) {
            return false;
        }
    }
    return true;
}

bool apply_relocations(const ELF64Header *hdr, const uint8_t *file_data,
                       uint64_t file_size, uint64_t load_base,
                       DepResolveContext *ctx, const char *soname) {
    if (!hdr || !file_data || !ctx || !soname || file_size == 0) {
        return false;
    }
    DynView view{};
    if (!read_dynamic_section(hdr, file_data, file_size, &view)) {
        // Static image: nothing to relocate.
        return find_phdr(hdr, file_data, file_size, PT_DYNAMIC) == nullptr;
    }
    return apply_view(&view, file_data, file_size, load_base, ctx, soname,
                      hdr);
}

namespace {

// (in_image/find_phdr live in the pipeline block above and are reused
// here; strnlen via strnlen_capped.)

// Translate a runtime VA to a file offset via PT_LOAD segments (only the
// file-backed prefix filesz, never bss tail).
bool va_to_offset(const ELF64Header *hdr, const uint8_t *image,
                  uint64_t size, uint64_t va, uint64_t *out_off) {
    if (hdr->phnum > 64) {
        return false;
    }
    if (hdr->phentsize < sizeof(ELF64ProgramHeader)) {
        return false;
    }
    uint64_t table = static_cast<uint64_t>(hdr->phnum) * hdr->phentsize;
    if (!in_image(hdr->phoff, table, size)) {
        return false;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != PT_LOAD) {
            continue;
        }
        if (va < p->vaddr) {
            continue;
        }
        uint64_t rel = va - p->vaddr;
        if (rel >= p->filesz) {
            continue;
        }
        uint64_t off = p->offset + rel;
        if (off < p->offset) {
            return false;
        }
        *out_off = off;
        return true;
    }
    return false;
}

// A dynstr offset is usable only if it starts a NUL-terminated string
// fully inside [strtab, strtab+strsz).
bool dynstr_ok(const char *strtab, uint64_t strsz, uint64_t off) {
    if (off >= strsz) {
        return false;
    }
    return strnlen_capped(strtab + off, strsz - off) < strsz - off;
}

} // namespace

const char *get_soname(const DynView *view) {
    if (!view || !view->dyn) {
        return nullptr;
    }
    for (uint64_t i = 0; i < view->dyn_count; ++i) {
        if (view->dyn[i].d_tag == DT_SONAME) {
            uint64_t off = view->dyn[i].d_val;
            if (!dynstr_ok(view->strtab, view->strsz, off)) {
                return nullptr;
            }
            return view->strtab + off;
        }
    }
    return nullptr;
}

size_t get_needed(const DynView *view, const char **needed) {
    if (!view || !view->dyn || !needed) {
        return 0;
    }
    size_t n = 0;
    for (uint64_t i = 0; i < view->dyn_count; ++i) {
        if (view->dyn[i].d_tag != DT_NEEDED) {
            continue;
        }
        if (n >= MAX_DEP_DEPTH) {
            return n;
        }
        uint64_t off = view->dyn[i].d_val;
        if (!dynstr_ok(view->strtab, view->strsz, off)) {
            return n;
        }
        needed[n++] = view->strtab + off;
    }
    return n;
}

bool read_dynamic_section(const ELF64Header *hdr, const uint8_t *file_data,
                          uint64_t file_size, DynView *out_view) {
    if (!hdr || !file_data || !out_view) {
        return false;
    }
    __builtin_memset(out_view, 0, sizeof(DynView));
    const ELF64ProgramHeader *dyn_phdr =
        find_phdr(hdr, file_data, file_size, PT_DYNAMIC);
    if (!dyn_phdr) {
        return false; // Static image: no dynamic section, not an error.
    }
    if (dyn_phdr->filesz < sizeof(ELF64Dynamic)) {
        return false;
    }
    if (!in_image(dyn_phdr->offset, dyn_phdr->filesz, file_size)) {
        return false;
    }
    uint64_t count = dyn_phdr->filesz / sizeof(ELF64Dynamic);
    if (count > 64) {
        count = 64;
    }
    const auto *dyn = reinterpret_cast<const ELF64Dynamic *>(
        file_data + dyn_phdr->offset);
    out_view->dyn = dyn;
    out_view->dyn_count = count;

    // First pass: locate the string/symbol tables.
    uint64_t strtab_va = 0;
    uint64_t symtab_va = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (dyn[i].d_tag == DT_NULL) {
            break;
        }
        switch (dyn[i].d_tag) {
            case DT_STRTAB:
                strtab_va = dyn[i].d_val;
                break;
            case DT_STRSZ:
                out_view->strsz = dyn[i].d_val;
                break;
            case DT_SYMTAB:
                symtab_va = dyn[i].d_val;
                break;
            case DT_SYMENT:
                out_view->syment = dyn[i].d_val;
                break;
            default:
                break;
        }
    }
    if (strtab_va == 0 || out_view->strsz == 0) {
        return false;
    }
    uint64_t str_off = 0;
    if (!va_to_offset(hdr, file_data, file_size, strtab_va, &str_off)) {
        return false;
    }
    if (!in_image(str_off, out_view->strsz, file_size)) {
        return false;
    }
    out_view->strtab =
        reinterpret_cast<const char *>(file_data + str_off);
    if (symtab_va != 0) {
        if (out_view->syment != sizeof(ELF64Symbol)) {
            return false;
        }
        uint64_t sym_off = 0;
        if (!va_to_offset(hdr, file_data, file_size, symtab_va, &sym_off)) {
            return false;
        }
        if (sym_off >= file_size) {
            return false;
        }
        out_view->symtab =
            reinterpret_cast<const ELF64Symbol *>(file_data + sym_off);
        out_view->sym_count = (file_size - sym_off) / out_view->syment;
    }

    // Second pass: relocation tables (strict subset: RELA with 24-byte
    // entries; REL and odd sizes fail closed).
    for (uint64_t i = 0; i < count; ++i) {
        if (dyn[i].d_tag == DT_NULL) {
            break;
        }
        uint64_t off = 0;
        switch (dyn[i].d_tag) {
            case DT_RELA:
                if (!va_to_offset(hdr, file_data, file_size, dyn[i].d_val,
                                  &off)) {
                    return false;
                }
                out_view->rela =
                    reinterpret_cast<const ELF64Rela *>(file_data + off);
                break;
            case DT_RELASZ:
                out_view->relasz = dyn[i].d_val;
                break;
            case DT_RELAENT:
                out_view->relaent = dyn[i].d_val;
                break;
            case DT_JMPREL:
                if (!va_to_offset(hdr, file_data, file_size, dyn[i].d_val,
                                  &off)) {
                    return false;
                }
                out_view->jmprel =
                    reinterpret_cast<const ELF64Rela *>(file_data + off);
                break;
            case DT_PLTRELSZ:
                out_view->pltrelsz = dyn[i].d_val;
                break;
            case DT_PLTREL:
                out_view->pltrel_type = dyn[i].d_val;
                break;
            case DT_TEXTREL:
                return false; // Text relocations need WX: reject.
            default:
                break;
        }
    }
    if (out_view->rela || out_view->relasz != 0) {
        if (out_view->relaent != sizeof(ELF64Rela)) {
            return false;
        }
        if (!out_view->rela ||
            !in_image(reinterpret_cast<const uint8_t *>(out_view->rela) -
                          file_data,
                      out_view->relasz, file_size)) {
            return false;
        }
    }
    if (out_view->jmprel || out_view->pltrelsz != 0) {
        if (out_view->pltrel_type != DT_RELA) {
            return false; // REL PLT: unsupported subset.
        }
        if (!out_view->jmprel ||
            !in_image(reinterpret_cast<const uint8_t *>(out_view->jmprel) -
                          file_data,
                      out_view->pltrelsz, file_size)) {
            return false;
        }
    }
    return true;
}

} // namespace elf
} // namespace kernel
