/// @file test_elf_shared.cpp
/// @brief Tests for ELF shared-object support (issue #95, spec
///        docs/specs/elf-shared-libs.md §5).

#include <test.hpp>
#include <logger.hpp>
#include <kernel/nexios_config.h>
#include <kernel/elf/elf.hpp>
#include <kernel/elf/elf_shared.hpp>
#include <kernel/elf/elf_loader.hpp>
#include <kernel/task/scheduler.hpp>
#include <kernel/task/task.hpp>
#include <kernel/memory/vmm.hpp>
#include <kernel/memory/pmm.hpp>
#include <kernel/memory/address.hpp>
#include <kernel/vfs/vfs.hpp>
#include <kernel/arch/irq_guard.hpp>
#include <kernel/test/test_isolate.hpp>
#include <string.hpp>

using namespace kernel;

#if defined(CONFIG_ARCH_X86_64)

namespace {

// Fixed layout for synthetic fixtures (spec §2.2: link at fixed VA).
// The boot fs (/) is read-only initrd, so fixtures live on tmpfs under
// /tmp and DT_NEEDED carries absolute paths (passthrough discovery);
// the /lib search branch is covered on its failure side (missing lib).
constexpr uint64_t kLibBase = 0x500000;
constexpr uint64_t kLibText = 0x500000;
constexpr uint64_t kLibData = 0x501000;
constexpr uint64_t kLibFunc = 0x500100; // myfunc VA (in TEXT).
constexpr uint64_t kLibGot = 0x501000;  // GOT slot VA (in DATA).
constexpr uint64_t kExecBase = 0x400000;
constexpr uint64_t kExecGot = 0x401000; // Exec GOT slot VA.

// dynstr: "\0<soname>\0myfunc\0" — soname at 1, myfunc after it.
constexpr uint64_t kSonameOff = 1;

uint64_t write_file(const char *path, const uint8_t *data, uint64_t size) {
    kernel::test::mark_vfs_touched();
    int ret = vfs::create(path, 0);
    if (ret != 0) {
        return 0;
    }
    vfs::Vnode *file = vfs::resolve(path);
    if (!file || !file->ops || !file->ops->write) {
        return 0;
    }
    int64_t written = file->ops->write(*file, data, size, 0);
    return static_cast<uint64_t>(written);
}

/// @brief Parametric synthetic shared object (ET_DYN, fixed base).
struct SoSpec {
    const char *soname;   // Absolute path (also DT_SONAME).
    const char *needs[4]; // DT_NEEDED list.
    uint64_t nneeds;
    uint64_t base;        // Fixed link/load base.
    bool has_func;        // Export "myfunc" at base+0x100.
    bool has_got_reloc;   // RELATIVE GOT slot at base+0x1000 -> base+0x100.
};

/// @brief Parametric synthetic executable (ET_EXEC, fixed base).
struct ExecSpec {
    const char *needs[4]; // DT_NEEDED list.
    uint64_t nneeds;
    uint64_t base;        // Fixed link/load base (0x400000).
    bool want_jump;       // JMPREL GLOB_DAT myfunc -> GOT at base+0x1000.
};

// DATA-page layout for synthetic images (all file offsets relative to
// the DATA file page at image+0x2000; VAs = data_va + offset).
constexpr uint64_t kDynstrOff = 0x0;
constexpr uint64_t kDynsymOff = 0x200;
constexpr uint64_t kRelaOff = 0x300;
constexpr uint64_t kJmprelOff = 0x380;
constexpr uint64_t kDynOff = 0x400;
constexpr uint64_t kMaxSynSyms = 8;

/// @brief Emit dynstr (soname/needed names + optional myfunc), dynsym,
///        RELA/JMPREL and DYNAMIC into a DATA file page.
/// @return DYNAMIC entry count (for PT_DYNAMIC filesz), 0 on overflow.
uint64_t emit_dynamic(uint8_t *data_page, uint64_t data_va, const char *soname,
                      const char *const *needs, uint64_t nneeds,
                      bool is_exec, bool has_func, bool has_got_reloc,
                      bool want_jump, uint64_t base) {
    // dynstr.
    uint64_t spos = 1;
    char *str = reinterpret_cast<char *>(data_page + kDynstrOff);
    str[0] = '\0';
    uint64_t soname_off = 0;
    if (soname) {
        soname_off = spos;
        for (uint64_t i = 0;; ++i) {
            if (spos >= kDynsymOff) {
                return 0;
            }
            str[spos++] = soname[i];
            if (soname[i] == '\0') {
                break;
            }
        }
    }
    uint64_t needed_off[4] = {0, 0, 0, 0};
    for (uint64_t k = 0; k < nneeds && k < 4; ++k) {
        needed_off[k] = spos;
        for (uint64_t i = 0;; ++i) {
            if (spos >= kDynsymOff) {
                return 0;
            }
            str[spos++] = needs[k][i];
            if (needs[k][i] == '\0') {
                break;
            }
        }
    }
    uint64_t myfunc_off = 0;
    if (has_func || want_jump) {
        myfunc_off = spos;
        const char *symname = "myfunc";
        for (uint64_t i = 0;; ++i) {
            if (spos >= kDynsymOff) {
                return 0;
            }
            str[spos++] = symname[i];
            if (symname[i] == '\0') {
                break;
            }
        }
    }
    uint64_t strsz = spos;
    // dynsym: null + myfunc (GLOBAL FUNC).
    auto *sym = reinterpret_cast<elf::ELF64Symbol *>(data_page + kDynsymOff);
    __builtin_memset(sym, 0, 2 * sizeof(elf::ELF64Symbol));
    uint64_t nsyms = 1;
    if (has_func || want_jump) {
        sym[1].st_name = static_cast<uint32_t>(myfunc_off);
        sym[1].st_info = (1 << 4) | 2;
        sym[1].st_shndx = has_func ? 1 : 0; // Defined vs UNDEF.
        // Fixed-address model: link-time st_value is absolute.
        sym[1].st_value = has_func ? base + 0x100 : 0;
        nsyms = 2;
    }
    // RELA: RELATIVE GOT slot (lib data page start) -> func VA.
    uint64_t nrela = 0;
    if (has_got_reloc) {
        auto *rela = reinterpret_cast<elf::ELF64Rela *>(data_page + kRelaOff);
        rela->r_offset = data_va;
        rela->r_info = (static_cast<uint64_t>(0) << 32) | 8;
        rela->r_addend = static_cast<int64_t>(data_va - 0x1000 + 0x100);
        nrela = 1;
    }
    // JMPREL: GLOB_DAT myfunc -> GOT slot (exec data page start).
    uint64_t njmp = 0;
    if (want_jump) {
        auto *jmp = reinterpret_cast<elf::ELF64Rela *>(data_page + kJmprelOff);
        jmp->r_offset = data_va;
        jmp->r_info = (static_cast<uint64_t>(1) << 32) | 7;
        jmp->r_addend = 0;
        njmp = 1;
    }
    // DYNAMIC.
    auto *dyn = reinterpret_cast<elf::ELF64Dynamic *>(data_page + kDynOff);
    uint64_t di = 0;
    auto put = [&](int64_t tag, uint64_t val) {
        dyn[di].d_tag = tag;
        dyn[di].d_val = val;
        ++di;
    };
    if (soname && !is_exec) {
        put(elf::DT_SONAME, soname_off);
    }
    for (uint64_t k = 0; k < nneeds && k < 4; ++k) {
        put(elf::DT_NEEDED, needed_off[k]);
    }
    put(elf::DT_STRTAB, data_va + kDynstrOff);
    put(elf::DT_STRSZ, strsz);
    put(elf::DT_SYMTAB, data_va + kDynsymOff);
    put(elf::DT_SYMENT, sizeof(elf::ELF64Symbol));
    if (nrela > 0) {
        put(elf::DT_RELA, data_va + kRelaOff);
        put(elf::DT_RELASZ, nrela * sizeof(elf::ELF64Rela));
        put(elf::DT_RELAENT, sizeof(elf::ELF64Rela));
    }
    if (njmp > 0) {
        put(elf::DT_JMPREL, data_va + kJmprelOff);
        put(elf::DT_PLTRELSZ, njmp * sizeof(elf::ELF64Rela));
        put(elf::DT_PLTREL, elf::DT_RELA);
    }
    put(elf::DT_NULL, 0);
    (void)nsyms;
    return di;
}

/// @brief Build a synthetic ET_DYN into buf (TEXT + DATA file pages).
/// @return File size (0x3000), 0 on overflow.
uint64_t build_so(uint8_t *buf, const SoSpec &spec) {
    __builtin_memset(buf, 0, 0x3000);
    auto *hdr = reinterpret_cast<elf::ELF64Header *>(buf);
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_DYN;
    hdr->machine = 0x3E;
    hdr->version = 1;
    hdr->entry = 0;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 3;
    auto *ph = reinterpret_cast<elf::ELF64ProgramHeader *>(
        buf + sizeof(elf::ELF64Header));
    uint64_t text_va = spec.base;
    uint64_t data_va = spec.base + 0x1000;
    ph[0].type = elf::PT_LOAD;
    ph[0].flags = elf::PF_R | elf::PF_X;
    ph[0].offset = 0x1000;
    ph[0].vaddr = text_va;
    ph[0].paddr = text_va;
    ph[0].filesz = 0x1000;
    ph[0].memsz = 0x1000;
    ph[0].align = 0x1000;
    ph[1].type = elf::PT_LOAD;
    ph[1].flags = elf::PF_R | elf::PF_W;
    ph[1].offset = 0x2000;
    ph[1].vaddr = data_va;
    ph[1].paddr = data_va;
    ph[1].filesz = 0x1000;
    ph[1].memsz = 0x1000;
    ph[1].align = 0x1000;
    for (uint64_t i = 0; i < 0x1000; ++i) {
        buf[0x1000 + i] = 0x90;
    }
    uint64_t ndyn = emit_dynamic(buf + 0x2000, data_va, spec.soname,
                                 spec.needs, spec.nneeds, false,
                                 spec.has_func, spec.has_got_reloc, false,
                                 spec.base);
    if (ndyn == 0) {
        return 0;
    }
    ph[2].type = elf::PT_DYNAMIC;
    ph[2].flags = elf::PF_R | elf::PF_W;
    ph[2].offset = 0x2000 + kDynOff;
    ph[2].vaddr = data_va + kDynOff;
    ph[2].paddr = data_va + kDynOff;
    ph[2].filesz = ndyn * 16;
    ph[2].memsz = ndyn * 16;
    ph[2].align = 8;
    return 0x3000;
}

/// @brief Build a synthetic ET_EXEC into buf.
/// @return File size (0x3000), 0 on overflow.
uint64_t build_exec(uint8_t *buf, const ExecSpec &spec) {
    __builtin_memset(buf, 0, 0x3000);
    auto *hdr = reinterpret_cast<elf::ELF64Header *>(buf);
    hdr->ident[0] = 0x7F;
    hdr->ident[1] = 'E';
    hdr->ident[2] = 'L';
    hdr->ident[3] = 'F';
    hdr->ident[4] = 2;
    hdr->ident[5] = 1;
    hdr->type = elf::ET_EXEC;
    hdr->machine = 0x3E;
    hdr->version = 1;
    hdr->entry = spec.base;
    hdr->phoff = sizeof(elf::ELF64Header);
    hdr->ehsize = sizeof(elf::ELF64Header);
    hdr->phentsize = sizeof(elf::ELF64ProgramHeader);
    hdr->phnum = 3;
    auto *ph = reinterpret_cast<elf::ELF64ProgramHeader *>(
        buf + sizeof(elf::ELF64Header));
    uint64_t data_va = spec.base + 0x1000;
    ph[0].type = elf::PT_LOAD;
    ph[0].flags = elf::PF_R | elf::PF_X;
    ph[0].offset = 0x1000;
    ph[0].vaddr = spec.base;
    ph[0].paddr = spec.base;
    ph[0].filesz = 0x1000;
    ph[0].memsz = 0x1000;
    ph[0].align = 0x1000;
    ph[1].type = elf::PT_LOAD;
    ph[1].flags = elf::PF_R | elf::PF_W;
    ph[1].offset = 0x2000;
    ph[1].vaddr = data_va;
    ph[1].paddr = data_va;
    ph[1].filesz = 0x1000;
    ph[1].memsz = 0x1000;
    ph[1].align = 0x1000;
    for (uint64_t i = 0; i < 0x1000; ++i) {
        buf[0x1000 + i] = 0x90;
    }
    uint64_t ndyn = emit_dynamic(buf + 0x2000, data_va, nullptr, spec.needs,
                                 spec.nneeds, true, false, false,
                                 spec.want_jump, spec.base);
    if (ndyn == 0) {
        return 0;
    }
    ph[2].type = elf::PT_DYNAMIC;
    ph[2].flags = elf::PF_R | elf::PF_W;
    ph[2].offset = 0x2000 + kDynOff;
    ph[2].vaddr = data_va + kDynOff;
    ph[2].paddr = data_va + kDynOff;
    ph[2].filesz = ndyn * 16;
    ph[2].memsz = ndyn * 16;
    ph[2].align = 8;
    return 0x3000;
}
/// @brief Bounded substring search (fail-chain content asserts).
bool chain_contains(const char *haystack, const char *needle) {
    uint64_t hlen = 0;
    while (haystack[hlen] != '\0' && hlen < 256) {
        ++hlen;
    }
    uint64_t nlen = 0;
    while (needle[nlen] != '\0' && nlen < 64) {
        ++nlen;
    }
    if (nlen == 0 || nlen > hlen) {
        return false;
    }
    for (uint64_t i = 0; i + nlen <= hlen; ++i) {
        uint64_t j = 0;
        while (j < nlen && haystack[i + j] == needle[j]) {
            ++j;
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

/// @brief Read a PT leaf for a VA in a foreign pml4 (W^X asserts).
///        Walks PML4->PDPT->PD->PT via HHDM; huge/missing entries fail.
uint64_t read_leaf_pte(uint64_t pml4, uint64_t va, bool *ok) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *l3 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (pml4 & ~0xFFFULL));
    uint64_t i3 = (va >> 39) & 0x1FF;
    uint64_t i2 = (va >> 30) & 0x1FF;
    uint64_t i1 = (va >> 21) & 0x1FF;
    uint64_t i0 = (va >> 12) & 0x1FF;
    if ((l3[i3] & 1) == 0) {
        *ok = false;
        return 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *l2 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l3[i3] & ~0xFFFULL));
    if ((l2[i2] & 1) == 0) {
        *ok = false;
        return 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *l1 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l2[i2] & ~0xFFFULL));
    if ((l1[i1] & 1) == 0 || (l1[i1] & (1ULL << 7)) != 0) {
        *ok = false; // Missing or huge: not a PT leaf.
        return 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *l0 = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                            (l1[i1] & ~0xFFFULL));
    *ok = true;
    return l0[i0];
}

