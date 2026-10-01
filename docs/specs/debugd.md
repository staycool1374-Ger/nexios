# Userspace GDB Debug Daemon + Bare-Metal Debug Discipline (v0.5.1 design paper)

**Doc ID:** NEX-SPEC-2026-09-25-001
**Status:** DRAFT (pending SIL3 design audit; no implementation scheduled)
**Milestone target:** v0.5.1 Bring-up Multi-Arch Boot (design); implementation phases span v0.6.x/v1.0.x per §12
**Issues:** #169 (primary: debugd), #186 (context: bare-metal requirements + runelf introspection)
**Related:** `docs/debugging.md` (QEMU-GDB flow — debugd complements, never replaces),
`docs/specs/exception-table-audit.md`, `docs/specs/syscall-abi-picolibc.md`
(ABI v1, §2 register conventions), `docs/specs/ipc.md` §4 (deferred-switch rules),
#50 (capability-based security), #77 (runelf end-to-end), #164 (POSIX signals),
#191 (server supervision/restart), #43 (idle-task safety monitors)

## 0. Corrections to the issue texts (binding)

- **#169 "sys_task_read_regs(target_cap, ...)" do not exist.** No
  cap-gated debug syscalls exist in the tree. §3 designs them; they are
  new kernel API, not wrappers around existing calls.
