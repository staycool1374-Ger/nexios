# Syscall ABI + picolibc Userspace (v0.5.0 design paper)

**Doc ID:** NEX-SPEC-2026-09-19-001
**Status:** DRAFT (planner review pending)
**Milestone target:** v0.5.0 picolib + abi
**Issues:** #67 (trap/IRQ numbers), #68 (register conventions),
#69 (syscall.h public header), #70 (versioned syscall table),
#71 (POSIX stubs), #72 (build picolibc), #73 (Makefile integration),
#74 (TLS on context switch), #75 (verify), #76 (POSIX time API)
**Related:** `docs/specs/syscall-fastpath.md`, `docs/specs/ipc-fastpath.md`,
`src/kernel/syscall/syscall.hpp`, `src/libc/syscall.h`,
`src/kernel/arch/hal/idt.hpp`, `src/kernel/arch/hal/timer.hpp`

## 0. Corrections to the issue texts (binding)

- **#70 "0–50 (`YIELD..HALT`)" is stale.** The live table is
  `SyscallNumber` 0–79, `MAX_SYSCALL = 80` (syscall.hpp:31-112): 0–50
  base, 51–60 caps/IRQ/IOMMU/MMIO, 61–62 user-MMIO, 63–68 frame/death,
  69–73 pager, 74–76 FAST IPC, 77–78 affinity, 79 TIMES. The spec below
  versions reality, not the issue text.
- **#68 cites Linux register conventions — the kernel uses its own.**
  The live contract (`src/libc/syscall.h` `__syscall5` + all working
  in-tree userspace ELFs) is 4-arg: x86_64 `rax=num, rbx/cx/dx/si`;
  aarch64 `x8=num, x0–x3`; riscv64 `a7=num, a0–a3`. The 6-word
  rsi/rdi/r8–r11 layout belongs to the FAST IPC path only
  (ipc-fastpath.md §3.2) and is out of scope here.
- **#67 "create `src/kernel/syscall/syscall.h`"** — the header already
  exists as `src/libc/syscall.h` (kernel+libc shared surface). #69
  completes it; no second header is created.

## 1. Trap / IRQ number table (#67, frozen under ABI v1)

x86_64 CPU exceptions 0–31 are unchanged (exception-table-audit.md).
Ownership per row (freeze the full set — omission invites collision):

| Vector | Name | Owner |
|---|---|---|
| 32 | TIMER (= IRQ0) | PIT/HPET/APIC tick (`InterruptVector::TIMER`, idt.hpp:75); never claimable (irq_delivery.cpp:117) |
| 33–47 | IRQ1–15 | PIC cascade; `IrqCap` window (`irq_delivery.cpp:107-108`, `IRQ_VECTOR_MIN=33`, `irq_line=vector-32`) |
| 48–255 minus below | MSI-X | per-vector claim (`irq_delivery.cpp:107-129`, `pci.cpp:545-553`) |
| 0x71/0x72 | TPR/batch probes | test probes (apic.hpp:137) |
| 0x73 | SHOOTDOWN_BATCH_VECTOR | TLB shootdown batching, issue #159 (`apic.hpp:139`, `shootdown_ipi.hpp:19-31`) |
| 0x80 | SYSCALL | sole live gate (`InterruptVector::SYSCALL`, idt.hpp:77; rejected by claim, irq_delivery.cpp:111) |
| 0xE0 | APIC_TIMER_VECTOR | scheduler tick, moved 64→0xE0 by issue #26 (`apic.hpp:129`, `pci.cpp:539`, `isr_stubs.asm:185-190`); allocator AND claim reject. NOTE: 64 is ordinary/claimable since #26 |
| 0xEC | SCHED_VECTOR | reschedule IPI (`apic.hpp:134`) |
| 0xEF | self-test | AP self-test vector (`apic.hpp:132`) |
| 0xFF | SPURIOUS | APIC spurious (claim rejects) |

aarch64/riscv64 abstract enum: `TIMER = 0, KEYBOARD = 1, SYSCALL = 2`
(idt.hpp:133-137; traps via `svc #0` / `ecall`). Rule: any new vector
assignment, or any change to this table, bumps ABI minor (§10).

## 2. Register conventions (#68, pinned)