/// @brief Read a u64 from a foreign pml4 via HHDM (test observation).
uint64_t read_target_u64(uint64_t pml4, uint64_t va, bool *ok) {
    uint64_t phys = VMM::virt_to_phys_in_pml4(va, pml4);
    if (phys == 0) {
        *ok = false;
        return 0;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto *p = reinterpret_cast<uint64_t *>(arch::HHDM_OFFSET +
                                           (phys & ~0xFFFULL) +
                                           (va & 0xFFFULL));
    *ok = true;
    return *p;
}

/// @brief Map an exec image's PT_LOADs into a scratch pml4 (mini Stage A:
///        the loader maps the exec before Stage B/C run).
/// @return true on success.
bool map_exec_image(const elf::ELF64Header *hdr, const uint8_t *image,
                    uint64_t size, uint64_t pml4) {
    if (hdr->phnum > 64) {
        return false;
    }
    for (uint64_t i = 0; i < hdr->phnum; ++i) {
        const auto *p = reinterpret_cast<const elf::ELF64ProgramHeader *>(
            image + hdr->phoff + i * hdr->phentsize);
        if (p->type != elf::PT_LOAD || p->memsz == 0) {
            continue;
        }
        if (p->offset > size || p->filesz > size - p->offset) {
            return false;
        }
        bool exec = (p->flags & elf::PF_X) != 0;
        uint64_t start = p->vaddr & ~(arch::PAGE_SIZE - 1);
        uint64_t end = (p->vaddr + p->memsz + arch::PAGE_SIZE - 1) &
                       ~(arch::PAGE_SIZE - 1);
        for (uint64_t va = start; va < end; va += arch::PAGE_SIZE) {
            uint64_t phys = PMM::alloc_user_page();
            if (phys == 0) {
                return false;
            }
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            auto *dst =
                reinterpret_cast<uint8_t *>(arch::HHDM_OFFSET + phys);
            __builtin_memset(dst, 0, arch::PAGE_SIZE);
            uint64_t lo = va < p->vaddr ? p->vaddr : va;
            uint64_t body_end = p->vaddr + p->filesz;
            if (lo < va + arch::PAGE_SIZE && lo < body_end) {
                uint64_t hi = va + arch::PAGE_SIZE < body_end
                                  ? va + arch::PAGE_SIZE
                                  : body_end;
                __builtin_memcpy(dst + (lo - va),
                                 image + p->offset + (lo - p->vaddr),
                                 hi - lo);
            }
            VMM::map_page_in_pml4(va, phys, true, exec, pml4);
            if (VMM::virt_to_phys_in_pml4(va, pml4) != phys) {
                PMM::free_page(phys);
                return false;
            }
        }
    }
    return true;
}

} // namespace

