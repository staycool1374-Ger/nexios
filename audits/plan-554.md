# PLAN: #554 / F1 — Object model: CapType += TCB/VSpace/PageTable/Notification/SchedContext, per-invocation retype target

```
Base: main @ adc03dbb9 ("#582 scheduler strand + scheduler_lock_ freeze", refs #582) — anchor verified
      via GitHub commits API on main, 2026-11-18.
Lane: dev / main. Production kernel code only. src/kernel/test/** is READ-ONLY (test sanctity);
      new tests go to testbed via QE (see TEST STRATEGY). Nothing is committed by the planner.
Normative reads: docs/specs/microkernel-transition.md §4.1 (229-233), §4.2 (235-244), §13.2-§13.3
      (783-804), §14 M11 row (843), §15 F1 row (858), §16 Q2 (885, reply objects DEFERRED);
      docs/specs/cspace.md §2.8.2 (289-380, incl. the "Frame only / object-storage-in-region" note
      at 375); audits/plan-582-transition-compat.md (all 34 lines, 4 named follow-ups).
Graphify/vault retrieval (audit check-7 artifacts, all run this session):
  1. graphify query_graph "capability system CapType retype Untyped target type" -> 76 nodes;
     surfaced untyped.hpp:40 (UntypedMem), untyped.cpp:132 (retype), untyped.cpp:49 (alloc_object),
     cap.cpp:169 (lookup) / cap_types.hpp:131 (handle_slot) / CNode, all community 126, kernel_object.hpp:60.
     DISPOSITION: USED — it named exactly the five TUs this plan touches and confirmed
     untyped/retype are one community with CNode+lookup.
  2. graphify shortest_path "Untyped" -> "TaskControlBlock": 4 hops, only via
     untyped.hpp --imports--> types.h --imports--> cpu_context.hpp --contains--> TaskControlBlock.
     DISPOSITION: USED — proves there is no Untyped->TCB object edge today (no TCB object exists),
     which fixes the F1 scope as vocabulary + carved storage, not scheduler integration.
  3. graphify get_neighbors "retype()" -> calls {lookup, occupied_count, policy_allows,
     claim_once, create_subrange, reset_claim}; contains {untyped.cpp}.
     DISPOSITION: USED — matches the failure-path inventory enumerated in STEP 5.
  4. graphify query_graph "kernel object capability registry KernelObject kind" -> 585 nodes.
     DISPOSITION: IGNORED (too broad; only KernelObject:kernel_object.hpp:60 retained, already read).
  5. obsidian obsidian_search_vault x3 ("microkernel transition capability retype",
     "retype CapType Untyped", "capability CSpace") -> 0 matches each (vault "jarvis").
     DISPOSITION: no vault note covers this subsystem, so specs/audits were read in-repo instead
     (listed above). Recorded as a gap, not a skip.
Staleness note: the graph has only 3 edges into CapType and no node for the new types (expected —
  they do not exist yet). Files were treated as truth for every line number below; every anchor was
  re-opened at adc03dbb9 during this session.
```

## AFFECTED FILES

- `src/kernel/cap/cap_types.hpp` — extend `CapType` (:32-44) with TCB=11, VSpace=12, PageTable=13,
  Notification=14, SchedContext=15 (append-only, existing 0-10 untouched); add
  `kCapTypeLast`/`cap_type_valid(uint64_t)`; static_assert-pin the new values; document the
  Task-vs-TCB split (spec §4.1: "TCB (split from Task as the schedulable object)").
- `src/kernel/cap/object_sizing.hpp` (NEW) — the fixed-size object table (compile-time only, no
  code): `struct ObjectSizing { size_t bytes; size_t align; bool carvable; bool variable_size; }` +
  `constexpr ObjectSizing object_sizing(CapType)` + named constants for the five kinds.
- `src/kernel/cap/cap_errors.hpp` (NEW) — X-macro `errors::CapError` (pattern: sync_errors.hpp:30-68,
  newest precedent mem_handover_errors.hpp:28-61): OK + 12 typed codes.
- `src/kernel/cap/untyped.hpp` — remove `CapType retype_target` (:45); `create` (:51-52) and
  `create_subrange` (:58-60) lose the `CapType target` parameter; class doc (:31-39) rewords
  "owns a range + may be retyped" with no fixed type.