- **#169 "TCP socket transport" is gated on Phase 9.** No kernel TCP/IP
  stack exists (#55 open). TCP transport is Phase 2 (§12); the v0.5.1
  transport is UART-IPC only, and the parser MUST NOT assume sockets.
- **#169 "asynchronous notification ... when a target hits #BP/brk" has
  no kernel mechanism.** Faults currently terminate user tasks
  (`riscv64_u_fault_handler`) or panic the kernel. §4 designs the
  debugger-attach routing that must land first.
- **#186 "Second Goal" (runelf introspection) needs shell integration.**
  Convenient scheduler/memory access for `runelf` targets is specified
  in §9; the `runelf` path itself is #77 (open) and out of scope here.
- **#186 POST codes are x86-only.** Port 0x80/0x300 cards do not exist
  on ARM/RISC-V; §8 normatively scopes POST to x86_64 and mandates the
  UART-crash-dump path on all arches instead.

## 1. Goals and non-goals

Goals:
- G1: Debug user ELFs (runelf targets, daemons) live on hardware and
  under QEMU via standard GDB, without freezing the whole machine
  (the QEMU `:1234` stub in `docs/debugging.md` freezes all CPUs and
  cannot debug a single task on bare metal at all).
- G2: Zero kernel bloat — all RSP parsing lives in a user task.
- G3: Bare-metal bring-up debuggability (early serial, POST, crash
  dumps) codified as normative constraints, not folklore.
- G4: Determinism preserved — debugging a task MUST NOT break the
  HRT guarantees of unrelated tasks (§10).

Non-goals (explicitly out):
- N1: Kernel self-debugging via debugd (kernel faults still panic;
  kernel debugging stays QEMU-GDB + serial).
- N2: TCP transport before the Phase 9 netstack exists.
- N3: Time-travel / reverse execution.
- N4: Debugging across a release gate (debugd is a debug-build and
  explicit-debug-mode facility; release builds MUST NOT expose the
  debug syscalls — see §10).

## 2. Architecture overview

```
 +------------------+   RSP over UART-IPC   +------------------+
 | GDB (host)       |<--------------------->| debugd (user task)|
 +------------------+                       +--------+---------+
                                                     | cap-gated debug syscalls
                                                     v (§3) + stop-event routing (§4)
                                             +--------+---------+
                                             | kernel: task control, breakpoints,
                                             | single-step, memory access
                                             +------------------+
```

- `debugd` is an ordinary user-space task (picolibc-linked), started
  only in debug mode / on explicit request. It owns no privilege
  beyond the debug capabilities explicitly granted to it (§3).
- The kernel exposes five debug syscalls (§3–§4: four data-plane calls plus
  `sys_task_debug_attach`) and a stop-event delivery channel (§4). No RSP,
  no string parsing, no packet buffers in the kernel.
- Relation to `docs/debugging.md`: QEMU-GDB remains the kernel-debug
  path. debugd is the *task-debug* path. The two MUST coexist (a
  kernel panic under QEMU-GDB while debugd holds a task stopped is
  legal; on panic all stop/park state is discarded with the machine —
  no resume-across-panic; a restarted debugd re-attaches explicitly
  per §10).

## 3. Kernel debug API (normative — new syscalls)

All four calls take a debug capability handle (`target_cap`), NOT a
raw pid. Capability semantics (grant/revoke, attenuation) follow #50;
until #50 lands, handles are kernel-issued unforgeable tokens scoped
 to one debugger↔target pair, revocable by killing either endpoint.
The initial handle for a task is issued to its launcher at spawn; attaching
to any other running task requires a grant from its launcher or supervisor.
Raw pid-to-handle lookup does not exist.

- `sys_task_read_regs(target_cap, &regs)` / `sys_task_write_regs`:
  arch-sized register file (x86_64 GPRs + RIP/RFLAGS;
  aarch64 x0–x30 + ELR/SPSR; riscv64 x1–x31 + sepc/sstatus).
  Reads/writes target the task's *saved trap frame*; if the target is
  currently running on any CPU the call requests a deferred stop at the
  next user-mode boundary (§4 stop mechanics) and returns `EAGAIN` until
  the corresponding stop event is queued — never torn reads, never parked
  mid-critical-section.
  Writing `pc`/`sepc`/`ELR` redirects execution (the continue/step
  primitive); writing other regs mutates state. Return: 0 or
  negative errno (`EBADF` bad handle, `ESRCH` target gone,
  `EPERM` revoked).
- `sys_task_read_mem(target_cap, virt, len, buf)` /
  `sys_task_write_mem`: copy through the *target's* page tables
  (never the debugger's). Unmapped ranges fail with `EFAULT`
  (partial copies report bytes done — the RSP `m` packet needs this
  for memory-region probing). Writes to executable pages are allowed
  (software breakpoints); I-cache coherence is the kernel's job
  (`fence.i` / ICIALLU / `ic ialluis` per arch — the #186
  QEMU-forgives-this pitfall is explicitly called out: TCG hides
  missing I-flushes, hardware does not).
- All four data-plane calls are valid only when the caller holds a live debug
  capability AND the target is in a debuggable state (alive,
  non-kernel; kernel tasks and PID 1's reaper internals reject with
  `EPERM`). Calls from interrupt context are rejected.
- Syscall numbers: allocated from the free tail (86–90: ATTACH,
  READ_REGS, WRITE_REGS, READ_MEM, WRITE_MEM), frozen under ABI v1
  like the rest of the table, with an explicit debug-only carve-out
  (release builds return `ENOSYS` per §10: same numbers, no behavior
  contract). (Correction, 2026-09-25: the draft's "caps range 51–60"
  was stale — that range holds CAP/IRQ/IOMMU/MMIO/FRAME/DEATH/PAGER.)
  Exact numbers assigned at implementation time; this spec reserves
  five contiguous slots (four data-plane + attach).
- Attach selectors (exact-five preservation): `attach(0, handle)` toggles
  detach for a valid owned handle (the only detach in Phase 1);
  `attach(1, child-pid)` mints a handle on parenthood proof (launcher
  claim); supervisor grants arrive in Phase 4 (§14 NEW). Raw pid-to-handle lookup
  does not exist.
- Grant resolution rule (normative, §14): handle resolution succeeds on
  an owned live binding OR a live grant with generation match, and
  only for `debugger_id == caller->id`. Denials are preserved exactly:
  `EPERM` (no grant / attenuation failure / kernel task), `EBADF`
  (bad, unknown, or foreign handle — the `debug_resolve` original),
  `ESRCH` (dead target), `EBUSY` (already debugged — a grant
  authorizes the attempt, never overrides `EBUSY`). No new errno
  exists for grants.
- Stops are taken at user-mode boundaries as observed at timer ticks:
  the tick records the interrupted PC per CPU, and the scheduler parks
  current when it carries a stop request, is RUNNING, and the recorded
  PC is below the user-space limit (nested ticks record kernel PCs and
  skip). No trap-epilogue surgery; no scheduler-lock taking in ISR
  context beyond what the tick already does.
- Copy-side faults (caller buffer) report EIO, distinct from walker-side
  EFAULT (unmapped target): tells the debugger which side faulted.

## 4. Stop-event routing (normative)

Console-quiet invariant (issue #270): a faulting user task never dumps
registers/CR/fault addresses on the console — only the one-line
`report_user_task_end` summary (dmesg 0xDC02 + serial + framebuffer).
Full detail stays reachable here: fault stops carry the vector/cause in
the StopEvent (num/addr fields), and `read_regs`/`read_mem` expose the
live trap frame on demand. The console dump block in kernel.cpp is
gated by `CONFIG_USER_FAULT_VERBOSE` (default off; marker
`ISSUE-270-USER-FAULT-VERBOSE`).

Today a user fault terminates the task. With a debugger attached:

- Attach model: `debugd` issues `sys_task_debug_attach(target_cap)` (the fifth
  reserved slot from §3 — no attach-flag alternative, so the §2 count is
  exact). Exactly one
  debugger per target; second attach fails `EBUSY`. Detach (or
  debugger death — see #191 supervision) resumes the target or, if
  it was fault-stopped, applies the default disposition (terminate).
  (Phase-2 implementation note, issue #226: "fault-stopped" means a
  genuine fault stop (kind FAULT); breakpoint/step stops are
  debugger-driven and resume like clean parks — GDB detach semantics.
  x86_64 additionally requires IDT gate 3 at DPL 3, otherwise a user
  `int3` raises #GP instead of #BP and breakpoints can never fire.)
- Stop conditions routed to the debugger instead of the default
  disposition: software breakpoints (§5), single-step traps,
  page faults / illegal instructions / alignment faults, and
  task death (exit/terminate — reported as `W`/`X` stop replies).
- While stopped, the target is descheduled and parked (never runs,
  never consumes budget — §10 accounting). Stops are taken only at
  user-mode boundaries (fault, breakpoint/trap return, or syscall-wait
  boundary with wait state preserved for resume-or-reap). Stop events queue
  to debugd in order; overflow policy is drop-oldest with a counter
  (bounded memory — no unbounded queues in an RT kernel), except task-death
  (`W`/`X`) events, which are never dropped (reserved slot). Every stop
  event carries a task-info snapshot (state, priority, budget remaining);
  this snapshot is the sole scheduler-introspection surface of §9 — no
  separate info syscall exists.
- Main-loop consumption contract (normative, §13 NEW): the debugd loop
  is the sole consumer of this queue — poll → dispatch → step/continue;
  it drains in loop order and never reorders §4 events. Debugger death
  and grant revocation compose here through the shared disposition
  function (fault-stopped terminate, cleanly-stopped resume).
- Breakpoint instructions per arch (single source of truth):
  x86_64 `int3` (0xCC), aarch64 `brk #0` (0xD4200000), riscv64
  `ebreak` (0x00100073) / `c.ebreak` where C is enabled. The kernel
  owns insertion bookkeeping (original instruction shadowed in
  kernel memory so overlapping breakpoints and detach-restore are
  exact); debugd only asks "break at VA". Source of truth on
  disagreement is the kernel shadow table — the controller keeps a
  write-through cache only (kernel wins, controller re-syncs).
- Single-step: arch-native where deterministic (x86 TF with IF masked
  across the one instruction); breakpoint-next-instruction emulation
  elsewhere — RISC-V (RVC length decode, SIE masked) and AArch64
  (fixed-4B ISA, temp brk at pc+4, no masking needed as the temp
  persists across preemption). The emulation avoids any dependence on
  the QEMU/silicon software-step debug model (MDSCR_EL1.SS); the SPSR.SS
  path is reserved for future use. MUST be specified per arch at
  implementation, not assumed uniform.

## 5. RSP parser core (`gdb_rsp.hpp`, header-only)

- Minimal packet subset (frozen for v1): `?`, `g/G`, `m/M`, `c/s`,
  `Z0/z0`, plus `qSupported` (feature negotiation: send fixed
  `PacketSize` + `swbreak+` only), `H` (thread selection — single
  thread model: `Hg0`/`Hc-1` accepted, everything else `E01`),
  `D` (detach), `k` (kill — maps to detach+terminate-target, NEVER
  kernel kill).
- Framing `$data#cs` with checksum verify (A–F/a–f), ACK (`+`) /
  retransmit (`-`) handling, `Ctrl-C` interrupt (`0x03` byte → stop
  the current continue).
- Zero-allocation discipline (from #169, binding): no `malloc`/`new`
  in packet processing; fixed-capacity stack buffers and ring
  views (`std::span`, `std::string_view`); parser state is a
  `constexpr`-bounded state machine. Worst-case packet size is a
  compile-time constant; oversize input is rejected, never grown
  into.
- Register serialization order per arch is specified in an appendix
  table at implementation time (GDB `g` blob layout: x86_64 Linux
  order, AArch64 order, RISC-V order) — the parser MUST NOT hardcode
  one arch; layout selected by target arch id at attach.

## 6. Task controller abstraction (`TargetTaskController`)

- Pure interface between RSP verbs and §3 syscalls (issue text's
  shape kept): `read_regs/write_regs/read_mem/write_mem/break_at/
  clear_break/step/cont/stop_reason`. No transport knowledge, no
  RSP knowledge.
- Controller owns: breakpoint shadow table (VA → original halfword/
  word, bounded, fixed capacity), stop-reason latching (last stop
  packet data cached for repeated `?`), and target-arch dispatch
  (register blob codecs per arch).
- Error mapping is total: every kernel errno has a defined RSP
  encoding (`E01` generic, `E0N` for the common cases, empty reply
  for unsupported — per GDB convention, never a dropped packet).

## 7. Transports (`ITransport`)

- Concept: `read_exact(span) -> bool`, `write_all(span) -> bool`,
  both with bounded blocking (caller-supplied timeout; infinite
  block is forbidden — a wedged host MUST NOT wedge the kernel;
  timeouts surface as detach-with-target-resumed, §10).
- v1 adapter: UART-IPC byte stream (console/UART endpoint; shares
  nothing with the early-boot polled serial of §8 — by the time
  debugd runs, the interrupt driver owns the UART; concurrent
  Logger output on the RSP stream is forbidden during a debug session
  (dedicated endpoint or Logger fully muted) — interleaved bytes would
  invalidate §5 framing).
- v2 adapter (gated on Phase 9 netstack): TCP listener. The parser
  and controller MUST compile and pass tests with the TCP adapter
  absent (transport independence is unit-tested by building with a
  mock transport only).
- Host side needs no custom tooling: stock `gdb` +
  `target remote /dev/ttyUSBx` (or TCP later).

## 8. Bare-metal debug discipline (normative, from #186)

Debugd starts late; everything before it must still be debuggable.
These are MUST-level constraints on the kernel, independent of #169:

- Early serial: until the interrupt/UART driver is up, all output
  uses strictly synchronous polled UART (16550 0x3F8 on x86; MMIO
  UART on ARM/RISC-V). IRQ ring buffers before driver init are
  forbidden (they mask the very faults that kill bring-up).
- POST codes (x86_64 only): hex checkpoints to port 0x80/0x300 for
  crashes before serial is alive. ARM/RISC-V have no POST card —
  there the UART-crash-dump path (§8.3) is mandatory instead.
- Fail-fast crash dump: the first-chance exception path dumps raw
  registers (CR2/CR3/RIP/SP; TTBR/ELR/SP; stval/satp/sepc/sp) to
  UART and halts (`cli;hlt` / `wfi`) on unrecoverable faults. No
  recovery attempts, no Logger formatting (Logger may be the
  casualty).
- QEMU-vs-hardware pitfalls (each is a normative MUST with the
  rationale from #186): never trust assumed memory maps (validate
  E820/UEFI/DT); TLB invalidation complete on every path QEMU-TCG
  forgives (`INVLPG`, `DSB+TLBI`, `SFENCE.VMA`); cache attributes
  correct for DMA (non-cacheable where required); unaligned access
  never relied upon; memory barriers present where SMP requires
  them (QEMU serializes what silicon reorders); spurious-IRQ
  handling from day one; EOI exactly once, never early, never
  missing; DMA cache maintenance explicit (flush before, invalidate
  after); DMA address limits honored (32-bit/4K constraints).

## 9. Scheduler/memory introspection for runelf targets

(#186 second goal; consumer is the `runelf` shell path, #77.)
debugd doubles as the introspection engine — no second mechanism:

- A `runelf` target can be launched *stopped under debugd*
  (`runelf --debug prog.elf`: loader holds the task at entry,
  debugd attaches, GDB connects). This is the convenient path #186
  asks for — full register/memory/breakpoint access from the first
  instruction, with zero new shell machinery beyond the flag.
- Scheduler introspection is read-only via the stop-event task-info
  snapshots (§4 — state/priority/budget, no separate call, no new
  kernel surface). No write path to scheduler state (a debugger that
  can reprioritize tasks cannot reason about HRT — forbidden).
- Memory introspection reuses `sys_task_read_mem` (target tables);
  ELF symbol awareness comes from the file on the host (GDB
  `file prog.elf`), never from kernel parsing.

## 10. RT and safety constraints (binding)

- debugd runs at the lowest debuggable priority under a bounded
  budget (sporadic-server envelope); a chatty host cannot starve
  HRT tasks. Stopped targets consume zero budget; because stops are
  deferred to user-mode boundaries (§4), a stopped target holds no
  scheduler locks, and terminating one runs the standard task-reaper
  path (IPC objects and kernel-side resources released).
- Release builds MUST NOT expose §3–§4 (syscalls return `ENOSYS`,
  attach impossible). A release binary with debugger hooks is a
  safety defect, not a feature.
- Watchdog interaction: a stopped target MUST NOT trip task
  watchdogs (#41); the kernel suspends watchdog accounting for
  debugger-stopped tasks, marks them supervisor-visible as
  `DEBUG_STOPPED`, and on continue/detach realigns (never back-charges)
  their supervision windows.
- Debugger death (crash/kill of debugd) MUST fail-safe: all its
  targets detach with default dispositions applied (fault-stopped
  targets terminate; cleanly-stopped targets resume). No orphaned
  parked tasks — this composes with #191 supervision (a restarted
  debugd re-attaches explicitly; stale breakpoints are restored
  from kernel shadows, never assumed).
- Determinism: RSP I/O never allocates, never blocks unboundedly,
  never takes scheduler locks; stop-event queue is fixed-capacity
  with drop-oldest accounting.
- Loop timeouts (normative, §13 NEW): every blocking wait of the
  main loop (event poll, continue-with-stop, reg/mem I/O) carries a
  bounded per-I/O timeout; expiry runs the single uniform procedure
  — cancel the wait, then detach-with-target-resumed per the
  dispositions above. There is no infinite wait in the loop; a wedged
  host (or wedged debugd) MUST NOT wedge the kernel or leave a
  target stopped.

## 11. Verification strategy

- Unit (host-side, no target): mock-transport tests for framing,
  checksums, ACK/retransmit, `g/G/m/M/c/s/Z0/z0` serialization per
  arch blob layout, oversize rejection, controller error mapping.
  These run in the normal gate (no hardware).
- Kernel API tests (per arch in the existing harness style):
  attach/EBUSY, regs round-trip, mem R/W incl. EFAULT partials,
  breakpoint insert/hit/clear, detach-resume, debugger-death
  fail-safe, release-build ENOSYS. Each new syscall gets the
  take-semantics + isolation treatment of the existing suite.
- Bare-metal checklist (§8) is verified by code audit + the
  bring-up runs (RPi4 #32–#39, RISC-V hardware when available),
  not by QEMU (QEMU cannot falsify the pitfalls by construction).
- Determinism tests: HRT task set runs its WCET envelope while a
  debug session hammers an unrelated target (sustained RSP load);
  any deadline miss fails the test.

## 12. Phased delivery (proposed sub-issues — no code scheduled here)

1. Kernel debug syscalls (§3) + tests (gated on #50 capability
   model for handle semantics; interim token design specified).
2. Stop-event routing + breakpoint shadows + per-arch step (§4–§5
   kernel half; arch matrix: int3 / brk / ebreak).
3. RSP parser core + mock-transport unit tests (§5–§6, host-runnable
   first — no kernel needed).
4. debugd main loop + UART-IPC transport (§2, §7 v1).
   REDUCED SCOPE (issue #232, v0.5.2): the main loop (§13 NEW) and
   the supervisor-grant model (§14 NEW) are specified now;
   the target C++ runtime, the launch/test-target story, and the
   UART-IPC transport endpoint are deferred to later milestones —
   deferred, not denied. No normative transport-byte or runtime
   sentence may appear outside this pointer until those phases.
5. `runelf --debug` integration (§9, gated on #77).
6. Bare-metal checklist audit (§8) against RPi4/RISC-V bring-up.
7. TCP transport (§7 v2, gated on Phase 9 netstack #55).
8. Determinism qualification (§10–§11 envelope tests).

## 13. debugd main loop v1 (normative — reduced Phase 4 scope)

Single-threaded event loop: one loop, no worker threads, no concurrent
RSP processing while a continue is outstanding except Ctrl-C stop
injection (§13.3). Transport bytes are out of scope (deferred
UART-IPC endpoint, §12 item 4); this section normatives the loop's
protocol behavior only.

- §13.1 Loop shape (pseudocontract): `recv RSP packet → validate +
  ACK → TargetTaskController dispatch → §3 syscall(s) → RSP response`.
  Exactly one outstanding request per connection; responses never
  interleave.
- §13.2 ACK/sequencing: `+` acknowledges a well-formed packet (sender
  retransmits on `-` or timeout); malformed packets are NACKed with
  `-` and dropped without dispatch. Ordering follows §13.1; the loop
  MUST NOT pipeline a second request before responding to the first.
- §13.3 Ctrl-C: byte `0x03` interrupts only the current outstanding
  continue (stop injection on that handle set, §4 stop mechanics).
  When idle it is a no-op heartbeat — never a global attach,
  broadcast, or detach.
- §13.4 Event consumption: poll (§3 event-poll selector) → dispatch
  stop event → step/continue (§3 step/continue selectors) →
  read/write regs/mem (§3 data selectors), referencing §4
  stop-routing/park/event-queue and §5–§6 RSP verbs without
  duplicating them. The queue drains in loop order (§4); the loop
  never reorders events.
- §13.5 Per-I/O timeout table: every blocking wait — event poll,
  continue-with-stop, reg/mem I/O — carries a bounded timeout
  (values assigned at implementation, all finite). Expiry runs one
  uniform procedure: cancel the wait, then
  detach-with-target-resumed per §10/§4 dispositions. There is no
  infinite wait; the "no infinite wait" sentence is a conformance
  requirement on the implementation, verified by the timeout tests
  in §11.
- §13.6 Discovery rule: the loop learns targets ONLY via (i)
  launcher-claim handles (parent-only §3 attach selector) or (ii)
  supervisor-grant objects (§14). Attach-style requests (in
  whatever RSP spelling the transport uses — no new verb is
  required) without a handle-or-grant are rejected `EPERM`.
  Pid-scan and pid-to-handle lookup do not exist.

## 14. Supervisor-grant model for non-child attach (normative)

Fulfills the §3 promise ("attaching to any other running task
requires a grant from its launcher or supervisor"). Issue #50
(capability attenuation) is a v0.7.1 placeholder with no wire
shape, so v1 defines a debug-specific grant record now; the
record reserves an opaque `attenuation` field for future
reconciliation with #50, which MUST NOT be interpreted until then.

- §14.1 Grantor roles: (a) a launcher may grant its claim right
  over its OWN claimed children to a named debugger; (b) PID 1
  (init/supervisor) may grant for any user task. Nobody else may
  grant — not arbitrary third-party debuggers, not the target
  itself. Kernel tasks and PID 1 internals are never grantable
  (data-plane `EPERM` per §3 stands regardless of grants).
- §14.2 Grant record shape: (grantee debugger-id, target
  reference, nonce/generation, single-use-or-expiry flag, opaque
  `attenuation` reservation). Mint path (decided): a DEDICATED
  NEW attach selector (number assigned at implementation —
  RECOMMENDED next free after the Phase-2 selectors; no new
  syscall number, selectors are cheap) taking
  (target-selector, grantee-id), callable only by §14.1 grantors.
  REJECTED alternative: mint-for-others through the existing
  handle-toggle path with no new selector — impossible without an
  impersonation or transfer operation, which would be a new
  selector in disguise; rejected for dishonesty. REJECTED
  alternative 2: waiting for #50 attenuation — blocks Phase 4 on
  v0.7.1; rejected by scope.
- §14.3 Resolve rule: handle resolution (cf.
  `debug_resolve` in `src/kernel/syscall/syscall_handlers_debug.cpp`,
  by reference without re-specifying code) succeeds on an owned live
  binding OR a live grant with generation match, and only for
  `debugger_id == caller->id`. Unknown/foreign handles fail with
  the original errors (`EBADF`); dead-target detach drops the owned
  binding as today.
- §14.4 Denial table (preserved exactly, no new errno). Mint path
  (target-selector key, cf. sel-1 claim precedent): `EPERM` = no
  grant / attenuation failure / kernel task / caller not a §14.1
  grantor; `ESRCH` = bad target reference or dead target;
  `EBUSY` = target already debugged. Handle-resolve path (cf.
  §14.3): `EBADF` = bad, unknown, or foreign handle; `ESRCH` =
  dead target. A grant authorizes the attach ATTEMPT only —
  grant-present-but-already-debugged is `EBUSY`, never overridden.
- §14.5 Revocation + death composition: revoke triggers are
  explicit revoke, grantor death, grantee death, and target
  death/exit. Any trigger invalidates the grant synchronously
  for subsequent resolves (generation bump; in-flight syscalls
  holding a resolved binding complete or fail closed, never
  re-resolve mid-call). Disposition runs through the ONE shared
  function with debugger death: fault-stopped targets terminate,
  cleanly-stopped targets resume (§4/§10). Grantee death drops
  granted bindings exactly like owned bindings — no orphaned
  parked tasks, composes with #191 supervision (restarted
  debugd re-attaches explicitly, §13.6 discovery reapplies).

## 15. Open questions (for audit, not blockers)

- Q1: Attach granularity — whole-task only, or per-thread when
  #52 threads land? (Spec assumes task; threads reopen it.)
- Q2: Should stop events integrate with POSIX signals (#164) or
  stay a parallel mechanism? (Spec keeps them parallel; signals
  are target-visible, stops are debugger-visible.)
- Q3: Multi-debugger (two GDBs, two targets) — allowed by design
  (one debugger per target) but untested at scale; no global
  debugger lock is specified — is that sufficient?
- Q4: `qXfer:features` / target XML — needed for modern GDB arch
  detection; deferred to implementation (fixed minimal XML per
  arch, no dynamic generation).
