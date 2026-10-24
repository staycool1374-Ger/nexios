# #170 Meson sweep — plan under pre-change audit, REV 2 (amendments A1–A9 applied)

No code changed. Revision of audits/pending_plan_170.md REV 1 per
audits/rejected_plan_170.md (A1–A9) + open-question decisions.
Issue: staycool1374-Ger/nexios #170 + IDE comment 6366624198.

# PLAN: Meson build-system migration (issue #170) — phased parity sweep, Make stays authoritative until cutover

## AFFECTED FILES

**New — spec / traceability**
- `docs/specs/build-system.md` — extensible per-topic requirements spec (Phase 0 deliverable).
- `docs/specs/build-system-parity.tsv` — machine-checkable parity ledger (`id | item | requirement | make_behavior | meson_solution | meson_target | ci_job | evidence_url | status`). Every row requires a CI evidence URL (A6).
- `tools/check-parity.py` — fails when any ledger row is not `PASS` with a CI evidence URL; enforces cutover precondition.
- `docs/build-system.md` — operator how-to (target table, builddir layout, bootstrap).

**New — Meson**
- `meson.build` (root) — project (`meson_version: '>=1.12.0'`), options, per-arch `add_project_arguments`, subdirs, kernel link, ISO/initrd staging, `test()`/`run_target()` map. Kernel objects get NO Meson defaults: `b_pie=false`, `b_staticpic=false`, no sanitizer/fortify (A9, asserted by `test_flag_parity.py`).
- `src/meson.build`, `src/libc/meson.build`, `userspace/meson.build`, `tests/meson.build` (host tests), `packaging/meson.build`.
- `cross/x86_64-elf.txt`, `cross/aarch64-elf.txt`, `cross/riscv64-elf.txt` — committed cross files, one `meson setup --cross-file …` command.
- `tools/meson/build.sh`, `tools/meson/qemu_test.py` (fail-closed verdict: missing SUMMARY / zero S-lines / 0-0 counts ⇒ FAIL via TIMEOUT/QEMU_EXIT path, never PASS; STALL-RECOVERED only on final PASS with nfail==0 — A1), `tools/meson/test_history.py` (mkdir-lock alongside test-history.txt: atomic mkdir, stale-lock timeout + trap cleanup, append-only — A5), `tools/meson/postlink.py`, `tools/meson/gen_test_config.py`, `tools/meson/gdb_run.py`, `tools/meson/package_iso.py`, `tools/meson/tests/*.py`, `tools/check_no_make_refs.sh` (covers .exp files, bug_report.yml, healthcheck.sh — A7), `.github/workflows/build-meson.yml` (meson-aarch64 REQUIRED — A8/Q3).

**Modified**
- `Dockerfile` — elf symlinks for all three arches; meson/ninja via pip pin (never apt — A8); `CMD` hint text.
- `.github/workflows/ci.yml` — pip-pin meson/ninja, elf aliases, Meson parity job (Make jobs stay required during dual).
- `.github/workflows/docker-release.yml` — dogfood dual-runner bounded: advisory → required with date/budget + flake-attribution labels (A8); retry-once retained.
- `scripts/run_all_classes.sh`, `scripts/run_config_matrix.sh`, `tools/deadline_matrix.sh`, `tools/renode/run-renode.sh`, `clion/qemu-frozen.sh`, `tools/chainrun.py` + chains, `tools/*.exp` ×5 (shell-smoke, fs-repro, verify-269-271, verify-281, verify-288 — ported or shimmed, NOT left Unchanged — A7).
- `AGENTS.md`, `prompts/PROMPT-dev.md`, `prompts/PROMPT-testdev.md`, `prompts/AGENTS-KERNEL-BRIEFING.md` §6, `prompts/PROMPT-audit.md`, `.github/ISSUE_TEMPLATE/bug_report.yml`, `scripts/healthcheck.sh` (require meson>=1.12.0 + ninja, fail not warn — A7), docs sweep list (clion-setup, debugging, test-harness, configuration, coverage, cspace, drivers, iommu, ipc-fastpath, external-pager, rust-userspace, microkernel-transition, syscall-abi-picolibc, README).
- `clion/CLION.md`, `clion/CMakeLists.txt` — KEPT during dual, RETIRED in the cutover commit, ledger row tracks it (Q1 decision).
- `prompts/PROMPT-dev.mdes` — DELETED in the docs-sweep phase, one source of truth (Q2 decision).
- `.gitignore` — drop registry/gen.mk entries at cutover.
- **Deleted at cutover (one commit):** `Makefile`, `mk/rules.mk`, `mk/` remainder.