- `src/kernel/cap/untyped.cpp` — `alloc_object` (:49-66) drops `target`; `create` (:70-95) and
  `create_subrange` (:97-106) drop the `target != Frame` rejects (:73-74, :101-102); `retype`
  (:132-237) rebuilt per STEP 5.
- `src/kernel/cap/carved_object.hpp` / `carved_object.cpp` (NEW) — `CarvedObjectCap : KernelObject`:
  MemPool descriptor + PMM payload for the five fixed-size kinds (FrameCap pattern: frame.hpp:29-65,
  frame.cpp:36-93). Zeroes its payload; dispose() returns payload pages (frame.cpp:78-81 precedent).
- `src/kernel/cap/cap.hpp` — `retype` declaration (:185-198): trailing `errors::CapError *err_out =
  nullptr`; doc block rewritten for per-invocation target, fixed sizes and typed errors.
- `src/kernel/syscall/syscall_handlers_cap.cpp` — `sys_cap_retype` (:115-135): reject
  out-of-range `target_type` before the enum cast (:128-129), pass `&err`, keep the frozen
  `-1`/handle return, debug-only typed diagnostic on the failure path.
- `src/kernel/ramdisk/ramdiskd.cpp` — the two call sites (`create` :62-63, `retype` :107) get the
  dropped argument / typed error; the existing dmesg failure row (:71-74) carries the code.
- `docs/specs/cspace.md` — §2.8.2 object snippet (:293-324), retype validation list (:345) and the
  "Retype targets — Frame only in iteration 1" paragraph (:375) are stale after this change;
  rewrite them for the carvable set, per-invocation target and typed errors. Non-normative doc.
- NOT touched: `src/kernel/test/**` (test sanctity), `test_expected_counts.hpp` (QE),
  `docs/specs/microkernel-transition.md` (normative spec; needs owner sign-off — see OPEN QUESTIONS),
  `src/kernel/nexios_config.h` (M4/#563 has uncommitted edits there per transition-spec Appendix A:903),
  `src/meson.build` (new .cpp files are auto-discovered by the `find` at :65-81 / :122-134).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-RT-1 single owner per byte.** For every byte of an Untyped region exactly one owner exists at
  any instant: the Untyped itself (`owns_region()` true), a `FrameCap`, a `CarvedObjectCap`, or the
  child Untyped created by the carve. Every retype failure path must restore this (the existing
  stretch/fail-closed logic at untyped.cpp:187-228 is the reference; it is preserved verbatim apart
  from the target-object factory call).
- **INV-RT-2 no cap spinlock across allocation/free.** Cap locks are taken only for the policy read
  (untyped.cpp:138-144), `lookup` (cap.cpp:169-195) and `install`/`remove`
  (cap.cpp:94-116, :118-138). PMM, MemPool, memset and dispose of the previous owner stay outside the
  locks (cap.cpp:54-57, :133-137). The new code adds no lock, no nesting, no new order.
- **INV-RT-3 no IRQ/scheduler interaction.** retype runs in syscall (task) context: no IrqGuard, no
  `scheduler_lock_`, no `reschedule()`, no BLOCKED transition, no ISR-reachable path. Therefore
  #582's INV-APPLY-1/2/3, INV-PARK-2 and INV-RQ-1/2 are untouched (audits/plan-582-transition-compat.md
  §Aligned rows 1-9, follow-ups 1-4 all live outside the cap subsystem).
- **INV-RT-4 bounded work.** The only new loop is zeroing at most `kTcbObjectBytes` (16 KiB) — bounded
  by a compile-time constant; no unbounded scan, no dynamic allocation, no blocking wait.
- **INV-RT-5 typed error totality.** `err_out` (when non-null) is written exactly once on every
  failure path and is untouched on success; it is a caller-stack pointer used only in task context,
  so no atomics and no cross-CPU visibility rule applies.
- **INV-RT-6 ABI is append-only.** CapType values 0-10 and their meanings are frozen (cap_types.hpp is
  a syscall-visible ABI — `sys_cap_retype` casts the raw arg at :128-129); new values are pinned by
  static_assert. `SYS_CAP_RETYPE` number (56), its arity and its `-1`/handle return encoding are
  frozen (T-1/Q3 freeze; syscall.hpp:86, :500).
- **INV-RT-7 accounting symmetry.** Each `CarvedObjectCap` = +1 `cap_objects` /`track_cap_object_add`
  (ResourceTracker, frame.cpp:46) and -1 on dispose; each carved page is a `pmm_pages_used` delta that
  returns to 0 when the cap is released; `CONFIG_CAP_MAX_UNTYPED` child accounting
  (untyped.cpp:51-53) is unchanged.
