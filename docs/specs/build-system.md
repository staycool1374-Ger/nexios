# Build System Requirements (build-system) — issue #170

Extensible per-topic requirements spec for the Makefile → Meson migration.
Each topic follows a fixed template: *Requirement / Current Make behavior
(file:line anchors) / Meson solution / Acceptance evidence / Owning issue*.
Extend by appending a section + ledger rows only. Status tracking lives in
`docs/specs/build-system-parity.tsv` (mechanical) and issue #170 (human).

## 0 Scope / non-goals

- **Requirement:** build-system parity only; zero kernel/test-source behavior change (audit amendment A9).
- **Make behavior:** N/A (this is the migration contract itself).
- **Meson solution:** every later section carries its own acceptance; cutover is mechanical via `tools/check-parity.py --require-all`.
- **Acceptance:** ledger all-`PASS` + recorded 3-arch full gates on #170.
- **Owning issue:** #170.

Non-goals (binding): no kernel/driver/scheduler/test-source change; no
packaging redesign (grub-mkrescue ISO, cpio initrd, FAT32, raw riscv BIN);
no replacement of the QEMU `-s -S` + CLion Remote Debug flow; no rewriting
historical records; no byte-identical ELF guarantee (equivalence =
object-set + per-TU flags + symbol-set + boot/gate); no new fatal lint
gates; no test-policy/test-history format change; no Meson default flags
(PIE/static-PIC/sanitizer/fortify) on kernel objects; coverage/profiling
flags host-tests only; no deletions outside the cutover list.

## 1 Toolchain & cross files

- **Requirement:** freestanding C++20 cross toolchains for x86_64-elf,
  aarch64-elf, riscv64-elf with exact arch flags; LTO and NO_LTO variants
  must never mix objects (known trap). Meson >= 1.12.0 (apt 1.3.2 fails
  the freestanding sanity check — Dockerfile pin is proof).
- **Make behavior:** `Makefile` derives `-elf-` triplets per `ARCH`
  (`x86_64-elf-g++` et al.); arch flag blocks per target
  (`Makefile:749-756` debug flags incl. `-flto`, x86 `-fno-pie -m64
  -mno-red-zone -mgeneral-regs-only -mcmodel=large`); `NO_LTO` switch
  rebuilds without `-flto` (`Makefile:755-756`, `rr-record:1286`).
  `Dockerfile` aliases only `x86_64-elf-*` today; riscv64 CI apt-installs
  meson+ninja (unpinned — must become pip-pin).
- **Meson solution:** committed `cross/<arch>-elf.txt` ×3 (`binaries`
  from `-elf-` triplets, `[host_machine]` cpu_family/cpu/endian,
  `needs_exe_wrapper=true`, `system='none'`; flags live in `meson.build`,
  not in cross files). One canonical command:
  `meson setup build/meson-<arch> --cross-file cross/<arch>-elf.txt
  -Darch=<arch>`. `meson.options`: `arch` (combo), `test_class`
  (string, default `none`), `config_defs` (array), `external_test_dir`,
  `profile_mode` (combo), `qemu_uefi` (string, auto-detect),
  `picolibc_version` (default 1.8.12), `clang_tidy` (bool). QEMU tests
  are host scripts — no `exe_wrapper`.
- **Acceptance:** `meson-aarch64` + `meson-x86_64` jobs configure/compile
  a trivial TU with the real flag set; `test_flag_parity.py` green
  (byte-for-byte vs frozen `flag-manifest-<arch>.txt`, incl.
  `b_pie=false`, `b_staticpic=false`, no sanitizer/fortify on kernel).
- **Owning issue:** #170 (Phase 1).

## 2 Source discovery & per-TU flags

- **Requirement:** identical translation-unit set and per-TU flag set as
  the Make build; no silent additions/omissions.
- **Make behavior:** `SRC_CXX := $(shell find src …)` (`mk/rules.mk:15`)
  with arch pruning (exclude `src/kernel/arch`, add back
  `arch/<arch>` + `arch/*.cpp` base), FPU-test exclusion list,
  `syscall_entry.asm` exclusion, external-test selftest subset
  (`SELFTEST_INFRA`/`SELFTEST_TESTS`); per-file rules generated into
  git-ignored `mk/cpp-rules.*.gen.mk` (`mk/rules.mk:188-197`).
- **Meson solution:** `src/meson.build` builds the source array with the
  same exclusions verbatim (moved into `tests/selftest_sources.build`
  for the selftest subset). The `.gen.mk` mechanism is deleted by design;
  parity is proven by object-set equality, not file similarity.
