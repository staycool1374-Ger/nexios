# ELF Shared-Object Support — DT_NEEDED Resolution for User Images

**Doc ID:** NEX-SPEC-2026-08-23-005
**Status:** DRAFT
**Milestone target:** v0.4.4 (after runelf lands)
**Inspiration:** Cyjon `kernel/exec.asm` + `library/kernel_library` (~700
lines total: ELF-from-storage load, then dependency resolution via section
headers — .strtab/.dynamic scans, DT_NEEDED iteration, one-time library
loading with dedup, GOT/PLT fixups).
**Related:** `docs/specs/elf-loader.md` (background loader, IMPLEMENTED,
runelf hook §10), `docs/specs/boundary.md`, VFS spec (fat32/tmpfs sources).

## 1. Current State (verified)

NexIOS loads **static, self-contained ELF executables**: the background
loader (elf-loader.md) chunks a file from tmpfs/fat32 into a user image,
validates phdrs, and retains the finished TCB for the future `runelf`
command. No PT_DYNAMIC handling exists; every binary must link everything.

Cyjon demonstrates that even dynamic linking stays tractable at low level if
constrained to linear section scans. We adopt the *pipeline shape*, not the
implementation (theirs is ring-0 monolithic; ours must respect the boundary
spec's W^X and size-bound rules).

## 2. Design Overview

Three stages, all inside the existing background loader task (preemptible at
chunk boundaries — unchanged RT guarantees):

```
Stage A  exec-load      (exists)   file → segments mapped, entry resolved
Stage B  dep-resolve    (new)      PT_DYNAMIC → DT_NEEDED list → recursive
                                   Stage-A on each needed .so (dedup by
                                   soname via loader singleton cache)
Stage C  relocate       (new)      walk DT_JMPREL/DT_RELA → write GOT.PLT
                                   entries; apply RELATIVE relocs eagerly,
                                   symbolic lazily where possible
```

### 2.1 Library storage & discovery

Convention: `/lib/<soname>.so` on the boot filesystem (fat32 SD card in the
maker scenario). Search order fixed: exact path from DT_NEEDED if absolute,
else `/lib/`. The dedup cache (`ElfLoader::loaded_libs_`) maps soname →
load_base; second requester gets the already-mapped image (read-only text +
shared data per mapping flags).

### 2.2 Address model — no ASLR, deterministic

Hard-RT project ⇒ libraries link at fixed preferred vaddr and we fail with
`E_LAYOUT` on overlap rather than rebasing silently. Deterministic layout is
auditable and matches the deadline-analysis philosophy (same binary ⇒ same
behavior).

### 2.3 Security/boundary constraints (binding)

- All relocation writes go through the same CheckedPtr discipline as syscalls
  (boundary.md principle 1); a reloc target outside the task's mapped
  regions ⇒ INVALID_ELF, cleanup identical to cancel.
- W^X preserved: relocatable pages are writable only between Stage C start
  and end, then re-protected RX/RO before the TCB becomes schedulable.
- Depth cap: DT_NEEDED recursion ≤ 8; cycle detection via in-progress set;
  violation ⇒ INVALID_ELF (fail-closed).
- Size bound: total mapped bytes (exec + libs) checked against the task's
  rlimit-equivalent before each Stage-A invocation.

## 3. Loader State Machine Extension

Existing states IDLE→LOADING→DONE/CANCELED stay; add sub-states inside
LOADING:

```
LOADING_EXEC → LOADING_DEPS(n) → LOADING_RELOC → DONE
```

Cancellation semantics unchanged: `cancel-load` at ANY sub-state performs
full teardown (unmap libs loaded *for this request* only if refcount drops
to zero — shared libs stay cached otherwise, matching the dedup goal).
Failure reporting to shell + dmesg identical format, extended with the
soname chain that failed.

## 4. crt0/consumer side

Out of scope here but noted: a minimal `crt0.S` + link recipe
(`--dynamic-linker` unused — we resolve in-kernel; binaries carry DT_NEEDED
and their own PLT stubs) will be provided under examples/ so a plain
`ld.lld -shared` produced library works. Toolchain note: lld emits
DT_NEEDED + standard rela — no patches expected.

## 5. Test Plan

New class `elf_shared`:
1. `single_lib_resolution` — exec with one DT_NEEDED; both images mapped,
   GOT entry points at lib symbol.
2. `dedup_two_execs_same_soname` — second load maps no extra lib pages.
3. `missing_lib_fails_cleanly` — zero page delta, dmesg names the soname.
4. `cycle_detected` — A needs B needs A ⇒ INVALID_ELF.
5. `depth_cap` — chain of 9 ⇒ rejected.
6. `cancel_during_deps` — cancel mid Stage-B; partial libs refcounted out.
7. `wx_restored_after_reloc` — post-DONE page dump shows RX/RO.
8. `layout_conflict_E_LAYOUT` — overlapping preferred bases rejected.

Regression gate: existing `elf_loader` tests unmodified (static ELFs have
no PT_DYNAMIC ⇒ Stage B/C skip trivially).

## 6. Non-Goals

- Full ld.so feature set (TLS offsets beyond initial-exec, versioned
  symbols, LD_PRELOAD semantics, lazy PLT binding with lazy-fault overhead —
  eager binding chosen precisely because lazy faults wreck WCET analysis).
- Kernel-side loading of shared objects into *kernel* address space.