#endif // CONFIG_ARCH_X86_64

// Runmode: kernel
// Testidea: Exec with one DT_NEEDED; both images mapped, GOT points at lib.
/* Pseudocode: build libfoo.so + exec needing it; run loader; assert both
   mapped and GOT entry == lib symbol VA. */
// Input: synthetic .so + exec in tmpfs /lib + /tmp.
// Expect: DONE; GOT entry resolves to the lib symbol.
// Depends: resolve_dependencies, apply_relocations, loader hook
JARVIS_TEST(elf_shared_single_lib_resolution, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_image[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kSoname[] = "/tmp/elfshared/libfoo.so";
    SoSpec lib_spec = {kSoname, {nullptr, nullptr, nullptr, nullptr}, 0,
                       kLibBase, true, true};
    uint64_t lib_size = build_so(lib_image, lib_spec);
    JARVIS_ASSERT(lib_size == 0x3000);
    ExecSpec exec_spec = {{kSoname, nullptr, nullptr, nullptr}, 1, kExecBase,
                          true};
    uint64_t exec_size = build_exec(exec_image, exec_spec);
    JARVIS_ASSERT(exec_size == 0x3000);
    vfs::mkdir("/tmp/elfshared", 0);
    JARVIS_ASSERT(write_file("/tmp/elfshared/libfoo.so", lib_image,
                             lib_size) == lib_size);
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    // Mini Stage A: the loader maps the exec before dep-resolve runs.
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, exec_size, pml4));
    elf::DepResolveContext ctx;
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    JARVIS_ASSERT(elf::resolve_dependencies(exec_hdr, exec_image, exec_size,
                                            &ctx, 0, 0x400000));
    // Both GOTs resolve to the lib symbol.
    bool ok = false;
    JARVIS_ASSERT(read_target_u64(pml4, kExecGot, &ok) == kLibFunc);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(read_target_u64(pml4, kLibGot, &ok) == kLibFunc);
    JARVIS_ASSERT(ok);
    // Teardown: drain refcounts, free buffers + scratch tables, unlink.
    for (uint64_t i = 0; i < ctx.acquired_count; ++i) {
        elf::SharedLibCache::instance().release(ctx.acquired[i]);
    }
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    kernel::test::mark_vfs_touched();
    vfs::unlink("/tmp/elfshared/libfoo.so");
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Second exec needing the same soname maps no extra lib pages.
/* Pseudocode: load execA (needs libfoo) then execB (needs libfoo);
   assert PMM free-page delta zero for lib pages; refcount drains to 0. */
