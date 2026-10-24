# Rejected plan amendments — issue #170 Meson migration (plan audit 2026-10-23)
Plan: audits/pending_plan_170.md. No source files to patch (pre-change review).
Developer: apply these to the plan (and then to the implementation) and request re-audit.

## A1 — test() timeouts + fail-closed verdict parser (fixes S1)
- Set every Meson `test()` `timeout:` from policy TSV per-class outer timeout x2 (stall-retry doubling), never the 30 s default. Add host test asserting each `test()` timeout >= 2x policy value.
- Make `qemu_test.py` verdict parse fail-closed: missing SUMMARY / zero S-lines / 0-0 counts => FAIL (TIMEOUT/QEMU_EXIT path, never PASS); replicate STALL-RECOVERED guard (PASS arm + nfail==0) and 0/0 fail-loud semantics; keep SUMMARY tokens byte-equivalent.

## A2 — LTO/buildtype staleness: wipe, not just assert (fixes S2)
- On `.lto-mode` or buildtype mismatch, wipe the builddir (or adopt per-buildtype dirs `build/meson-<arch>-<buildtype>`); a bare assert is insufficient (known LTO/NO_LTO object-mixing trap).
- `test_lto_mode.py` must prove mismatch => clean reconfigure/rebuild. Freeze byte-for-byte flag manifests and assert Meson injects no defaults (`b_pie=false`, `b_staticpic=false`, no sanitizer/fortify for kernel).

## A3 — wrong-class boot: dependency edge + pre-QEMU gate + release reset (fixes S2)
- Wire `test-config` change as a real Meson dependency: `-Dtest_class` change rebuilds `test-config.txt` => initrd/cpio/ISO re-stage. Single source of truth = builddir file; legacy `initrd/tests/test-config.txt` is a one-direction compat copy with a dep edge.
- Assert `.baked-test-config[-<arch>]` against requested `-Dtest_class` BEFORE launching QEMU (fail-loud abort). Replicate `make release` semantics: release bakes `none` unless overridden, with the same pre-QEMU stamp assert.

## A4 — artifact staging contract (fixes S2)
- Pin per-arch QEMU artifact selection in `qemu_test.py`: riscv64 raw BIN via objcopy with frozen flags, x86_64/aarch64 ELF; debug vs release ISO dirs never overwrite each other; `postlink.py` arch/buildtype/LTO asserts stay.
- Extend `test_stage_paths` to cover ELF-vs-BIN, `fat32.img`, `initrd.cpio`, `.baked-test-config[-<arch>]`, both ISO paths, and gdb-log dirs.

## A5 — history lock + checkout namespacing (fixes S2)
- `test_history.py` mkdir-lock: lock dir alongside `test-history.txt`, atomic `mkdir`, stale-lock timeout + trap cleanup; append-only, TIME measured, STATUS rows per AGENTS.md.
- Namespace every /tmp harness path by full-checkout hash, not directory basename (basename collides); document :1234 as non-namespaced (serialize debug-test/rr-replay).

## A6 — dual-system drift: mechanical freeze enforcement (fixes S2)
- Add a CI check: any PR touching `Makefile`/`mk/` without a Meson mirror or ledger update fails. Every parity-ledger row needs a CI evidence URL; `check-parity.py --require-all` + recorded 3-arch full gates stay the cutover gate.

## A7 — docs sweep completeness (fixes S2 + S3)
- Port or shim the five `tools/*.exp` files that spawn make (`shell-smoke.exp`, `fs-repro.exp`, `verify-269-271.exp`, `verify-281.exp`, `verify-288.exp`): after the cutover delete-commit they break hard, plan may not list them Unchanged. Include `.github/ISSUE_TEMPLATE/bug_report.yml`, `scripts/healthcheck.sh` (require meson>=1.12.0 + ninja, fail not warn; pip-pin, apt 1.3.2 too old), and the `PROMPT-dev.mdes` duplicate disposition in the sweep; extend `check_no_make_refs.sh` to these paths. Historical records stay exempt.

## A8 — CI coverage (fixes S2)
- Pin meson>=1.12.0 via pip in Dockerfile + both workflows (never apt). Add the meson-aarch64 required job (see Q3). Bound the dogfood dual-runner period (advisory then required with date/budget) with flake-attribution labels. Give the CodeQL default-setup assumption a ledger row with an evidence URL.

## A9 — zero-behavior-change guard (fixes S2)
- State explicitly: Meson adds no default flags to kernel objects; --coverage/profiling flags scope to host tests only; registry custom_target ordering proved byte-identical (plus per-function objdump spot-check); no test-policy/test-history format change.

## Open-question decisions (advisory)
- Q1 CLion CMake shim: KEEP during dual, RETIRE in the cutover commit (ledger row tracks it).
- Q2 PROMPT-dev.mdes duplicate: DELETE in the docs-sweep phase (one source of truth).
- Q3 meson-aarch64 CI job: ADD as required (cross-arch parity is a mandatory gate).

## Prior-cycle cautions disposition
- %u/unsigned-varargs: N/A (new code is Python).
- Single-reader hardware assumptions: untouched by design.
- Test files read-only: CONFIRMED — plan adds only new host-side tools/meson/tests/*.py.
- Check-7 retrieval: SATISFIED — issue #170 thread carries graphify query + dispositions and vault search.