- **Acceptance:** `test_source_set.py` (Meson list == git-derived Make
  list minus documented exclusions; object counts equal) +
  `test_flag_parity.py` green.
- **Owning issue:** #170 (Phase 3).

## 3 Codegen (registry, test-config, build identity)

- **Requirement:** generated sources byte-identical to the Make build;
  no new generated headers; no kernel source edits.
- **Make behavior:** `test_registry.gen.hpp` via
  `tools/gen_test_registry.py` (`Makefile:515-543`); test class baked via
  `initrd/tests/test-config.txt`; build identity is compile-time flags
  only (`src/kernel/build_info.hpp` stringifies `-DNEXUS_ARCH_NAME` /
  `-DBUILD_TYPE`; `nexios_config.h` is static — there is NO generated
  version/config header in the tree today).
- **Meson solution:** `custom_target('test-registry-gen')` (same
  generator, same file ordering, builddir on the include path —
  quoted `#include "test_registry.gen.hpp"` keeps working);
  `gen_test_config.py(get_option('test_class'))` writes the builddir
  `test-config.txt` AND the one-direction compat copy to
  `initrd/tests/test-config.txt` (gitignored) with a real Meson
  dependency edge; preserve the no-generated-header design exactly.
- **Acceptance:** generated header hash equals Make output for the same
  source set/arch (`test_gen_registry.py`); `configuration_build`
  class passes under Meson.
- **Owning issue:** #170 (Phase 2).

## 4 Object/link model, LTO

- **Requirement:** LTO and NO_LTO (and per-buildtype) objects never mix;
  link inputs/flags identical; post-link steps identical.
- **Make behavior:** `-flto` in `LDFLAGS` for debug (`Makefile:751`),
  omitted under `NO_LTO=1`; known mixing trap when toggling in place;
  post-link CRC patch + staging (`_release-elf-finalize:839`,
  `check-release-stamp:850`, `check-build-stamp:698`).
- **Meson solution:** default `-Db_lto=true`; GDB flows reconfigure
  `-Db_lto=false`. On `.lto-mode`/buildtype mismatch the builddir is
  WIPED and reconfigured clean (assert-only is insufficient — A2).
  `postlink.py`: copy, CRC patch, `readelf` arch assert, `.lto-mode`
  marker, `.baked-test-config[-<arch>]`, then legacy-path staging.
  Blob objcopy uses explicit renames from the Meson-layout default names
  (`_binary_packaging_*`, `_binary_src_*`) to the identical public symbols
  (`_binary_initrd_cpio_*`, `_binary_build_fat32_img_*`,
  `_binary_libc_verify_img_*`, `_binary_errno_probe_img_*`,
  `_binary_fork_marker_img_*`, `_binary_ap_trampoline_*`) — objcopy
  derives defaults from the input path, so the rename source differs
  from Make while the result is identical.
- **Acceptance:** `test_lto_mode.py` (mismatch ⇒ clean rebuild; no
  `-flto` objects after flip); symbol-set equality (`nm`) vs Make.
- **Owning issue:** #170 (Phase 3).

## 5 Artifact paths & compatibility layer

- **Requirement:** the 20 in-tree consumers keep working — every
  documented path exists with the right content after the documented
  build target (renode `.resc`, `tools/gdb/*`, `chainrun.py`,
  `clion/qemu-frozen.sh`, docs).
- **Make behavior:** `build/kernel-debug.elf`, `build/kernel.elf`,
  `build/kernel-riscv64.bin` (+ release BIN), `build/fat32.img`,
  `build/initrd.cpio`, `build/.baked-test-config[-<arch>]`,
  `debug/nexios-rtos.iso`, `release/nexios-rtos.iso`, gdb logs,
  `build/host/*`, `build/coverage/`, `build/profiling/`.
- **Meson solution:** postlink + packaging stage the legacy paths from
  the builddir (compat layer is part of the link target). QEMU artifact
  selection pinned per arch in `qemu_test.py`: riscv64 raw BIN (objcopy
  frozen flags), x86_64/aarch64 ELF; debug/release ISO dirs disjoint (A4).
- **Acceptance:** `test_stage_paths.py` covers ELF-vs-BIN, fat32.img,
  initrd.cpio, baked-configs, both ISOs, gdb-log dirs.
- **Owning issue:** #170 (Phases 3/5).

## 6 Test harness & policy

- **Requirement:** the `execute-test <arch> <build> <class>` matrix runs
  identically: host watchdog, serial-log verdict parsing, SUMMARY tokens,
  stall-retry, history rows, GDB batch targets.