// Input: two execs sharing one soname.
// Expect: shared phys pages; refcount 2 then 0 after teardown.
// Depends: SharedLibCache dedup + refcount
JARVIS_TEST(elf_shared_dedup_two_execs_same_soname,
            "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_image[0x3000];
    static uint8_t exec_a[0x3000];
    static uint8_t exec_b[0x3000];
    static const char kSoname[] = "/tmp/elfshared2/libfoo.so";
    SoSpec lib_spec = {kSoname, {nullptr, nullptr, nullptr, nullptr}, 0,
                       kLibBase, true, true};
    JARVIS_ASSERT(build_so(lib_image, lib_spec) == 0x3000);
    ExecSpec ea = {{kSoname, nullptr, nullptr, nullptr}, 1, kExecBase, true};
    JARVIS_ASSERT(build_exec(exec_a, ea) == 0x3000);
    ExecSpec eb = {{kSoname, nullptr, nullptr, nullptr}, 1, 0x410000, true};
    JARVIS_ASSERT(build_exec(exec_b, eb) == 0x3000);
    vfs::mkdir("/tmp/elfshared2", 0);
    // Whole measured section under one IrqGuard: PMM free_memory is
    // global, and the load path yields (reschedule) — without frozen
    // ticks a concurrent daemon allocation would perturb the delta.
    // Fixture files are created inside the window so their blocks
    // (allocated at write, freed at unlink) balance inside it too.
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    JARVIS_ASSERT(write_file(kSoname, lib_image, 0x3000) == 0x3000);
    uint64_t pa = VMM::clone_kernel_pml4();
    uint64_t pb = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pa != 0 && pb != 0);
    auto *ha = reinterpret_cast<elf::ELF64Header *>(exec_a);
    auto *hb = reinterpret_cast<elf::ELF64Header *>(exec_b);
    JARVIS_ASSERT(map_exec_image(ha, exec_a, 0x3000, pa));
    JARVIS_ASSERT(map_exec_image(hb, exec_b, 0x3000, pb));
    static elf::DepResolveContext cxa;
    static elf::DepResolveContext cxb;
    elf::init_resolve_context(&cxa, pa, elf::ElfLoader::generation());
    elf::init_resolve_context(&cxb, pb, elf::ElfLoader::generation());
    JARVIS_ASSERT(elf::resolve_dependencies(ha, exec_a, 0x3000, &cxa, 0,
                                            kExecBase));
    JARVIS_ASSERT(elf::resolve_dependencies(hb, exec_b, 0x3000, &cxb, 0,
                                            0x410000));
    // Both GOTs resolve; the second load shared the lib pages.
    bool ok = false;
    JARVIS_ASSERT(read_target_u64(pa, kExecGot, &ok) == kLibFunc);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT(read_target_u64(pb, 0x411000, &ok) == kLibFunc);
    JARVIS_ASSERT(ok);
    auto *slot = elf::SharedLibCache::instance().find(kSoname);
    JARVIS_ASSERT(slot != nullptr);
    JARVIS_ASSERT(slot->refcount == 2);
    // execB hit-shared the lib TEXT: unmap it from pb BEFORE any
    // teardown (pa's blind per-pml4 teardown frees the shared phys
    // exactly once; pb must not see it).  Uses the production helper.
    JARVIS_ASSERT(cxb.acquired_count == 1);
    elf::unmap_acquired_ro(pb, &cxb, 0);
    // Drain one owner: still cached for the other.
    for (uint64_t i = 0; i < cxa.acquired_count; ++i) {
        elf::SharedLibCache::instance().release(cxa.acquired[i]);
    }
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kSoname) != nullptr);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kSoname)->refcount ==
                  1);
    for (uint64_t i = 0; i < cxb.acquired_count; ++i) {
        elf::SharedLibCache::instance().release(cxb.acquired[i]);
    }
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kSoname) == nullptr);
    elf::free_resolve_images(&cxa);
    elf::free_resolve_images(&cxb);
    VMM::free_user_pages(pa);
    PMM::free_page(pa);
    VMM::free_user_pages(pb);
    PMM::free_page(pb);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kSoname);
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Missing .so fails cleanly with zero page delta.
// Input: exec needing libmissing.so (absent from /lib).
// Expect: null TCB; zero PMM delta; dmesg names the soname.
// Depends: dep-resolve NOT_FOUND path
JARVIS_TEST(elf_shared_missing_lib_fails_cleanly,
            "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t exec_image[0x3000];
    static const char kMissing[] = "/tmp/elfshared3/libmissing.so";
    ExecSpec ee = {{kMissing, nullptr, nullptr, nullptr}, 1, kExecBase,
                   false};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, 0x3000, pml4));
    static elf::DepResolveContext ctx;
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    JARVIS_ASSERT(!elf::resolve_dependencies(exec_hdr, exec_image, 0x3000,
                                             &ctx, 0, kExecBase));
    // The fail chain names the missing soname (the loader posts this
    // chain to dmesg; direct calls assert the mechanism).
    JARVIS_ASSERT(chain_contains(ctx.fail_chain, "libmissing.so"));
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kMissing) ==
                  nullptr);
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: A needs B needs A is rejected as a cycle.
// Input: liba.so needing libb.so needing liba.so.
// Expect: INVALID_ELF; refcounts 0; cache empty.
// Depends: in-progress cycle set
JARVIS_TEST(elf_shared_cycle_detected, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_a[0x3000];
    static uint8_t lib_b[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kA[] = "/tmp/elfshared4/liba.so";
    static const char kB[] = "/tmp/elfshared4/libb.so";
    SoSpec sa = {kA, {kB, nullptr, nullptr, nullptr}, 1, 0x510000, false,
                 false};
    JARVIS_ASSERT(build_so(lib_a, sa) == 0x3000);
    SoSpec sb = {kB, {kA, nullptr, nullptr, nullptr}, 1, 0x520000, false,
                 false};
    JARVIS_ASSERT(build_so(lib_b, sb) == 0x3000);
    ExecSpec ee = {{kA, nullptr, nullptr, nullptr}, 1, kExecBase, false};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    // Frozen ticks + fixtures inside the window (see dedup note).
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    vfs::mkdir("/tmp/elfshared4", 0);
    JARVIS_ASSERT(write_file(kA, lib_a, 0x3000) == 0x3000);
    JARVIS_ASSERT(write_file(kB, lib_b, 0x3000) == 0x3000);
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, 0x3000, pml4));
    static elf::DepResolveContext ctx;
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    JARVIS_ASSERT(!elf::resolve_dependencies(exec_hdr, exec_image, 0x3000,
                                             &ctx, 0, kExecBase));
    // Failed request leaves no cache residue (in-progress cleared,
    // refcounts drained by the fail path).
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kA) == nullptr);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kB) == nullptr);
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kA);
    vfs::unlink(kB);
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Dependency chains deeper than 8 are rejected.
// Input: 9-deep DT_NEEDED chain.
// Expect: rejected with DEPTH; no partial mappings leak.
// Depends: MAX_DEP_DEPTH guard
JARVIS_TEST(elf_shared_depth_cap, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t img[0x3000];
    static uint8_t exec_image[0x3000];
    static char paths[9][32];
    // Chain exec -> l1 -> ... -> l9 (9 libs deep; the 9th exceeds the cap).
    for (uint64_t i = 0; i < 9; ++i) {
        const char *prefix = "/tmp/elfshared5/l";
        uint64_t p = 0;
        while (prefix[p] != '\0') {
            paths[i][p] = prefix[p];
            ++p;
        }
        paths[i][p++] = static_cast<char>('1' + i);
        paths[i][p++] = '.';
        paths[i][p++] = 's';
        paths[i][p++] = 'o';
        paths[i][p] = '\0';
    }
    vfs::mkdir("/tmp/elfshared5", 0);
    // Frozen ticks first; fixtures are created inside the window so
    // their blocks (allocated at write, freed at unlink) balance too.
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    for (uint64_t i = 0; i < 9; ++i) {
        const char *next = (i + 1 < 9) ? paths[i + 1] : nullptr;
        const char *needs[4] = {nullptr, nullptr, nullptr, nullptr};
        uint64_t nneeds = 0;
        if (next) {
            needs[0] = next;
            nneeds = 1;
        }
        SoSpec spec = {paths[i], {needs[0], nullptr, nullptr, nullptr},
                       nneeds, 0x510000 + i * 0x10000, false, false};
        JARVIS_ASSERT(build_so(img, spec) == 0x3000);
        JARVIS_ASSERT(write_file(paths[i], img, 0x3000) == 0x3000);
    }
    ExecSpec ee = {{paths[0], nullptr, nullptr, nullptr}, 1, kExecBase,
                   false};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, 0x3000, pml4));
    static elf::DepResolveContext ctx;
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    JARVIS_ASSERT(!elf::resolve_dependencies(exec_hdr, exec_image, 0x3000,
                                             &ctx, 0, kExecBase));
    for (uint64_t i = 0; i < 9; ++i) {
        JARVIS_ASSERT(elf::SharedLibCache::instance().find(paths[i]) ==
                      nullptr);
    }
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    kernel::test::mark_vfs_touched();
    for (uint64_t i = 0; i < 9; ++i) {
        vfs::unlink(paths[i]);
    }
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Cancel mid Stage-B releases partial acquisitions.
// Input: slow multi-lib load + request_cancel during LOADING_DEPS.
// Expect: loader IDLE; partial libs refcounted out; refcounts 0.
// Depends: cancel path + release discipline
JARVIS_TEST(elf_shared_cancel_during_deps, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_a[0x3000];
    static uint8_t lib_b[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kA[] = "/tmp/elfshared6/liba.so";
    static const char kB[] = "/tmp/elfshared6/libb.so";
    static const char kApp[] = "/tmp/elfshared6/app.elf";
    SoSpec sa = {kA, {nullptr, nullptr, nullptr, nullptr}, 0, 0x510000,
                 true, true};
    JARVIS_ASSERT(build_so(lib_a, sa) == 0x3000);
    SoSpec sb = {kB, {nullptr, nullptr, nullptr, nullptr}, 0, 0x520000,
                 true, true};
    JARVIS_ASSERT(build_so(lib_b, sb) == 0x3000);
    ExecSpec ee = {{kA, kB, nullptr, nullptr}, 2, kExecBase, true};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    vfs::mkdir("/tmp/elfshared6", 0);
    JARVIS_ASSERT(write_file(kA, lib_a, 0x3000) == 0x3000);
    JARVIS_ASSERT(write_file(kB, lib_b, 0x3000) == 0x3000);
    JARVIS_ASSERT(write_file(kApp, exec_image, 0x3000) == 0x3000);
    elf::ElfLoader::reset();
    JARVIS_ASSERT(elf::ElfLoader::request_load(kApp) ==
                  elf::LoadResult::OK);
    // Yield so the loader progresses (cancel may land in Stage A, B,
    // or C — the invariant below holds for every phase).
    for (int i = 0; i < 3; ++i) {
        Scheduler::reschedule();
    }
    JARVIS_ASSERT(elf::ElfLoader::request_cancel() ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    // Invariant on every cancel path: IDLE, no completed TCB, no
    // retained refcounts (partial acquisitions released).
    JARVIS_ASSERT(elf::ElfLoader::state() == elf::LoadState::IDLE);
    JARVIS_ASSERT(elf::ElfLoader::take_completed() == nullptr);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kA) == nullptr);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kB) == nullptr);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kA);
    vfs::unlink(kB);
    vfs::unlink(kApp);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Post-DONE page dump shows RX/RO (W^X restored).
