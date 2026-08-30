# SIL 3 Audit Report — Iteration 3 (Issue #100 aarch64 boot-path fixes)

- **Date (UTC):** 2026-08-30T21-00-08Z
- **Auditor:** Independent SIL 3 auditor (static analysis only; no tests executed)
- **Target:** cumulative `git diff HEAD~1 -- ':!audits'` (commit `adde2324` + uncommitted iteration-1/2 corrective patches; worktree = audit target, verified via `git status` / worktree reads)
- **History:** iter 1 REJECTED (report-2026-08-30T20-03-11Z.md) → iter 2 REJECTED (report-2026-08-30T20-40-06Z.md, third split site) → this iteration.
- **Facts on record:** x86_64 debug `all` 978/978 post-fix; memory_pmm 8/8; aarch64 boots to init's daemon wait, 0 user faults.

---

## FOCUS (a): Iteration-2 S1 fix — COMPLETE ENUMERATION of aarch64 descriptor-emission sites in vmm.cpp

Every site that synthesizes an aarch64 stage-1 descriptor was enumerated (grep `PAGE_AF|PAGE_ATTR_NORMAL|PAGE_TABLE|PAGE_PRESENT` across `src/`, plus targeted reads of every hit). **All leaf sites now emit bits[1:0]=11 (PRESENT|TABLE) + AttrIndx=1 (PAGE_ATTR_NORMAL=1<<2) + AF (PAGE_AF=1<<10):**

| # | Site | Location (HEAD worktree) | Encoding | Verdict |
|---|------|--------------------------|----------|---------|
| 1 | `get_table` 2MiB split → 512 L3 leaves | vmm.cpp:151-160 | `huge_base + i*0x1000 \| base_flags \| PAGE_TABLE \| PAGE_ATTR_NORMAL`; `base_flags` mask carries `PAGE_PRESENT\|PAGE_AF\|PAGE_UXN\|PAGE_PXN\|PAGE_ATTR_NORMAL` | ✔ bits[1:0]=11, AttrIndx=1, AF inherited (boot.S blocks set AF=bit10) |
| 2 | `get_table` fresh L0/L1/L2 table desc | vmm.cpp:205-206 | `new_page \| PAGE_PRESENT \| PAGE_TABLE` | ✔ table desc bits[1:0]=11; AF correctly absent (RES0 in table descriptors) |
| 3 | `map_page` 2MiB split → 512 L3 leaves | vmm.cpp:320-335 | same as #1 | ✔ |
| 4 | `map_page` table desc after split | vmm.cpp:340-343 | `new_pt_phys \| PAGE_PRESENT \| PAGE_TABLE` + AF-RES0 comment | ✔ |
| 5 | `map_page` direct leaf | vmm.cpp:357-366 (write :373) | `PAGE_PRESENT \| PAGE_TABLE \| PAGE_AF \| PAGE_ATTR_NORMAL` (+AP/PXN) | ✔ |
| 6 | `map_page_in_pml4` 2MiB split | vmm.cpp:579-589 | same as #1; table desc :589 `PAGE_PRESENT \| PAGE_TABLE` | ✔ |
| 7 | `map_page_in_pml4` direct leaf | vmm.cpp:608-617 (write :626) | full quad | ✔ |

**No other aarch64 leaf-emission path exists:**
- vmm.cpp:223-269 and 530-548 are `#if defined(CONFIG_ARCH_RISCV64)` branches; vmm.cpp:368/619 are x86 branches.
- `buffer_pool.cpp` (own walk, L75-131) only **clears** descriptors (`= 0`) and maps via `VMM::map_page_in_pml4` (L344, L406) — covered by #6/#7.
- `deep_copy_user_pages` (fork) **copies leaves verbatim** (`flags = src_pt[pt_idx] & ~PAGE_FRAME_MASK`, dst = data|flags, vmm.cpp:958-968) — leaf encodings preserved; see S3-1 for its intermediate table descriptors.
- boot.S writes only L1/L2 **block** descriptors (bits[1:0]=01, valid at block level) — no L3 leaves.
- vmm.hpp:227-246: `PAGE_AF=1<<10` ✔, `PAGE_ATTR_NORMAL=1<<2` ✔ (AttrIndx=1; boot.S:197 MAIR `0xFF00` → Attr0=Device, Attr1=Normal-WB ✔), `PAGE_TABLE=1<<1` ✔.

