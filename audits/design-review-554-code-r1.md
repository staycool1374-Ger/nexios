# AUDIT REPORT 2026-11-18T23:07Z
PATCH: /tmp/pending_patch_554.diff (uncommitted production diff on main @ adc03dbb9, ~1010 lines)
FILES: docs/specs/cspace.md, src/kernel/cap/cap.hpp, src/kernel/cap/cap_errors.hpp (NEW), src/kernel/cap/cap_types.hpp, src/kernel/cap/carved_object.cpp (NEW), src/kernel/cap/carved_object.hpp (NEW), src/kernel/cap/object_sizing.hpp (NEW), src/kernel/cap/untyped.cpp, src/kernel/cap/untyped.hpp, src/kernel/ramdisk/ramdiskd.cpp, src/kernel/syscall/syscall_handlers_cap.cpp

## SCOPE
SIL 3 code audit of NexIOS issue #554 (F1 capability object model). Normative design:
audits/plan-554.md (binding build spec) + APPROVED audits/design-review-554-r1.md with 4
S3 provisos. graphify-out/ churn + test-history.txt rows present in the worktree are
outside the audited diff per the commission. No src/kernel/test/** paths touched. No
test files in scope: assertion-masking (check 3) is vacuously satisfied.

## PROVISO VERIFICATION (all four binding S3 directives)
- (1) HHDM zeroing pattern: PASS. carved_object.cpp:62-66 zeroes via
  arch::HHDM_OFFSET + phys with an explicit comment citing ramdiskd.cpp:82-84 as the
  translation precedent (not FrameCap, which performs no translation). arch::HHDM_OFFSET
  and arch::PAGE_SIZE resolve via <constants.hpp> (lib/constants.hpp:36-68), defined
  identically on all arches from CONFIG_* — same translation the pager paths use.
- (2) Citation nits: PASS. cspace.md:129 (new) records CONFIG_CAP_MAX_UNTYPED default 64
  (was "default 16"); the remaining S3-2 items were plan-doc citations (untyped_handover
  path, syscall line number, test-policy rows) requiring no implementation change, and the
  implementation cites no stale line numbers.
- (3) sizeof asserts deferred, declaration-only: PASS. object_sizing.hpp:61-64 carries the
  explicit declaration-only note naming F4/F5/M13 as the consumers that must add
  static_assert(sizeof <= kObjectBytesX); the sole in-lane pin is the TCB
  static_assert (carved_object.cpp:37-39), evaluated per-arch at compile time (fail-closed).
- (4) Construction-over-setter + Q-q2 follow-up: PASS/PASS. No widening setter exists
  (carved_object.hpp:61-62 documents construction-only); issue #585 ("F1 doc follow-up
  (Q-q2)") verified FILED and open (owner-filed 2026-11-18, documentation label, lists
  exactly the three stale normative rows: microkernel-transition §3.2 row 56, §4.2 text,
  Appendix A record). Owner sign-off still pending — recorded below as merge precondition.

## DEVIATION REVIEW (allowed: carved stretch path)
Plan step 5(d) prescribed widen-then-release; implementation uses release() of the prefix
target + bounded direct free of the remainder (untyped.cpp:265-275, construction-only per
S3-4). Single-owner outcome verified: CarvedObjectCap::dispose frees exactly
bytes/PAGE_SIZE prefix pages (bytes is PAGE-multiple by validated construction,
carved_object.cpp:49 + untyped.cpp:196); the loop frees [carve_bytes, ut->size) with
first_page = carve_bytes/PAGE_SIZE — no overlap, no gap, whole region exactly once,
parent stays spent. INV-RT-1 holds on this path and on all 8 failure paths (each frees
the carved prefix via target dispose exactly once and the remainder via child dispose or
the bounded loop exactly once; reset_claim on OBJECT_EXHAUSTED keeps the region with the
Untyped). Frame path is byte-identical to the pre-change logic (stretch via fc->count).

## DIMENSION VERDICTS (PROMPT-audit checks)
- S1 safety: CLOSED. No dynamic heap allocation (MemPool only + placement new into
  pool blocks, existing precedent; no new/delete/malloc/free). No new locks, no lock
  nesting, no IrqGuard/scheduler interaction (retype stays task-context; policy read
  under cspace->lock_ only, PMM/MemPool/memset/dispose outside locks — INV-RT-2/3
  preserved; claim_once CAS untouched). Syscall truncation S1 closed:
  cap_type_valid() gates raw_value <= 15 BEFORE the enum cast (syscall_handlers_cap.cpp:
  134), so 0x10B can never become TCB; fail direction is the frozen generic-fail face.
  Zeroing is mandatory and bounded (<= kObjectBytesMax = 16 KiB, INV-RT-4). No
  double-free (analysis above + ResourceTracker add/remove symmetry preserved on every
  path; no new counter type introduced).
- Spec compliance (§4.1/§4.2): PASS. CapType append-only 11-15 with static_assert pins,
  values 0-10 untouched; no Reply type (Q2 deferral honored). Per-invocation target,
  six carvable kinds (Frame + 5 fixed), fixed sizes with SHORT_RANGE/SIZE_MISMATCH typed
  errors, non-destructive validation (every pre-claim failure releases the pin with the
  parent intact), revoke path generic (CarvedObjectCap::revoke -> KernelObject::revoke;
  lookup refuses revoked via acquire(), cap.cpp:190-191; slot.type records the requested
  kind at install, untyped.cpp:282, so per-type lookup identity holds). Endpoint/CNode/
  Mmio/Irq/IoMmuDma/Msix/Wdog/Untyped/Task/Null all fall into the default
  non-carvable arm -> TARGET_UNSUPPORTED (fail-closed default for any future CapType).
  err_out totality verified branch-by-branch (13 failure sites, each writes exactly once
  when non-null, all null-guarded; untouched on success — INV-RT-5).
- Arch parity (check 8): PASS (static). Zero arch-gated blocks added; every size derives
  from CONFIG_PAGE_SIZE (== arch::PAGE_SIZE on x86_64/aarch64/riscv64 per prior anchor);
  HHDM_OFFSET likewise config-derived; TCB static_assert fail-closes per-arch at compile
  time. No MMIO/fence/page-model assumptions, no inline asm.
- Fail-closed (check 9): PASS. Every allocation/IPC/syscall path returns -1 with the
  typed reason; no silent fallback, no unchecked return; #if defined(CONFIG_DEBUG) block
  is diagnostic-only (no control-flow divergence, CODING_STYLE §6).
- Capability isolation (check 10): PASS. Child installed with rights|WRITE with an
  in-code no-new-authority justification (caller already holds WRITE over the parent);
  carved objects unmappable until F5 (revoke no-op documented with the F5 invalidation
  pointer); no new cross-domain channel.
- Evidence (check 11): SUFFICIENT. test-history.txt rows verified in-tree:
  2026-11-19 cap_untyped 18/18, capability 170/170, 0 failures. No crash/hang fix -> no
  GDB obligation. Limitation (no finding): 3-arch build-clean + style-Errors-0 are
  developer-attested; the auditor holds no build/test-execution remit and records the
  attestation plus the corroborating rows above. Check-7 retrieval artifacts for this
  task live in audits/plan-554.md (4 graphify queries with dispositions + 3 vault
  searches, verified by design-review-554-r1); the #554 thread itself carries only the
  work-begun comment and test evidence lives in test-history.txt rather than thread
  posts — filed below as an S3 process note, not a defect in this patch.

## FINDINGS
- [S3] src/kernel/cap/cap.hpp:170 — "Revocation of the parent Untyped's derivations
  reclaims every carved object" overstates the F1 mechanism
  WHY: At F1 there is no parent->derivation cascade on per-slot cap::revoke of the
  parent Untyped (identical to the Frame contract); whole-CNode revoke covers carved
  slots generically. True under the CNode-revoke parse, misleading under the per-slot
  parse — reword at the next doc touch; behavior is correct either way.
- [S3] Process note — evidence placement + unattested build matrix
  WHY: Check-7/11 artifacts for this lane live in-repo (plan + test-history rows,
  both verified above) rather than as #554 thread posts, and the 3-arch build/style
  gates are developer-attested without auditor-executable confirmation (no remit).
  No action in this diff; replica lanes should post gate logs to the issue thread.

## MERGE PRECONDITION (not a code defect, blocks merge regardless of verdict)
Owner sign-off of issue #585 (Q-q2 normative rows: microkernel-transition.md §3.2 row
56, §4.2 sentence, Appendix A record) is still pending (#585 open). The stale window
opens the moment this diff merges; merge must wait for the owner decision per the
APPROVED design verdict.

DECISION: APPROVED