**Unchanged (explicit):** run-test.exp, qemu-file-log.sh, test-policy.tsv/awk, coverage_report.py, gen_test_registry.py, validate_style.py, patch_code_crc.py, mkfat32img.py, build-picolibc.sh, tools/gdb/*, renode .resc, shell-smoke.exp content (invocation ported per above), linker scripts, all kernel sources.

## INVARIANTS
- Build-system only, zero behavior change (A9): no Meson default flags on kernel objects; coverage/profiling flags scope to host tests only; registry ordering byte-identical + per-function objdump spot-check; no test-policy/test-history format change.
- Three builddirs one per arch; on `.lto-mode` or buildtype mismatch the builddir is WIPED and reconfigured clean — assert-only is insufficient (A2). `test_lto_mode.py` proves mismatch ⇒ clean rebuild. Flag manifests frozen byte-for-byte.
- `-Dtest_class` change is a real Meson dependency: rebuilds test-config.txt ⇒ initrd/cpio/ISO re-stage (A3). Single source of truth = builddir file; legacy `initrd/tests/test-config.txt` is a one-direction compat copy with a dep edge. `qemu_test.py` asserts `.baked-test-config[-<arch>]` against the requested class BEFORE launching QEMU (fail-loud); release bakes `none` unless overridden (same stamp assert as `make release`).
- Artifact contract + PINNED selection (A4): `qemu_test.py` pins per-arch QEMU artifact (riscv64 raw BIN via objcopy with frozen flags; x86_64/aarch64 ELF); debug/release ISO dirs never overwrite each other; postlink arch/buildtype/LTO asserts stay. `test_stage_paths.py` covers ELF-vs-BIN, fat32.img, initrd.cpio, baked-configs, both ISOs, gdb-log dirs.
- Every Meson `test()` timeout = per-class policy outer timeout ×2 (meson-test multiplier honored), NEVER the 30 s default; host test asserts each timeout ≥ 2× policy (A1). Verdict parse fail-closed (above). All QEMU tests `is_parallel: false`; `/tmp` harness paths namespaced by full-checkout hash (basename collides — A5); `:1234` documented non-namespaced (debug-test/rr-replay serialize).
- `test-history.txt`: exactly one row per run via the mkdir-lock appender (A5).
- Gate classification preserved (style Errors fail; tidy/doxygen non-blocking). No new build-time network. No new dev dependency beyond meson/ninja (bootstrap documented; pip-pin, never apt — A8).
- Make authoritative until cutover; Makefile freeze is CI-ENFORCED: any PR touching `Makefile`/`mk/` without a Meson mirror or ledger update fails (A6).

## PHASES
0. Spec + ledger (blocks everything); every row open with requirement + Make behavior anchors; ledger posted on #170.
1. Toolchain & cross files (meson>=1.12.0; flags mirror byte-for-byte; manifests frozen; bootstrap doc; Dockerfile/CI pip-pin + elf aliases).
2. Codegen (registry custom_target, byte-identical ordering, no kernel edits; test-config option-driven with dep edge + compat copy + pre-QEMU gate per A3; per-file-rules mechanism deleted by design; NO new generated version header — preserve build_info.hpp stringification exactly).
3. Compile/link (same source discovery/exclusions; NASM/dtb/blob custom_targets with identical redefine-sym triples; link + postlink asserts + legacy staging; LTO wipe-on-mismatch per A2).
4. Harness (qemu_test.py verbatim port + A1 timeouts/verdict + A4 artifact pinning + A3 baked-config gate; test() per class from policy TSV; run_targets for non-test flows; run_all_classes loop preserved; history via mkdir-lock appender).
5. Packaging (initrd/cpio/fat32/ISO in builddir + legacy staging; x86-only ISO semantics; off-x86 release-elf + baked-config; release selftest off-x86 fails loud).
6. Lint/docs/analysis (build.sh runs style gate; tidy/doxygen non-blocking run_targets; coverage run_targets, x86_64-only guards as configure errors).
7. CI adoption + IDE consequences (build-meson.yml: meson-x86_64/aarch64-REQUIRED/riscv64/tests/parity; dogfood dual bounded; CodeQL default-setup gets a ledger row with evidence URL — A8; cross files committed + one setup command; stable codegen paths; test() tokens intact; symbol paths pinned; clion-setup/debugging rewrite with Remote Debug flow EXPLICITLY preserved; clion CMake shim kept-then-retired).
8. Agent/process docs sweep (~174 lines + .exp files + bug_report.yml + healthcheck.sh + PROMPT-dev.mdes deletion; check_no_make_refs.sh covers all; historical records exempt).
9. Cutover (check-parity --require-all green with evidence URLs + recorded 3-arch full gates; single delete commit; close #170 after post-cutover green CI; LEARNINGS entry).

## NON-GOALS (binding)
No kernel/test-source behavior change; no packaging redesign; no replacement of QEMU -s -S + CLion Remote Debug; no rewriting historical records; no byte-identical ELF guarantee (equivalence = object-set + flags + symbols + boot/gate); no new fatal lint gates; no test-policy/test-history format change; no Meson default flags on kernel objects; coverage/profiling host-tests only; no deletions outside the cutover list.

## TEST STRATEGY
Host tests (stub-first, no QEMU): test_cross_files, test_flag_parity (+no-defaults assert), test_source_set, test_gen_registry, test_stage_paths (extended per A4), test_policy_timeouts (≥2× policy, no 30 s default — A1), test_history_lock (N concurrent ⇒ N intact rows — A5), test_lto_mode (mismatch ⇒ clean rebuild — A2), test_verdict_parser (fail-closed incl. missing SUMMARY, zero S-lines, 0/0, STALL-RECOVERED guard — A1). Existing classes under Meson: configuration_build, testrunner, debug_syscall, selftest, safe, arch_aarch64, arch_riscv64, then full gates x86_64 debug+release, aa64, rv64. CI matrices as Phase 7.

## OPEN QUESTIONS — DECIDED
- Q1 CLion CMake shim: KEEP during dual, RETIRE in cutover commit (ledger row).
- Q2 PROMPT-dev.mdes duplicate: DELETE in docs-sweep phase.
- Q3 meson-aarch64 CI job: ADD as REQUIRED.