**⇒ Iteration-2 S1 is fully remediated at all three split sites and both leaf-flag sites.**

## FOCUS (b): No regression vs iteration-2 verified-good list

- **vectors.S stash-carry** (iter-1 S1): worktree el0_sync/el0_irq stash user x0/x1 (`stp x0,x1,[sp,#-16]!`) *before* the `adrp` clobber; stash carried via x1, re-stashed on the task kstack, popped on both the fault and SVC exits ✔.
- **syscall_entry.S slot-0 ret** (iter-1 S2): `str x0,[sp,#272]` then `str x0,[sp,#0]` **before** the `scheduler_save_rsp_to` arm check; `.sys_no_switch` path reads +272 ✔.
- **want_end math / live-state test** (iter-1 S2): `pmm_window_live_state` computes `want_end = base + min(window_pages, total-base)` — algebraically identical to `compute_window_pages`' clamp ✔; x86 (base=0) window `[0, min(32768,total))` byte-identical to pre-window behavior ✔; multi-page loops now `idx + count <= window_end_page_` — eliminates the old unsigned-wrap `i <= limit - count` hazard ✔.
- **tlbi ordering**: `dsb sy; tlbi vmalle1; dsb sy; isb` after TTBR0 write ✔.
- **Dispatch guard**: `f_rflags` now scoped inside `#if CONFIG_ARCH_X86_64` (scheduler.cpp:2294-2314); aarch64 branch validates save-area ELR (+256)/SPSR (+264), mode ∈ {EL0t, EL1h} — layout matches vectors.S save area and task.cpp/elf.cpp initial frames (pad,pad,SPSR,ELR,SP_EL0 above 31 GPRs = 248B) ✔.
- **Per-arch test counts**: `memory_pmm` 5→8 registered *and* expected (test_expected_counts.hpp) ✔; new x86-only guards (`tss_iopb_layout_valid`, `irq_delivery_ack_pic_state`, `validate_iommu_pair` — call sites iommu.cpp:87-126/131-166 guarded, `restore_line_mask` void-cast) are symmetric; aarch64 expected counts for cap_irq/cap_mmio are 0 ⇒ `validate_class_count` short-circuits (expected==0), no false mismatch ✔.
- **GIC/timer/SPSR**: IGROUPR=0 Group-0-only + enable-only CTLR + `intid>=1020` spurious/special guard ✔; timer re-arms CNTP_TVAL from stored `tick_interval_` with 0-guard ✔; SPSR 0x10→0x0 at all three initial-frame sites (bit4 = AArch32-on-eret) ✔.
- **Concurrency**: no dynamic allocation in syscall/IRQ/fault critical paths; `IrqGuard` RAII in the fault handler; `fault_reported[]` single-writer (el0_sync task context, DAIF masked on exception entry) ✔. No assertion masking introduced; no uninitialized vars.

## FOCUS (c): Standing S3s

- **H2 epilogue limitation** — vectors.S NOTE (~L329-332) documents the missing deferred-switch generation re-check / isr-nesting depth guard / H2 liveness re-validation on `irq_context_switch_common`. Still documented, still acceptable for the aarch64 port stage. ✔
- **Fault-handler walk checks** — scheduler.cpp `aarch64_el0_fault_handler` gates every level deref on the valid bit (`l0e&1` → `l1e&1` → `l2e&1`), walks via HHDM only, masks `page_table_ & ~0xFFF`. ✔

## FINDINGS

