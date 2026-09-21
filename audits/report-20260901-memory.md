# AUDIT REPORT 2026-09-01T06-34-48Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/memory/vmm.cpp, src/kernel/arch/aarch64/test_aarch64.cpp, src/kernel/test/test_expected_counts.hpp, userspace/de-probe.c, userspace/ud-probe.c, test-history.txt

## FINDINGS

### 1. Fix correctness — three arch-guarded table-descriptor sites (vmm.cpp:893-899, 926-937, 964-971)
Verified against `vmm.hpp` constants: on `CONFIG_ARCH_AARCH64`, `PAGE_PRESENT=1<<0`,
`PAGE_TABLE=1<<1`, `PAGE_WRITE=0`, `PAGE_USER=1<<6` (vmm.hpp:227-245). Pre-patch the
L0/L1/L2 descriptors were `phys | PRESENT | WRITE(0) | USER(1<<6)` → bits[1:0]=01 with
bit6 (AP[0]) set. Per ARMv8-A, table descriptors at L0-L2 require bits[1:0]=0b11 and
bits[11:2] are RES0 — the post-patch `phys | PAGE_PRESENT | PAGE_TABLE` (bits[1:0]=11,
nothing else) is the exact, minimal valid table-descriptor encoding. All three `#else`
arms are byte-identical to the pre-patch x86_64 expression (diff confirmed) — no x86
descriptor change. PASS.

### 2. Scope containment — leaf copy and huge-page paths untouched
`dst_pt[pt_idx] = dst_data | flags` (vmm.cpp:989) and the two verbatim huge-page copies
(vmm.cpp:915, 953) fall outside all three hunks and are unchanged in the working tree.
The aarch64 L3 leaves already carry AF(bit10)/AttrIndx(bits[4:2])/AP from
`map_page_in_pml4` (vmm.cpp:608-617) and are inherited verbatim via the `~PAGE_FRAME_MASK`
flag split. Huge-page/leaf discrimination `(x & (PRESENT|TABLE)) == PRESENT` reads
correctly against the new 0b11 table descriptors (they are no longer misclassified as
block/huge entries). PASS.

### 3. Regression test is a valid positive gate (test_aarch64.cpp:651-733)
- L0/L1/L2 asserts `(desc & 0x3) == 0x3` fail on pre-fix code (bits[1:0]=01 pre-fix),
  pass on post-fix (0b11). `c4[pml4_idx] & (1<<1)` similarly. Valid gate.
- Leaf asserts: bits[1:0]=11 (PAGE_TABLE inherited), AF bit10 present, 48-bit frame mask
  `0x0000FFFFFFFFF000` used for leaf extraction — correct, avoids leaking UXN(54)/PXN(53)
  into the frame value (the documented mid-test abort/`LEAK: PMM +10` cause). PASS.
- `child_leaf == VMM::virt_to_phys_in_pml4(TEST_VA, child_pml4)` exercises a real child
  MMU walk end-to-end; post-fix it succeeds because every intermediate descriptor is a
  valid 0b11 table. `map_page_in_pml4`/`get_table` (aarch64) already build valid parent
  tables (vmm.cpp:206), so the parent side is not part of the gate. PASS.

### 4. Test teardown symmetry — no leak, no double-free
Parent: leaf (`alloc_user_page`) + PDPT/PD/PT (all `get_table(..., user_alloc=true)` →
`alloc_user_page`, vmm.cpp:194) are USER-owned; `free_user_pages(parent_pml4)` frees all
four, `free_page(parent_pml4)` frees the kernel pml4. Child: deep-copy allocates
PDPT/PD/PT/leaf via `alloc_user_page` (vmm.cpp:887/920/958/982) → `free_user_pages(child_pml4)`
+ `free_page(child_pml4)`. Symmetric, each page freed exactly once; the leaf `user_page`
is intentionally NOT freed separately (unlike the pre-existing cross_arch test pattern),
so no double-free. `free_user_pages` clears walked entries to prevent re-walk. PASS.

### 5. Userspace probe guard (de-probe.c:10-16, ud-probe.c:11-13)
`#if defined(__x86_64__)` wraps only the x86-specific inline asm (`ud2`, `divq %rcx`).
`__x86_64__` is a GCC predefined macro for x86_64 targets and is not defined for
aarch64/riscv64 cross compilers, so the cross-arch userspace build (userspace/*.c →
`userspace/%.c.elf`, mk/rules.mk:179) now compiles with an inert `main(){_exit(0);}`.
x86_64 behavior is unchanged — the asm still fires. The only loaders of these probes,
test_exc_table.cpp, are `#if defined(CONFIG_ARCH_X86_64)` guarded (lines 1/235), so
inert probes are never loaded on other arches. Evidence: exc_table 3/3, arch_aarch64
23/23 in test-history. PASS.

### 6. No dynamic allocation / concurrency / lock interaction
Patch changes descriptor bit patterns only; all allocations pre-exist (`alloc_user_page`
for tables/leaves). No IrqGuard, spinlock, or scheduling-boundary change; the function
runs in fork-syscall context (single-core, no locks). The x86_64 page-table build is
byte-identical, so no x86 critical-section interference. PASS.

### 7. Expected counts and history
`arch_aarch64` registration 22→23 (test_aarch64.cpp:761) matches the
`test_expected_counts.hpp` row `0,23`. The `all` (x86-only) counts are unaffected — the
aarch64 column for `all` is 0 and the new test lives under the `arch_aarch64` class only.
test-history.txt rows follow the mandated `<ts> <class> PASSED: <n> FAILED: <n> TIME: <t>`
schema. PASS.

### 8. Preprocessor asymmetry check
Outer guard `#if X86_64 || AARCH64` (vmm.cpp:873) keeps the `#else` arms reachable only
on x86_64; the RISC-V branch is a separate `#elif` implementation and is unaffected.
All three new `#if/#else/#endif` blocks are balanced and syntactically correct
(confirmed in working tree). No missed implementations, no uninitialized variables.

## PATCH
(not applicable — APPROVED)

DECISION: APPROVED