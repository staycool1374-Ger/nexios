# DESIGN REVIEW 554-r1 — F1 capability object model (plan audit, no code)

PLAN: `audits/plan-554.md` (uncommitted, dev lane)
BASE: main @ `adc03dbb9` (verified: HEAD `git log`, branch `main`, clean apart from plan + 2 scratch files)
SCOPE: design audit only — no diff exists; PROMPT-audit applied where applicable to a plan
(evidence standard, check-7 retrieval, arch parity, fail-closed, no test edits).
Normative reads completed in full: `docs/specs/microkernel-transition.md` §4.1 (227-234),
§4.2 (235-244), §13.2–§13.3 (783-814), §14 M11 row (:843), §15 F1 row (:858),
§16 Q2 (:885); `docs/specs/cspace.md` §2.8.2 (289-380, incl. :375 note);
`audits/plan-582-transition-compat.md` (all 34 lines); CODING_STYLE / AGENTS-KERNEL-BRIEFING
as needed. Tree anchors below re-opened at HEAD this session (no trust in plan line numbers).

## Anchor spot-checks (all PASS, substance exact; citation nits → S3-2)

1. `src/kernel/cap/cap_types.hpp:32-44` — enum 0–10 contiguous, `Wdog = 10` (:43). Append-only 11–15 valid; range-gate sound.
2. `src/kernel/cap/untyped.cpp` — `alloc_object` :49–66, `create` :70–95, `create_subrange` :97–106,
   `retype` :132–237, policy read :138–144, lookup :147–150, pre-check :161–168, claim :171–174,
   factory :179–185, child :192–202, install :204–229. Every STEP-5 citation lands.
3. `src/kernel/syscall/syscall_handlers_cap.cpp:115-135` — CapType cast at :129 (plan says :128–129, → S3-2).
4. `src/kernel/ramdisk/ramdiskd.cpp` — `create` :62–63, retype site :107, dmesg row :71–74. Exact.
5. `src/kernel/cap/frame.cpp:64-84` — dispose loop :79–80 (plan ":78–81" close enough); **no phys→VA
   translation anywhere in `FrameCap::create` (:36–48)** → S3-1.
6. `src/kernel/task/task.cpp:946-955` — TCB ≤16384 (aarch64, 8256 B) / ≤8192 elsewhere; 16 KiB carve covers all.
7. `src/kernel/cap/cap.hpp:196-198` — current 6-arg `retype`; defaulted `err_out` keeps source compat.
   Call sites verified: `test_ramdisk.cpp:85`, `ramdiskd.cpp:107` (both 6-arg).
8. `src/kernel/memory/untyped_handover.cpp:53-55` — 3-arg `create_subrange` call confirmed; **path is
   `memory/`, plan omits the directory** → S3-2.
9. `arch::PAGE_SIZE == CONFIG_PAGE_SIZE` on all three arches — x86_64 `page_table_impl.hpp:168`,
   aarch64 `:110`, riscv64 `:119`. Exact. `CONFIG_PAGE_SIZE`/`CONFIG_CAP_MAX_UNTYPED=64`
   (`nexios_config.h:137-164, :590-591`; Q7 DECIDED 64 ✓ — note `cspace.md:382` still says "default 16",
   step-8 rewrite must fix → S3-2).
10. All 15 lock-in tests present at cited lines (`test_cap_untyped.cpp`: 324, 380, 410, 450, 481, 507,
    556, 620, 703, 735, 782, 828, 890, 925, 964 — names verified, incl. Endpoint+CNode rejection at
    :450–464 and the frozen-`-1` matrix asserts at :935–941).
11. M11 gates `capability` + `proc_elf` (`test_registry.cpp:1196, :1220`), `run_ramdisk_group` (:1255);
    `capability` unrestricted by absence (default `arch_scope|any`, `test-policy.tsv:22`) → S3-2 citation nit.
12. #582-compat (4 follow-ups): #1 §5.6 latency pointer (doc, owner) — untouched by cap lane ✓;
    #2 M4 sequencing — plan names rebase/re-verify, adds no `nexios_config.h` key ✓; #3 Q6-vs-deferral
    — no new deferral/fallback path here ✓; #4 detector gating — no IRQ/scheduler path touched
    (INV-RT-3) ✓. No conflict.

## Dimension verdicts

**S1 (safety): all three plan-flagged S1s are CLOSED at plan level, not merely named.**
- Double-free/stretch (untyped.cpp:196-201): plan prescribes widen-byte-span-to-whole-region before
  `release()` (setter-or-construct, identical invariant outcome), keeps claim-set⇒parent-owns-nothing,
  mandates 8-path recheck, pins 3 oracle tests (735/782/964). Implementer has an exact rule. → S3-4 only.
- Stale page-table zeroing: mandatory bounded `__builtin_memset` (INV-RT-4, ≤16 KiB) + QE test 6.
  Translation resolvable in-tree (ramdiskd.cpp:82-84 HHDM write, incl. user-owned segments) → S3-1 only.
