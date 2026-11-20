# PLAN: F10 (#563) — mem-mgr bootstrap close-out: per-row bounds, compose-time demand check, all-or-nothing handover

**Issue:** #563 (F10, spec §15) · milestone v0.6.1 · base main (M4 kernel half landed 911112f7b, QE half 4ef60e146 on testbed)
**Phase split:** Phase A = verify what M4 already landed (no code). Phase B = the residual F10 owes per
audits/plan-562.md App. D (the F9 handoff), spec §8.2 item 2, INV-MK-2, §7.1/§7.3.

**Retrieval (check-7 evidence, with dispositions):**
- graphify query_graph("Untyped memory handover root task bootstrap mem-mgr") — **used**: surfaced
  untyped_handover.hpp:HandoverStatus, the pmm.hpp freeze/handover accessors, untyped.cpp retype/create,
  note_frozen_deny. **Ignored** the 141 truncated breadth nodes (scheduler/logger boilerplate).
- graphify query_graph("PMM frozen pools BufferPool fallback removal") — **used**: the BufferPool→PMM edge,
  the six alloc_*/_err entry points, mem_pressure. **Ignored** VMM/span nodes (F5 lane).
- graphify query_graph("memory bounds per-row Untyped region mem-mgr bootstrap") — **used**: located
  region(), the nexios_config.h consumer set, PerCpu. **Ignored** the validate_style.py community (tooling).
- graphify shortest_path("untyped_handover.hpp" → "mem-mgr") — **used (weak, 3 hops via types.hpp →
  mempool.hpp)**: proves no in-tree row/PD structure links to handover, so the per-row quota must be built
  here (drives steps 13–15).
- obsidian_search_vault "microkernel" — **used**: audits/design-review-557-r2.md (mem-mgr enforceability
  layers; transfer_all_to has zero callers), audits/plan-557.md.
- obsidian_search_vault "mem-mgr" — **used**: audits/design-review-557-r1.md [S2] never-mem-mgr
  unenforceable + the kPdRoleCount==13 tripwire ⇒ F10 must NOT add PdRole::MEM_MGR (M5/M12, Q-M4-1 A).
- obsidian_search_vault "Untyped" — **used**: audits/design-review-554-code-r1.md (retype/frame/carved
  semantics; cap_untyped 18/18).
- obsidian_search_vault "plan-582" — **used**: located audits/plan-582-transition-compat.md, then read it in
  full (34 lines); no conflict with F10 (its item 2 sequencing note is satisfied — M4 has landed).
