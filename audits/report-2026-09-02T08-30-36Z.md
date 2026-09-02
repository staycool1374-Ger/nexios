# SIL 3 Audit Report — Issue #10 (MSI-X Vector Infrastructure) — RE-AUDIT (iter-3)

- **Auditor:** SIL 3 auditor subagent (read-only; no files modified, no tests run)
- **Date:** 2026-09-02T08:30:36Z
- **Patch reviewed:** `audits/pending_patch.diff` (cumulative, against `main`, 21 files). Iter-1 and iter-2 APPROVED (`audits/report-2026-09-02T07-22-14Z.md` [REJECTED→fixed], `audits/report-2026-09-02T07-28-38Z.md` [APPROVED]).
- **Scope of this re-audit:** audit the NEW fix added since iter-2 — `PMM::reserve_range()` (`src/kernel/memory/pmm.{hpp,cpp}`) and the boot-time reservation of GRUB's Multiboot2 info structure (`src/kernel/kernel.cpp`), added to fix a release-build framebuffer regression (`shell_tasks_memory_columns`). Re-check prior content for regressions.
- **Rules applied:** `prompts/CODING_STYLE.md` §5 (fail closed), §6 (fully bounded loops / no primitive reinterpret_cast to a physical MMIO address — §4), §7 (test isolation / ResourceTracker). Issue #10 discipline; PMM bitmap/owner/free_pages_ invariants.

---

## 1. Correctness of `PMM::reserve_range()` (`src/kernel/memory/pmm.cpp:537-554`)

Read directly in the worktree (not just the diff). The implementation:

```cpp
void PMM::reserve_range(uint64_t start_phys, uint64_t end_phys) {
    if (start_phys >= end_phys) return;
    sync::IrqSpinLockGuard lock(pmm_lock_);
    uint64_t start_idx = start_phys / PAGE_SIZE;
    uint64_t end_idx = (end_phys + PAGE_SIZE - 1) / PAGE_SIZE;
    if (start_idx < window_base_page_) start_idx = window_base_page_;
    if (end_idx > window_end_page_)    end_idx = window_end_page_;
    for (uint64_t idx = start_idx; idx < end_idx; ++idx) {
        if (bitmap_test(idx)) continue;      // already allocated -> skip
        bitmap_set(idx);
        owner_set_kernel(idx);
        --free_pages_;
    }
}
```

- **Bitmap index math / OOB:** `start_idx`/`end_idx` are page indices derived from physical addresses and clamped to `[window_base_page_, window_end_page_)`. `window_end_page_ <= total_pages_` by construction (`compute_window_pages`, pmm.hpp:163-175), so every `idx` iterated satisfies `idx < total_pages_`; the `ENSURE(index < total_pages_)` in `bitmap_set/bitmap_test/owner_set_kernel` (pmm.cpp:559,578,597) can never trip. **No OOB.**
- **`free_pages_` decrement consistency:** `--free_pages_` executes only when `bitmap_test(idx)` is false, i.e. exactly once per page that transitions free→allocated. Already-allocated pages are skipped — **no double decrement**. This mirrors the accounting in `try_alloc_kernel` (pmm.cpp:194-196). `free_pages_` remains equal to the count of clear bitmap bits within the window.
- **Window bounds / empty range:** `start_phys >= end_phys` early-returns; if both endpoints clamp such that `start_idx >= end_idx` the loop body never runs. **No underflow, no spurious reservations.**
- **`end_phys + PAGE_SIZE - 1` overflow:** only conceivable if `end_phys` approached `UINT64_MAX`; here `end_phys = info_ptr + total_size` with a small `uint32_t total_size` at a real low physical address. **Non-issue.**
- **Idempotence/overlap with the kernel image / PMM bitmaps:** if the info range overlaps a page PMM::init already reserved, `bitmap_test` returns true and it is skipped — no double count. Safe in every layout.

**Verdict:** correct, bounded, leak-free, and consistent with the allocator's own accounting.

## 2. Boot ordering (`src/kernel/kernel.cpp:694-705`)

```cpp
kernel::PMM::init(mem_size, arch::PAGE_SIZE_2M, kend, ram_base);   // 691
kernel::VMM::init();                                               // 692
if (kernel::gs::get_multiboot_magic() == 0x36D76289) {            // 699
    uint64_t info_ptr = kernel::gs::get_multiboot_info_ptr();
    if (info_ptr != 0) {
        auto *info = reinterpret_cast<Multiboot2Info *>(info_ptr);
        kernel::PMM::reserve_range(info_ptr, info_ptr + info->total_size);
    }
}
```

- **Placed after PMM::init and before any allocation:** `VMM::init()` allocates **nothing** — it only zeroes residual boot-constructed page-table entries (`src/kernel/memory/vmm.cpp:40-116`, verified — no `PMM::alloc_*` in `VMM::init()`). The first real allocation is `MemPool::init()` (kernel.cpp:755) and later `Framebuffer::init()` (kernel.cpp:809). The reservation therefore precedes every allocation that could have landed on the multiboot info page. **Correct ordering.**
- **`info->total_size` read is safe:** the same physical pointer is already dereferenced identically at kernel.cpp:604 (`mb2_find_tag(6)`) and `mb2_find_tag` (multiboot2.hpp:84-88) reads `info->total_size` directly — this code has run successfully before `PMM::init`, proving the info is valid and identity-mapped at this boot point. The reservation re-reads the same valid structure. **Bounds-safe and valid.**
- **`Multiboot2Info` deref:** only the 4-byte `total_size` first field is read; the whole structure is within the reserved range. Safe.

## 3. Is reserving the multiboot info the RIGHT fix (vs masking a deeper issue)?