- **INV-RT-8 existing semantics preserved.** Prefix-only PAGE-aligned carve, exhaustion model (parent
  spent + child remainder), non-destructive validation before the claim, fail-closed after the claim,
  `claim_once()` single winner, WRITE-right requirement on the Untyped slot. The exhaustion-model
  reading of spec §4.2's "physical range and watermark" is deliberate: the child-Untyped chain *is*
  the watermark, and replacing it would break locked-in tests (see SIL risks, S2-a).
- **INV-RT-9 arch neutrality.** All new logic is arch-independent and sizes come from
  `CONFIG_PAGE_SIZE` (nexios_config.h:137-164), which is exactly `arch::PAGE_SIZE` on all three
  arches (x86_64 page_table_impl.hpp:168, aarch64 :110, riscv64 :119).

## STEP-BY-STEP CHANGES

1. **cap_types.hpp — type vocabulary.** Insert after `Wdog = 10` (:43):
   `TCB = 11` (schedulable object; distinct from `Task = 1`, which stays the join/thread authority),
   `VSpace = 12`, `PageTable = 13`, `Notification = 14`, `SchedContext = 15`. Add
   `constexpr CapType kCapTypeLast = CapType::SchedContext;` and
   `constexpr bool cap_type_valid(uint64_t raw) noexcept { return raw <= kernel::to_underlying(kCapTypeLast); }`
   (uses the existing `to_underlying`; no new include). Add static_asserts pinning 11-15 and update
   the enum's doc comment (:30-31) to say "carvable fixed-size kinds: Frame, TCB, VSpace, PageTable,
   Notification, SchedContext" and to record that Endpoint/CNode/Mmio/Irq/IoMmuDma/Msix/Wdog/
   Untyped/Task/Null are NOT retype targets (they are MemPool-resident, cspace.md:375).
2. **object_sizing.hpp (new).** `#pragma once`, include `<types.hpp>`, `<kernel/nexios_config.h>`,
   `<kernel/cap/cap_types.hpp>`. Define `struct ObjectSizing { size_t bytes; size_t align; bool
   carvable; bool variable_size; };` and constants:
   `kObjectBytesPageTable = CONFIG_PAGE_SIZE`, `kObjectBytesVSpace = CONFIG_PAGE_SIZE`,
   `kObjectBytesNotification = CONFIG_PAGE_SIZE`, `kObjectBytesSchedContext = CONFIG_PAGE_SIZE`,
   `kObjectBytesTcb = 4 * CONFIG_PAGE_SIZE` (16 KiB = worst case `sizeof(TaskControlBlock)`, 8256 B on
   aarch64 per task.cpp:946-951; uniform page count on all three arches — INV-RT-9), plus
   `kObjectBytesMax = kObjectBytesTcb`. `constexpr ObjectSizing object_sizing(CapType t)`: switch
   over the six carvable kinds (Frame -> {0, PAGE_SIZE, true, /*variable=*/true}), the five fixed kinds
   -> {bytes, PAGE_SIZE, true, false}; `default` -> {0, 0, false, false}. `static_assert` that every
   fixed size is a non-zero multiple of `CONFIG_PAGE_SIZE`. Doc-comment each kind with its consumer
   issue (TCB -> #557 F4, VSpace/PageTable -> #558 F5, SchedContext -> #190 M13, Notification ->
   §8.1/§5 sync work) so the audit sees named consumers, not speculative types. No .cpp, no state.