- obsidian_search_vault "STATIC_POOLS_ONLY / F10 / M4 / audit report" — **ignored (0 matches)**: no vault
  coverage for M4/F10 (matches the QE note on #563).
- github issue_read #563 + all 10 comments — **used**: landed scope, Q-M4-1..4 decisions, iter-2 audit,
  remaining QE items (handover-caps-cover-free-pool, no-double-claim, freeze-denies, STATIC_POOLS_ONLY leg).

## AFFECTED FILES

- src/kernel/memory/untyped_handover.cpp — boot_handover() becomes all-or-nothing (capacity pre-check +
  reverse rollback); transfer_all_to() gets a destination-capacity pre-check; status() populates runs_capped
  (a dead field today).
- src/kernel/memory/untyped_handover.hpp — contract docs only (all-or-nothing, M5-seam pre-check); no
  signature change.
- src/kernel/memory/mem_handover_errors.hpp — new MEM_HANDOVER_ERR_BOUNDS (+ error_string<> row): the typed
  error for "declared handover demand exceeds a §7.3 bound".
- src/kernel/memory/pmm.hpp — new handover_dropped_runs() accessor; reword the :74-75 "every post-freeze
  entry point denies" claim to the page-table-pool-exempt form (see OQ-1).
- src/kernel/memory/pmm.cpp — g_handover_dropped counter + increments at both reservoir drop sites and the
  marking-loop cap; accessor. (Deny placement inside alloc_page_table* stays F21's — step 18.)
- src/kernel/cap/untyped.hpp, src/kernel/cap/untyped.cpp — UntypedMem::live_count() (relaxed read of
  g_live_untypeds, untyped.cpp:45) so the handover can pre-check CONFIG_CAP_MAX_UNTYPED headroom.
- src/kernel/cap/cap.hpp, src/kernel/cap/cap.cpp — CNode::free_slot_count(), a bounded O(CSLOT_COUNT) scan
  under the leaf slot lock (no allocation, no reference taken) for the two handover capacity checks.
- src/kernel/task/task.hpp — TCB object_budget_/object_count_ members + effective_object_budget()/
  set_object_budget(); attach_object() gains a [[nodiscard]] bool result; detach_all_objects() bound comes
  from the accessor (was the flat CONFIG_MAX_PER_TASK_OBJECTS).
- src/kernel/task/task.cpp — attach/detach accounting, walk bound, and the two attach call sites (:241
  sporadic server, :272 ensure_cspace) handling a refusal fail-closed.
- src/kernel/task/boot_sdf.hpp — RowBounds struct, sdf_role_bounds(PdRole), pure sdf_check_bounds(PdRole,
  const CapSpec*, uint64_t, errors::SdfError&); PdRole enum untouched (13-row tripwire).
- src/kernel/task/boot_sdf.cpp — per-row bounds table (13 rows) + the check at the head of sdf_compose()
  (:670-675) + task.set_object_budget(row.object_budget) on a successful compose.
- src/kernel/task/sdf_errors.hpp (new) — X-macro SdfError (SDF_ERR_OK, SDF_ERR_BOUND_EXCEEDED,
  SDF_ERR_ROW_TOO_WIDE, SDF_ERR_OBJECTS_EXCEEDED, SDF_ERR_CSPACE_EXCEEDED) with error_string<>.
- src/kernel/nexios_config.h — one new key CONFIG_MAX_PER_TASK_OBJECTS_INFRA 256 (+ guard block:
  >= CONFIG_MAX_PER_TASK_OBJECTS and <= 65535). M4 owns this file, so in scope; keep to key + guard +
  comment (~8 lines), no reflow.
- src/kernel/log/dmesg_catalog.hpp — one catalog row for the SDF bound rejection (next free code in the
  TASK base; step 16 forbids picking a +N code blind).
- docs/specs/memory.md — freeze/handover contract: all-or-nothing, demand-vs-bound, per-row object budget.
- docs/specs/configuration.md — the two flag legs (CONFIG_STATIC_POOLS_ONLY=1, CONFIG_MEM_MGR_BOOTSTRAP=1)
  with exact per-arch meson setup -Dconfig_defs=... lines.
- **No test file is touched** (src/kernel/test/** is read-only here; QE owns the sketch in §TEST STRATEGY).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-F10-1 — single ownership of a handed-over frame.** A frame is either PMM-free or handover-marked +
  cap-owned, never both. Enforced today by bitmap marking inside freeze_pools() (pmm.cpp:600-607) plus the
  handover_owns() guard in free_page/free_page_err (:995, :1408). Phase B keeps minting and marking in
  lockstep by making boot_handover() all-or-nothing (step 7): the current partial-failure path can mint
  runs 0..k, return with g_done == false, and a retry re-mints those same runs into further slots — two
  caps, one region.
- **INV-F10-2 — freeze is once, monotonic, never rewound.** g_pmm_frozen (pmm.cpp:82) never clears and
  freeze_pools() is idempotent (:508). The handover table and freeze flag are TU statics outside the
  snapshot buffer, while the bitmap marking happens pre-baseline, so snapshot_restore() rewinds to the
  marked state — consistent by construction, and a QE test pins it.
- **INV-F10-3 — freeze ordering.** Freeze runs at the end of composition (taskdefs.cpp:869-878) inside one
  arch::IrqGuard, BEFORE the per-arch AP start gate (smp::publish_scheduler_ready(): :884 x86_64,
  :889 aarch64, :894 riscv64). Phase B must not move this point: post-gate, APs allocate concurrently and
  the freeze walk would race real allocations.
- **INV-F10-4 — lock order.** freeze_pools() takes pmm_lock_ (IrqSpinLock) alone and returns before
  handover starts; boot_handover() therefore runs with no pmm_lock_ held and takes, in order, CNode::lock_
  (leaf) → MemPool/ResourceTracker. Never take pmm_lock_ while holding a CNode lock; never take
  scheduler_lock_ on either path. CNode::free_slot_count() is a single leaf take (same class as lookup())
  and must not be called with scheduler_lock_ held.
- **INV-F10-5 — deny paths allocate nothing.** note_frozen_deny() (pmm.cpp:43-49), mem_pressure::latch() and
  the new SDF-rejection diagnostic are zero-alloc, lock-free, no Logger — safe inside pmm_lock_/OOM windows
  (the latch header already states this contract).
- **INV-F10-6 — demand ≤ bound at compose time (INV-MK-2).** Every row's declared demand (caps, CSlots,
  kernel objects, Untyped quota, death watches) is checked against its configured bound before any mint; a
  violation is a typed refusal with no side effect, never a partial install.
- **INV-F10-7 — the object-list walk is exactly as long as its budget.** detach_all_objects() must never be
  the shortest bound in the chain: its trip count comes from effective_object_budget() (row-set, clamped),
  and attach_object() refuses past that same value, so a row owning N objects always has a walk of >= N.
- **INV-F10-8 — storage discipline.** The new TCB members must fit the largest MemPool class
  (task.hpp:1474 static_assert; kMemPoolMaxBlockSize = 8192 x86_64/riscv64, 16384 aarch64). Place both in
  the existing 32-bit budget group to reuse padding; the build is the proof on all three arches.
- **Cross-arch boundary.** The freeze/handover point is arch-neutral (taskdefs.cpp); what differs per arch
  is the PMM window (window_base_page_..window_end_page_, pmm.cpp:108-110) — x86_64 base 0 over the
  multiboot2 usable map, aarch64 from the DTB memory node, riscv64 the low-half-identity window minus device
  leaves. Consequences: (a) run counts and sizes are NOT portable, so tests assert invariants
  (pages_handed + pages_retained, caps_minted == runs_total) and never a literal run count; (b) handover
  frames are marked USER-owned (pmm.cpp:605) because VMM user mappings ENSURE(is_user_page) on all three
  arches — that bit is load-bearing, not cosmetic.

## STEP-BY-STEP CHANGES

### Phase A — verify the landed M4 half (no code, evidence only)

1. **Confirm landed anchors** (each was read this session; re-confirm at the implementation tip):
   | Item | Anchor | State |
   |---|---|---|
   | freeze at composition end | taskdefs.cpp:869-878 (IrqGuard → freeze_pools() → bootstrap-gated boot_handover() → apply_boot_handover) | LANDED |
   | freeze + handover accounting | pmm.cpp:506-620 (run table :88-92, handover_pages/retained/run_*) | LANDED |
   | double-ownership guard | pmm.cpp:995 / :1408 (handover_owns() in both free paths) | LANDED |
   | root Untyped registry | untyped_handover.cpp:27-87, :89-136 (M5 transfer seam), :138-147 (status) | LANDED |
   | BufferPool PMM fallback removed | buffer_pool.cpp:354-415 (alloc_err, :381 latch, :386 BUF_ERR_OOM) | LANDED |
   | STATIC_POOLS_ONLY denies (6 entries + _err twins) | pmm.cpp:453,680,724,764,806,835,873,909,1218,1259,1295,1336 | LANDED |
   | sys_frame_create frozen deny (flag-scoped) | syscall_handlers_shm.cpp:60-63 | LANDED |
   | per-task budget hooks incl. user pages | pmm.cpp is_over_budget at every alloc entry; scheduler_budget.cpp:60 | LANDED |
   | R-MEM pressure latch | mem_pressure.hpp:18-25, pmm.cpp:43 | LANDED (M5 sink stubbed) |
   | M4 config keys | nexios_config.h:302-330 (BOOTSTRAP 0, MAX_RUNS 64, RESERVE_PAGES 512, pressure pct) | LANDED |
   | CAP_MAX_UNTYPED 64 | nexios_config.h:590 | LANDED (Q-M4-2 B) |
   | TCB size assert | task.hpp:1471-1475 | LANDED |
2. **Re-baseline the gates on the clean tree** before any Phase-B edit (one boot per class, one
   test-history.txt row each, measured times only): x86_64 core_memory, capability, core_basic, core_task,
   ipc, selftest; aarch64 and riscv64 core_memory + capability directed. run_all_classes.sh is the
   release-version path — not for this phase. Compare with the #563 figures (x86_64 core_memory 129/129
   after the QE port, capability 170/170).
3. **Confirm** git diff --stat -- src/kernel/test is empty. If the testbed QE half has not merged to main,
   record the gap (it does not block Phase B code).

### Phase B — the residual F10 owes

4. src/kernel/nexios_config.h: add CONFIG_MAX_PER_TASK_OBJECTS_INFRA (default 256) directly after
   CONFIG_MAX_PER_TASK_OBJECTS (:564-566) with a comment naming Q7 (256 for mem-mgr/supervisor rows, 32
   elsewhere) and stating that no row uses 256 yet (it is the ceiling the setter clamps to). Add the guard
   pair in the guard block near :1472: >= CONFIG_MAX_PER_TASK_OBJECTS and <= 65535 (the TCB field is
   uint16_t, so the guard is what keeps the assignment lossless).
5. src/kernel/cap/untyped.hpp/.cpp: add static uint32_t live_count() noexcept returning a relaxed
   __atomic_load_n(&g_live_untypeds). Doc-block: "demand input for the handover pre-check; a read, not a
   reservation — the bound itself is still enforced in alloc_object()."
6. src/kernel/cap/cap.hpp/.cpp: add uint32_t free_slot_count() const noexcept — one SpinLockGuard on lock_,
   bounded for(i < CONFIG_CSLOT_COUNT) counting unoccupied slots, no reference taken, 0 for a revoked node.
   Doc-block the lock class ("leaf; same take class as lookup(); never with scheduler_lock_ held") and that
   it is advisory — install() remains the authority.
7. src/kernel/memory/untyped_handover.cpp — make boot_handover() all-or-nothing (INV-F10-1):
   (a) keep the g_done guard, then read runs = PMM::handover_run_count() (0 → NO_FREE_RUNS);
   (b) pre-check demand: headroom = CONFIG_CAP_MAX_UNTYPED - UntypedMem::live_count(); free_slots =
   cs->free_slot_count(). If runs > headroom or runs > free_slots → mem_pressure::latch(HANDOVER_CAPPED) +
   dmesg ERROR + return the new MEM_HANDOVER_ERR_BOUNDS (no mint, no g_run_count bump, g_done stays false);
   (c) mint loop as today, but on any failure (create_subrange null, install < 0) roll back: walk the
   installed entries in reverse calling cs->remove(g_runs[i].slot), zero g_run_count/g_source, latch +
   dmesg, return the typed error (replaces the bare returns at :57-59 and :68-70);
   (d) success path unchanged (g_done = true, OK, dmesg +INFO row).
8. src/kernel/memory/untyped_handover.cpp — transfer_all_to(): inside the existing phase-1 verify loop also
   read dcs->free_slot_count() once and refuse with CSPACE_FULL BEFORE phase 2 when free_slots < g_run_count
   (closes the iter-1 S3 "transfer pre-check"; a partial phase-2 move today leaves caps split across two
   CSpace with g_transferred == false). No API change.
9. src/kernel/memory/pmm.cpp/.hpp — add g_handover_dropped + handover_dropped_runs(); increment it (a) in
   commit_run when a run is evicted as smaller than the reservoir tail and (b) in the marking loop when
   g_handover_runs already equals CONFIG_MEM_HANDOVER_MAX_RUNS. Then set st.runs_capped =
   PMM::handover_dropped_runs() in untyped_handover::status() (:138-147) — the field exists and is never
   written today (dead diagnostic = S3 finding).
10. src/kernel/task/task.hpp — add to the budget group: uint16_t object_budget_{0}; uint16_t
    object_count_{0}; plus uint32_t effective_object_budget() const noexcept (0 →
    CONFIG_MAX_PER_TASK_OBJECTS, else clamped to [CONFIG_MAX_PER_TASK_OBJECTS,
    CONFIG_MAX_PER_TASK_OBJECTS_INFRA]) and void set_object_budget(uint32_t) noexcept (same clamp). **Zero
    means row default on purpose**: TCBs are memset-zeroed at all four creation sites (LEARNINGS caution) and
    a zero default keeps every existing task at 32 without touching creation paths or the snapshot
    byte-capture.
11. src/kernel/task/task.cpp — attach_object() → [[nodiscard]] bool: refuse (no mutation, return false) when
    object_count_ >= effective_object_budget(), else link + ++object_count_. detach_object() and
    detach_all_objects() decrement per unlink (floor at 0). Call sites: init_sporadic_server (:241) — on
    refusal release the server and leave the task without one (typed path, no panic); ensure_cspace()
    (:266-274) — on refusal node->release() and leave cspace_ null (fail closed: "no cap, no crash", the same
    shape as a failed CNode::create).
12. src/kernel/task/task.cpp:319-338 — detach_all_objects() loop bound becomes effective_object_budget()
    instead of the flat macro; keep the poison guard and the keep_cspace behaviour untouched.
13. src/kernel/task/sdf_errors.hpp (new) — X-macro enum + error_string<> (module-error-header rule,
    CODING_STYLE §5.3); every new refusal at the sdf_check_bounds() boundary carries a specific code, never
    a generic false.
14. src/kernel/task/boot_sdf.hpp/.cpp — struct RowBounds { uint32_t max_caps; uint32_t object_budget;
    uint32_t untyped_quota; uint32_t death_watches; } (all members initialized, §10.3), RowBounds
    sdf_role_bounds(PdRole) backed by one constexpr 13-row table (today: max_caps = kMaxCapsPerRole,
    object_budget = CONFIG_MAX_PER_TASK_OBJECTS, untyped_quota = 0, death_watches = 0; the mem-mgr/
    supervisor 256 / Untyped-quota rows land with their SDF rows at M5/M12 — the mechanism, not a
    speculative row, is what F10 owes), and the pure bool sdf_check_bounds(PdRole, const CapSpec*, uint64_t
    count, errors::SdfError&) checking count <= row.max_caps; count <= row.object_budget;
    count + kRowReservedSlots <= CONFIG_CSLOT_COUNT (kRowReservedSlots a named constant, default 0);
    row.untyped_quota <= CONFIG_CAP_MAX_UNTYPED; row.death_watches <= CONFIG_CAP_MAX_DEATH_WATCHES. No
    locks, no state — pure, so QE exercises the negative branches directly.
15. src/kernel/task/boot_sdf.cpp — in sdf_compose(), after sdf_row() and before the sdf_policy_only block
    (:672-676): run sdf_check_bounds(); on failure emit the new dmesg row and return false (fail closed,
    nothing minted). On the success path (after the rollback-committed end of the mint loop) call
    task.set_object_budget(row.object_budget). Do not touch the existing count > kMaxCapsPerRole guard (it
    stays as the stack-array bound) and do not add a PdRole value.
16. src/kernel/log/dmesg_catalog.hpp — add one row for the SDF bound rejection. First grep the catalog for
    the next free code in the chosen base (kDmesgBase_TASK; PMM is occupied to +4, BUFPOOL to +1) — a blind
    +N collides silently (LEARNINGS trap) — then reference the code, never a literal, from step 15.
17. docs/specs/memory.md + docs/specs/configuration.md — document the all-or-nothing handover contract, the
    demand-vs-bound check and the per-row object budget (memory.md), and the two flag legs with exact
    per-arch commands (configuration.md): meson setup build/meson-<arch>-pools1 --cross-file
    cross/<arch>-elf.txt -Darch=<arch> -Dconfig_defs=-DCONFIG_STATIC_POOLS_ONLY=1, and the -bootstrap1 twin
    with -DCONFIG_MEM_MGR_BOOTSTRAP=1. Both flags default to 0 and no gate builds them today (the M4 dev
    built meson-x86_64-pools1 by hand) — that is build rot, so the legs get documented and enumerated for QE.
18. F21 (#573) boundary record — one comment on #563 (no code duplication): alloc_page_table()
    (pmm.cpp:949-979) and alloc_page_table_err() (:1372-1396) reach their pool free-list before any deny;
    per OQ-1 this plan treats the page-table pool as a §7.2 fixed kernel pool (legal post-freeze) and only
    rewords pmm.hpp:74-75. The deny placement and the alloc_user_page/_table assert surface stay #573's; if
    #573 lands first, this step shrinks to a doc-only diff.
19. Run the gates (§TEST STRATEGY), post the results as a #563 comment, then request the SIL 3 audit with
    git diff main -- <files> > audits/pending_patch.diff (diff-patch protocol).

## TEST STRATEGY

**New tests (QE, testbed branch only — the production diff carries zero test edits; sketch handed to QE):**
- untyped_handover_caps_cover_free_pool (core_memory) — assert status().frozen, caps_minted == runs_total,
  Σ region(i).pages == pages_handed, pages_handed + pages_retained == free_pages_at_freeze, and
  runs_capped == handover_dropped_runs().
- untyped_handover_no_double_claim (core_memory) — free_page(region(0).phys) leaves PMM::free_memory()
  unchanged (bitmap untouched) and free_page_err() returns PMM_ERR_INVALID; a second boot_handover(root)
  returns ALREADY_FROZEN with caps_minted unchanged.
- untyped_handover_bounds_precheck_rollback (core_memory) — pure leg: headroom < runs returns BOUNDS with
  caps_minted == 0; integration leg: a snapshot-restored retry after a forced capacity shortfall leaves
  g_run_count == 0 with a tracker delta of 0 (proves the rollback, not just the pre-check).
- untyped_handover_snapshot_immune (core_memory) — after snapshot_create()/snapshot_restore() the freeze
  flag, run table and marked-bitmap state are unchanged (INV-F10-2); alloc_page() still never returns a
  handover frame.
- mem_pressure_latch_once (core_memory) — two latches → count() == 2, exactly one dmesg row,
  pending_notify() true, clear() resets to NONE.
- freeze_denies_all_pmm_entry_points (core_memory, meaningful only in the pools1 leg) — assert
  CONFIG_STATIC_POOLS_ONLY == 1 first (never silently pass), then each of the six entry points returns 0 /
  typed error post-freeze and the deny latched once; the #else branch asserts the flag is 0 and a control
  alloc still succeeds (fail direction explicit both ways).
- sdf_bounds_reject_over_demand (capability) — pure sdf_check_bounds() negatives (count > max_caps; count >
  object_budget; synthetic bounds with untyped_quota > CAP_MAX_UNTYPED and death_watches >
  CAP_MAX_DEATH_WATCHES) each returning the specific SdfError, plus a positive sweep over all 13 real rows.
- object_budget_walk_bound (core_task) — set a 4-object budget, attach 4 and refuse the 5th (attach_object
  false, list length unchanged), detach_all_objects() walks all 4; tracker delta 0; budget 0 means 32.
- mk_inv_no_kernel_alloc (core_memory, §13.3 planned test, INV-MK-1 after M4) — composite of the pools1
  deny sweep + "BufferPool exhaustion returns typed OOM with no PMM movement" (the latter already exists on
  testbed as buffer_pool_no_pmm_fallback, 4ef60e146 — do not duplicate).
- **Flag legs**: run the M4 gate set under -DCONFIG_STATIC_POOLS_ONLY=1 and under
  -DCONFIG_MEM_MGR_BOOTSTRAP=1 on all three arches; each leg's first assert is the in-guest CONFIG_* == 1 so
  a mis-built leg fails loudly instead of passing vacuously.

**Existing classes to run (implementation lane, one boot per class):**
- x86_64 debug: core_memory (freeze/handover/budget), capability (compose bounds, cap_untyped, retype),
  core_basic (buffer-pool contract, 98/98 baseline), core_task (TCB field + attach accounting), ipc
  (BufferPool OOM path), selftest (157/157 CI gate), servers (the M12 gate class; PD rows unaffected).
- aarch64 + riscv64 debug, directed: core_memory and capability (the per-arch lists exclude these by design —
  never substitute the x86 list), plus the pools1/bootstrap1 legs.
- Rationale: core_memory + capability are the §14 M4 gate classes; servers is the M12 row; core_task is where
  a per-TCB field regression surfaces; selftest is the CI gate. The 2x3 matrix is the release-version gate,
  not this phase.
- Discipline: append one test-history.txt row per run (measured times), derive timeouts from the last row
  x1.5, and treat a zero-S-line TIMEOUT as a host-load retry on the identical binary — never as green.

## SIL 3 / SAFETY RISKS

- [S1] **Partial handover + retry double-mints a region.** Today boot_handover() returns mid-loop with
  g_done == false and g_run_count > 0 (untyped_handover.cpp:57-70), so a retry installs a second cap for
  already-transferred frames: two owners, one region. — Mitigation: step 7 (pre-check + reverse rollback) +
  the QE rollback test. Highest-value correctness item in F10.
- [S1] **Failed handover after marking = permanent pool loss.** freeze marks frames allocated
  (pmm.cpp:600-607); a later handover failure leaves them marked-but-unminted (no cap can ever free them).
  — Mitigation: the pre-check makes that failure unreachable in the composed configuration; any residual
  failure latches HANDOVER_CAPPED + dmesg ERROR + typed result and stays visible in the boot log; the gate
  test asserts result == OK and pages_handed + pages_retained == free-at-freeze on all three arches.
- [S1] **TCB growth breaks the MemPool class fit.** Two new members could push sizeof(TaskControlBlock) past
  kMemPoolMaxBlockSize (8192 B on x86_64/riscv64). — Mitigation: both members in the existing 32-bit budget
  group (padding reuse), task.hpp:1474 static_assert as the build-time proof, measured sizeof recorded per
  arch on the issue.
- [S1] **attach_object refusal changes ensure_cspace behaviour.** A reachable over-budget attach must leak
  neither the CNode nor a half-initialized task. — Mitigation: step 11 releases the object and leaves the
  pointer null (identical shape to a failed CNode::create); the refusal path gets an explicit test.
- [S2] **Corrupted object_budget_ unbounding the walk.** A poisoned/restored TCB field could make
  detach_all_objects() trip on garbage. — Mitigation: effective_object_budget() clamps to the
  [32, INFRA] window, so the walk is bounded by a compile-time constant regardless of the stored value
  (mechanical defence, not trust).
- [S2] **Compose-time check regressing green rows.** A mis-sized CSLOT/max_caps relation would reject a row
  that composes today. — Mitigation: values-only table over the existing 13 rows, a positive sweep test per
  row, and the check runs before any mint (no partial install).
- [S2] **New CNode API under the wrong lock.** free_slot_count() inside a scheduler_lock_ window would break
  the leaf-lock discipline. — Mitigation: documented lock class, single take, both call sites boot-time
  (pre-gate, no scheduler lock), no release inside the lock.
- [S2] **Deny legs never compiled (build rot).** Both flags default 0 and nothing builds them; the
  PMM_ERR_FROZEN_DENY-did-not-compile incident is the precedent. — Mitigation: step 17 documents the legs and
  QE runs them per arch with an in-guest CONFIG_* == 1 assert first.
- [S2] **dmesg code collision.** A blind +N silently collides. — Mitigation: step 16 greps the catalog for the
  next free code and adds the row in the same hunk.
- [S3] **runs_capped dead field.** Never written today (untyped_handover.hpp:37 vs status()); a
  defined-but-unused diagnostic is an audit finding. — Mitigation: step 9 populates it.
- [S3] **Hard-coded aggregate init in sdf_compose.** int slots[kMaxCapsPerRole] = {-1 x 8} (boot_sdf.cpp:695)
  must follow CONFIG_DRIVER_PD_MAX_CAPS if the compose bound is ever resized.
- [S3] **pmm.hpp:74-75 over-claims.** "every post-freeze entry point denies" is false for the page-table pool
  path. — Mitigation: reword per OQ-1; no semantic change in a default build.
- [S3] **Doc/spec drift.** INV-MK-1's row (microkernel-transition.md:74) still lists alloc_user_page /
  alloc_page_table as ungated. — Mitigation: raise on the issue for the spec owner (normative table, not
  F10's edit under Q6 sign-off rules) rather than editing it here.

## OPEN QUESTIONS

- **OQ-1 (post-freeze page-table pool legality, F21 boundary).** alloc_page_table() (pmm.cpp:949-979) and
  alloc_page_table_err() (:1372-1396) draw from the reserved page-table pool BEFORE any CONFIG_STATIC_POOLS_ONLY
  deny, and that pool stays allocatable post-freeze. Is that the intended §7.2 semantics ("the kernel keeps
  fixed pools sized at build time" ⇒ this is such a pool, so post-freeze pool draws are legal and only the
  general-PMM fallback denies), or must the pool path deny too? **This plan assumes the former (option A)** —
  consistent with §7.2 and with the fact that the fallback leg already denies via alloc_page() — and
  therefore only rewords pmm.hpp:74-75 (step 18) instead of adding gates. Option B (deny inside the pool
  path) would make ELF/libc page-table allocation fail after composition in ways nothing tests today; it
  needs owner sign-off and belongs to #573's lane if chosen. If the owner picks B, step 18 becomes a handoff
  and the pools1 leg gains two deny assertions.