Yes. GRUB's Multiboot2 info is a bootloader-placed boot-time structure in conventional memory that the PMM bitmap never knew about. The kernel's PMM:init already reserves the kernel image and the PMM bitmap/freelist storage (pmm.cpp:98-108); the multiboot info is the analogous missing reservation. The root cause is a genuine boot-time reservation gap exposed when issue #10's larger kernel shifted GRUB's info placement into the PMM free window — **not** an allocator defect. Reserving the range is the correct, appropriately-scoped fix (no over-broad reservation of unrelated GRUB regions is needed: the tags are consumed during `memory_init` and only the info structure persists as live boot data). **Not masking a deeper issue.**

## 4. aarch64 / riscv64 gating

The guard `get_multiboot_magic() == 0x36D76289` is a no-op on aarch64/riscv64: `multiboot_magic` is a `constinit uint64_t = 0` (global_state.cpp:39) and is only set to the Multiboot2 magic inside the `#if defined(CONFIG_ARCH_X86_64)` branch of `higherhalf_entry` (kernel.cpp:494, via `try_set_multiboot`). aarch64/riscv64 set `dtb_ptr`/`hart_id` instead (kernel.cpp:499-509) and never set the magic. **Correctly x86_64-gated; no cross-arch effect.**

## 5. ResourceTracker / test-isolation interaction

- `reserve_range` does **not** call `ResourceTracker::track_pmm_alloc()` (verified pmm.cpp:537-554 — no `ResourceTracker` call), so the `pmm_pages_used` counter is untouched. No ResourceTracker delta.
- The PMM bitmap + owner bitmap + `free_pages_` are captured into the test-isolation baseline at `snapshot_create()` (test_isolate.cpp:349-354), which runs at **test time — after boot**. The multiboot info pages are reserved at boot (before any snapshot), so they are part of the baseline and never allocated/freed during tests. `PMM::rebuild_free_list()` (pmm.cpp:161-177) re-derives `free_head_` from the bitmap, so the reserved bits are simply absent from the free list. **No PMM delta; the permanent boot reservation is correctly folded into the baseline.**

## 6. Regression — does this fix the `shell_tasks_memory_columns` release regression correctly?

The root cause chain: the larger issue-#10 kernel shifted GRUB's info to 0x7CD000, above the PMM reserved end; an early pre-`Framebuffer::init` allocation overwrote `total_size` (read back as 21), so `mb2_find_tag(8)` found no framebuffer tag → `Framebuffer::available()` false → `Terminal::instance_` null → the release-only `shell_tasks_memory_columns` capture buffer stayed empty. The fix reserves the whole info range (including the page holding `total_size` and the framebuffer tag) so no allocator can ever touch it. With `total_size` intact, `mb2_find_tag(8)` finds the tag, `Framebuffer::available()` is true, `Terminal::instance_` is populated, and the capture buffer fills. **The logic is correct and addresses the true root cause** (verified statically; no test run per auditor constraints). The reservation is a no-op on non-multiboot (default-magic 0) boots, so it cannot perturb non-release or non-GRUB paths.

## Prior approved content (iter-1/iter-2) — regression re-check

The MSI-X core (msix.cpp, irq_delivery kind-gating, kernel-reserved vectors 64/0xFF, dual-lookup syscalls, test_isolate `MsixCap::snapshot_reset`, expected-count updates) is unchanged in substance by this fix. The only PMM-touching addition is `reserve_range`, which operates purely at boot and on boot-time data — it does not interact with the MSI-X vector/cap/slot machinery. No regression.

---

## FINDINGS

- [S3] src/kernel/kernel.cpp:702 — CODING_STYLE §6 — `reinterpret_cast<Multiboot2Info *>(info_ptr)` uses a primitive reinterpret_cast to a physical address; however it exactly mirrors the pre-existing, pre-approved boot-time pattern in `mb2_find_tag` (multiboot2.hpp:84) reading a RAM structure (not the §4 MMIO-physical-write case) and is consistent with the codebase, so it is a non-blocking observation, not a defect.
- *(none)* — [S1/S2/S3] `reserve_range` correctness — **clean** — bitmap index math is window-clamped and inside `total_pages_` (no OOB), `free_pages_` decremented exactly once per newly-reserved page, no double decrement on already-allocated pages, idempotent and consistent with `try_alloc_kernel`.
- *(none)* — [S1/S2/S3] boot ordering — **clean** — reservation is after `PMM::init`/`VMM::init` (neither allocates) and before the first allocation and `Framebuffer::init`; `info->total_size` is proven valid/mapped by the earlier `mb2_find_tag(6)` read.
- *(none)* — [S1/S2/S3] right-fix vs masking — **clean** — GRUB's info is a genuine boot-time reservation gap (analogous to the kernel image/PMM-bitmap reservations), not an allocator defect; fix is correctly scoped.
- *(none)* — [S1/S2/S3] arch gating — **clean** — the `0x36D76289` magic guard is a no-op on aarch64/riscv64 (`multiboot_magic` stays 0); x86_64 only.
- *(none)* — [S1/S2/S3] ResourceTracker / test isolation — **clean** — `reserve_range` never touches `track_pmm_alloc`, and the reservation happens at boot before the PMM snapshot baseline, so the reserved pages are part of the baseline; no PMM delta.
- *(none)* — [S1/S2/S3] `shell_tasks_memory_columns` regression — **clean** — protecting the info range keeps `total_size`/framebuffer tag intact so `Framebuffer::available()`/`Terminal::instance_` resolve correctly; no-op on non-multiboot boots.

---

DECISION: APPROVED
