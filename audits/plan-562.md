# PLAN: F9 (#562) — static bound re-sizing, VALUES-ONLY residual (IOPB 4→8, IOMMU mappings 32→128) + Q7 doc sync

Base: `main` = `5304d8883de6d5bb5dfa6c1812d3d2d74989e374` (read from `.git/refs/heads/main`;
this agent has no shell, so no `git log` — every claim below is backed by a file:line read at that base).
Issue #562 OPEN, milestone v0.6.1, labels `feature`/`sil3-relevant`/`microkernel-transition`/`kernel:core`;
work-begun comment already posted (2026-11-20).

**Headline:** the recon was right that M1/F1/F4/M4 landed most of F9 — but the residual is **not empty**.
Four Q7 rows are still off-tree: two are values-only and land here (`CONFIG_IOPB_MAX_TASKS` 4→8,
`CONFIG_IOMMU_MAX_MAPPINGS` 32→128), one is structurally unlandable in a values-only patch
(`MAX_PER_TASK_OBJECTS` per-row 256) and goes to the M4/M11 handoff, and one is F21's (#573) not F9's
(`alloc_page_table` freeze gap). Appendix A carries the per-item evidence, Appendix B the consumer-assert
matrix, Appendix C the retrieval artifacts (audit check 7), Appendix D the M4 handoff note.

## AFFECTED FILES

