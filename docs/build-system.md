# Build System Operator How-To (issue #170)

Dual-system period: **Make stays authoritative** until the Phase 9 cutover
gate (`tools/check-parity.py --require-all` + recorded 3-arch full gates).
This doc covers the Meson side. Requirements live in
`docs/specs/build-system.md`; status in `docs/specs/build-system-parity.tsv`.

## Prerequisites

macOS: `brew install ninja` + pip `meson==1.12.0` (pinned — apt 1.3.2 fails
the freestanding sanity check):
`pip install 'meson==1.12.0' 'ninja==1.13.2'`.
Linux CI: same pins via pip (never apt), plus `-elf-` toolchain aliases
(see `Dockerfile`).

## Configure (one builddir per arch — never mix)

```sh
meson setup build/meson-x86_64 --cross-file cross/x86_64-elf.txt -Darch=x86_64
meson setup build/meson-aarch64 --cross-file cross/aarch64-elf.txt -Darch=aarch64
meson setup build/meson-riscv64 --cross-file cross/riscv64-elf.txt -Darch=riscv64
```

Defaults: `buildtype=debug`, `-Db_lto=true`. NO_LTO variant:
`meson configure build/meson-<arch> -Db_lto=false` (forces full recompile;
never mix LTO objects — the link asserts `.lto-mode`). Release builds
always add `-Db_lto=false` (mirrors Make, whose release LDFLAGS never
carry `-flto`).

## Build

```sh
tools/meson/build.sh [builddir]   # style gate, wipe-on-LTO-mismatch, compile
meson compile -C build/meson-<arch> [target]   # direct ninja (skips the gate)
```

`build.sh` fails the build when `tools/validate_style.py` reports errors,
wipes + asks for reconfigure on `.lto-mode` mismatch (never mixes LTO
objects), then builds the default target set (kernel, ISO, registry,
test-config, blobs).

## Test

```sh
meson test -C build/meson-<arch> --suite host          # parity + host tests
meson test -C build/meson-<arch> --suite x86_64-debug  # QEMU suites (26 classes)
meson test -C build/meson-<arch> 'qemu/x86_64/debug/<class>'  # one class
python3 tools/meson/qemu_test.py --arch A --build B --class C [--no-history]
python3 tools/meson/qemu_test.py --arch x86_64 --build debug --dump-counts
MESON_TEST_CLASS=<cls> meson compile -C build/meson-<arch> debug-test   # GDB batch
MESON_GDB_SCRIPT=tools/gdb/test-batch.gdb MESON_SHELL_CMDS=cmds.txt meson compile -C build/meson-<arch> debug-shell
```

Known runs: `host` suite is self-contained; QEMU suites build + boot the
real artifacts. `debug-test`/`debug-shell` use a separate
`build/meson-<arch>-nolto` tree (A2: GDB flows never churn the main tree).

`tools/meson/build.sh` is the build entry point (style gate, then compile).

## Interactive boot

```sh
meson compile -C build/meson-x86_64 boot-debug       # debug ISO, 1 CPU
meson compile -C build/meson-x86_64-release boot-release  # release ISO, 2 CPUs
```

Each builddir offers its own boot target (the other name fails loud there).
QEMU opens a framebuffer window and the serial console is the invoking
terminal (`mon:stdio`); there is no GDB stub and no watchdog verdict — the
guest runs until the window is closed (or `Ctrl-A X` in the terminal).
The target boots only a `test_class=none` tree: a builddir with a class
baked in fails with an explicit error instead of booting into a suite
(reconfigure `-Dtest_class=none` + rebuild for interactive use).
Debug boots single-CPU; release boots `-smp 2` with the no-LTO release
link (release with LTO is unsupported: an inline-only `memset` has no
out-of-line definition, so LTO static-init fails to link; parity with the
no-LTO release link, and the target refuses a `-Db_lto=true` release
builddir instead of silently booting a mixed tree).

## Verify flags

```sh
python3 tools/meson/tests/test_flag_parity.py build/meson-<arch> <arch>
```

Compares real per-TU commands against
`tools/meson/flag-manifest-<arch>.txt`. Harmless Meson/ninja scaffolding
(dup `-Wall -g -std=c++20`, depfiles, `-D_GLIBCXX_ASSERTIONS=1`,
`-D_FILE_OFFSET_BITS=64`, `-O0` shadowed by our `-Og`, `-flto=auto`)
is normalized with justification in the script header.

## Lint / docs / config / analysis

```sh
meson compile -C build/meson-<arch> check-style     # style gate (fails on Errors>0)
meson compile -C build/meson-<arch> test-policy-check config-summary
meson compile -C build/meson-<arch> clang-tidy      # non-blocking by design
meson compile -C build/meson-<arch> doxygen-check audit symbols objdump
MESON_AUDIT_HPP=a.h MESON_AUDIT_CPP=b.cpp meson compile -C build/meson-<arch> audit
```

Coverage/profiling use dedicated builddirs (flags change every object):
`meson setup build/meson-x86_64-cov --cross-file cross/x86_64-elf.txt -Darch=x86_64 -Dprofile_mode=gcov`
(`func`, `coverage_func`, `coverage_line`, `sampling` analogous; x86_64 only).
Run `qemu_test.py` inside the coverage builddir per class, then
`tools/coverage_report.py` as today.