- **Make behavior:** `execute-test:1123` → `_do_execute_test:1136`
  (recursive-make worker: configure class, build, boot QEMU with
  `QEMU_SYSTEM` per arch `:170/:198/:234` + `QEMU_UEFI:81`/`OVMF_CODE`,
  file-backed debugcon/COM1, watchdog on serial growth, verdict parse,
  `[HOST-WATCHDOG]`, scoped pkill, stall-signature retry from
  `test-policy.tsv`, `STATUS:` tagging, one history row per run);
  `test-full:1204` (per-class loop, `SUMMARY` line);
  `debug-test:1219` / `debug-shell:1555` (GDB batch, `-s -S` stub);
  `rr-record:1286` / `rr-replay:1292`.
- **Meson solution:** `tools/meson/qemu_test.py` (verbatim port, nested
  `meson configure -Dtest_class=` + compile = the honest recursive-make
  worker); `tests/meson.build`: one `test()` per class generated from
  the policy TSV at configure time, `timeout:` = per-class policy outer
  timeout ×2 (never the 30 s default — A1), `is_parallel: false`;
  verdict parse FAIL-CLOSED (missing SUMMARY / zero S-lines / 0-0 ⇒
  FAIL via TIMEOUT/QEMU_EXIT, never PASS; STALL-RECOVERED only on final
  PASS with nfail==0 — A1); `run_target`s for debug-test/debug-shell/
  rr/dump-counts/symbols/objdump; `run_all_classes.sh` keeps its loop
  and delegates appends to `tools/meson/test_history.py` (mkdir-lock:
  atomic mkdir alongside test-history.txt, stale-lock timeout + trap
  cleanup — no `flock` on macOS — A5); `/tmp` paths namespaced by
  full-checkout hash (`:1234` documented non-namespaced — A5).
- **Acceptance:** `test_policy_timeouts.py` (every timeout ≥ 2× policy,
  none default), `test_history_lock.py` (N concurrent ⇒ N intact rows),
  `test_verdict_parser.py` (all arms incl. fail-closed + 0/0 fail-loud);
  Meson gates: selftest + configuration_build + testrunner +
  debug_syscall, then full `test-full` equivalence ×3 arches.
- **Owning issue:** #170 (Phase 4).

## 7 Packaging

- **Requirement:** identical initrd/fat32/ISO outputs; x86_64-only ISO
  semantics preserved; off-x86 release-ELF flow preserved.
- **Make behavior:** initrd tree + `cpio -o -H newc`, `mkfat32img.py`,
  ISO root + `grub-mkrescue` staging to `debug/`/`release/`;
  `release:798/859` (x86_64-only ISO), `release-elf:835/861`
  (off-x86 ELF + baked-config), baked-config pre-QEMU assert.
- **Meson solution:** `packaging/meson.build` reproduces the initrd
  tree byte-for-byte (fstab, rc, root/bin ELFs, rust-hello, picolibc
  verify, xmodem/sh blobs, test-config), ISO root inside the builddir,
  staging to legacy paths; release semantics + baked-config assert
  (incl. release-bakes-`none` default — A3) enforced in `qemu_test.py`
  before QEMU launch.
- **Acceptance:** artifact upload + ISO boot check (x86_64), BIN upload
  (riscv64), `test_stage_paths.py`.
- **Owning issue:** #170 (Phase 5).

## 8 Lint / docs / coverage / profiling

- **Requirement:** style gate stays fatal-on-Errors; tidy/doxygen stay
  non-blocking; coverage/profiling flows unchanged and x86_64-only.
- **Make behavior:** `check-style:572` (`validate_style.py`, Errors==0
  gate); `clang-tidy:1715` (non-blocking `|| true`); `doxygen-check:583`;
  `check-config:587`, `config-summary:598`, `test-policy-check:613`;
  `coverage-build:994` / `coverage-class:1009` / `coverage:1038`;
  `profiling:871` / `profile-class:902`; `stack-usage-report:1802`;
  `audit:1787`; `test-debugd-host:1740` (host RSP tests).
- **Meson solution:** `tools/meson/build.sh` runs style gate then
  compile; `check-style` also a fast `test('style')`; `meson compile`
  alone stays style-free (parity with `make debug`). Tidy/doxygen/
  config/stack-usage as non-blocking `run_target`s — never `test()`,
  never in `all` (A9 guard). Coverage via `-Dprofile_mode`, same
  report/extract scripts, x86_64-only guards as configure-time errors
  (firing only when coverage is explicitly requested off-x86 — S3 note).