- Syscall truncation (`static_cast<CapType>` turns 0x10B→TCB): `cap_type_valid()` gate before cast (step 6)
  + QE test 8; fail direction stays frozen generic-fail. Closed.
- Concurrency: no new lock, no nesting, task-context only (INV-RT-2/3); `claim_once` CAS untouched
  (untyped.cpp:108-112); PMM/MemPool/dispose stay outside `cspace->lock_` (cap.cpp:54-57, :133-137).
  Revoke no-op is safe by construction (no consumer can map a carved object until F5; F5 pointer recorded).
  INV-RT-1/5/6/7/8 stated as checkable rules. No S1/S2 defect found.

**S2 (correctness/completeness): scope is spec-faithful; preserved-semantics claims verify.**
- Vocabulary + carved storage with per-type ops deferred (TCB→#557 F4, VSpace/PageTable→#558 F5,
  SchedContext→#190 M13) matches F1 row (:858) + M11 aggregate (:843); CAP_DELETE untouched (F2 #555);
  derivation tree untouched (#50). Extra touched files (cap.hpp decl, syscall dispatch, ramdiskd sites,
  cspace.md doc) are necessary consequences, not scope creep.
- Reply excluded: §4.1 conditions Reply on Q2; Q2 (:885) defers reply objects. Faithful.
- Watermark: §4.2 "range and watermark only" vs child-Untyped exhaustion model — plan states the
  equivalence explicitly, refuses to "improve" the model in F1, gates any strict-watermark change on
  QE re-owning 4 locked-in tests (556/620/735/964). Correct call; drift is resolved, not hand-waved.
- §4.2 "fail with a typed error" under frozen `-1` ABI: typed codes at kernel API (testable in-kernel
  via QE tests 2/3/5) + debug-only transport; userspace channel correctly deferred to owner (Q-q1).
- M4 coordination: 3-arg handover call compiles under the new 3-arg signature; no config key added;
  rebase condition named. Sufficient.
- `err_out` totality via one-write-per-branch + matrix-test coverage. Closed at plan level.

**S3 (robustness/process): cross-arch genuine, sanctity clean, gates right.**
- Arch neutrality real (not asserted): all sizes derive from `CONFIG_PAGE_SIZE`; per-arch TCB
  `static_assert` fail-closes any arch where 16 KiB is short; gate runs build + 3 classes × 3 arches,
  full 2×3 matrix pre-release. No single-arch reasoning.
- Test sanctity: zero `src/kernel/test/**` edits; 9 QE tests concrete (names + assertions + oracle);
  `test_expected_counts.hpp` left to QE per §13.2. Gate classes ⊇ M11 (`capability`, `proc_elf`).
- Check-7 artifacts present in-plan (4 graphify queries with dispositions, 3 vault searches with
  0-hit gap recorded, staleness note). No process violation.
- Open questions genuinely non-blocking: Q-q1 forced by T-1/Q3 freeze (owner must bless any new
  syscall out-arg); Q-q2 owner-sign-off routing is the only compliant path for normative rows
  (§3.2 row 56, §4.2 sentence, Appendix A record) — but the follow-up must be FILED before merge (S3-4).

## FINDINGS

- [S3] step 3 misattributes the HHDM precedent — rule precision
  WHY: `FrameCap::create` (frame.cpp:36-48) performs no phys→VA translation and no zeroing; the real
  in-tree HHDM-write precedent is `ramdiskd.cpp:82-84` (`arch::HHDM_OFFSET + seg->phys`, incl.
  user-owned segments). Point the implementer at the pattern that exists.
- [S3] anchor/citation imprecisions (4×, substance verified above) — audit hygiene
  WHY: (a) `untyped_handover` path is `src/kernel/memory/untyped_handover.cpp:53-55`, plan omits
  `memory/`; (b) syscall CapType cast at `syscall_handlers_cap.cpp:129`, not :128-129; (c) "capability
  green everywhere" rests on default `arch_scope|any` (test-policy.tsv:22) by absence, not on :41-46
  (those are the x86-only rows); (d) step-8 rewrite must also fix `cspace.md:382` ("default 16" vs
  tree/Q7 value 64). Correct at implementation; none changes the design.
- [S3] placeholder object sizes lack struct backing — forward-compat guard
  WHY: only TCB gets a `sizeof` static_assert; VSpace/PageTable/Notification/SchedContext single-page
  constants are speculative until F4/F5/M13 structs land. Require those consumers to add
  `static_assert(sizeof(X) <= kObjectBytesX)` in `object_sizing.hpp` (or their own TU) and note it there.
- [S3] two open implementation/process tails — resolve before merge, not before implementation
  WHY: (a) step 5(d) leaves setter-vs-construct open — either satisfies the widen-then-release rule,
  but the developer must pick one and the Phase-3 auditor must verify the single-owner outcome
  against INV-RT-1; (b) file the Q-q2 normative-doc follow-up issue (stale §3.2 row 56, §4.2 sentence,
  Appendix A record) before F1 merges so the stale window is tracked, not silent.

DECISION: APPROVED
