# Dynamic Test Coverage

> Status: **Phase B implemented** (function-level, 2026-09-06, issue #122).
> Phase A (line/branch level via real gcov) is designed but not implemented.

## 1. Why

Static guessing cannot tell which kernel code the test suite actually
executes.  NexIOS measures it dynamically: an instrumented kernel records
every function that is *entered* during a real QEMU boot of a real test
class; the host unions the per-class results and reports coverage per
subsystem.

## 2. How to run

```sh
make coverage-build                 # build the instrumented kernel once
make coverage-class CLASS=safe      # boot one class, capture its dump
make coverage                       # build + run COVERAGE_CLASSES + report
```

Artifacts:

| Path | Content |
|---|---|
| `build/coverage/<class>/serial.raw` | serial capture of one class run |
| `build/coverage/<class>/kernel.elf` | ELF of that run (addresses shift per relink) |
| `build/coverage/report.md` | per-area / per-file tables, 0 % files |
| `build/coverage/report.html` | same data as HTML |

Tunables: `COVERAGE_CLASSES`, `COVERAGE_MAX_FUNCS` (default 16384),
`COVERAGE_TIMEOUT` (default 300 s), `COVERAGE_EXCLUDES`.

Build flags (`COVERAGE_FLAGS`, x86_64 only):

```
-g -Og -fno-inline -fno-inline-functions -fno-omit-frame-pointer
-DCONFIG_DEBUG -DCONFIG_COVERAGE
-DCONFIG_COVERAGE_MAX_FUNCS=<n>
-finstrument-functions
-finstrument-functions-exclude-file-list=src/kernel/gcov/,gcov_handler.cpp
```

No `-flto`: LTO renumbers and inlines functions and would break
address → symbol attribution.

## 3. Kernel side (`src/kernel/gcov/gcov_handler.cpp`)

`__cyg_profile_func_enter()` registers the entered function in a bounded
open-addressing table (`MAX_FUNCS` entries, `2 × MAX_FUNCS` slots, linear
probe bound 32) — O(1), no allocation, no locks, safe in IRQ context.
Registration *is* the coverage signal; a function appears in the dump if and
only if it was entered.

Table-full or probe exhaustion is **fail-closed**: the entry is dropped and
counted (`dropped_funcs`), never mis-attributed to another index.  The host
tool refuses to merge a run with `dropped > 0`.

Dump frame (little-endian, emitted by `gcov_flush_to_serial()` from
`shutdown_kernel()` **before** the ACPI sleep write, with interrupts
disabled):

```
"\n@@COVBEGIN@@\n"
"FUNC"  u32 count
count × (u64 addr, u8 called)      // called == 1 (wire compatibility)
"STAT"  u32 registered  u32 dropped  u32 checksum
"END!"
"\n@@COVEND@@\n"
```

`checksum` is the wrapping u32 sum of the count, entry, registered and
dropped bytes.  A stalled UART emits `@@COVABORT@@` instead of the end
sentinel; the host rejects such a capture.  All serial polls are bounded.

## 4. Host side (`tools/coverage_report.py`)

1. Parse every `serial.raw`, validate both sentinels and the checksum.
2. Resolve executed addresses against that class' own ELF (`nm`).
3. Union the results by *(source file, function name)* — relinking shifts
   addresses, so names are the stable identity. Itanium ABI duplicate
   variants are collapsed before unioning: C1/C2 constructors count as one
   entry (C1), D0/D1/D2 destructors as one (D1). The base-object/deleting
   twins are never entered for classes that are never used as bases
   (e.g. `CheckedPtr<T>`), so counting them would inflate the denominator
   with permanently uncoverable entries (issue #142).
4. Build the universe of functions with `nm` and attribute every function to
   a source file with a single batched `addr2line` call per class.
5. Report per area (`src/kernel/<area>`, `src/lib`, …), per file, worst
   first, plus the largest absolute gaps and the list of 0 % files.

Test code (`src/kernel/test/**`) is reported separately and excluded from
the headline kernel figure.  `src/kernel/gcov/**` is deliberately excluded
from instrumentation, so it always shows 0 %.

## 5. Known limitations (Phase B)

- Function granularity only — no line, branch or call-count data.
- Instrumented runs use deeper stacks (`CONFIG_COVERAGE` doubles the stack
  tiers); timing-sensitive classes (`*_hrt`, `wcet*`, `*latency*`,
  `bench_*`) are excluded from the default class list.
- Assembly stubs, ISR entry paths and the boot loader are not attributed.
- `make coverage-build` runs `clean`, which removes `build/` **including
  previous captures**.

## 6. Phase A (designed, not implemented)

The cross toolchain ships a freestanding-safe `libgcov.a` (only undefined
symbol: `strlen`) and `gcov.h`, exporting `__gcov_info_to_gcda()` and
`__gcov_filename_to_gcfn()`; `x86_64-elf-gcov`, `x86_64-elf-gcov-tool`
(merge), `lcov` and `genhtml` are installed.  Plan: build with
`-fprofile-arcs -ftest-coverage`, intercept `__gcov_init()` to collect the
`gcov_info*` set, serialise each `.gcda` stream over COM1 with framing,
reconstruct the files next to the `.gcno`s, then run `x86_64-elf-gcov` +
`lcov`/`genhtml`; merge per-class runs with `x86_64-elf-gcov-tool merge`.