3. **carved_object.hpp/.cpp (new).** `class CarvedObjectCap : public KernelObject` following the
   ThreadCap tag pattern (thread_cap.hpp:39-72) and the FrameCap storage pattern (frame.hpp:29-65):
   - members: `static constexpr uint32_t kCarvedObjectTag = 0x4F424A43U; /* 'OBJC' */`,
     `uint32_t tag = kCarvedObjectTag;`, `CapType kind = CapType::Null;`, `uint64_t phys = 0;`,
     `size_t bytes = 0;`, `bool is_user = false;`.
   - `static CarvedObjectCap *create(CapType kind, uint64_t phys, size_t bytes, bool is_user) noexcept`:
     reject `!object_sizing(kind).carvable || object_sizing(kind).variable_size` -> nullptr;
     reject `bytes != object_sizing(kind).bytes` -> nullptr; `MemPool::alloc(sizeof(CarvedObjectCap))`,
     placement-new, `mark_pool_backed()`, set fields, `track_cap_object_add()`; then zero the payload
     with one bounded `__builtin_memset` over `bytes` through the HHDM base of `phys` (the same
     translation the FrameCap/pager paths use — resolve it explicitly, do not assume `phys + HHDM`
     arithmetic; INV-RT-4). Document: zeroing is mandatory because a stale page-table root is a
     privilege-escalation primitive (spec §4.2 "no partial object"), and Frame deliberately keeps raw
     contents (existing contract, untouched).
   - `void dispose() noexcept override`: `for (size_t i = 0; i < bytes / arch::PAGE_SIZE; ++i)
     PMM::free_page(phys + i * arch::PAGE_SIZE);` then `track_cap_object_remove()` + `MemPool::free(this)`
     — exactly frame.cpp:78-83 (no `free_user_pages`; FrameCap does not use it, PMM accounting parity).
   - `void revoke() noexcept override { KernelObject::revoke(); }` with a comment stating that no
     mapping/registry invalidation is needed at F1 (no user mapping of a carved object exists until F5
     wires VSPACE_MAP) and where F5 must add it.
   - `bool is_shared() const noexcept override { return true; }`.
   - In `carved_object.cpp`: include `<kernel/task/task.hpp>` and add
     `static_assert(sizeof(TaskControlBlock) <= kObjectBytesTcb, "TCB object storage too small");`
     (compile-time fail-closed; complements task.cpp:949-955 and task.hpp:1403, does not replace them).