| Arch | Gate | Number | Args (4) | Return |
|---|---|---|---|---|
| x86_64 | `int $0x80` | rax | rbx, rcx, rdx, rsi | rax |
| aarch64 | `svc #0` | x8 | x0–x3 | x0 |
| riscv64 | `ecall` | a7 | a0–a3 | a0 |

C bridge: `syscall_handler(number, arg0..arg3, regs)` (kernel.cpp) →
`Syscall::handle`. **Error convention (binding):** every handler
returns `uint64_t`; success ≥ 0, failure = two's-complement small
negative (`(uint64_t)-errno`). libc evaluates the predicate in `long`
arithmetic: `(long)ret < 0 && (unsigned long)ret > -4096UL` ⇒
`errno = -(long)ret, return -1` (Linux-style). Reachable failures
return codes per CODING_STYLE §5 (never ENSURE); each new call documents
its reachable errno set. Negative test pins the boundary: `-4095`
maps to errno, `-4097` is a valid success value.
No handler may return a kernel pointer as a success value.

## 3. syscall.h public header (#69, single source of truth)

`src/libc/syscall.h` is consumed by kernel-side tests, in-tree libc,
and picolibc stubs alike. Requirements:

- `SYS_*` defines mirror `SyscallNumber` 1:1, including the missing
  51–79 (caps … TIMES) and the §5 additions. Drift rule: a
  `syscall_abi` test asserts `SYS_MAX == MAX_SYSCALL` and spot-checks
  band boundaries — staged: 0–79 frozen now (0/51/57/63/69/74/77/79),
  80–85 asserted only after §4 lands.
