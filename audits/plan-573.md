# PLAN: F21 (#573) — dead CONFIG_MEMPOOL_* close-out, single-source TCB size pin, STATIC_POOLS_ONLY page-table deny coverage

**Issue:** #573 (F21, spec §7.2 / §15) · milestone v0.6.1 · labels kernel:memory, sil3-relevant
· lane: developer, branch main · base: current tip (re-confirm anchors at implementation time;
all line numbers below are tip-relative from this session's reads).

**Phase split (each increment committed + code-audited separately, no stacking):**
- **Phase 0** — verify what #563/M4 already landed for items 1 and 2 (no diff).
- **Increment A** (steps 1–6, items 1+2 close-out) — assert/config surface only, **zero runtime
  behaviour change in any build**.
- **Increment B** (steps 7–14, item 3 residual) — the page-table deny pair; compiled only under
  CONFIG_STATIC_POOLS_ONLY=1, so default builds stay byte-equivalent in behaviour.

**Finding that shapes the whole plan (evidence, read this session):** items 1 and 2 of the issue
body are **already landed** by #563/M4 — nexios_config.h:293-300 documents the deletion of the
three CONFIG_MEMPOOL_* macros and tools/check-config.py:312-319 errors on resurrection;
task.hpp:1574 already carries `static_assert(sizeof(TaskControlBlock) <= kernel::kMemPoolMaxBlockSize)`.
Item 3 is **12 of 14** public alloc entry points landed (alloc_user_page, alloc_user_contiguous and
their `_err` twins all deny already); the residual is the pair `alloc_page_table()` /
`alloc_page_table_err()` (pmm.cpp:962, :1385), whose **pool fast path** draws post-freeze with no
deny (only the `alloc_page()` fallback denies). So #573 is a close-out: make the landed state
provable, and close the last two entry points.

**Retrieval (check-7, this session):**
- graphify query `CONFIG_MEMPOOL static pools MemPool init TaskControlBlock size STATIC_POOLS_ONLY
  alloc_user_page` (BFS d=3, 55 nodes) — **used**: MemPool/pools_/POOL_COUNT, alloc_user_page, the
  PMM `_err` families, is_block_pinned/find_pool. **Ignored**: scheduler/IPC/ELF breadth
  (unrelated communities), initrd::init (name collision only).
- audits/plan-563.md (read in full) + audits/report-20261118-142140-563-iter2.md +
  audits/design-review-563-r1.md Dim 7 — **used as normative precedent**: M4 deliberately left the
  page-table deny placement to #573 (OQ-1); the audit "DECIDED: option A stands" covered M4's own
  scope only; the accepted S3 that the six `_err` twins return generic OOM constrains this plan
  (it must neither widen nor silently fix that).
- audits/plan-562.md App. A.1 — **used**: why the page-table gap is F21's and not F9's.
- specs: docs/specs/memory.md §6.1 (:162-173), docs/specs/configuration.md §2.3 (:184-204),
  docs/specs/microkernel-transition.md §7.2 (:460-473), INV-MK-1 (:74), §8.2 item 5 (:553-556),
  §15 F21 row (:1004) — **used as the acceptance text**; the spec calls the page-table pair an
  explicit GAP ("carry no such assert") and §7.2 ends with "REQUIRED (F21, before M4's gate):
  every post-init PMM entry point SHALL be covered".
- GitHub issue #573 body (read via MCP) — matches the three-item scope verbatim; 0 comments, no
  dependencies stated.

## AFFECTED FILES

**Increment A (assert / config surface; no runtime change):**
- src/kernel/memory/mempool.cpp — hoist the local sizes[]/counts[] tables (:46-50) to file-scope
  constant-initialized arrays and add the compile-time pin that makes kMemPoolMaxBlockSize provably
  equal to the table's top class (today mempool.hpp:30-33 *claims* to be the single source for that
  entry; nothing enforces it).
- src/kernel/task/task.cpp — delete the duplicate literal pin (`sizeof(TaskControlBlock) <= 16384 /
  8192`, :1021-1030); the constant-derived assert in task.hpp is the single source. The block also
  sits in the wrong place (above install_page_table).