- `src/kernel/nexios_config.h` — two value rows + their matching `#error` message rows only
  (`:703-705`, `:1700-1702`, `:863-865`, `:1733-1735`). No new knob, no knob removal, no structural
  change: §3.1's closed-set rule is not engaged. Exact touched lines are enumerated in Appendix E so
  M4 (#563) can rebase cleanly.
- `docs/specs/microkernel-transition.md` — §7.3 table (`:459-464`): four stale value cells
  (`CAP_MAX_UNTYPED` 16→64, `CAP_MAX_DEATH_WATCHES` 16→32, `IOMMU …/_MAPPINGS` `8 / 32`→`8 / 128`,
  `IOPB_MAX_TASKS` `4 (x86_64)`→`8 (x86_64)`); §11 arch-parity row (`:788`) `IOPB, 4 tasks`→`8 tasks`.
  §15 F9 row (`:911`), §16 Q7 row (`:935`) and §7.2 freeze text (`:440-451`) are **not** touched
  (they already state the decided values / belong to M4).
- `docs/specs/configuration.md` — §2 tunable table: `:136` `CONFIG_IOPB_MAX_TASKS | 4`→`8`,
  `:139` `CONFIG_IOMMU_MAX_MAPPINGS | 32`→`128`.
- `docs/specs/iommu.md` — `:40` tunable row `CONFIG_IOMMU_MAX_MAPPINGS`(32)→(128).
- **Not touched (explicitly):**
  - `src/kernel/arch/x86_64/hal/iopb.cpp` / `src/kernel/iommu/iommu.{hpp,cpp}` — both already consume
    the macros (`g_iopb_pool[CONFIG_IOPB_MAX_TASKS][8192]`, `IoMmuMapping maps[CONFIG_IOMMU_MAX_MAPPINGS]`);
    no code change is needed for a values-only patch.
  - `src/kernel/test/**` (test sanctity — QE owns tests on `testbed`), including
    `test_expected_counts.hpp`: no test registration changes, so no count rows move (Appendix B).
  - `tools/check-config.py` — parses `nexios_config.h` and has **no** rule for `IOPB*`, `IOMMU*`,
    `CSLOT_COUNT`, `CAP_MAX_UNTYPED` or `MAX_PER_TASK_OBJECTS` (grep: 0 matches), so no validator edit.
  - `scripts/run_config_matrix.sh` — sweeps only the 6 deadline/WCET dimensions × 8 families
    (`configuration.md` §2.1); none of the two values is a matrix dimension.
  - `prompts/ROADMAP.md`, `prompts/BUGS.md` — roadmap is pointers-only; bugs live on GitHub.

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-B1 (single source + paired guard).** Every bound is defined exactly once in
  `nexios_config.h` behind `#ifndef` and accompanied by a literal-value `#error` message
  (`:` sanity block, LEARNINGS digest "#ifndef + value #error as a pair"). The value edit and its
  `#error` text edit land in the **same** commit; the message's "(default N)" must match the `#define`.
- **INV-B2 (IOPB pool width).** `t.iopb_slot_` is `uint8_t` with sentinel `IOPB_SLOT_NONE` (`task.hpp:978`,
  `:`init `425`); the claim set is a `uint32_t` bitmask and the slot is used as `1u << slot`
  (`iopb.cpp:45,74,80,82,96,135,169,202`). Therefore `CONFIG_IOPB_MAX_TASKS` MUST stay `<= 32`;
  8 satisfies it with headroom. Pool is a fixed `.bss` array `iopb.cpp:42` → 8 × 8192 B = 64 KiB
  (was 32 KiB); no RT-path allocation, no new lock (`g_iopb_lock` rank Leaf, unchanged).
- **INV-B3 (IOMMU table).** `IoMmuDomain::maps[CONFIG_IOMMU_MAX_MAPPINGS]` (`iommu.hpp:42-57`) inside
  `g_domains[CONFIG_IOMMU_MAX_DOMAINS]` (`iommu.cpp:70`) — pure static `.bss`: 8 × 128 × 24 B = 24 KiB
  (was 6 KiB, +18 KiB). All domain/mapping mutations stay under the single leaf `g_lock_`
  (`iommu.hpp:59-63`); the larger table only lengthens bounded scans (`iommu.cpp:573,616,628,675`) —
  **no lock-order, no new critical section, no IRQ-off window**.
- **INV-B4 (no ABI / snapshot drift).** Capability handle layout depends on `CONFIG_CSLOT_COUNT` only
  (`cap_types.hpp:126-139`, 6 slot bits at 64) — unchanged. `iopb_slot_` and `IoMmuDomain` are TCB/static
  state, not handle-encoded. `scheduler_snapshot_fields.cpp:188,269` copy `iopb_slot` as `uint8_t`
  — value-agnostic.
- **INV-B5 (ResourceTracker).** No new kernel resource type, no counter semantics change, no
  `test_isolate.cpp` layout change; every gate class must still report a **0 delta**.
- **INV-B6 (riscv64 image-bound fail-stop).** `arch/riscv64/boot.S:40-58` fail-stops the boot when
  `kernel_phys_end >= (_boot_stack_rv - 2 MiB)`. `.bss` grows ≈ +50 KiB on x86_64 and ≈ +18 KiB off-x86.
  #510 measured `kernel_phys_end 0x80C77000` vs bound `0x81E00000` (~17.6 MB headroom) → safe, but the
  riscv64 boot MUST be re-run and the new `kernel_phys_end` recorded in the issue comment.
- **INV-B7 (test sanctity).** Zero files under `src/kernel/test/` change; the change is compile-time only,
  so debug/release behavior cannot diverge (CODING_STYLE §6) and the release subset is unaffected.

## STEP-BY-STEP CHANGES

1. `src/kernel/nexios_config.h:703-705` — change the value to 8 and annotate the origin in the same
   style as the neighbouring Q7 rows (`CAP_MAX_DEATH_WATCHES` at `:751`):
   `#define CONFIG_IOPB_MAX_TASKS 8 // F9/Q7: 8 (was 4; 64 KiB .bss on x86_64)`.
   Leave the four doc-comment lines above (`:699-702`) semantically untouched except optionally
   nothing — do not reflow them (rebase isolation, Appendix E).
2. `src/kernel/nexios_config.h:1700-1702` — update the paired guard message only:
   `#error "CONFIG_IOPB_MAX_TASKS must be >= 1 (default 8)"`.
3. `src/kernel/nexios_config.h:863-865` — change `CONFIG_IOMMU_MAX_MAPPINGS` to 128, annotated
   `// F9/Q7: 128 per domain (was 32; +18 KiB .bss across 8 domains)`; keep the doc-comment at `:861-862`
   as-is.
4. `src/kernel/nexios_config.h:1733-1735` — update the paired guard message:
   `#error "CONFIG_IOMMU_MAX_MAPPINGS must be >= 1 (default 128)"`.
5. `docs/specs/microkernel-transition.md:459-464` (§7.3 table) — set the four cells to the Q7 values:
   `CONFIG_CAP_MAX_UNTYPED | 64`, `CONFIG_CAP_MAX_DEATH_WATCHES | 32`,
   `CONFIG_IOMMU_MAX_DOMAINS / _MAPPINGS | 8 / 128 per domain`, `CONFIG_IOPB_MAX_TASKS | 8 (x86_64)`.
   Leave `CONFIG_CSLOT_COUNT | 64`, `CONFIG_MAX_PER_TASK_OBJECTS | 32` and the other rows as they are
   (32 is still the tree truth — see Appendix D). Keep the §7.3 two-column layout and the
   "New values … are an owner decision (Q7, F9)" sentence at `:470` unchanged.
6. `docs/specs/microkernel-transition.md:788` (§11 arch-parity row) — `IOPB, 4 tasks` → `IOPB, 8 tasks`
   (x86_64 column only; aarch64/riscv64 columns keep "not applicable, fail-closed").
7. `docs/specs/configuration.md:136` — `CONFIG_IOPB_MAX_TASKS | 4` → `| 8` (keep the prose, it carries no
   hardcoded slot count).
8. `docs/specs/configuration.md:139` — `CONFIG_IOMMU_MAX_MAPPINGS | 32` → `| 128`.
9. `docs/specs/iommu.md:40` — `CONFIG_IOMMU_MAX_MAPPINGS`(32) → (128).
10. Post the M4 handoff note (Appendix D text) as a comment on **#562** and cross-link it on **#563**
    — no file change, no code.
11. Verification (no code steps beyond 1-4; commands are the only valid entry points per
    AGENTS-KERNEL-BRIEFING §6): `python3 tools/check-config.py` (rc 0);
    `tools/meson/build.sh` for each arch; `python3 tools/meson/build.sh --arch <a> --nolto`;
    then the class gates in TEST STRATEGY; `grep -n CONFIG_DEBUG_IPC_SCHED
    src/kernel/debug/ipc_sched_trace.hpp` stays commented out before any suite run.
12. `graphify update .` after the edits (docs/config edges) + append one `test-history.txt` row per
    class run (mandatory, append-only).

## TEST STRATEGY

- **New tests on `main`: none.** Test sanctity — F9 ships zero test-file edits, and no registration
  count changes, so `test_expected_counts.hpp` stays byte-identical (set steps 1-4 are compile-time
  constants; `capability`/`cap_iommu`/`core_memory` counts are unchanged).
- **Existing classes to run (debug unless stated), per arch:**
  - x86_64: `capability` (171 registered / 170 measured baseline → must stay 170 with 0 fails; carries
    `test_cap_mmio.cpp:498` `ioport_pool_exhaustion_fails_closed`, which iterates
    `enum { MAX = CONFIG_IOPB_MAX_TASKS }` and therefore self-adapts to 8 — the direct IOPB regression
    probe), `cap_iommu` (12/12; `test_cap_iommu.cpp:777` `iommu_bounded_exhaustion_fails_closed` maps
    exactly `CONFIG_IOMMU_MAX_MAPPINGS` distinct frames then asserts the overflow fails closed — the
    direct IOMMU regression probe), `core_memory`, `core_basic`, `configuration_build`, `selftest`
    (CI gate), plus `core_sync`/`core_sched`/`ipc` as layout smoke (any .bss-induced memory-map drift
    shows up here first).
  - aarch64 / riscv64: `capability` (143), `cap_iommu` (10), `core_memory`, `core_basic`,
    `configuration_build` (8), `selftest` per arch entry list; riscv64 additionally a boot smoke
    (INV-B6) with `kernel_phys_end` captured from the serial log.
  - Release leg: `python3 tools/meson/qemu_test.py --arch x86_64 --build release --class selftest`
    (config values are compiled into both build types; a release link proves nothing diverged).
- **Rationale per class:** `capability`/`cap_iommu` are the only classes that exercise the two changed
  bounds end-to-end; `configuration_build` is the existing home for config-surface assertions (see the
  QE sketch); `core_memory`/`core_basic` cover the PMM/MemPool paths that the larger `.bss` and the
  128-frame mapping test touch; `selftest` is the CI gate.
- **QE sketch (testbed only — dev lane must not write it):**
  - `config_q7_bounds_pinned` (`configuration_build`): compile-time `static_assert` tripwires on the
    five Q7 values (`CAP_MAX_UNTYPED == 64`, `CSLOT_COUNT == 64`, `CAP_MAX_DEATH_WATCHES == 32`,
    `IOPB_MAX_TASKS == 8`, `IOMMU_MAX_MAPPINGS == 128`) + `JARVIS_ASSERT` on the same constants, in the
    `test_config_checks.cpp` house style (`config_ceiling_ge_max_prio` pattern). Fails at **compile**
    time if a future patch silently reverts a Q7 value.
  - `ioport_pool_bound_matches_config` (`capability`, x86-only): claim `CONFIG_IOPB_MAX_TASKS` slots
    (all succeed), the `+1` claim fails, teardown releases every slot and `iopb_loaded_owner()` is null
    — asserts the bound **and** the fail-closed direction, not just the constant.
  - `iommu_mapping_bound_matches_config` (`capability`): the existing exhaustion test already iterates
    the macro; add the explicit `JARVIS_ASSERT_EQ(128u, kMaxMaps)`-style pin so a silent revert fails
    loudly rather than passing with 32.
  - Count rows: `test_expected_counts.hpp` needs the corresponding `configuration_build` row bump
    (`10/8/8` → `11/9/9`) and nothing else; `capability` rows stay as measured.
  - Gate classes to re-run on `testbed`: `configuration_build`, `capability`, `cap_iommu` ×3 arches.

## SIL 3 / SAFETY RISKS

- **[S1] riscv64 boot fail-stop on image growth** — `.bss` +18-50 KiB moves `kernel_phys_end`; if it ever
  crossed `_boot_stack_rv - 2 MiB` the board fail-stops before the stack exists (silent '!' + SBI
  shutdown, easy to misread as a hang). Mitigation: riscv64 build + boot smoke in the same cycle,
  `kernel_phys_end` value quoted on the issue; revert-trigger if it regresses.
- **[S2] IOPB mask-width coupling** — `1u << t.iopb_slot_` over a `uint32_t` claim mask caps the bound at
  32; 8 is safe but the coupling is undocumented. Mitigation: state it in the plan-driven code review
  (no code change in F9); if the owner ever wants >32 slots the mask must widen to `uint64_t` —
  optionally a `static_assert(CONFIG_IOPB_MAX_TASKS <= 32)` in `iopb.cpp` as a F21-class hardening item
  (not in this values-only patch).
- **[S2] `ioport_pool_exhaustion_fails_closed` now creates 9 concurrent TCBs** — verified fit:
  `sizeof(TaskControlBlock)` is pinned ≤ `kMemPoolMaxBlockSize` 8192 with 64 blocks in that class
  (`mempool.cpp:49-50`, `task.hpp:1474`), and `test_sched_helpers.hpp:290` uses
  `TaskControlBlock::create`. Mitigation: the gate run itself; if the class ever reddens at the TCB
  allocation, the bound — not the pool — is the suspect.
- **[S2] `iommu_bounded_exhaustion_fails_closed` now allocates 129 PMM pages, 129 `FrameCap`s and two
  129-entry stack arrays** (2064 B; kernel stack is 64 KiB, `CONFIG_STACK_SIZE 65536` at `:177`).
  Verified fit (`FrameCap` ≈ 56-64 B → 64 B MemPool class with 320 blocks); budget check is inert for
  the harness (`memory_budget_pages_ == 0` → unlimited, `pmm.cpp:407-423`). Mitigation: `cap_iommu` on
  all three arches + tracker delta 0 asserts already in the test.
- **[S2] Normative-spec divergence** — §7.3 still advertises `CAP_MAX_UNTYPED 16` and
  `DEATH_WATCHES 16` while the tree carries 64/32; Q7 says "landed via F9/#562", so leaving them stale
  makes the F9 closure claim unprovable and re-opens the §7.3 conflict list against M4. Mitigation:
  doc steps 5-9 in the same commit.
- **[S3] `alloc_page_table()`/`alloc_page_table_err()` pool fast paths bypass the post-freeze deny**
  (`pmm.cpp:949-979`, `:1372-1396`; the fallback legs delegate to the gated
  `alloc_page`/`alloc_page_err`). This is exactly the F21 (#573) catalogue row ("extend … to
  `alloc_user_page`, `alloc_user_contiguous`, `alloc_page_table`"), not F9 work; the other two named
  entry points are already gated (`:723-731`, `:908-915`). Recorded for F21/QE, not fixed here.
- **[S3] `MAX_PER_TASK_OBJECTS` stays flat 32** — see Appendix D; latent truncation risk in
  `detach_all_objects` (`task.cpp:319-338`) the day a row owns >32 objects. M4/M11 owned.
- **[S3] Release-vs-debug parity** — the change is `#define`-only, so no behavior branch is introduced;
  release `selftest` still runs to prove the release leg links with the new pool sizes.

## OPEN QUESTIONS

- **Q-F9-1 (scope: includes IOMMU?)** §15's F9 row (`:911`) names only "Untyped, per-task objects, CNode
  slots, IOPB, death watches", while §16 Q7 (`:935`) says the whole Q7 set is "landed via F9/#562" and
  explicitly lists "IOMMU 8 domains / 128 mappings per domain". No other follow-up (F1/F4/F5/M4) owns
  IOMMU bounds. **Default taken in this plan: land 128 in F9.** Owner confirm — if the answer is "not
  F9", drop step 3/4/7/9 and file a separate issue instead of leaving a decided value unlanded.
- **Q-F9-2 (spec edit authority)** the §7.3/§11 doc edits are edits to a normative spec; the #582
  precedent required owner sign-off for normative rows, while Q7 already *is* the owner decision for
  these values. **Default taken: land as bookkeeping to match Q7 and note it in the work-begun comment.**
  If sign-off is required per-row, hold steps 5-6 until it is posted.

---

## APPENDIX A — VERIFIED RESIDUAL (per item, evidence at `5304d8883`)

| Q7 row (§16:935) | Decided | Tree value | Evidence (file:line, read this session) | Verdict |
|---|---|---|---|---|
| `CAP_MAX_UNTYPED` | 64 | **64** | `nexios_config.h:585-592` (`#define … 64` + M4/Q-M4-2 comment); consumers `cap/untyped.cpp:43-56` (`alloc_object` live-bound), `:90-94` (`create` bound) | **LANDED** (M4 `911112f7b`) |
| `CSLOT_COUNT` | 64 / CNode | **64** | `nexios_config.h:571-576`; consumers `cap/cap.hpp:64` (`CSlot slots[CONFIG_CSLOT_COUNT]`), `cap/cap.cpp:53,79,103,124,150,174,183,190,208,500,520`, `cap_types.hpp:130-139` (6 slot bits), `untyped.cpp:223`, `syscall_handlers_iommu.cpp:68` | **LANDED** |
| `CSLOT_COUNT` — "2 CNodes for infra servers" | — | 1 root CNode/task | only production `CNode::create` caller is `task.cpp:269`; `boot_sdf.cpp:670` `sdf_compose` mints caps into that node | **NOT a config value → M4/M5 handoff** (App. D) |
| `CAP_MAX_DEATH_WATCHES` | 32 | **32** | `nexios_config.h:746-752` (`… 32 // M1/Q7: 22 SDF rows + spares (was 16)`); consumer `ipc/death_notify.hpp:124` (`kMaxWatches`) | **LANDED** (M1) |
| dead `CONFIG_MEMPOOL_*` | removed | **removed** | `nexios_config.h:293-300`; resurrection is a hard error in `tools/check-config.py:312-319`; `docs/specs/configuration.md:73` | **LANDED** (M4/F21-part) |
| TCB size `static_assert` | present | **present** | `task/task.hpp:1474-1476` (`sizeof(TaskControlBlock) <= kernel::kMemPoolMaxBlockSize`), `:1480-1484` (FPU-state pins); pool table `memory/mempool.cpp:46-50` | **LANDED** (M4) |
| `CONFIG_STATIC_POOLS_ONLY` flag + freeze denies | all post-init PMM entries | 6/6 primary + 6/6 `_err` twins gated; **page-table entries not** | flag `nexios_config.h:1128-1130` (+ guard `:1801-1802`), freeze `pmm.cpp:506`, `is_frozen` `:619`; denies `alloc_page:453`, `alloc_contiguous:680`, `alloc_user_page:724`, `alloc_page_colored:764`, `alloc_page_colored_err:806`, `alloc_user_page_colored:835`, `alloc_user_page_colored_err:873`, `alloc_user_contiguous:909`, `alloc_page_err:1218`, `alloc_contiguous_err:1259`, `alloc_user_page_err:1295`, `alloc_user_contiguous_err:1336` | **PARTIAL → F21 #573, not F9** (App. A.1) |
| `MAX_PER_TASK_OBJECTS` | 256 mem-mgr/supervisor rows, 32 else | **32 flat** | `nexios_config.h:561-566`; sole consumer is a *walk bound*: `task.cpp:319-338` `detach_all_objects`; `attach_object` (`task.cpp:280-290`) enforces **no** per-task budget; no budget field on the TCB, no row budget in `boot_sdf.hpp:145/151` | **NOT LANDABLE in a values-only patch → M4/M11 handoff** (App. D) |
| `IOPB_MAX_TASKS` | 8 (x86_64) | **4** | `nexios_config.h:699-705`; consumer `arch/x86_64/hal/iopb.cpp:42` (`g_iopb_pool[CONFIG_IOPB_MAX_TASKS][8192]`), claim scan `:79`, mask `:45` | **RESIDUAL → steps 1-2, 6, 7** |
| IOMMU domains / mappings | 8 / 128 | **8 / 32** | `nexios_config.h:854-865` (domains 8 ✓, mappings 32 ✗); consumer `iommu/iommu.hpp:42-57` (`maps[CONFIG_IOMMU_MAX_MAPPINGS]`), `iommu/iommu.cpp:70` (`g_domains[CONFIG_IOMMU_MAX_DOMAINS]`) | **RESIDUAL (mappings only) → steps 3-4, 8, 9** |
| `MAX_TASKS` 64 / `FAST_PAYLOAD` 48 / `ZOMBIE_LIMIT` 32 / `STORM_LIMIT` 256 | unchanged | **64 / 48 / 32 / 256** | `nexios_config.h:58`, `:228`, `:982`, `:123` | **VERIFIED UNCHANGED** (no edit) |
| SDF demand-vs-bound check (§7.3 conflicts) | — | absent | `sdf_compose` exists (`boot_sdf.cpp:670`) but carries no bound validation | **EXPLICITLY OUT OF F9 → M4 #563 owns** (App. D) |

**A.1 — why the page-table gap is F21's, not F9's.** `alloc_page_table()` (`pmm.cpp:949-979`)
takes the pool free-list fast path *without* a `g_pmm_frozen` check; only its fallback leg reaches the
gated `alloc_page()`. `alloc_page_table_err()` (`:1372-1396`) is the same shape. §15's F21 row
(`:923`) names exactly these three entry points, and `pmm.hpp:74-75` already advertises "every
post-freeze entry point denies" — a doc-vs-code gap owned by #573. Fixing it needs a code edit in
`pmm.cpp`, which the F9 sequencing contract forbids (values-only), so it is recorded here and handed
to F21/QE instead of being silently folded in.

**A.2 — nothing else was invented.** Every row above is either *landed* (with the exact `#define`
and consumer read) or *explicitly routed* to the owning issue. No knob is added or removed, so the
§3.1 closed-certified-set rule (syscalls) is untouched.

## APPENDIX B — CONSUMER / ASSERT MATRIX

**B.1 — asserts and guards that constrain the two values being changed**

| Site | Text | Effect of the edit |
|---|---|---|
| `nexios_config.h:1700-1702` | `#if CONFIG_IOPB_MAX_TASKS < 1` / error "… (default 4)" | guard passes at 8; **message must be edited** (step 2) or it lies |
| `nexios_config.h:1733-1735` | `#if CONFIG_IOMMU_MAX_MAPPINGS < 1` / error "… (default 32)" | guard passes at 128; **message must be edited** (step 4) |
| `arch/x86_64/hal/iopb.cpp:45,74,80,82,96,135,169,202` | `1u << slot` on `uint32_t g_iopb_claimed` | safe at 8; hard ceiling 32 (S2 note) |
| `iopb.cpp:42` | `static uint8_t g_iopb_pool[N][8192]` | .bss 32→64 KiB |
| `iommu/iommu.hpp:55` | `IoMmuMapping maps[CONFIG_IOMMU_MAX_MAPPINGS]` | .bss +18 KiB over 8 domains |
| `iommu/iommu.cpp:90` | `static_assert(CONFIG_IOMMU_MAX_BUSES <= 8)` | unaffected (BUSES untouched) |

**B.2 — asserts that must stay green but are NOT satisfied by these values (re-verified, no edit)**

| Site | Assert | Status at the post-edit values |
|---|---|---|
| `cap/derivation.hpp:70` | `kCapMaxDerivations >= 4u * CONFIG_CSLOT_COUNT` | 256 >= 4×64 = 256 — **exactly** satisfied; any future `CSLOT_COUNT` raise breaks it (do not touch CSLOT here) |
| `cap/derivation.hpp:72-74` | `CAP_MAX_DEPTH >= 1`, `kRevokeEdgeBudget >= CAP_MAX_DEPTH + 1` | untouched |
| `cap/cap.cpp:373` | `CAP_MAX_DEPTH + 1 <= kCapMaxDerivations` | untouched |
| `cap/vspace_map.cpp:43` | `kMaxVspaceMappings > 0` (F5, `vspace_map.hpp:98` = 32) | untouched (F5's bound is not a Q7 row) |
| `cap/frame_map.cpp:64-65` | `CONFIG_CAP_MAX_FRAME_MAPS > 0` | untouched (8) |
| `task/task.hpp:1474` | `sizeof(TaskControlBlock) <= kMemPoolMaxBlockSize` | untouched; TCB size does not change (no TCB field added) |
| `task/tcb_control.cpp:37,42` | F4 tripwire: `PdRole::PROCFS_SRV+1 == kPdRoleCount` and `kPdRoleCount == 13` | untouched — **and this is why the mem-mgr SDF row (which would move the count) is not added in F9** |
| `cap/object_sizing.hpp:89-98` | page-alignment of carved sizes | untouched |
| `driver/fb_backing.hpp:44-45` | FB backing geometry | untouched |

**B.3 — test-side consumers of the changed macros (all self-adapting, no test edit needed)**

| Test | Uses | Behavior at the new bound |
|---|---|---|
| `test_cap_mmio.cpp:498` `ioport_pool_exhaustion_fails_closed` (x86-only registration, `:1171`) | `enum { MAX = CONFIG_IOPB_MAX_TASKS }` | claims 8, asserts the 9th fails, tears down → 9 live TCBs (verified fit, S2) |
| `test_cap_iommu.cpp:777` `iommu_bounded_exhaustion_fails_closed` (registered on **all** arches, `:862`) | `CONFIG_IOMMU_MAX_DOMAINS`, `CONFIG_IOMMU_MAX_MAPPINGS` | 8 domains + 128 mappings + 1 overflow; 129 PMM pages, 129 `FrameCap`s (verified fit, S2) |
| `test_cap_death.cpp:303` | `CONFIG_CAP_MAX_DEATH_WATCHES` | unchanged (already 32) |
| `test_cap_untyped.cpp:969` | `CONFIG_CAP_MAX_UNTYPED` | unchanged (already 64) |

**B.4 — count rows.** No test is added or removed on `main`: `capability` 170 (x86) / 143 (aa64, rv64),
`cap_iommu` 12 / 10 / 10, `core_memory` 129, `core_basic` 98, `configuration_build` 10 / 8 / 8,
`selftest` 157 all stay as measured. `test_expected_counts.hpp` is therefore untouched by the dev lane.

## APPENDIX C — RETRIEVAL (audit check-7 artifacts)

| # | Query (MCP) | Disposition |
|---|---|---|
| 1 | `graphify query_graph "config bounds consumers: IOPB pool per-task, CNode slot count, death watch registry, Untyped live bound"` (BFS depth 2, 191 nodes) | **used** — surfaced `nexios_config.h`, `CNode`/`CSlot`/`DerivationTable`, `DeathNotify`, `untyped.cpp` (`alloc_object`/`create`/`retype`), `iopb`. Confirms the change surface is config→consumer only, no shared-state object is resized. |
| 2 | `graphify shortest_path nexios_config.h → iopb` | **used** — 3 hops (`nexios_config.h --imports--> gdt.hpp --contains--> TSSBlock --defines--> iopb`): the IOPB bound reaches the pool **only** through the TSS bitmap owner, i.e. no other module embeds the slot count. |
| 3 | `graphify shortest_path nexios_config.h → DeathNotify` | **used** — 3 hops via `types.hpp`/`death_notify.hpp`: the registry is a TU-local static table (`kMaxWatches`), so a value change cannot ripple into IPC structures. |
| 4 | **ignored (of #1)** — `LoopBoundsChecker`/`Checker`/`assert.hpp`/`logger.hpp` nodes | style-tool and logging nodes; no bound consumer, no sizing surface. |
| 5 | `obsidian_search_vault "static bound re-sizing IOPB death watches Untyped CNode slots"`, `"microkernel transition F9 bounds Q7 owner decision"` | **used (negative)** — 0 matches: the vault has **no** F9/bounds coverage, so there is no prior audit precaution for these values to honour; recorded so the auditor does not read the absence as a skipped read. |
| 6 | `obsidian_search_vault "IOPB_MAX_TASKS"` | **used** — `specs/configuration.md:139` (the vault mirror of the doc table ⇒ step 7/8 must be mirrored), `audits/report-2026-09-01T16-24-40Z.md:19` (slot retained until task cleanup on a grant-failure path — **no** coupling to the slot *count*), `prompts/STATE.md` (`iopb.hpp/.cpp` ownership map). |
| 7 | `obsidian_search_vault "CAP_MAX_UNTYPED"` | **used** — `audits/report-20260829-cap-rejected.md:12` (S3: `untyped.cpp` used to PMM-allocate before the live-bound check — an ordering trap if the bound is ever touched) and `audits/plan-554.md:100` (child accounting). Neither is re-opened by a value raise; recorded to keep the auditor from re-filing them. |
| 8 | `obsidian_search_vault "microkernel"` (control) | **used** — 5 hits in `audits/design-review-554-*`, proving the vault is reachable and the empty F9 result in #5 is *coverage*, not an outage. |
| 9 | `audits/plan-582-transition-compat.md` | **used** — grep for `F9|562|IOPB|UNTYPED|CSLOT|PER_TASK|DEATH_WATCH|IOMMU_MAX` → 0 matches; its Q3/§5.6 slope-bound discussion is scheduler IPC, **no conflict** with these bounds. |

## APPENDIX D — M4 (#563) / M11 HANDOFF NOTE (post verbatim on #562, cross-link on #563)

> **F9 handoff — what a values-only patch cannot land (owner/M4 action).**
> F9 (#562) lands only the Q7 *values*: `CAP_MAX_UNTYPED` 64, `CSLOT_COUNT` 64,
> `CAP_MAX_DEATH_WATCHES` 32, `IOPB_MAX_TASKS` 8, `IOMMU_MAX_MAPPINGS` 128 (and the already-landed
> dead-`CONFIG_MEMPOOL_*` removal + TCB `static_assert`). Three Q7 items are **not** values and need
> M4/M11 structures:
> 1. **`MAX_PER_TASK_OBJECTS` = 256 for mem-mgr/supervisor rows, 32 elsewhere.** There is no per-task
>    object budget today: `attach_object` (`task.cpp:280-290`) enforces none, and the only consumer,
>    `detach_all_objects` (`task.cpp:319-338`), uses `CONFIG_MAX_PER_TASK_OBJECTS` as a **walk bound**.
>    Required: a budget field on the TCB set by `sdf_compose` (`boot_sdf.cpp:670`) from the row
>    (mem-mgr 256, supervisor 256, else 32), and `detach_all_objects` bound by that field. Until then a
>    row owning >32 objects truncates its snapshot-restore walk (links left dangling into the MemPool
>    rewind) — the flat 32 is left in place deliberately, not forgotten.
> 2. **`CSLOT_COUNT` "2 CNodes for infra servers"**: production mints exactly one root CNode
>    (`task.cpp:269`, the only `CNode::create` caller); the second CNode is a compose/row property, not a
>    config value.
> 3. **SDF demand-vs-bound check (§7.3 conflicts)**: explicitly out of F9 — `sdf_compose` owns
>    validating each row's demand against the bound it is composed with.
> Also for F21 (#573), not F9: `alloc_page_table()` (`pmm.cpp:949-979`) and `alloc_page_table_err()`
> (`:1372-1396`) reach their pool free-list **before** the `CONFIG_STATIC_POOLS_ONLY` deny (only the
> fallback legs are gated), so `pmm.hpp:74-75`'s "every post-freeze entry point denies" is not yet true.

## APPENDIX E — M4 REBASE ISOLATION (exact lines F9 touches in `nexios_config.h`)

F9 edits **8 lines** in `nexios_config.h` (4 subject lines + 4 guard-message lines) across 4 separated
hunks; nothing else in the file is reflowed or reordered, so the M4 patch can rebase hunk-by-hunk.

| Hunk | Line(s) | Current text (at `5304d8883`) | After F9 |
|---|---|---|---|
| 1 | `703` | `#ifndef CONFIG_IOPB_MAX_TASKS` | unchanged |
| 1 | `704` | `#define CONFIG_IOPB_MAX_TASKS 4` | `#define CONFIG_IOPB_MAX_TASKS 8 // F9/Q7: 8 (was 4; 64 KiB .bss on x86_64)` |
| 1 | `705` | `#endif` | unchanged |
| 2 | `1701` | `#error "CONFIG_IOPB_MAX_TASKS must be >= 1 (default 4)"` | `… (default 8)"` |
| 3 | `863` | `#ifndef CONFIG_IOMMU_MAX_MAPPINGS` | unchanged |
| 3 | `864` | `#define CONFIG_IOMMU_MAX_MAPPINGS 32` | `#define CONFIG_IOMMU_MAX_MAPPINGS 128 // F9/Q7: 128 per domain (was 32; +18 KiB .bss)` |
| 3 | `865` | `#endif` | unchanged |
| 4 | `1734` | `#error "CONFIG_IOMMU_MAX_MAPPINGS must be >= 1 (default 32)"` | `… (default 128)"` |

Adjacent M4-authored content that F9 must **not** disturb: `nexios_config.h:293-300` (dead-MEMPOOL
note), `:302-330` (the 4 M4 keys), `:585-592` (`CAP_MAX_UNTYPED 64` + Q-M4-2 comment),
`:1457-1469` (M4 guard block). Documentation hunks are in `docs/` only; the spec appendix row
`microkernel-transition.md:951` (M4 record) and `:440-451` (§7.2 freeze text) are untouched.