- `NEXIOS_ABI_MAJOR` / `NEXIOS_ABI_MINOR` macros (§10).
- Freestanding-clean: only `<sys/types.h>`, `<sys/utsname.h>`,
  `<time.h>`-subset headers (the three already used by syscall.h:24-26);
  no hosted-libc dependency (picolibc provides the rest at #72).

## 4. Versioned syscall table (#70)

- **ABI v1.0** at v0.5.0: numbers 0–79 frozen with current semantics.
- **Extension rule (binding):** append-only. New calls take the next
  free number, `MAX_SYSCALL` advances, never reuse/renumber/remove.
  Incompatible semantic change ⇒ ABI major bump + new number (old
  number kept as alias or retired-never-reused).
- **New numbers (this milestone):** `CLOCK_GETTIME = 80`,
  `NANOSLEEP = 81`, `TIMER_CREATE = 82`, `TIMERFD_CREATE = 83`,
  `ABI_VERSION = 84` (returns `major<<16|minor`, always succeeds),
  `TLS_SET = 85`. `MAX_SYSCALL` 80 → 86.

## 5. POSIX syscall stubs (#71)

`src/libc/picolib_stubs.c` implements the picolibc syscall interface
over `__syscall5`: `_write→WRITE`, `_read→READ`, `_sbrk→BRK`,
`_exit→EXIT`, `_open→OPEN`, `_close→CLOSE`, `_fstat→FSTAT`,
`_lseek→LSEEK`, `_getpid→GETPID`, `_kill→KILL`. `_sbrk` grows via BRK
against `program_break` (red-zone cap at STACK_VADDR preserved —
MP-2.2). All user pointers cross `CheckedPtr`/`safe_copy` as today;
errno mapping per §2. Coexists with existing `src/libc/*.c`
(no flag-day for in-tree userspace ELFs).

## 6. Build picolibc (#72) + Makefile integration (#73)

- Meson builds pinned `PICOLIBC_VERSION` (Makefile variable; default
  = newest stable verified at #72 implementation to build `x86_64-elf`
  static — tag recorded in the #72 commit message + LEARNINGS, not
  invented here), static `libc.a` + `libm.a` only, freestanding flags,
  sysroot under `build/` (gitignored by rule).
- Makefile: `picolibc` / `picolibc-clean` targets, sysroot path var,
  build-order edge (sysroot before userspace link), link rule for
  picolibc-based user programs; existing userspace ELFs unchanged.
- Vendored tarball `third_party/picolibc-$(PICOLIBC_VERSION).tar.gz`
  (new dir): no network fetch during `make build` (offline rule).

## 7. TLS on context switch (#74)

- New TCB field `tls_base_` (user VA, 0 = unset). TCB memset
  discipline: set explicitly at all 4 zeroing sites — task.cpp
  create/create_user/clone + elf.cpp finalize (the `iopb_slot_`
  precedent, LEARNINGS.md:134). Clone inherits (userspace re-sets).
- `TLS_SET` (85): rejects non-canonical and non-user-half addresses
  (48-bit canonical check; 57-bit when LA57 is enabled — query the
  live paging mode, never assume) with a dedicated error code;
  stores under the scheduler lock. x86_64 loads FS_BASE only —
  per-CPU GS is untouched (swapgs audit invariant holds).
- Load path per arch on every user dispatch: x86_64 `WRMSR FS_BASE`,
  aarch64 `TPIDR_EL0`, riscv64 `tp` — publish the target in
  `switch_to_task` alongside CR3, apply in the ISR epilogue before
  user return (AGENTS-KERNEL-BRIEFING.md:21-30,126-132). The apply
  takes no lock and never runs across a reschedule (CODING_STYLE §11.1).
- Test snapshot must capture/reset `tls_base_` (test_isolate parity).

## 8. Verify (#75)

New userspace program (picolibc-linked) exercising `printf`, `malloc`
(`_sbrk` growth + free-list reuse), `scanf` (READ path) from a Ring 3
task; kernel class `libc_verify` asserts outputs via real dispatch.
Pass = hosted-C program runs to EXIT with correct output, zero
ResourceTracker delta.

## 9. POSIX time API (#76)

- `CLOCK_MONOTONIC` = `arch::Timer::ns_monotonic()` (v0.4.7 HRT);
  `CLOCK_REALTIME` = boot wall offset (x86 RTC / PL031) + monotonic
  delta. `struct timespec` conversions saturate (no 64-bit wrap).
- `NANOSLEEP`: wheel-armed bounded wait reusing the #18 machinery
  (`TimerWheel::arm`/`cancel` + per-TCB timeout slot with
  generation tag, the `recv_timeout_handle`/`gen` pattern in
  ipc.cpp:705-732 / task.hpp:496-506): zero timeout returns
  immediately (no infinite-wait encoding exists on this call);
  level-triggered re-apply on missed wakeups; killable via
  signals and death-drain (CODING_STYLE §11.2–11.3).
- `TIMER_CREATE` (one-shot/periodic wheel handles, generation-tagged,
  fail-closed table-full) + `TIMERFD_CREATE` (fd-table-owned expiry
  counter readable via READ; follows fd ownership/revoke rules).
- New timer-handle allocations are a ResourceTracker resource type
  (counters + track_* + test_isolate coverage) if not wheel-internal.
- picolibc `clock`, `time`, `nanosleep` route here.

## 10. Versioning over all areas (binding)

| Area | Version | Rule |
|---|---|---|
| Kernel | v0.5.0-dev → v0.5.0 | version.hpp + ROADMAP header (this cycle) |
| Syscall ABI | 1.0 | `NEXIOS_ABI_MAJOR/MINOR` + runtime `ABI_VERSION`; §4 rule |
| IRQ vectors | frozen §1 | new assignment ⇒ ABI minor bump |
| POSIX surface | ABI v1 + §5/#76 | additions only, same rule as §4 |
| picolibc | `PICOLIBC_VERSION` pin | bump = deliberate commit + re-verify (#75) |
| Docs | Doxyfile/README at release | same release procedure as v0.4.10 |

## 11. Test strategy (stub-first per PROMPT-dev.md)

- `syscall_abi`: frozen numbers (band boundaries), `ABI_VERSION`
  query == macros, 4-arg round-trip per live arch, error mapping.
- `libc_verify`: #75 program (printf/malloc/scanf), zero-leak.
- `posix_time`: clock sources monotonicity, nanosleep bound +
  expiry, timer one-shot/periodic, timerfd read semantics.
- Regressions: `syscall`, `storage` (fd paths), `smp`/`core`
  (TLS switch + affinity), `arch_cross`, `selftest`, full gates.

## 12. Safety notes (for planner/auditor)

- TLS base must be validated user-half (arbitrary FS base = S1).
- MSR/TPIDR load must complete before user return on every path,
  including the FAST path (S2 — wrong-thread errno/state).
- Nanosleep/timer waits obey §11.1–11.3 (no lock across reschedule,
  dequeue on BLOCKED, bounded or woken-with-timeout).
- Timerfd fds obey fd ownership/revoke/close-drain (S2).
- No kernel pointers across the ABI (§2); ENSURE-free reachable
  paths (CODING_STYLE §5 — version query, table-full, bad clockid
  return codes).