- src/kernel/task/task.hpp — delete the two dead `// PROBE-DISABLED …` comment lines (:1572-1573,
  leftover of a local probe; no tool consumes that marker — repo-wide grep = 2 hits, both here).
  Keep :1574-1575 untouched.
- tools/check-config.py — extend the §6 MemPool guard (:311-319) from CONFIG_MEMPOOL_NUM_POOLS to
  all three deleted names, so resurrecting BLOCK_SIZES/BLOCK_COUNTS also fails.
- src/kernel/nexios_config.h — comment-only: extend the F21 block (:293-300) to name where the
  single source is pinned (mempool.cpp top-class assert → kMemPoolMaxBlockSize → task.hpp TCB
  assert). No key added, no key removed.

**Increment B (item 3 residual; CONFIG_STATIC_POOLS_ONLY only):**
- src/kernel/memory/pmm.cpp — deny at the head of alloc_page_table() (:962) and
  alloc_page_table_err() (:1385), in the exact shape of the 12 landed siblings.
- src/kernel/memory/pmm.hpp — reword the freeze_pools() contract (:71-78, currently "the §7.2 fixed
  page-table pool stays allocatable post-freeze — its deny placement is F21's, #573") and add the
  deny note to the alloc_page_table/_err doc blocks (:131-137).
- docs/specs/microkernel-transition.md — §7.2 status paragraph (:468-473) and the INV-MK-1 row
  (:74) GAP/VIOLATED cells become the landed state; **the normative SHALL text is not edited**.
- docs/specs/memory.md — §6.1 first bullet (:164-166): all 14 alloc entry points deny, page-table
  pool fast path included.
- docs/specs/configuration.md — §2.3 (:200-204): one expectation line for the pools1 leg (post-freeze
  page-table draws now refuse, so no post-freeze task creation on that leg — it already fails
  earlier at alloc_contiguous).

**Not touched (lane rules):** every file under src/kernel/test/, test_expected_counts.hpp,
tools/test-policy.tsv, initrd test config — QE-owned, read-only here. QE handoff is enumerated in
§TEST STRATEGY. audits/plan-573.md (this file) is the only doc this lane creates now.

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-F21-1 — default builds are behaviour-identical.** Every line added in Increment B lives
  inside `#if CONFIG_STATIC_POOLS_ONLY` (flag default 0, nexios_config.h:1151-1153); with the flag
  off the preprocessed function body must be textually unchanged apart from the two removed lines.
  Proof: default-leg class counts match the pre-diff rows in test-history.txt.
- **INV-F21-2 — refusal has zero side effects.** The deny precedes is_table_pool_configured(), the
  pmm_lock_ scope, bitmap_set, owner_set_kernel, --free_pages_, ResourceTracker::track_pmm_alloc and
  the s_post_snap_overlay_ write. A refused call changes no PMM accounting, so test_isolate snapshot
  deltas stay 0 for a refused probe.
- **INV-F21-3 — deny path is lock-free.** note_frozen_deny() (pmm.cpp:43-49) is zero-alloc,
  lock-free and must never be called with pmm_lock_ held (INV-F10-5). Entry placement keeps that
  true and matches all 12 landed sites.
- **INV-F21-4 — alloc_page_table's lock section is unchanged.** The pool scan keeps its own inner
  IrqSpinLockGuard scope (:966-986) which closes **before** the alloc_page() delegation (:987); the
  deny adds no lock and does not move the scope. No scheduler lock is taken anywhere on the path.
- **INV-F21-5 — exactly one live TCB size pin.** After Increment A a repo-wide grep for
  `sizeof(TaskControlBlock) <=` returns exactly one live assert (task.hpp:1574) plus the new
  table↔constant equality assert; no literal 16384/8192 pin remains in task.cpp.
- **INV-F21-6 — the pool table has one definition and one top-class proof.**
  kMemPoolMaxBlockSize (mempool.hpp:34-39) equals kPoolSizes[POOL_COUNT-1] by construction after
  Increment A; the hoist must not change storage (both forms are constant-initialized .rodata,
  ~144 B, no guard variable, no runtime init).
- **INV-F21-7 — no new diagnostic codes.** Increment B reuses note_frozen_deny() (dmesg
  kDmesgBase_PMM + 2, mem_pressure::PMM_FROZEN_DENY). No dmesg_catalog.hpp row, no
  MemPressureReason value, no new PmmError member → no code-collision surface.
- **INV-F21-8 — 14 of 14.** Post-change, `grep -c 'note_frozen_deny();' src/kernel/memory/pmm.cpp`
  equals the number of public alloc entry points (7 primaries + 7 `_err` twins = 14), i.e. every
  pmm.hpp entry declared between :99 and :172 that allocates.
- **Cross-arch boundary.** Both increments are arch-neutral source (mempool.cpp, pmm.cpp, task.*);
  what differs per arch is only the constant value (8192 x86_64/riscv64, 16384 aarch64) — which is
  exactly what the new equality assert proves per arch. PAGE_SIZE-derived pool page math and the
  page-table pool window are untouched. Verification must therefore be the 3-arch build + directed
  aarch64/riscv64 class legs, never x86-only.

## STEP-BY-STEP CHANGES

### Phase 0 — verify the landed half (no code, evidence only)

0.1 Re-confirm at the implementation tip, and paste the evidence into the issue comment:
    | Item | Anchor | Expected state |
    |---|---|---|
    | CONFIG_MEMPOOL_* absent from nexios_config.h | :293-300 (comment-only block) | `grep -rn 'CONFIG_MEMPOOL_' src/ docs/ tools/` → deletion note + check-config guard + doc mention only |
    | resurrection guard | tools/check-config.py:311-319 | errors on CONFIG_MEMPOOL_NUM_POOLS only (→ fixed in step 5) |
    | TCB pin | task.hpp:1574-1575 (kMemPoolMaxBlockSize) | live; duplicate literal pin at task.cpp:1021-1030 |
    | user-page denies | pmm.cpp:737-743 (primary), :1308-1313 (_err) | 0 / PMM_ERR_USER_OOM |
    | user-contiguous denies | pmm.cpp:922-928, :1349-1354 | same |
    | page-table denies | pmm.cpp:962, :1385 | **absent in the pool fast path** (only the alloc_page* fallback denies) |
0.2 `git diff --stat main -- src/kernel/test` must be empty; if the testbed half of #563 is not on
    main, record it (does not block this lane).
0.3 Record baseline rows (one test-history.txt row per run, measured times only): x86_64 debug
    core_memory, core_basic, selftest; aarch64 + riscv64 debug core_memory directed.

### Increment A — items 1+2 close-out (commit A, then code audit)

1. src/kernel/memory/mempool.cpp: move the two literal tables out of MemPool::init() into an
   anonymous namespace above it, as `constexpr size_t kPoolSizes[MemPool::POOL_COUNT]` and
   `kPoolCounts[MemPool::POOL_COUNT]` (values byte-identical to :46-50), and have init() read them
   (no other change inside the loop). Directly after them:
   `static_assert(kPoolSizes[MemPool::POOL_COUNT - 1] == kernel::kMemPoolMaxBlockSize, "MemPool top
   class must equal kMemPoolMaxBlockSize (F21 #573)");` — this is the "wire" half of item 1: the
   constant is now *proven* to be the table's top entry instead of merely documented as such.
2. src/kernel/memory/mempool.cpp: keep a short comment at the new file-scope tables naming the
   three consumers (table ↔ kMemPoolMaxBlockSize ↔ task.hpp TCB assert) so the single-source rule
   is discoverable without reading mempool.hpp.
3. src/kernel/task/task.cpp: delete :1021-1030 (the `#if defined(CONFIG_ARCH_AARCH64) …
   static_assert(… <= 16384) … #else … <= 8192 … #endif` block) and leave one comment line
   `// TCB size pin lives in task.hpp (kMemPoolMaxBlockSize); do not duplicate literals here.`
   Deletion only — do not relocate it.
4. src/kernel/task/task.hpp: delete the two `// PROBE-DISABLED …` lines (:1572-1573). Touch nothing
   else in the file (the TCB layout is load-bearing for the #353/#373 asserts below it).
5. tools/check-config.py: replace the single-name guard at :318-319 with a loop over
   ("CONFIG_MEMPOOL_NUM_POOLS", "CONFIG_MEMPOOL_BLOCK_SIZES", "CONFIG_MEMPOOL_BLOCK_COUNTS"),
   emitting the existing message shape ("<name> is dead (F21); size the table in mempool.cpp").
   Before committing, grep the repo for live uses (`grep -rn 'CONFIG_MEMPOOL_' --include='*.txt'
   --include='*.ini' --include='*.conf' --include='meson*' .`) to prove no config sets them — a
   false rejection would break check-config for the release matrix.
6. src/kernel/nexios_config.h: extend the F21 comment block (:293-300) by one or two lines naming
   the pin chain (mempool.cpp top-class static_assert → kMemPoolMaxBlockSize → task.hpp TCB
   static_assert). Comment-only; no macro, no guard, no reflow of neighbouring blocks.

Increment A gate: 3-arch build/link (15.1–15.2) + core_memory on x86_64 and directed aarch64/riscv64
(count parity with Phase 0 rows) + `make check-style` Errors 0 + check-config 0 on all three arches.
Then hand `git diff main -- <files> > audits/pending_patch.diff` to the auditor.

### Increment B — item 3 residual (commit B, then code audit)

7. src/kernel/memory/pmm.cpp alloc_page_table() (:962): insert at the very top of the function,
   before `if (!is_table_pool_configured())`:
   `#if CONFIG_STATIC_POOLS_ONLY` / `if (g_pmm_frozen) { note_frozen_deny();
   ASSERT(errors::PmmError::PMM_ERR_FROZEN_DENY); return 0; }` / `#endif` — byte-for-byte the shape
   of alloc_user_contiguous() (:922-928). Entry placement is deliberate: it covers both the pool fast
   path and the alloc_page() fallback with exactly one latch/dmesg emission (no double count) and it
   precedes every state read.
8. src/kernel/memory/pmm.cpp alloc_page_table_err() (:1385): same guard at the top, returning
   `errors::PMM_ERR_FROZEN_DENY` (not PMM_ERR_TABLE_OOM: the pool is not exhausted, and
   error_string() would print "page table pool exhausted" for a freeze refusal). Order: deny → then
   the existing is_table_pool_configured() delegation → alloc_page_err fallback. Add a one-line
   comment marking the deliberate asymmetry with the six `_err` twins (accepted S3, see SIL-8).
9. src/kernel/memory/pmm.hpp freeze_pools() doc (:71-78): replace the "stays allocatable post-freeze
   — its deny placement is F21's, #573" clause with the resolved contract: under
   CONFIG_STATIC_POOLS_ONLY **every** post-freeze PMM entry point denies (typed PMM_ERR_FROZEN_DENY
   on the `_err` paths, ASSERT + 0 on the uint64_t paths); the reserved page-table pool is drawable
   only pre-freeze. Keep the "handover accounting is live in all builds" sentence and the
   no-nesting lock note.
10. src/kernel/memory/pmm.hpp alloc_page_table (:131-133) and alloc_page_table_err (:134-137) doc
    blocks: add one line each — "post-freeze: denies under CONFIG_STATIC_POOLS_ONLY
    (PMM_ERR_FROZEN_DENY); pool fast path and fallback alike".
11. docs/specs/microkernel-transition.md §7.2 (:468-473): rewrite the *status* sentences ("Today it
    asserts only in … are not gated, so the switch does not yet forbid PMM allocation after init")
    to the landed state: all seven alloc pairs (14 entry points) deny, including the reserved
    page-table pool fast path; the REQUIREMENT sentence stays as the standing rule. Do not touch
    §7.2's fixed-pool list.
12. docs/specs/microkernel-transition.md INV-MK-1 row (:74): GAP cell → "closed (M4 + F21/#573: all
    14 alloc entry points deny under the flag)"; VIOLATED cell → keep the historical site list but
    mark each as deny-gated post-freeze (sys_frame_create, BufferPool fallback, elf_loader page
    allocation, vmm.cpp page-table allocation on map). Row-state text only — the SHALL clause is
    untouched.
13. docs/specs/memory.md §6.1 (:164-166): extend the first bullet so the coverage claim is exact
    (14 entry points, page-table pool included) and add the consequence note: post-freeze VM growth
    and task creation fail closed with a typed error, never a panic.
14. docs/specs/configuration.md §2.3 (:200-204): append the pools1-leg expectation line — the leg now
    refuses post-freeze page-table draws, so it cannot create tasks after freeze (it already refused
    earlier at alloc_contiguous); keep the "Do NOT flip the defaults" paragraph as is.

Sequencing: Increment B must not land before Increment A is audited APPROVED; neither increment
touches a test file.

### Gates (run after each increment; one test-history.txt row per class run)

15.1 `bash scripts/healthcheck.sh` → exit 0 (halt on anything else).
15.2 Build/link all three arches (x86_64, aarch64, riscv64), debug. Only the known non-blocking
     clang-tidy warnings tolerated.
15.3 `make check-style` → Errors: 0 (the only blocking style gate).
15.4 check-config on all three arches (`meson compile check-config -C <builddir>`) → 0, plus the
     negative fixture: a scratch build with `-Dconfig_defs=-DCONFIG_MEMPOOL_BLOCK_SIZES=16` must now
     fail (proves step 5; do not commit the scratch dir).
15.5 qemu classes, default build, debug: x86_64 core_memory (pmm, static_pools, page_tables,
     mem_handover groups), core_basic (buffer-pool contract), core_task (TCB creation + kslot),
     selftest (CI gate). aarch64 + riscv64 **directed core_memory only** (their per-arch lists
     exclude the rest by design — never substitute the x86 list).
15.6 pools1 leg (item 3's only behavioural surface), per docs/specs/configuration.md §2.3, by hand,
     on **all three** arches:
     `meson setup build/meson-<arch>-pools1 --cross-file cross/<arch>-elf.txt -Darch=<arch>
     -Dconfig_defs=-DCONFIG_STATIC_POOLS_ONLY=1` → configure + build + **link green** (mandatory;
     this is the evidence STATE records for the leg). Then one x86_64 runtime attempt:
     `python3 tools/meson/qemu_test.py --arch x86_64 --build debug --class core_memory --builddir
     build/meson-x86_64-pools1`. Record the outcome verbatim. If the leg flails, build the identical
     pools1 leg at the parent commit and A/B the same class: only a delta attributable to this diff
     is a finding; a pre-existing leg failure is reported on #573 with both logs (never waived
     silently, never left unowned).
15.7 Evidence greps pasted into the issue comment: note_frozen_deny() call count == 14;
     `grep -rn 'sizeof(TaskControlBlock) <=' src/kernel` == 1 live assert;
     `grep -rn 'CONFIG_MEMPOOL_' src/ tools/ docs/` == deletion note + guard + doc mention only.

## TEST STRATEGY

**New tests (QE, testbed branch only — this lane's diff carries zero test edits; sketch for QE):**
- `freeze_denies_all_pmm_entry_points` **extension** (src/kernel/test/test_mem_handover.cpp,
  core_memory): add `alloc_page_table() == 0` and `alloc_page_table_err() == PMM_ERR_FROZEN_DENY` to
  the pools1 sweep and bump the latch-count control from 6 to 8; keep the
  `static_assert(CONFIG_STATIC_POOLS_ONLY == 1)` first-assert rule.
- `static_pools_page_table_disabled_after_init` (test_static_pools.cpp, pools1 leg): both entry
  points refuse after mark_init_done()/freeze_pools(); in the `#else` leg assert the flag is 0 and a
  control `alloc_page_table() != 0` still succeeds (both directions explicit; free the page).
- `mempool_top_class_matches_tcb_pin` (core_memory): `MemPool::alloc(kMemPoolMaxBlockSize) !=
  nullptr` (the top class really is that big), `sizeof(TaskControlBlock) <= kMemPoolMaxBlockSize`,
  `MemPool::POOL_COUNT == 9`; frees the block. Pins Increment A at runtime, per arch.
- `mk_inv_no_kernel_alloc` **restructure** (core_memory, pools1 leg): create_user after freeze now
  refuses (it already refused at alloc_contiguous and will now also refuse the page tables); assert
  the refusal is clean — nullptr, PMM free_pages_ref() delta 0, latch still PMM_FROZEN_DENY —
  instead of relying on a live task, so the test cannot pass vacuously.
- `freeze_deny_err_twins_typed` (optional, core_memory): assert alloc_page_table_err returns
  PMM_ERR_FROZEN_DENY (not TABLE_OOM), documenting the deliberate asymmetry.
- Host-side (testbed tooling lane, optional): a check-config negative fixture for dead-name
  resurrection; the dev-lane proof is step 15.4.

**Existing classes to run (this lane):** x86_64 debug core_memory (the deny/pmm/static-pools
groups), core_basic (buffer pool), core_task (TCB creation + kslot), selftest (CI gate); aarch64 +
riscv64 debug core_memory directed. Rationale: the diff touches the two allocator TUs, one header
and one doc set; no scheduler/IPC/lock-order change, so wide aggregates buy nothing this cycle.
run_all_classes.sh (2×3 matrix) is the **release-version** gate, not this phase. Discipline: one
test-history.txt row per run with the measured time; derive timeouts from the last row ×1.5; a
zero-S-line TIMEOUT is a host-load retry on the identical binary, never a green.

## SIL 3 / SAFETY RISKS

- [S1] **The page-table deny changes the pools1 leg's post-freeze behaviour** (pool draws used to
  succeed silently; now they refuse). A cert-leg boot could reach a page-table split mid-map and
  take a path nothing exercises today. — Mitigation: the deny returns the fail-closed shapes callers
  already handle (vmm.cpp:293-296 and :320-323 plus the per-arch page_table_impl splits return
  nullptr/0 → typed failure, no ENSURE/panic on the path); gate 15.6 (3-arch build+link mandatory,
  x86_64 runtime attempt A/B'd if it flails); QE's two deny asserts make the semantics explicit.
  Post-freeze task creation already refused at alloc_contiguous (pmm.cpp:695), so no configuration
  that worked yesterday loses a capability — the leg becomes uniform, not narrower.
- [S1] **A deny added ahead of the pool scan could mask real pool exhaustion** in the pools1 leg
  (TABLE_OOM vs FROZEN_DENY). — Mitigation: entry placement checks the frozen case first and leaves
  the exhaustion paths untouched (identical code below the guard); step 8 returns the specific code;
  the QE optional test pins it.
- [S2] **Removing the task.cpp duplicate assert could read as weakening a build-time guard.** —
  Mitigation: the survivor is strictly stronger (constant-derived, arch-parameterized, co-located
  with the type) **and** Increment A adds the table↔constant equality assert, so the chain
  table→constant→TCB is proven at compile time on all three arches; evidence grep in 15.7.
- [S2] **A bad hoist in mempool.cpp could change pool sizing or add runtime init** (the tables feed
  every pool allocation in the kernel). — Mitigation: values byte-identical, constant-initialized
  .rodata, no guard variable, no change inside the init() loop; proof is core_memory count parity
  plus the mempool tests' exact-count assertions.
- [S2] **The check-config extension could reject a legitimate configuration** and break the release
  matrix. — Mitigation: step 5 greps the repo for live uses before landing; the guard fires only on
  presence of the three deleted names; 15.4 includes the negative fixture and the normal 3-arch run.
- [S2] **Spec drift if the doc steps are skipped** (INV-MK-1 today advertises a closed gap; §7.2 a
  stale status). — Mitigation: steps 11–14 ride in Increment B's diff, so the audit sees them;
  microkernel-transition.md:74 is the normative row a future auditor reads first.
- [S3] **Historical text says "six entry points"** (STATE.md, #563 comments, test comments). —
  Mitigation: not rewritten (historical records); the issue comment states 14/14 with file:line
  evidence.
- [S3] **The six `_err` twins still return generic PMM_ERR_OOM** (accepted #563 S3). — Mitigation:
  explicitly out of scope; this plan neither widens nor silently fixes it, and step 8's FROZEN_DENY
  choice is justified in a diff comment so the asymmetry reads as deliberate.
- [S3] **PROBE-DISABLED comment removal** — comment-only; no tool consumes the marker (repo grep =
  the 2 lines themselves). Listed so the deletion is not mistaken for a lost assertion.
- [S3] **The pools1 leg remains a build+link gate in practice** (documented build rot,
  configuration.md §2.3). — Mitigation: kept visible on #573 and handed to QE with exact commands; it
  is never presented as runtime coverage.

## NON-GOALS

- No test-tree, test_expected_counts, tools/test-policy.tsv or initrd-config edit (QE owns them).
- No flip of the CONFIG_STATIC_POOLS_ONLY / CONFIG_MEM_MGR_BOOTSTRAP defaults (M12/certification).
- No retrofit of the six `_err` twins to PMM_ERR_FROZEN_DENY (separate item).
- No MemPool class resize, no page-table-pool resize, no handover/freeze changes, no bootstrap1 leg.
- No new dmesg catalog row or MemPressureReason value (reuse PMM_FROZEN_DENY + kDmesgBase_PMM+2).
- No change to alloc_page_table's pool-then-fallback algorithm, lock scope or overlay bookkeeping.
- No spec SHALL-text edits — only status/GAP cells that record what landed.
- No tooling cleanup beyond the two dead PROBE-DISABLED lines.

## OPEN QUESTIONS

- **OQ-1 (blocking, ratified in the plan audit — decision recorded, not left to the coder).** Which
  makes alloc_page_table §7.2-compliant: **A (chosen)** deny at entry so the pool fast path refuses
  post-freeze (spec §7.2 "every post-init PMM entry point SHALL be covered"; INV-MK-1's GAP cell
  names this exact pair), or **B** keep the pool drawable post-freeze as a "fixed pool sized at build
  time" and close the item with documentation only (the M4 design-review's option A, which deferred
  the decision here)? This plan implements A (steps 7–8) because B leaves the certification switch
  unable to forbid a post-freeze physical-page allocation and keeps the INV-MK-1 GAP row
  permanently stale. If the audit rules B, steps 7–8 are deleted, steps 9–12 are reworded to record
  the exemption, and #573 closes with a documented exemption instead of a deny.
- **OQ-2 (non-blocking, process).** Steps 11–14 edit normative spec status cells. Precedent treats
  normative tables as owner-reviewed; this plan assumes status/GAP edits that merely record landed
  code are dev-lane, with the diff reference posted on #573 for owner ratification. If prior
  sign-off is required, steps 11–14 degrade to an issue comment.
- **OQ-3 (non-blocking, verification scope).** Is pools1 acceptance build+link on three arches
  (mandatory) plus one x86_64 runtime attempt (best-effort, A/B'd against the parent commit), or must
  the leg boot green before #573 closes? This plan assumes the former — the leg's post-freeze denies
  predate this diff and its runtime expectations are QE-owned.

## CLOSE CRITERIA (#573)

1. Increment A audited APPROVED (audits/report-<ts>.md posted on the issue) — dead-macro close-out
   and single-source pin landed.
2. Increment B audited APPROVED (or OQ-1 ruled B with the exemption recorded in both specs).
3. Evidence greps (15.7): 14/14 alloc entry points deny; exactly one live TCB size assert; no
   CONFIG_MEMPOOL_* macro anywhere.
4. Gates green: 3-arch debug build+link; `make check-style` Errors 0; check-config 0 on three arches
   plus the negative fixture; x86_64 core_memory/core_basic/core_task/selftest and directed
   aarch64/riscv64 core_memory at pre-diff counts; pools1 leg build+link on three arches.
5. One test-history.txt row per run with measured times; no abnormal-termination row unexplained.
6. QE handoff comment on #573 listing the five test deltas (or a testbed issue reference), so
   coverage lands on testbed without this lane touching a test file.
7. Spec cells (§7.2 status, INV-MK-1 GAP) no longer describe a closed gap.