// Input: successful shared load.
// Expect: text RX, data RO/NX via PTE walk.
// Depends: Stage-C re-protect
JARVIS_TEST(elf_shared_wx_restored_after_reloc,
            "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_image[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kSoname[] = "/tmp/elfshared7/libfoo.so";
    SoSpec lib_spec = {kSoname, {nullptr, nullptr, nullptr, nullptr}, 0,
                       kLibBase, true, true};
    JARVIS_ASSERT(build_so(lib_image, lib_spec) == 0x3000);
    ExecSpec ee = {{kSoname, nullptr, nullptr, nullptr}, 1, kExecBase, true};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    vfs::mkdir("/tmp/elfshared7", 0);
    JARVIS_ASSERT(write_file(kSoname, lib_image, 0x3000) == 0x3000);
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, 0x3000, pml4));
    static elf::DepResolveContext ctx;
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    JARVIS_ASSERT(elf::resolve_dependencies(exec_hdr, exec_image, 0x3000,
                                            &ctx, 0, kExecBase));
    // Post-reloc PTE dump: text RX (P+X, NX clear), data RW+NX.
    constexpr uint64_t kNx = 1ULL << 63;
    constexpr uint64_t kW = 1ULL << 1;
    bool ok = false;
    uint64_t text_pte = read_leaf_pte(pml4, kLibBase, &ok);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT((text_pte & 1) != 0);
    JARVIS_ASSERT((text_pte & kNx) == 0);
    uint64_t data_pte = read_leaf_pte(pml4, kLibBase + 0x1000, &ok);
    JARVIS_ASSERT(ok);
    JARVIS_ASSERT((data_pte & 1) != 0);
    JARVIS_ASSERT((data_pte & kW) != 0);
    JARVIS_ASSERT((data_pte & kNx) != 0);
    for (uint64_t i = 0; i < ctx.acquired_count; ++i) {
        elf::SharedLibCache::instance().release(ctx.acquired[i]);
    }
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kSoname);
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Overlapping preferred bases are rejected with LAYOUT.
// Input: lib whose fixed vaddr overlaps the exec segments.
// Expect: LAYOUT failure; null TCB; nothing mapped.
// Depends: fixed-address overlap check
JARVIS_TEST(elf_shared_layout_conflict, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_image[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kSoname[] = "/tmp/elfshared8/libevil.so";
    // Lib linked at the exec's own base: fixed-address overlap.
    SoSpec lib_spec = {kSoname, {nullptr, nullptr, nullptr, nullptr}, 0,
                       kExecBase, false, false};
    JARVIS_ASSERT(build_so(lib_image, lib_spec) == 0x3000);
    ExecSpec ee = {{kSoname, nullptr, nullptr, nullptr}, 1, kExecBase, false};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    arch::IrqGuard pm_quiet{};
    uint64_t free_before = PMM::free_memory();
    vfs::mkdir("/tmp/elfshared8", 0);
    JARVIS_ASSERT(write_file(kSoname, lib_image, 0x3000) == 0x3000);
    uint64_t pml4 = VMM::clone_kernel_pml4();
    JARVIS_ASSERT(pml4 != 0);
    auto *exec_hdr = reinterpret_cast<elf::ELF64Header *>(exec_image);
    JARVIS_ASSERT(map_exec_image(exec_hdr, exec_image, 0x3000, pml4));
    static elf::DepResolveContext ctx;
    // Exec range registered for overlap checks (the loader fills these
    // from its Stage-A range; direct tests set them explicitly).
    elf::init_resolve_context(&ctx, pml4, elf::ElfLoader::generation());
    ctx.exec_base = kExecBase;
    ctx.exec_size = 0x2000;
    JARVIS_ASSERT(!elf::resolve_dependencies(exec_hdr, exec_image, 0x3000,
                                             &ctx, 0, kExecBase));
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kSoname) ==
                  nullptr);
    elf::free_resolve_images(&ctx);
    VMM::free_user_pages(pml4);
    PMM::free_page(pml4);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kSoname);
    JARVIS_ASSERT(PMM::free_memory() == free_before);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

