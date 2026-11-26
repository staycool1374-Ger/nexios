# Audit report — OVMF preflight mirror fix (2026-11-26)

- Artifact: `audits/pending_patch.diff` (== `git diff main -- scripts/run_all_classes.sh`, single file)
- Plan: planner subagent `ses_de885c46effeUfgG8gm61zdbef` (mirror `qemu_test.py:find_ovmf()`)
- Implementation: commit `ecee0b110` (testbed), verified `bash -n` clean,
  `find_ovmf` eval prints Cellar edk2 path rc=0 locally, rc=1 with shadowed
  bases under `set -u`, `NEXIOS_POLICY_DUMP=1` exits 0 with 26 classes
- Auditor: sil3_auditor subagent `ses_de8820c46ffet4Y6qHHLOsymmM`
  (read-only; shell denied under auditor cage, recorded as limitation)

## Checklist disposition (all PASS)

1. Confinement: single file `scripts/run_all_classes.sh`; zero `src/`,
   `tools/`, test-tree hunks.
2. Mirror fidelity: `OVMF_BASES` = 3 bases in `qemu_test.py:81-85` order;
   `OVMF_ALT` = 5 fallbacks in `qemu_test.py:96-102` order. Sole
   `secboot` token is the exclusion comment mirrored from the authority
   (`qemu_test.py:94-95`) — documentation of exclusion, not a searched
   image.
3. Fail-fast shape: `exit 1` precedes `LOGDIR`/`mkdir`; `NEXIOS_POLICY_DUMP`
   exit stays above; `qemu_test.py` SystemExit guards (168-171, 207-208)
   untouched and still authoritative.
4. Shell hygiene: all test/exec expansions quoted; intentional word-split
   only over hardcoded lists; no `set -e`/`pipefail`; outer `c`
   unclobbered (`local base fw`, `#!/usr/bin/env bash`).
5. Length ≤80 cols; comment claims verified true against current
   `qemu_test.py`.
6. Failure direction: `return 1` only when nothing found; caller keys off
   exit status; `[ -d ]` before `find`; `[ -f ]` (not `-e`) for exact paths.
7. Retrieval: none run — zero kernel surface (pure harness shell);
   direct file evidence substitutes (`qemu_test.py:79-105`, `168-171`,
   `204-208`).

## Non-blocking notes (S4, no action)

- S4: shell error message generalizes to "requires OVMF firmware" while
  `qemu_test.py:169` still names `edk2-x86_64-code.fd` (cosmetic only).
- S4: checklist item 2's literal "no `secboot` substring" is over-strict;
  recommend "no secboot firmware *path*".

## Verdict

DECISION: APPROVED