4. **cap_errors.hpp (new).** X-macro table `CAP_ERROR_CODES`, `enum CapError : uint64_t`,
   `static_assert(CAP_ERR_OK == 0)`, `error_string<CapError>` — same shape as sync_errors.hpp:30-68.
   Codes (all reachable; see mapping in step 5): OK=0, INVALID_ARGS, BAD_HANDLE, POLICY_DENIED,
   TARGET_UNSUPPORTED, SIZE_ZERO, SIZE_UNALIGNED, RANGE_EXCEEDED, SHORT_RANGE ("Untyped region smaller
   than the fixed-size object"), SIZE_MISMATCH ("explicit size differs from the object's fixed size"),
   NO_CSLOT, ALREADY_RETYPED, OBJECT_EXHAUSTED, CHILD_UNAVAILABLE, INSTALL_FAILED. No new dmesg catalog
   row: no `dmesg_push*` site is added (catalog collision check from LEARNINGS #354 therefore does not
   apply).
5. **untyped.hpp/.cpp — drop the fixed target; rebuild retype.** (a) Delete `retype_target` (:45) and
   the `CapType target` parameters (:51-52, :58-60), with `alloc_object` (:49-50) and both factories
   updated; delete the `target != CapType::Frame` rejects (:73-74, :101-102) and the
   `ut->retype_target` writes/reads (:62, :154, :195). Reword the class doc (:31-39) and the
   `create_subrange` comment: the Untyped records region + carve state only; the child remainder is
   itself type-agnostic. (b) New `retype` signature (cap.hpp:196-198 + untyped.cpp:132-133):
   `int retype(CNode *cspace, uint64_t untyped_handle, CapType target_type, size_t size,
   CapRights rights, uint32_t *gen_out = nullptr, errors::CapError *err_out = nullptr) noexcept` —
   source-compatible with every existing 5- and 6-arg call (test_cap_untyped.cpp:340/392/426/…,
   test_ramdisk.cpp:85, ramdiskd.cpp:107). Keep the `int` return (-1/index) as the frozen ABI face.
   (c) Validation order (preserving today's order so the pinned tests keep their meaning):
   `!cspace` -> INVALID_ARGS; policy read deny (:138-144) -> POLICY_DENIED; `lookup(Untyped, WRITE)`
   fail (:147-150) -> BAD_HANDLE; `!object_sizing(target_type).carvable` -> TARGET_UNSUPPORTED
   (replaces :154's `target_type != ut->retype_target`; this is what keeps Endpoint/CNode rejected);
   size==0 -> `variable_size ? SIZE_ZERO : <use fixed bytes>`; `size % PAGE` -> SIZE_UNALIGNED;
   size > ut->size -> RANGE_EXCEEDED; fixed kind and `size` supplied and `size != fixed` ->
   SIZE_MISMATCH; fixed kind with `fixed > ut->size` -> SHORT_RANGE; slot pre-check (:161-168) ->
   NO_CSLOT; `claim_once` lost (:171-174) -> ALREADY_RETYPED; target-object creation failure (:179-185)
   -> OBJECT_EXHAUSTED; child creation failure (:192-202) -> CHILD_UNAVAILABLE; install failures
   (:204-229) -> INSTALL_FAILED. Exactly one `*err_out = ...` per branch, none on success; every early
   return still does `ut->release()` exactly once (unchanged).
   (d) Object factory: replace the hard-coded `FrameCap::create(...)` (:179-180) with a local
   `create_target(ut, target_type, carve_bytes)` helper — Frame -> `FrameCap::create(ut->phys,
   carve_bytes / arch::PAGE_SIZE, ut->is_user)` (byte-identical to today); the five fixed kinds ->
   `CarvedObjectCap::create(target_type, ut->phys, carve_bytes, ut->is_user)`. `carve_bytes` =
   `size` for Frame, `object_sizing(kind).bytes` otherwise. The stretch-fail-closed (:196-201) uses
   `fc->count` today and must become kind-agnostic: set the target's byte span to the whole region
   (add the needed setter or construct with the stretched size) before `release()`, so the whole
   region returns to PMM exactly once — this is the one place where a mechanical port can create a
   double-free and must be re-derived, not copied.
   (e) Install: `cspace->install(target, target_type, rights, gen_out)` (today hard-codes
   `CapType::Frame` at :204) so the slot records the caller's chosen type; the child remains
   `CapType::Untyped` with `rights | CapRights::WRITE` (:218-219, unchanged authority argument).
6. **syscall_handlers_cap.cpp — boundary hardening + typed plumbing.** In `sys_cap_retype`
   (:115-135): before the cast at :128-129 add
   `if (!cap::cap_type_valid(target_type)) return static_cast<uint64_t>(errors::SYS_ERR_GENERIC_FAIL);`
   (today `static_cast<CapType>(uint64_t)` truncates: 0x10B silently becomes TCB — LEARNINGS #337
   class of trap). Declare `errors::CapError err = errors::CAP_ERR_OK;`, pass `&err`, and on
   `idx < 0` emit one `#if defined(CONFIG_DEBUG)` `Logger::error`/raw_write line containing
   `errors::error_string(err)` (diagnostic only — no control-flow divergence, CODING_STYLE §6) while
   still returning `encode_user_handle(..., -1)` = `SYS_ERR_GENERIC_FAIL` (frozen; the existing
   matrix test asserts exactly `(uint64_t)-1`, test_cap_untyped.cpp:935-941).
7. **ramdiskd.cpp — keep boot path honest.** Update `create` (:62-63) for the dropped parameter; at
   the retype site (:107) capture the `CapError` and pass the code (not the bare index) into the
   existing failure dmesg row (:71-74 is the sibling row for `create`; add/point the retype failure
   row the same way, reusing an existing DAEMON catalog slot and re-checking `dmesg_catalog.hpp` for
   collisions before choosing it).
8. **cspace.md.**
   Replace the `retype_target` field in the §2.8.2 snippet (:293-324) with the type-agnostic form;
   rewrite validation item 2 (:345) as the step-5(c) ladder; rewrite the "Retype targets — Frame only
   in iteration 1" paragraph (:375) into the F1 statement: six carvable kinds, target per invocation,
   fixed compile-time sizes with `SHORT_RANGE`/`SIZE_MISMATCH` typed errors, object storage in the
   carved region, Endpoint/CNode still not carvable. Add the fixed-size table (one row per kind) with
   the page counts and the per-kind consumer issue.
9. **Verification sweep (developer).** `tools/meson/build.sh` per arch (x86_64, aarch64, riscv64);
   `make check-style` (style ERRORS block `make build`); `meson test -C build/meson-<arch>` for the
   host-side parity/policy tests (new .cpp files change the discovered source set); then the gate
   classes in TEST STRATEGY, one class at a time, appending a `test-history.txt` row per run.
   Then `graphify update .` and the SIL 3 audit (production change ⇒ audit mandatory).

## TEST STRATEGY

- **No test file is modified or added on main** (test sanctity; `src/kernel/test/**` and
  `test_expected_counts.hpp` are QE-owned). The change is validated on main by the *existing*
  lock-in tests, which must stay byte-identically green — they pin precisely the semantics this plan
  preserves: `retype_frame_exact_size_end_to_end` (test_cap_untyped.cpp:324), `retype_oversize_*`
  (:380), `retype_twice_fails_no_double_free` (:410), `retype_wrong_target_type_fails` (:450,
  Endpoint+CNode rejected — this is why D1 excludes them), `retype_requires_write_right` (:481),
  `retype_revoked_untyped_fails_single_free` (:507), `retype_subrange_carve_creates_child_remainder`
  (:556), `retype_two_level_split_child_retyped_again` (:620), `retype_unaligned_*` (:703),
  `retype_child_dispose_frees_only_remainder` (:735), `retype_parent_dispose_after_carve_frees_nothing`
  (:782), `retype_full_table_precheck_fails_closed` (:828), `untyped_live_bound_counts_children`
  (:964), `sys_cap_retype_dispatch_end_to_end` (:890), `sys_cap_retype_validation_matrix` (:925).
- **Classes to run, in this order** (fix-one-at-a-time discipline; test-history timeouts):
  1. `cap_untyped` — fine class, direct owner of every touched behaviour (`--class cap_untyped`).
  2. `capability` — aggregate (test_registry.cpp:1196-1215: cap_core, cap_lifecycle, cap_syscall,
     cap_ipc, cap_untyped, cap_mmio, cap_mmio_user, cap_irq, cap_irq_notify, x86-only cap_msix,
     cap_iommu, cap_shm, cap_death, cap_pager, cap_broker) — the M11 gate class per §14:843.
  3. `proc_elf` — second M11 gate class (§14:843); covers ELF/TCB paths downstream of TCB sizing.
  4. `storage` — ramdiskd boot path (`boot_allocate` create + `retype`) is inside
     `run_ramdisk_group` (test_registry.cpp:1255); a signature/ownership regression would surface here.
  5. `selftest` — CI gate.
- **Cross-arch gate (mandatory):** `build.sh` + `capability`, `proc_elf`, `cap_untyped` on
  `aarch64` and `riscv64` as well (`capability` has no `arch_scope` restriction in
  tools/test-policy.tsv:41-46 — it is expected green everywhere). Full evidence is
  `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} debug` before merge-to-release, plus one
  release leg per arch.
- **New tests handed to QE for testbed** (names + assertions only; stub-first per PROMPT-dev.md §3,
  `JARVIS_REGISTER_TEST`, `test_expected_counts.hpp` rows are QE's):
  1. `mk_retype_target_types` (spec §13.3:804) — each of the six carvable kinds retypes from a
     sufficient Untyped; the slot type equals the requested type; `lookup` with a different CapType
     fails.
  2. `retype_fixed_size_short_range_typed_error` — 1-page Untyped + `CapType::TCB` -> -1 and
     `CAP_ERR_SHORT_RANGE`; `owns_region()` still true; a subsequent Frame retype on the same
     Untyped succeeds (non-destructive).
  3. `retype_fixed_size_mismatch_typed_error` — explicit size != fixed size -> `CAP_ERR_SIZE_MISMATCH`.
  4. `retype_untyped_has_no_target_type` — two identical Untypeds retyped to two different kinds both
     succeed (proves the fixed-target removal).
  5. `retype_unsupported_target_typed_error` — Endpoint/CNode/Null -> `CAP_ERR_TARGET_UNSUPPORTED`,
     parent intact.
  6. `retype_carved_object_zeroed` — a carved non-Frame payload reads all-zero.
  7. `retype_carved_object_lifecycle_no_leak` — carve TCB+VSpace, remove slots, release: PMM pages and
     `cap_objects` back to baseline (ResourceTracker via `test_isolate`).
  8. `sys_cap_retype_target_type_range` — `target_type` 0x10B and 0x100 both rejected.
  9. `retype_child_remainder_any_type` — sub-range carve of a fixed kind leaves a child Untyped that
     retypes to a different kind.
- **Tracker/oracle:** every listed run must show ResourceTracker delta 0 (no new counter type is
  introduced — `cap_objects`/`cap_slots`/PMM only).

## SIL 3 / SAFETY RISKS

- [S1] **Double-free / ownership hole in the new non-Frame path** — a kind-agnostic stretch-fail-closed
  port (step 5(d)) that releases the target without first widening its byte span frees only the carved
  prefix while the child later frees the remainder, or frees the same page twice. Mitigation: derive
  the stretch per kind instead of copying the FrameCap branch; keep the "claim set ⇒ parent owns
  nothing" rule; re-check every one of the 8 failure paths against INV-RT-1; the existing pinned tests
  (retype_child_dispose_*, retype_parent_dispose_after_carve_*, untyped_live_bound_counts_children)
  are the regression oracle.
- [S1] **Stale page-table root handed to a future VSpace/PageTable consumer** — a carved-but-unzeroed
  region is an attacker-influenced page table the moment F5 maps it. Mitigation: mandatory bounded
  zeroing in `CarvedObjectCap::create` (INV-RT-4), asserted by new test 6.
- [S1] **Silent type truncation at the syscall boundary** — `static_cast<CapType>(uint64_t)` makes
  0x10B a TCB (and 0x100 a Null). Mitigation: `cap_type_valid()` gate before the cast (step 6) +
  new test 8; fail direction is the frozen generic failure, never a wrong-type object.
- [S2] **Spec/semantics drift on the "watermark" requirement** — §4.2 says the Untyped records range +
  watermark; the tree keeps the child-Untyped exhaustion model (locked in by 4 tests). Mitigation:
  state the equivalence explicitly in untyped.hpp and cspace.md; do not "improve" the model in F1;
  a strict in-region watermark is only admissible once those tests are re-owned by QE.
- [S2] **Coordination with uncommitted M4 (#563)** — the working tree already contains M4's
  `untyped_handover.{hpp,cpp}`, which calls `create_subrange(phys, size, is_user)`
  (untyped_handover.cpp:53-55) and edits `nexios_config.h`. The new 3-arg signature keeps that call
  compiling, but if M4 lands first with a target argument, or adds a `CONFIG_CAP_*` row, rebase and
  re-verify before touching `untyped.*`. Mitigation: this plan adds no `nexios_config.h` key at all.
- [S2] **Fixed-size UX divergence** — Frame requires an explicit size; fixed kinds accept 0 = "the
  type's size". Undocumented, this reads as an ABI inconsistency and will be misused. Mitigation:
  document in cap.hpp's retype doc block and in cspace.md step 8; typed errors distinguish the cases
  (SIZE_ZERO vs SHORT_RANGE vs SIZE_MISMATCH).
- [S2] **Typed error never observed on a branch** — an `err_out` write missed on one early return
  leaves a stale OK. Mitigation: one-write-per-branch checklist at review time; the QE matrix asserts
  the code on the specific branches (tests 2, 3, 5) so a miss fails a test rather than a review.
- [S3] **New TU not picked up / parity gate** — `src/kernel/cap/carved_object.cpp` is discovered by
  `find` (src/meson.build:65-81) but changes the source set the parity/policy tests compare; run
  `meson test -C build/meson-<arch>` for all three arches.
- [S3] **Style/checker gates** — new headers need `#pragma once`, ≤80 columns, no single-character
  names (avoid `t`/`p` in `object_sizing`), no magic numbers outside the sizing header, and a
  `// NOLINTBEGIN(bugprone-easily-swappable-parameters)` around `create(kind, phys, bytes, is_user)`
  (precedent task.cpp:944). `make check-style` must report Errors: 0.
- [S3] **Doc-lie risk** — microkernel-transition.md:162 (T-1 row 56) will read stale once this lands
  ("target fixed per Untyped"); it is normative and needs owner sign-off, so this lane only records it
  (see OPEN QUESTIONS) instead of editing it.

## OPEN QUESTIONS

- **Q-q1 (owner, not blocking implementation): does userspace need the typed retype error in this
  cycle?** Plan takes: typed error at the kernel API only (`cap::retype` out-param), because
  `SYS_CAP_RETYPE`'s `-1` encoding is frozen (T-1/Q3) and four existing assertions expect exactly
  `(uint64_t)-1` (test_cap_untyped.cpp:937-941). If the owner wants it surfaced now, the only
  non-breaking channel is a user-pointer out-arg on the unused 5th syscall argument (needs
  `safe_copy_to_user` + a new ABI note) — say so before step 6 is implemented.
- **Q-q2 (sequencing, not blocking):** the normative spec rows that become stale (microkernel-transition
  §3.2 row 56, §4.2 "target fixed per Untyped") plus the Appendix A implementation-record row are
  owner-sign-off edits; confirm they can be filed as a doc follow-up rather than folded into #554.