// Runmode: kernel
// Testidea: Loader-level dynamic load succeeds end to end: TCB handoff
//           records both sonames; both GOTs resolve in the task pml4;
//           destroy_completed_tcb drains refcounts (release_task_libs).
// Input: app needing liba+libb via request_load; take + destroy.
// Expect: DONE TCB with needed_lib_count 2; GOTs correct; cache empty
//         after destroy; zero tracker delta.
// Depends: loader hook, TCB handoff, destroy drain
JARVIS_TEST(elf_shared_loader_success_handoff, "PRE: isolate | POST: none") {
#if defined(CONFIG_ARCH_X86_64)
    static uint8_t lib_a[0x3000];
    static uint8_t lib_b[0x3000];
    static uint8_t exec_image[0x3000];
    static const char kA[] = "/tmp/elfshared9/liba.so";
    static const char kB[] = "/tmp/elfshared9/libb.so";
    static const char kApp[] = "/tmp/elfshared9/app.elf";
    SoSpec sa = {kA, {nullptr, nullptr, nullptr, nullptr}, 0, 0x510000,
                 true, true};
    JARVIS_ASSERT(build_so(lib_a, sa) == 0x3000);
    SoSpec sb = {kB, {nullptr, nullptr, nullptr, nullptr}, 0, 0x520000,
                 true, true};
    JARVIS_ASSERT(build_so(lib_b, sb) == 0x3000);
    ExecSpec ee = {{kA, kB, nullptr, nullptr}, 2, kExecBase, true};
    JARVIS_ASSERT(build_exec(exec_image, ee) == 0x3000);
    vfs::mkdir("/tmp/elfshared9", 0);
    JARVIS_ASSERT(write_file(kA, lib_a, 0x3000) == 0x3000);
    JARVIS_ASSERT(write_file(kB, lib_b, 0x3000) == 0x3000);
    JARVIS_ASSERT(write_file(kApp, exec_image, 0x3000) == 0x3000);
    elf::ElfLoader::reset();
    JARVIS_ASSERT(elf::ElfLoader::request_load(kApp) ==
                  elf::LoadResult::OK);
    elf::ElfLoader::wait_loader_idle();
    auto *t = elf::ElfLoader::take_completed();
    JARVIS_ASSERT(t != nullptr);
    JARVIS_ASSERT(t->page_table_ != 0);
    JARVIS_ASSERT(t->needed_lib_count == 2);
    {
        bool has_a = false;
        bool has_b = false;
        for (uint64_t i = 0; i < t->needed_lib_count; ++i) {
            if (strcmp(t->needed_libs[i], kA) == 0) {
                has_a = true;
            }
            if (strcmp(t->needed_libs[i], kB) == 0) {
                has_b = true;
            }
        }
        JARVIS_ASSERT(has_a && has_b);
    }
    bool ok = false;
    JARVIS_ASSERT(read_target_u64(t->page_table_, kExecGot, &ok) ==
                  0x510100);
    JARVIS_ASSERT(ok);
    elf::ElfLoader::destroy_completed_tcb(t);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kA) == nullptr);
    JARVIS_ASSERT(elf::SharedLibCache::instance().find(kB) == nullptr);
    kernel::test::mark_vfs_touched();
    vfs::unlink(kA);
    vfs::unlink(kB);
    vfs::unlink(kApp);
#else
    Logger::warn("elf_shared test skipped (not x86_64)");
#endif
    JARVIS_TEST_PASS();
}

void register_elf_shared_tests() {
    Logger::info("Registering ELF shared-object tests");
    JARVIS_REGISTER_TEST(elf_shared_single_lib_resolution);
    JARVIS_REGISTER_TEST(elf_shared_dedup_two_execs_same_soname);
    JARVIS_REGISTER_TEST(elf_shared_missing_lib_fails_cleanly);
    JARVIS_REGISTER_TEST(elf_shared_cycle_detected);
    JARVIS_REGISTER_TEST(elf_shared_depth_cap);
    JARVIS_REGISTER_TEST(elf_shared_cancel_during_deps);
    JARVIS_REGISTER_TEST(elf_shared_wx_restored_after_reloc);
    JARVIS_REGISTER_TEST(elf_shared_layout_conflict);
    JARVIS_REGISTER_TEST(elf_shared_loader_success_handoff);
}