### S3-1 (pre-existing, out-of-patch-scope, latent): `deep_copy_user_pages` emits invalid aarch64 table descriptors
- **Where:** src/kernel/memory/vmm.cpp:893-894, 921-922, 949-950 (not touched by this patch).
- **Why:** The function is compiled for x86_64 **and** aarch64 (`#if defined(CONFIG_ARCH_X86_64) || defined(CONFIG_ARCH_AARCH64)`, vmm.cpp:873) but emits intermediate descriptors as `phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER`. On aarch64 `PAGE_WRITE=0` and `PAGE_USER=1<<6`, so bits[1:0]=**01** — a reserved/invalid encoding at L0/L1/L2 (only 0b11 is a valid table descriptor). Every walk in a forked address space would translation-fault.
- **Scope ruling:** pre-existing (untouched by this diff), and fork has no boot-path caller on aarch64 (arch_aarch64 class never reached test execution; init/daemons use exec, not fork) — latent, not reachable today. Per the audit mandate (audit only what the patch changes) this does not block; it MUST be fixed before aarch64 fork is enabled/tested. **Required follow-up issue on #100 or a new issue.**

### S3-2 (standing, now fully characterized): parked-task zombie window after EL0 fault
- **Where:** src/kernel/arch/aarch64/vectors.S el0_sync fault path (`bl aarch64_el0_fault_handler; eret`) + src/kernel/task/scheduler.cpp:3112-3124 (park).
- **Why:** After park (BLOCKED + dequeued) the handler `eret`s to ELR+4, so the parked task keeps executing user code until the next tick dispatches another task (≤ 1 tick, ~10 ms). Verified safe for the double-fault case: `dequeue_ready` → `ReadyQueueManager::remove` checks `rq_priority_`/`contains` and force-clears flags (scheduler.cpp:312-314, ready_queue_manager.cpp:77-93) — idempotent, no ready-queue corruption. Residual risk: a subsequent SVC from the zombie task would run syscall_handler against a BLOCKED `current`. Single-core, report-once, self-limiting, no kernel-memory exposure ⇒ S3. Document alongside the H2 follow-up.

### S3-3: PMM `free_pages_` reporting skew on non-x86 archs
- **Where:** src/kernel/memory/pmm.cpp:70 (`free_pages_ = total_pages_`), :508-510 (free_page below-window rejection).
- **Why:** On aarch64 the ~1 GiB below-window span counts as "free" but is never allocatable; `free_memory()` (pmm.hpp:133) over-reports. No branching consumer reads `free_pages_` (OOM retry loops are allocation-failure-driven; test baselines are delta-based via `free_pages_ref()`), so this is reporting-only. Suggest subtracting below-window pages at init or documenting the skew.

### S3-4: split-path attribute/AP coarsening (latent)
- **Where:** vmm.cpp:152-155, 321-323, 580-582.
- **Why:** Splits force AttrIndx=1 (Normal WB) and do **not** carry parent AP bits (mask omits AP[1:0]). Correct for the only current block creators (boot.S kernel blocks, AP=00 EL1-RW; RAM Device→Normal is the intended fix), but a future split of a Device MMIO block or a user 2MiB block would silently upgrade attributes/downgrade permissions. Latent; partially documented in the map_page comment. Track with the fork follow-up.

---

## PATCH

Not applicable — DECISION: APPROVED (no S1/S2 findings).

## Evidence basis
- Full read of `audits/pending_patch.diff` (24 files); targeted worktree reads: vmm.cpp (all descriptor sites), vmm.hpp, boot.S, vectors.S, syscall_entry.S, scheduler.cpp (dispatch guard, dequeue_ready, fault handler), pmm.cpp/pmm.hpp, kernel.cpp, buffer_pool.cpp, test_expected_counts.hpp, test_pmm.cpp, test_cap_irq.cpp, test_cap_mmio.cpp, irq_delivery.cpp, timer.cpp, gic.hpp, interrupt_controller.cpp, syscall_handlers_iommu.cpp.
- No tests executed by the auditor; developer-reported facts (978/978 x86, 8/8 memory_pmm, aarch64 boot to daemon wait) are consistent with test-history.txt rows 2026-08-30T22:18/22:45.