- **Acceptance:** style gate green in `meson-x86_64` job; tidy output
  present-but-non-blocking; coverage report unchanged.
- **Owning issue:** #170 (Phase 6).

## 9 IDE / CLion

- **Requirement:** CLion works natively with Meson (since 2023.3);
  live-debug flow survives verbatim; the port unlocks
  `compile_commands.json` + structured `meson test` instead of expect
  scraping. (Source: issue comment 6366624198.)
- **Make behavior:** `clion/` CMake indexing shim + `CLION.md`;
  `docs/clion-setup.md` + `docs/debugging.md` reference `make …` lines
  and build-tree paths; GDB flow = manual QEMU `-s -S` + IDE Remote
  Debug (`target remote 127.0.0.1:1234`, symbol file,
  `source tools/gdb/kernel.py`).
- **Meson solution:** (1) committed cross files + one canonical
  `meson setup` command (CLI source of truth, IDE mirrors it — never a
  fresh native IDE configure); (2) codegen as `custom_target()`s with
  stable output paths; (3) `test()`/`run_target()` map keeps SUMMARY
  tokens intact (CLion test UI + history tooling read the same stream);
  (4) symbol paths pinned by the compat layer. `docs/clion-setup.md`:
  Meson open/setup, `meson compile` builds, three arch builddirs,
  per-config `compile_commands.json` (kills the wrong-arch LSP
  false-error class); §4 live-debug flow UNCHANGED and explicitly NOT
  replaced by an IDE runner. Retire `clion/CLION.md` +
  `clion/CMakeLists.txt` at cutover (kept during dual — Q1 decision).
- **Acceptance:** CLion opens the configured builddir, per-TU flags
  resolve, `meson test` tree populates, Remote Debug attaches to a
  manually-started QEMU (documented screenshot/flow check by owner).
- **Owning issue:** #170 (Phase 7).

## 10 CI / CD

- **Requirement:** all gates preserved and extended; Meson jobs start
  advisory, flip to required at cutover; dogfood validates the env image
  under both systems during dual.
- **Make behavior:** `ci.yml` (x86_64 debug+release ISO builds, host
  debugd tests, riscv64 toolchain + `test-full riscv64 debug`);
  `docker-release.yml` (build-push + dogfood: `make build` +
  `execute-test debug_syscall` with retry-once); CodeQL via GitHub
  default setup (no in-tree workflow).
- **Meson solution:** new `build-meson.yml` (`meson-x86_64`,
  `meson-aarch64` REQUIRED — Q3 decision, `meson-riscv64`
  timeout-30 + TCG multiplier 2, `meson-tests-x86_64`,
  `parity-make-vs-meson` dual-only). `ci.yml`: pip-pin meson/ninja,
  elf aliases. Dogfood: dual-runner bounded (advisory → required with
  date/budget + flake labels), retry-once retained (A8). CodeQL: ledger
  row with evidence URL that default setup still covers Meson C++ (A8).
  Meson pin `==1.12.0` / ninja `==1.13.2` everywhere (never apt).
- **Acceptance:** all Meson jobs green alongside Make jobs during dual;
  required-check flip at cutover; post-cutover `main` green ×3 arches.
- **Owning issue:** #170 (Phase 7).

## 11 Dual-system exit criteria

- **Requirement:** Make stays authoritative; the dual period is bounded
  by the milestone (v0.5.6), not the calendar; cutover is mechanical.
- **Make behavior:** N/A.
- **Meson solution:** Makefile freeze CI-ENFORCED (PR touching
  `Makefile`/`mk/` without Meson mirror or ledger update fails — A6).
  `tools/check-parity.py --require-all` (every row `PASS` + evidence
  URL) + recorded 3-arch full gates = the cutover gate. Single delete
  commit (`Makefile`, `mk/`), `.gitignore` cleanup, required-check flip,
  `check_no_make_refs.sh` green.
- **Acceptance:** gate output posted on #170; #170 closes only after
  post-cutover `main` CI is green ×3 arches; LEARNINGS entry.
- **Owning issue:** #170 (Phase 9).

## 12 Traceability

- **Requirement:** every requirement traces to an issue row, a target,
  and CI evidence.
- **Meson solution:** this spec (§0–§11) + `build-system-parity.tsv`
  (one row per checklist item + IDE consequences + `make help` targets)
  + `tools/check-parity.py`. `docs/build-system.md` is the operator
  how-to (replaces `make help`).
- **Acceptance:** `--require-all` green.
- **Owning issue:** #170 (Phase 0).
