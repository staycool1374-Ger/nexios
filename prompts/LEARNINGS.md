# LEARNINGS — Knowledge Base (FEEDBACK transition artifact)

> Implementation guidance only: traps, invariants, and handling rules that
> future work must not re-violate. No history — dates, commits, gate counts,
> and audit sagas live in git, test-history.txt, and the issues themselves.
> Each entry keeps its `### #N` header (the `check-feedback` gate requires
> the string); entries judged to carry no durable lesson were removed.

## Active cautions (curated digest — read at session start; cap ~12 lines)

- User-range byte math: guard `count <= LIMIT / sizeof(T)` BEFORE multiplying — `count*sizeof` wraps to 0 and passes the `size==0` early-true; fix every site sharing the idiom, and bump both fine + aggregate count rows (#337).
- TCBs are memset-zeroed, never ctor-built: set every non-zero sentinel at all 4 memset sites (create/create_user/clone/elf finalize) plus a mechanical defense that never trusts the value (see #3).
- Release the primitive lock BEFORE reschedule; every BLOCKED leaves the ready queue (INV-2); waits are bounded or scheduler-mediated; test bridge sequences guard add_task too (§11, #208).
- Test waits use need_resched + hlt (wait_for_termination_safe); BLOCKED + dequeue_ready; bare pause-spins starve ticks under TCG. Rendezvous tests must wake on message arrivals, never on third-party timing (tick-dependent peers/killers hang under load); cyclic-capable walks get trip-count fail-stops, inserts unlink-first.
- Post-edit grep every site after repeated-pattern edits (replaceAll missed the differently-indented handle_irq drain — auditor S2, #307).
- x86 trap regs[17]=RIP [18]=CS [19]=RFLAGS [20]=RSP(user-only): CS-gate SP; new .cpp needs gen.mk regen; GAS: `msr daifset, #2`; test TUs compile in release so CONFIG_DEBUG seam users must be guarded (#308).
- Single-asm stack-save for GPR dumps (multi-block mov corrupts); exc_table is x86-only; release 0/0 rows are link-boot, not coverage (#306).
- Work-begun retrieval comment BEFORE implementing (iter-1 REJECT was procedural check-7 with zero code findings); `init` is not a class (`task_init` is) (#305).
- Manual QEMU runs must mirror harness flags (UEFI pflash for El Torito ISOs); isa-debugcon chardevs + self-exit for byte-exact port proof; never SIGKILL proof runs (#304).
- Flake hunts: identical binary + controlled load>2 (uptime per run) + 3 consecutive runs; reap load procs after; TEST_SERIAL_LOG per run; stash-prove off-arch failures on unmodified main (#383).
- No hardcoded addresses anywhere: arch HAL headers own every MMIO/memmap literal (tests include them); LSP evaluates headers in the wrong arch config (e.g. x86 HHDM) — the build is truth (#387).
- riscv fw_cfg sits in UART 2MB Device leaf (check existing coverage before adding boot mappings); per-arch DTB addresses differ, dumpdtb per arch; VMM phys helpers reject sub-page addrs (page-aligned staging + manual offsets); fw_cfg DMA needs the file index in control[31:16] (#394).
- Paper gates lose to test reality: convict access removals with a live run first (planner C28 broke 8 green mock-port DMA tests — reverted verbatim); claim_slot arch traps — riscv SYSCALL=2 is in-window (reject unconditional), x86 entries_ always populated (has_handler probe non-x86 only); GIC/PLIC preempt same-CPU → IrqGuard-before-lock off-x86, x86 keeps lock-only (#354).
- JARVIS_TEST_PASS() does not return — explicit return + teardown-before-assert; flake-vs-regression gets a stash control first (worktree + picolibc copy when the fix is committed).
- Channels: test-time Logger routes to debugcon (immediate, unbuffered); serial lags ~4KB; klog ring (clear/run/read in IrqGuard) for logger asserts; `%d` reads 8 bytes — use `%lx` + u64 casts. COM1-only runs are blind in test mode (all Logger output goes to debugcon); file-backed debugcon buffers until process exit (useless for live watch); SeaBIOS prints nothing without a TTY (use OVMF or expect-grade TTY).
- send_sync has no reply matching: it pops the oldest queued message, so any inbox holding a non-reply (grant, stale reply) during send_sync eats it AND fails the call (#296) — callers must drain solicited messages first (plain recv, no authorize) before any authorizing call; design new IPC waits with sender matching.
- send_sync reply contract (userspace): in-tree libc drops `reply_buf` — the kernel writes the reply over the REQUEST buffer; read result/payload from `req[]` (result `req[0..8)`, payload `req[8..40)`), keep `req` ≥ 40 B (see #315, contract follow-up #317).
- E2E serial is multiplexed: terminal CRLF/tab expansion + `\n`-prefixed trailers + async kernel logs + shared-/dev/tty dual readers corrupt naive window compares — translation-free golds (no 0x0A/0x09), framing-aware windowing, quiet guests (drop the HD; OVMF+q35+TCP is the proven path, SeaBIOS IDE-CD wedges code 0009 here).
- Verify edit drafts against `git diff` content before building: tool reads can misreport a single line (a phantom `va_list` copied into stdio.h broke the build — caught by build, restored byte-identical).
- Concurrent checkouts share one host: scope every host-kill to a per-checkout `-name` tag with a trailing-delimiter guard (prefix trap: `jarvis` ⊂ `jarvis_test`) and namespace every fixed /tmp harness path per checkout — unscoped pkill + shared verdict/serial files cause mutual kills and wrong PASS/FAIL attribution (#372).
- Audit check #7 needs graphify/vault queries with dispositions on the thread BEFORE the call; rejected patches apply verbatim but verify first (git apply --check — patches can be corrupt).
- TEMP-DIAG fully reverted (grep-verified); QEMU reaped after every session; `-serial mon:stdio` eats typed bytes — TCP/file serial for E2E input.
- Freeze ticks (IrqGuard) around ISR-observed measurements; expiries derive from the live clock, never fixed bases; relative-over-absolute timing asserts (≥3× headroom); watchdog kills get STATUS history rows.
- Zero-S-line TIMEOUT + identical-binary rerun green = TCG stall, not regression (check host load first); debug-green never implies release-green (build release before any full gate).
- Hunt-loop evidence is file-grade or it didn't happen: tee every loop to `/tmp/jarvis-hunt-<issue>-<YYYYMMDD-HHMMSS>.log`, record per-iteration PASS/TIMEOUT + load average inline, and post the file path + tally as an issue comment the same session (serial log is shared/singleton — raw output not tee'd is lost); never pipe make/test through `head` (SIGPIPE kills the run). GDB stops the guest: one interrupt + full capture + detach, never touch a timed run; `call` lies on the TCG stub (raw reads only); S-lines live in debugcon, not serial.
- Pure-move splits: boundary-asserted chunks, file-statics to the first-consumer shared home (never per-TU copies), using-decls for every bare cross-TU name, second-`namespace`-blocks closed before reopening (trust the compiler over stale LSP diagnostics); test class names must match the registry exactly (`Unknown test class` = zero tests + QEMU_EXIT stall).
- Wall-truth telemetry: use rewind-proof raw TSC (`ticks_` is snapshot-rewound); additive summary lines go AFTER the expect `====` terminator; unobservable targets get DOCUMENT decisions, not servos (#359).
- Virtualized wall accuracy gates nothing on any arch (aarch64 +28%, riscv64 +27.6% over) — bare metal only; verify numbers in the raw serial capture, never the expect-processed stream (#384).
- riscv trap epilogue now validates deferred arms (gen-check + validate_pending_switch) like x86 — an unvalidated apply resumes freed/foreign memory with stale current, killing the wrong task (#385); M-threshold=7 is OpenSBI boot state, not corruption; fixtures must live above the post-W^X image range.
- GIC version comes from PIDR2, never TYPER; TVAL reads are derived (cval-count) — gate `remaining()` on CTL; validator relaxations only remove failures, never add them (#349).
- Readdir lists only the underlying fs: mount points live solely in the VFS mount table, so every `ops->readdir` caller hides them — overlay at the VFS layer (resolve + vnode_identity_match, spelling-independent), never per-fs; synthesized entries need a resolve-fallback for type probes (#346).
- Timeout wake is edge-triggered: one expiry → one READY wake (skip duplicate READY+queued re-wakes); EDF never returns current (self-pick guard) — both one-line guards, no new locks, stranded READY still rescued (#345).
- TCG wall dilation is load-bearing: size every timeout tier from measured walls; re-verify flakes before bisecting.
- Reverted ENSUREs are evidence, not failure: an entry ENSURE that fires on a legal path proves the contract — not the code — is wrong (defer_kill/flush/set_task_ready all assumed a held lock the lock-free tail doesn't hold). Revert immediately, document the true holder discipline, and re-verify; never narrow the test to protect the ENSURE. Take-site placement (at the guard, not the entry) is the correct form for conditional takes; try_lock-skip paths must NEVER get holder ENSUREs (skip is the design).
- IRQ-window hoists need caller-side reentrancy analysis FIRST: proving "no ISR calls X" is insufficient — prove "no ISR path can reach a caller of X while the window is open" (reschedule runs unlocked by design, so timer ISRs nest fully). A hoist that widens any ISR-reachable window needs identical-binary goldens + full core before/after, and a revert on the first audit S1/S2.
- test-history.txt is append-only evidence: never dedupe, never rewrite rows, never fill TIME from memory — a wrong-but-plausible entry is worse than a missing one (re-run to measure; mark unmeasurable runs TIME 0 with explicit STATUS).
- Reap stray QEMUs after every session (tool-killed interactive runs survive reparented, burning ~25% CPU each, masquerading as load spells); recipe-only Makefile edits don't rebuild cpio/ISO — rm the artifacts to force repack.
- Interactive user-task tests need a scripted handoff (`source`: loadelf/sleep/runelf/sleep — `fg` is a stub, /dev/tty shared); TCP-serial output before connect is lost (send CR first for a fresh prompt); convict user-space faults by pristine-binary control experiment, never by re-reading the derivation.
- Cross-arch: link all three arches for shared-header edits; arch-guard test bodies; riscv low half is identity/MMIO (clone copies L0[0]); aarch64 needs HHDM aliases + 48-bit phys masks; S-mode cannot touch M-mode CSRs; riscv64 never folds __builtin_memcmp (no freestanding provider) — manual compares in tests.
- QEMU `-kernel` passes no DTB on aarch64 (x0==0, DTB-primary needs a slot-table fallback); QEMU virtio-mmio is legacy v1 (PFN/Align queues); PCI addrs are uint64_t (ECAM HHDM truncates to 0x10000004 in u32); release-boot silence is never assumed working — prove it (#355, #393 was SIMD-before-fp_enable).
- Logger `%lx` already prefixes `0x` (print_hex) — never write literal `0x%lx`; dmesg catalog codes collide silently at push time — check `dmesg_catalog.hpp` before picking `+N` (#354: +11 was framebuffer's, +12 + catalog row fixed it).
- Snapshot restore: release every capability-registry `KernelObject` pin (FrameUserMap/MmioUserMap/PagerRegistry) BEFORE `MemPool::restore_pool_data()` byte-rewinds block data — release-after-rewind over-releases (L→0) and disposes a cap the restored CSpace still points at (slot occupied/gen-matched but `acquire()` reads 0xDD poison = #320); a lazy-mapping pre-snapshot daemon is the minimal trigger.
- IPC-quiescence: the suite snapshot must be taken with EVERY task inbox empty — the per-task `MessageQueue` is embedded in the TCB and byte-captured, so an in-flight message replays on every `snapshot_create`/restore (#321). Convict/dismiss a "duplicate IPC" theory with counters (distinctive-type sends in `IPC::send` vs receipts in `IPC::recv`) before touching the queue; when a bug is non-reproducible, ship a structural guarantee + forward invariant test, not a fabricated A/B.
- Harness stall handling: retry a class ONCE on the zero-S-line stall signatures only (`RESULT: TIMEOUT`/`QEMU_EXIT`), never on a genuine test FAIL; mark `STALL-RECOVERED` only when the retry actually PASSes (guard the recovered tag on both the PASS arm and `nfail==0`), `STALL-UNRECOVERED` otherwise; size the outer per-class timeout for the doubled worst case (#322). Distinguish "no summary because it stalled" from "no summary because it crashed" before retrying.
- reschedule() arms only, ticks switch: harness yields never bound a higher-prio peer's progress — deterministic cross-task tests mask the tick across wake→decision→act (peer frozen, observed state provable); polling for a mid-flight state cannot catch tick-driven jumps (#267).
- Never switch ARCH mid-task: the arch-change clean wipes build/ (including any project-local toolchain); shared `find build` patterns must exclude non-build homes (a `*.d` DIRECTORY breaks DEPFILES -include) (#273).
- Arm-or-skip symmetry: a silent-skip writer (walk-fail → return) must never pair with an always-armed record — map the span before install; page-aligned memsz deliberately skips DATA-after (anti-corruption), so regression fixtures use the exact live segment shape (#271).
- Flat-store integrity: scrub the carve (not the grant) for pristine-zero; CRC32 the metadata region (verify at load) + each file's LIVE bytes (verify lazily, computed on close); verify the loaded dir array BEFORE publishing the cache (`cached ⇒ verified`) so a concurrent lookup never sees unverified bytes; a 96 B dir entry straddles a 512 B block — the store must split it (the naive "skip straddling entries" silently drops every entry ≥ index 5) (#318).
- Pre-suite stall (#299/#280) is HARNESS STARVATION, not a dead tick or stuck dispatcher: `is_test_active()` gates BOTH the on_tick monitor-wake and the harness-nonpreempt protection, so the window before the suite is marked active leaves the prio-10 harness unprotected against the prio-127 monitor woken every tick. Mark the suite active (inject the test context + set_test_active) BEFORE `ensure_monitor()`/`snapshot_create()`. Distinguish starvation from a dead tick with counters: `ticks`/`rate_monotonic_schedule`/`switch_to_task` advancing = ticks alive (#300 is the dead-tick case: a task-context on_tick corrupts the APIC source).
- VMM split/flag-day discipline (#363): pure-move splits verify by reconstruction (concatenated chunks == original bytes) with a shared-internal header up front for file-statics (per-TU copies would fork throttle counters); bulk renames need a post-pass token grep (`0xFFF` glues into `kPageOffsetMaskF...`); test leaf-phys extraction must use the canonical translation or production frame mask (NX survives `& ~0xFFF`); HAL reuse needs a bit-identity proof (attr SH/NS+AP diverges); new TUs need gen.mk regen; `-Werror` makes overload removal atomic (missed caller = build break).
- Glue prototypes must match stub definitions exactly (int vs long): a wrong `long` decl suppresses call-site sign-extension, so -1 arrives as +4294967295 and `ret < 0` never fires while int truncation still reads -1 — prove with disassembly (`cltq` present/absent), then pin eno in-test (#303).
- Text pages must never pass through NX while live: map executable first (RWX window), then clear W — try-map installs non-exec + flushes internally, so an NX window triple-faults the next fetch (supervisor I-fetch #PF err=0x11 at the executing page). Split fills must carry the full permission set (x86 NX, aarch64 AP bits) or post-harden splits silently re-widen; aarch64 RO checks key on AP_RO alone (EL1-only AP=10 leaves are RO too) (#376).
- Post-W^X, HHDM fixtures live above the hardened image range (low PD entries are split, no longer huge) — relocate same-shape pages (idx 16/17/18), asserts unchanged in strength, never weakened to masks (#376).
- Daemon replies route to `msg.args[0]`, never to the IPC sender: probing with a bogus pid strands the requester's send_sync (daemon's reply-send fails fast, requester hangs) — always pass the live sender pid; transient register/unregister pairs keep fixtures zero-residue (#303 item 4).
- picolibc errno-function knob is `-Derrno-function=__errno`, NOT `-Dthread-local-storage=true` (TLS emits PT_TLS and breaks the loader-unchanged gate); TP locality needs no ELF TLS; the knob lives in tools/build-picolibc.sh, the sysroot stays gitignored (#303).
- Checked-unmap discipline (#366): reworked `_err` probes presence BEFORE delegating to the side-effecting void twin; cap-unmap twins skip revoked/IO checks (revoke path unmaps itself); revoke/teardown fold-and-ignore with `(void)` (no logging under registry locks — lock-order hazard), log-and-continue only on already-logging boot/teardown paths; hole-coverage tests ship in the same commit (probe-guarded, never unmap live kernel state); test-history rows carry measured times only (re-run, don't estimate).
- Audit check #7 needs per-change artifacts (#324): session-start graphify/vault retrieval does not cover later increments — every non-trivial tick posts its own query + dispositions or eats a process-only REJECT.
- One-shot safety arms must retain, never rely on re-detection (#324): a disarming scan or TERMINATED-skipping scan never re-fires, so a spared kill intent is lost until reap — compact-and-retain with a generation guard; same handshake closes lock-free check-then-act races (skip is the safe direction).
- Harness task is persistent suite state (#360): keep its CNode across snapshot_restore (daemon parity, #275) + pre-create it before the baseline capture, or every harness ensure_cspace mints a +1 CNode residual; kept pointers stay safe because the pool rewind resets slot bytes to baseline-empty.
- Never re-create what the factory installs (#371): create() ships a private cloned PML4 (MP-1.2) — a manual re-clone orphans the factory page (+1 PMM/test); drop creator refs on every cap path (creator/slot/pin ledger — a missing release reads as +1 Cap/MemPool, #360/#371).
- Fault injection must fail ATTEMPTS, never presence (#361): countdown gates go immediately before PMM::alloc_*, after presence/split-need checks, or present-table hits consume the count nondeterministically; per-call reset + single-shot auto-disarm + RAII guard + snapshot_reset backstop; free_pages_ref deltas are measured post-teardown (frames freed after capture read +N, not zero).
- Checkout-scoping is naming-convention only (#372): TAG = dir basename, so same-basename checkouts in different parents still collide — keep dirnames unique per host; GDB :1234 stub is NOT namespaced (debug-test/rr-replay from two checkouts fight over the port).
- Aggregate splits are pure regroups: script-count moved groups (each exactly once), Σ(new dump-counts)==old value, measured beats hand sums; pre-QEMU SIGTERM with zero guest output is a transient harness kill — identical rerun green, record both rows (#364).

## Entry format

```
### #<issue> — <short title>
- **Learned:** what new insight/technique/pitfall emerged this cycle
- **Adapted:** where this knowledge now lives (spec, AGENTS.md, code comment)
- **Style re-surface:** which CODING_STYLE.md rules were relevant and must stay in focus
```

## Entries

### #385 — riscv64 exec_slots hang: unvalidated deferred-switch apply (audit APPROVED, committed, issue CLOSED pending QE + #387)

- **Learned:** (1) The riscv trap epilogue applied deferred switches on `save_rsp_to != 0` ALONE — no generation re-check, no target validation (x86 has both; aarch64 documents the gap as an S3 limitation). A stale arm to freed-then-recycled memory resumes a foreign context with stale `current`: sret into an unmapped yield stub → U-fault terminates the wrong task (init) → apparent hang. Post-mortem slots read zero (consumed) — absence of arms proves nothing. (2) OpenSBI sets the M-mode PLIC threshold to 7 at boot — M-context test reads are meaningless once the kernel moves to S-context (not corruption; tables verified correct). (3) Post-W^X the kernel image covers HHDM fixture blocks (L1[2]/L1[3]) — fixtures must live above the image range. (4) `PMM::free_page` is idempotent-safe (bitmap guard) and MMIO frees no-op (window guard) — kill over-free theories by code reading before burning GDB runs.
- **Adapted:** x86-mirror guards in riscv `apply:` (gen capture + re-check → record_skip; validate call pre-load; abort path with fixup). `arch_riscv64` 24/30 + no hang ×3 deterministic; x86 `elf_loader`/`selftest` green; 3-arch link green. Handoffs: QE fixture relocation + PLIC S-context test updates; #387 (riscv fork fails, clone-path suspect).
- **Style re-surface:** validate-before-resume for every deferred-switch consumer; keep validation + side-effect-undo in one shared helper (drop_arm) so asm ports stay thin.

### #349 — aarch64/riscv64 IRQ + timer parity (audit APPROVED, committed, issue CLOSED)

- **Learned:** (1) QEMU TVAL reads are derived (`cval - count`), not stored — gate `remaining()` on CTL.ENABLE or a written 0 reads back nonzero. (2) riscv64 `set_frequency` passed a relative interval as an absolute SBI deadline (immediate-fire when boot mtime exceeds it) — absolute `now + interval` everywhere. (3) `GICD_TYPER[15:11]` is NOT a version field — version comes from `GICD_PIDR2[7:4]`, fail-safe to v2. (4) Validator relaxations are FAIL→PASS-only and can never regress a green suite — safe direction for fail-closed review. (5) GDB good-first-publish + multi-publish execution trace convicts return-path faults without touching code (method reused from #356).
- **Adapted:** GICv2 full init + PLIC S-context + CVAL/SBI absolute deadlines + storm limiter (`CONFIG_IRQ_STORM_LIMIT`) + docs §6 + gotcha #20; BOOT_ONLY latches with CTLR read-back; EOI ownership single-choke. Gates: aarch64 6 classes green, riscv64 5 classes green (+1 known QE test-side), x86 all green + selftest 148/148. Audit APPROVED zero findings (`audits/report-2026-10-19T15-04-51Z.md`; two prior APPROVED with 4 S3s dispositioned same-cycle).
- **Style re-surface:** saturating arithmetic on every deadline add (`now > ~0ULL - x ? ~0ULL : now + x`); sentinel `~0ULL` for never-acked state; no test files in kernel diffs.

### #384 — per-arch wall-time truth (docs-only close, audit APPROVED, issue CLOSED)

- **Learned:** (1) Virtualized wall measurement gates nothing on any arch: aarch64 +28% / riscv64 +27.6% over the ±25% band where x86 warn-skips — bare metal is the only quality gate, no TCG accuracy enforcement anywhere. (2) Survey before instrumenting: the Timer HAL is complete x3 with zero kernel stubs (`calibrate`/`raw_counter`/`rdtsc`/oneshot all real); riscv64 `remaining_ns()` hard-0 is an S-mode hardware constraint, not a stub. (3) A pre-first-test panic (ELR/sepc=0 at task spawn, daemons degraded) is a subsystem-routing signal — evidence went to #356 (process parity), not the timing issue. (4) Authoritative numbers live in the raw serial capture: x86 file-backed vs `-kernel` pty (where `\r` overwrites digits in the expect-processed view) — verify in the raw stream, never trust processed output. (5) Additive telemetry without a live consumer question is scope creep: the `TSC_FREQ_HZ` line died with its cancelled campaign.
- **Adapted:** gotcha #19 + deadline.md §7 per-arch verdict (OVERALL_CAP_S stays 3600); QE handoff (5 missing-test items) on #384; spawn evidence on #356. Gates: x86 hrt 6/6 + timing 19/19; off-x86 hrt 5/6 (expected ungated fails); timing panics → #356. Audit APPROVED zero findings (`audits/report-2026-10-18T22-05-51Z.md`).
- **Style re-surface:** docs-only changes still need audit + retrieval artifacts; test-history TIME measured or TIME 0 with explicit STATUS (corrected a from-memory 2734→2724 ms row same-cycle — wrong-but-plausible is worse than missing).

### #359 — TCG wall-time truth decided DOCUMENT + TSC_CYCLES telemetry (audit APPROVED, committed, issue CLOSED)

- **Learned:** (1) When measurement proves the target unobservable (no in-guest wall ref finer than 1 s RTC; workload-dependent TCG TSC rate), DOCUMENT-limitation is the correct engineering outcome — a runtime servo can at best fix long-run averages while adding ISR-adjacent SIL3 risk. (2) Ticks and ns are divergent virtual clock domains (periodic-APIC bus vs PIT-calibrated TSC); a servo on one cannot true the other. (3) Per-run telemetry must use a rewind-proof counter (`arch::Timer::raw_counter`, never snapshot-rewound `ticks_`), and additive summary lines go AFTER the expect `====` terminator so verdict parsing (`FAILED+====`, no end-anchor) is provably unaffected. (4) Emulated/self-audits do not close issues — only an independent sil3_auditor APPROVED verdict counts; process-only REJECTs (check #7) are cured with on-thread retrieval artifacts, no code change.
- **Adapted:** `TSC_CYCLES` summary line (src/lib/test.cpp: static + 4 run_* assignments + post-`====` print) + gotcha #19 in AGENTS-KERNEL-BRIEFING.md + wall-truth section in docs/specs/deadline.md (`OVERALL_CAP_S` stays 3600, accuracy gate is KVM/HW). Gates: hrt_monotonic 6/6, timing_core 19/19 (1 WARN-residual), random 17/17, full gate 25/26 + core_tracing rerun green. Audit APPROVED zero findings (`audits/report-2026-10-18T21-22-40Z.md`, real sil3_auditor; emulated REJECT→APPROVED trail superseded).
- **Style re-surface:** output-only harness changes still need expect-regex + renode-parser impact analysis; reuse cross-arch APIs (`raw_counter` x3) instead of new arch code; test-history rows carry measured times only.

### #361 — deterministic table-alloc fault injector + runtime rollback test (audit APPROVED, commit pending)

- **Learned:** (1) A partial-failure injector must gate allocation ATTEMPTS (post presence/split-need checks, pre-alloc) — gating lookups makes consumption order-dependent and nondeterministic. (2) A never-installed VA inside a boot huge window still resolves via the huge entry: asserting it clear mistakes the baseline mapping for a rollback leak — prove non-installation via the untouched PD slot instead. (3) PMM-delta captures that precede frame frees must expect +N (freed-after-capture), not zero; print each conjunct separately before folding into one assert, or a single FAIL line hides which limb broke.
- **Adapted:** VMM injector API (arm/disarm/begin/maybe + snapshot reset, x86_64 _try-path 3 gates, release no-op/absent) + vmm_map_range_rollback_segment (arm(2) across a 2 MiB boundary) + count rows (vmm_path 5, core_tracing 22); gates green incl. release-build-clean. Report `audits/report-2026-10-18T13-15-11Z.md`.
- **Style re-surface:** [[nodiscard]] on the status-returning gate; Doxygen on the VMM API decls; release/test-behavior parity (§6: gate code compiled out, trivial-pass branches).

### #371 — capability residual WARNs (PMM +1 x5, small-cap +1) (audits APPROVED, commit landed)

- **Learned:** (1) A manual `t->page_table_ = clone_kernel_pml4()` on a create()d task orphans the factory PML4 — create() installs a private clone since MP-1.2 (task.cpp:1136); all 5 Family-A tests shared the shape. (2) Conviction ladder for counter-vs-reality divergence: bisection (skip MAP still WARNs ⇒ not the map path; skip clone clears it) + post-drain is_allocated census on every footprint page (PML4/frame/kstack-span all clear yet counter +1 ⇒ lost-pointer orphan, not a teardown skip) + exact arithmetic (+18 = 16 kstack + factory PML4 + manual PML4). (3) A registry-map (not install) creator ref leaks identically to the install case — the delta-clean twin with release is the discriminator.
- **Adapted:** 5 clone deletions + 1 creator release (test-only); gates capability 155/155 zero WARNs (HEAD stash control: 24). Report `audits/report-2026-10-18T12-36-01Z.md`.
- **Style re-surface:** single-funnel init (never re-create factory resources); explicit refcount ledger per path (creator/slot/pin/lookup); test-history rows carry measured times only.

### #360 — harness-CNode + ioport creator-ref residuals (audits APPROVED, commit landed)

- **Learned:** (1) `restore_task_fields` detach NULLs the persistent harness cspace every cycle → each harness ensure_cspace mints +1 CNode; keep parity with daemons (#275) + pre-baseline creation cures it. (2) Worker-installed caps need the creator-ref drop even without slot-remove when death-drain disposes via the CNode. (3) The [RESOURCE] WARN call stack is the check path (addr2line), never the leak site — resolve frames before theorizing.
- **Adapted:** harness keep in scheduler_snapshot_fields.cpp + pre-create in snapshot_create; ioport worker release. Gates cap_pager 15/15 + cap_mmio 20/20 zero WARNs; capability 155/155 (control 24). Report `audits/report-20261018-121007.md`.
- **Style re-surface:** slot-holds-reference convention at every install site; snapshot/restore pointer-vs-content discipline (pointer persists, bytes rewind to baseline).
### #364 — core aggregate split into 7 core_* classes (audit APPROVED, merged into main)

- **Learned:** (1) Aggregate splits are pure regroups: verify mechanically by script-counting moved group calls (61, each exactly once) + Σ(new dump-counts)==old aggregate value (98+93+112+52+118+85+20=578); measured dump-counts are truth over hand-summed fine rows (core_memory measured 118 vs hand 120 — stale-row drift). (2) Planner-proposed partitions must be checked for silently dropped groups (planner's 518 vs actual 580 — data_structures + pip/pcp/pi_donation dropped from the partition). (3) A pre-QEMU harness SIGTERM (make `Terminated: 15` right after LD, zero guest output, no S-lines) is a transient harness kill, not a test defect — identical rerun green; record both rows STATUS-tagged. Distinct from zero-S-line TCG stalls (guest ran, no summary).
- **Adapted:** test_registry.cpp (7 core_* lambdas replacing `core`), test_expected_counts.hpp (7 measured rows + k_aggregates[]), scripts/run_all_classes.sh (CLASSES 16→22 aggregates), Makefile (help/timeout-filter/examples) — issue #364.
- **Style re-surface:** CODING_STYLE.md §2 naming (`core_*` semantic names; no `be*` prefix that trips the is_bench heuristic; exact-strcmp uniqueness vs `core_isolation`) + §8 formatting (4-space, ≤80 cols in the new lambdas).

### #362 — order-sensitive path-tracer fold (audit APPROVED, commit pending)

- **Learned:** (1) XOR is order-blind by construction (a^b==b^a) — a
  path-control scheme claiming "wrong order" detection must NOT use
  XOR; use an order-sensitive fold `chain = chain*K + bit` (K odd
  golden). (2) Expected values MUST be computed by the same constexpr
  fold (`path_expected(...)`), never hand-derived — a hand value and
  the runtime fold drift silently. (3) When the fold is not invertible,
  break/resume validation becomes "chain unchanged since handoff"
  (compare to the checkpoint), not a suffix-XOR. (4) Keep the
  bit-distinctness static_assert (OR of enum bits == all-ones mask) —
  a duplicated bit hides a skipped step; the fold-value asserts no
  longer cover it. (5) A 2-arg API change needs a caller grep
  (path_resume_validate had exactly one caller).
- **Adapted:** `path_mix`/`path_expected` in path_tracer.hpp; 3
  registries express expected via the fold; resume API 1-arg; new
  reorder test; path_tracer 7/7, core 577/577, release 21/21, bench
  38 cyc/step.
- **Style re-surface:** constexpr helpers over hand-derived constants;
  static_assert on enum bit-uniqueness.

### #347 — XOR path tracing (CLOSED; A–E all APPROVED)

- **Learned:** (1) XOR traces detect missing/duplicate steps, never
  reordering — checkpoints between order-sensitive steps are the only
  order proof. (2) Every traced function needs its own registry; a
  step/insert anchor must be OUTSIDE the conditional block it marks.
  (3) Instrumenting private functions forces a visibility decision
  (map_phys_range_try → public; justified: no hidden state). (4)
  Test TUs compile in release too — CONFIG_DEBUG-only symbols need
  `#if` guards with a trivial release pass; debug-only vars in
  mixed-branch code need `[[maybe_unused]]`. (5) Count rows: bump the
  FINE row AND every aggregate that includes it (bench 27→28); aggregate
  drift is warn-only but flagged. (6) Debug-only skip masks must be
  compiled out in release. (7) Adding any TU perturbs timing and can
  toggle the pre-existing reaper/zombie flake (zombie_count!=0 at
  test_zombie_cleanup.cpp:126) — confirm against HEAD + standalone
  before blaming the slice; host load ~3 correlates.
- **Adapted:** new `path_tracer.{hpp,cpp}` + per-subsystem registries
  (`scheduler_path`, `task_path`, `vmm_path`); 5 test classes
  (path_tracer 6, scheduler_dispatch_path 4, task_path 5, vmm_path 4,
  bench_path_tracer 1); core 576/576; release core 21/21; overhead
  25 cyc/step (TCG). Follow-up #361 (safe rollback injector).
- **Style re-surface:** [[nodiscard]] on validators; constinit state;
  static_assert on enums/expected constants; release-safe test guards.

### #347-D — VMM range-map segments (audit APPROVED, commit pending)

- **Learned:** (1) Instrumenting a private function forces a visibility
  decision — map_phys_range_try had to become public (judged safe: no
  hidden state, explicit pml4). (2) A full-PMM-drain fixture to force a
  mid-range alloc failure PAGE-FAULTED the kernel (freed an in-use
  page) — do not drain the PMM outside the OOM tests' guard; filed
  #361 for a safe injector. (3) Always fix a misleading coverage claim
  rather than leave a docstring asserting coverage that does not exist.
- **Adapted:** `vmm_path.hpp` (5 steps, 3 outcomes) + steps in
  map_phys_range_try; vmm.hpp visibility; 4 tests; #361 follow-up.
- **Style re-surface:** release build needs `[[maybe_unused]]` on
  variables only used in CONFIG_DEBUG branches.

### #347-C — task create/cleanup segments (audit APPROVED, commit pending)

- **Learned:** (1) Step/insert anchors must sit OUTSIDE the conditional
  blocks they mark — inserting after `parent->remove_child()` or
  `waiting_on_queue = nullptr` put the step inside the `if`, so the
  common path skipped it (chain 0x3FAF vs 0x3FFF; bits 4+6 missing).
  (2) Invalid class name silently runs *something* and stalls — verify
  the class exists in test_registry.cpp (zombie_cleanup ≠
  scheduler_zombie). (3) A reproducible core QEMU_EXIT is not a
  regression until reproduced on the committed baseline (git stash -u
  + rebuild + same class); host-load stall cluster strikes here.
  (4) Release build must be checked for new test files (TF_KERNEL
  still compiles); guard debug-only symbol access with #if and give a
  trivial release pass branch.
- **Adapted:** `task_path.hpp` (5+14 bits, static_asserts) + 19 step
  sites; 5 tests green; scheduler_zombie 5/5; core 572/572; release
  ISO builds.
- **Style re-surface:** N/A (build Errors 0).

### #347-B — dispatch segments (audit APPROVED after 1 REJECT, commit pending)

- **Learned:** (1) Test-task priorities: HIGHER number = higher
  priority (prio-11 starves prio-10, #242) AND must stay within
  CONFIG_PRIORITY_CEILING (127) — enqueue() indexes the level array
  directly, so prio-200 wrote OOB and broke the whole RQ (picked
  idle, chain 0). (2) Debug-only symbols in test TUs need
  CONFIG_DEBUG guards in the TEST file too (auditor S2, release
  compile break — rejected patch applied verbatim). (3) Capture
  results → unwind (arm clear, candidate re-enqueue, hook clear,
  drain) → assert LAST, or assert-abort leaks. (4) reaper flake:
  zombie_count!=0 in core but 5/5 standalone + 567/567 retry =
  stale-zombie timing, note and proceed.
- **Adapted:** `scheduler_path.hpp` + 8 step sites (reschedule arm,
  next_task select, 6× switch_to_task apply); 4 tests green;
  release ISO builds.
- **Style re-surface:** N/A (build Errors 0).

### #347-A — PathTracer core (audit APPROVED, commit pending)

- **Learned:** (1) XOR detects missing/duplicate steps, NEVER pure
  permutation (a^b == b^a) — documented in the header; order proof
  needs checkpoints between order-sensitive steps. (2) New .cpp
  needs `rm mk/cpp-rules.*.gen.mk` (generated, git-ignored) or the
  link fails with "No rule to make target". (3) `git add -N` new
  files before `git diff main` or the audit patch silently omits
  them. (4) "Error while processing" on ~every file is pre-existing
  batch-tool noise, not a slice signal. (5) Aggregate count rows
  stay drifted by convention (core 557 row vs 563 observed — green);
  only fine rows must match.
- **Adapted:** `src/kernel/core/path_tracer.{hpp,cpp}` + 6 tests
  (path_tracer 6/6, core 563/563).
- **Style re-surface:** N/A (new files clean; build Errors 0).

### #348-F — verification sweep, closing #348

- **Learned:** (1) Sweep discipline: re-run owning aggregates on the
  FINAL tree even when slices were green (headers are ubiquitous).
  (2) test-history rows need measured times — copied times are
  fabrication; re-run rather than paste. (3) One core QEMU_EXIT in
  the sweep (retry green) matches the session's flake cluster, not
  new cause (no source changed since green runs). (4) Count rows:
  fine rows bumped with tests; aggregate totals stay drifted by
  convention (observed > row tolerated, verified not enforced).
- **Adapted:** full gate sweep green (selftest/core/ipc/capability/
  proc_elf + hrt/bench/stress/testrunner); counts reconciled
  (20/11/13/6). #348 CLOSED (A–E + F, all audits APPROVED).
- **Style re-surface:** N/A (verification only, no source change).

### #348-E — constraints (audit APPROVED, commit pending)

- **Learned:** (1) Constraints are review-cheap to audit (restrict-only
  ⇒ behavior-neutral by construction when everything compiles).
  (2) Overload sets need care: constraining one overload reshapes
  resolution — verify pointer/integer/enum dispatch unchanged
  (nearly dropped the T* overload mid-edit). (3) Evaluated-no-change
  is a complete result for an audit tick (designated init already
  modern). (4) Long core runs flake-cluster under host load
  (zombie leftovers, partial reads, QEMU_EXIT) — re-run twice before
  digging; isolated-class green + re-run green = proceed with note.
- **Adapted:** 5 template constraints; designated-init evaluated.
- **Style re-surface:** ad-hoc requires for single-use callables over
  new concepts (no API without a second user).

### #348-D2 — Span/bit primitives + bounded adoption (audit APPROVED)

- **Learned:** (1) New-utility scope discipline: ship minimal API
  (dropped subspan — logic without users), static_assert the rest,
  adopt narrowly, document evaluated-skips like real decisions.
  (2) `capture_state` IGNORING its size arg was a latent bug class —
  Span+ENSURE fixed it for free. (3) #347-compat is a design input,
  not an afterthought: stable VMM signatures + stateless primitives
  keep the tracing design space open. (4) bit_cast does NOT replace
  inttoptr idiom or explicit-endian shifts — say so explicitly or
  someone will "clean them up" later.
- **Adapted:** span.hpp + bit.hpp with goldens; 2 Span adoptions.
- **Style re-surface:** transparency as a documented property for
  future-traced code.

### #348-D1 — enum-class migration (audit APPROVED, commit pending)

- **Learned:** (1) Scoped-enum migration is compiler-complete under
  -Werror: bulk rename + signature migration, iterate errors to zero.
  (2) ABI boundaries convert explicitly (static_cast + legacy-semantics
  comment); internals go fully typed. (3) `==0`/`!=0` and bare-`0`
  args are the long tail — grep them, don't eyeball. (4) Core
  QEMU_EXIT during task-creation bursts (pool8 draining, ~1000th
  task) is an order/load flake signature: isolated class green +
  re-run green + no migrated type on the path = proceed with note.
- **Adapted:** Enum concept + to_underlying; CapRights/IpcFlags class
  + ops + full migration; syscall boundary conversions.
- **Style re-surface:** short enumerators on scoped enums; NONE = 0
  sentinel instead of bare 0.

### #348-C — compile-time (audit APPROVED, commit pending)

- **Learned:** (1) Script value-semantics kill: `& ~0xFFF` → `& ~MASK`
  where MASK already contains the `~` inverts every address — boot
  triple-faults with zero serial. Bisect by stash-groups + clean
  rebuild (rules out incremental staleness). (2) constexpr needs
  bodies in headers (link users fail otherwise); in-class asserts
  can't evaluate private members (incomplete-class) — drop or move
  out with access. (3) evaluated-no-change is a valid slice outcome
  (macros un-convertible; sv39 assert covered by runtime tests).
  (4) riscv build catches what x86 can't (assert placement) — always
  link all three before commit.
- **Adapted:** constexpr validators + goldens; table mask + asserts;
  enum anchors; HHDM/SSTATUS naming.
- **Style re-surface:** masks need documented domains (table vs PTE);
  scripts reviewed as code (again).

### #348-B — low-risk attributes (audit APPROVED, commit pending)

- **Learned:** (1) Scripted edits need diff-review before build:
  one script stripped `static` from 3 tick locals (watchdog died —
  gates caught it) and another injected `noexcept` at 4 call sites
  (review caught it). Mechanical ≠ safe; always `git diff` scripts.
  (2) `[[nodiscard]]`-style placement rules bite again: attributes lead
  on statics; `constinit` needs static storage (automatics reject).
  (3) `using enum` with zero applicable sites = no churn (same class
  as the issue's own evaluated items). (4) Zombie-count tests are
  timing-sensitive under load — one leftover-zombie flake, green on
  re-run; re-run, don't chase.
- **Adapted:** likely/unlikely legs; constinit trivial statics;
  noexcept leaves+defs; checker noexcept-skip.
- **Style re-surface:** hints never change semantics (verify by
  reasoning, not testing); scripts reviewed as code.

### #348-A — nodiscard (audit APPROVED, commit pending)

- **Learned:** (1) `[[nodiscard]]` goes LEADING on statics
  (`[[nodiscard]] static T f()`; mid-position is a hard error).
  (2) -Werror makes the ignored-site set exhaustive by construction —
  just iterate the build. (3) Assert-choices need loss analysis:
  NONBLOCK contention sends MUST stay unchecked (fold or void), paired
  benchmark loops fold into existing asserts. (4) The memory checker's
  30-char snippet window breaks under attribute prefixes — strip
  attributes first. (5) Real bugs surface: ramdisk regrant reported OK
  on undelivered grants (now NOGRANT).
- **Adapted:** nodiscard on all error/status decls; ~20 call sites
  fixed; checker snippet fix.
- **Style re-surface:** explicit discard needs a rationale comment;
  benchmark purity is a valid rationale.

### #325-F — shared-table/TLB audit (audit APPROVED, closing #325)

- **Learned:** (1) Audit-first again: merge/TLB/PCID/free-vs-use were all
  already coherent (ownership skips, CPU0 pin, switch-flush, pcid purge,
  dead-table rule, closed #157/#158) — the deliverable is contract docs
  + one immutability test, not code. Knowing when NOT to change code is
  the audit's highest-value outcome. (2) Verify the full invariant
  chain on paper before writing docs: map-flush + switch-flush + pin +
  never-reactivate must ALL hold for local-only to suffice — one broken
  link (e.g. exec without switch-flush) voids the contract.
  (3) #325 full-cycle lessons: verify issue premises first (half the
  charter was stale/half-done); caller-migration slices must run owning
  aggregates; adjacent-type params need extra review; refusal-loop
  drains + hang caps for OOM tests; refusal beats exact-need under SMP.
- **Adapted:** merge/TLB/free-vs-use contract docs; pt_merge 5→6.
  #325 CLOSED (A–F all APPROVED).
- **Style re-surface:** contract docs carry the why-chain (§9); tests
  mirror file conventions (synthetic kernel-owned tables).

### #325-E — fork deep-copy audit (audit APPROVED, commit pending)

- **Learned:** (1) Audit BEFORE assuming defect: flag parity + MMIO skip
  were already correct verbatim on all arches — encoded as tests, zero
  code change for 80% of the charter. (2) The real defect was the
  quiet one: user huge entries copied BY VALUE = shared writable page
  parent/child; unreachable + fail-closed is strictly safer than alias.
  (3) OOM "partial state" needs the full teardown chain verified
  (destroy→cleanup→free_user_pages + USER-ownership of every partial),
  not just the caller's destroy call. (4) Hand-installed table entries
  in tests orphan lower tables — capture + free bypassed levels
  explicitly (free_user_pages only sees linked levels).
- **Adapted:** huge fail-closed ×4 sites; OOM contract doc; 3 tests
  (process_pml4_clone 10→13).
- **Style re-surface:** fail-closed over alias (§5 reachability rule);
  contract docs state the full invariant chain (§9).

### #325-D — NX posture (audit APPROVED, commit pending)

- **Learned:** (1) Policy defaults beat per-call discipline: flipping the
  cap-map default to NX fixed 2 registries + pager with zero caller edits;
  explicit opt-in (`executable=true`) keeps code paths possible.
  (2) Walk-only inspectors (RO precedent) are the sanctioned way to pin
  PTE bits from tests — no execution probes needed. (3) Migration
  audit order: data-only void→5-arg flips are behavior-safe iff the
  pages provably never execute (stacks/heaps/buffers); code/data-mixed
  (ELF pins) must keep X — check segment content, not just the call.
- **Adapted:** NX-default cap errs + bools; 6 data-site NX migrations;
  `pte_is_executable_in_pml4`; cap_mmio 19→20.
- **Style re-surface:** defaulted params preserve callers (§9 docs must
  state behavior changes); arch-bit symmetry in inspectors.

### #325-C — unmap naming + B swapped-arg catch

- **Learned:** (1) Caller-migration slices must run the aggregates OWNING
  the migrated callers: B ran cap_mmio/ipc/selftest/proc_elf but never
  `capability`, missing a swapped-arg bug (u64↔bool against
  (user,pml4,executable)) that mapped pager frames into pml4 0 — C's
  aggregate run caught it. (2) Adjacent bool/u64 params defeat review;
  the disabled swappable-params tidy check exists for exactly this.
  (3) Present-to-zero leaves make kernel NULL-derefs silent (supervisor
  present leaf) instead of #PF — the safety case for clear-leaf unmaps.
  (4) Not-present is provable unprivileged: offset-read 0 post-unmap
  (present-to-zero would yield the offset). (5) WARN-level +1 cap
  residuals pre-date #325-C (bisected, no factory in diff) — filed #360,
  PASS-with-WARN is not FAIL.
- **Adapted:** clear-leaf cap-unmaps + convention docs; pager arg fix;
  cap_mmio 18→19; #360 filed.
- **Style re-surface:** flush discipline (primitive flushes on mutating
  path only); Doxygen contract accuracy (§9).

### #325-B — real error propagation + B0 PMM pool-head fix

- **Learned:** (1) 100k takes from a 32k-page window PROVES a cycling free
  list — arithmetic beats theory; hang-guard caps turn stalls into loud
  failures. (2) `rebuild_free_list` reset `free_head_` but never
  `pool_free_head_` despite the hpp promising both — doc-vs-code gaps are
  real bugs; the OOM tests are now its regression proof. (3) Restoring a
  boot huge entry RE-MAPS the VA (virt != 0 is correct post-restore).
  (4) Kernel data mapped through a split PT is NOT freed by snapshot
  rewind — manual PD save/restore+free is the sanctioned pattern
  (cross_arch doctrine). (5) OOM-killer null + BSP IrqGuard + settle
  rounds = deterministic pool exhaustion under SMP; table-pool exact-need
  math is fragile (staleness/racers) — refusal-loops absorb both.
  (6) Caller migration must preserve NX bit-for-bit: my first pass
  silently flipped stack/heap to executable — caught pre-audit by
  re-reading the 5-arg call.
- **Adapted:** map_page_try/err real; executable param on in_pml4_err;
  elf/loader/pager migrations; memory_page_tables 9→11, core 555→557;
  A-test KEEP 3→4 (rollback was vacuous — clone eats one).
- **Style re-surface:** checked arithmetic (§6) on every base+count;
  Requires-preconditions (§9); bounded loops with hang-guard caps (§6).

### #325-A — VMM cap-map transactions (commit pending, audit APPROVED)

- **Learned:** (1) `alloc_page_table` falls back to the general pool when the
  table pool is empty — pool-exhaustion tests must drain BOTH pools exactly
  (table via `pool_used/total` + range check, general via intrusive HHDM
  chain, O(1) storage). (2) First SIL 3 REJECT this cycle caught two REAL
  arithmetic bugs of mine (phys wrap, mmio size roundup overflow) — the
  auditor earns its keep; the diff-patch protocol (apply verbatim + fix S3s
  + re-audit) works. (3) Shared range worker (`map_phys_range_try`)
  kills loop duplication between frame/mmio cap-map — factor before the
  auditor asks.
- **Adapted:** REVOKED/EMPTY_RANGE codes; fallible `map_page_in_pml4_try` +
  identical void wrapper; `_err` cap-map with rollback; cap_mmio 14→18.
- **Style re-surface:** checked arithmetic (§6, UINT64_MAX guards on every
  base+count), Requires-precondition docs (§9), bounded test loops (§6).

### #327 — aarch64 stack-marker link break (stale issue premise)

- **Learned:** (1) Verify issue premises against the tree before designing: the "empty span" claim died to one `grep` (`kernel_stack[16KiB]` in shared kernel.cpp) + `readelf` (spans 0x4028/0x4010/0x13F10) — the real defect was aarch64-only `adrp` reachability, a smaller fix than the issue scoped. (2) On aarch64, ANY reference to a low-VMA object (even address-of) emits a potentially out-of-range `adrp` — placement tests must use absolute linker symbols (immediate-materialized) instead. (3) Answering "are there placement tests" with a test that locks layout via immediates works on all arches without touching the checked values.
- **Adapted:** aarch64-only value-check guard; `integrity_stack_markers_span` (memory 2→3, core 554→555); SIL 3 APPROVED (`report-20261015T152834Z`, S3-only).
- **Style re-surface:** arch-guard symmetry (decl/def/call); minimal diff where nothing is broken (riscv untouched).

### #319 — TSC calibration hygiene landing (wall truth deferred to #359)

- **Learned:** (1) Under TCG the virtual TSC rate is workload-dependent (PIO exits retard it, pause-spins yield the vCPU, warm compute runs ~1GHz) while the PIT/APIC follow different virtual clocks — so no boot-time PIT scalar is wall-true for all regimes, and variance across boots (250M→363M) is environmental, not algorithmic. Prove it with host-timestamped markers (WALLMARK: ticks 0.8x, ns 4x, TSC 1G) + external E2E (`sleep 2` wall), never by more in-guest theorizing. (2) Revert discipline held twice: PIO-poll rewrite moved the number the wrong way (250M), warm recal read 3.76G with frozen mono — both reverted immediately, no stacking. (3) A test that cannot pass on the CI platform must warn-skip there (hypervisor CPUID bit, oneshot precedent), enforcing strictly only where trustworthy — a permanently-red gate is worse than an honest skip.
- **Adapted:** programmed PIT + min-window + median calibration (same gates/fallback); hypervisor-gated `hrt_wall_accuracy_bound` (hrt 5→6, deadline 145→146); SIL 3 APPROVED (`report-20261015T150149Z`); remainder filed as #359 (v0.5.5).
- **Style re-surface:** §5 fail-closed preserved (guards + fallback untouched); §6 named bounds reused (no new unbounded loops); §10 named sentinels for new constants.

### #358 — readdir cursor-stride follow-up (overlay skipped live)

- **Learned:** (1) Readdir `pos` is a per-hook CURSOR, not a yielded ordinal — initrd advances per raw cpio name, fat32 per raw slot (free/LFN skips included); any index derived as `pos - yielded_count` inflates and skips. Derive phase boundaries in cursor units (re-walk to the EOF cursor). (2) A fake that advances 1:1 per entry cannot catch cursor bugs — stride the fake (phantom slots) to lock real hook semantics; the negctrl then discriminates the exact shipped bug. (3) "No progress" reports need a stray-process check first: two orphaned QEMUs (aborted expect runs) held the disk lock and confused the session — reap + `Failed to get write lock` is the tell. Expect probes must `send "exit"` + expect eof so no strays survive.
- **Adapted:** EOF-cursor boundary + stride-2 fake; live `ls` E2E probe (`/tmp/jarvis-358-ls.exp`); SIL 3 APPROVED zero findings (`report-2026-10-15T11-41-59Z`).
- **Style re-surface:** §5 fail-closed on skew (pos < eof guard); §6 terminating walks (stuck-hook break kept); no new locks (§11 untouched).

### #344 — dump-counts mux-chardev redirect fix

- **Learned:** A `sed` substitution against a QEMU flag string fails silently when the flags evolve (mux `chardev:dbg` replaced `mon:stdio`) — the target then runs against unredirected output and reports "not found" instead of the real cause. Every flag-rewriting recipe needs a grep-guard per pattern plus a loud `ERROR + exit 1` when none matches; the coverage target already carried the correct idiom — grep the Makefile for the working precedent before inventing a new redirect.
- **Adapted:** `_run_dump_counts_qemu` now tries `chardev:dbg` → `mon:stdio` → loud failure; SIL 3 APPROVED zero findings (`report-2026-10-15T06-57-55Z`).
- **Style re-surface:** build-system changes still need retrieval artifacts + SIL 3 audit; reproduce-first applies to tooling too (the exact `grep: No such file` signature was captured pre-fix).

### #346 — readdir mount-point overlay (ls hid /mnt, /mnt/ramdisk)

- **Learned:** (1) `readdir` enumerates only the underlying fs's own entries while `resolve()` consults the mount table both ways — so mount points are traversable but never listed; the defect necessarily hid every mount (`/dev`, `/proc`, `/tmp` too), not just the reported two. (2) Vnode-identity matching (`resolve(parent)` + `vnode_identity_match`) makes the overlay spelling-independent (relative paths, `.`, trailing slashes need zero normalization) and path-free, so the same helper serves both `cmd_ls` (has path) and `sys_readdir` (fd only, no reverse map). (3) All in-tree readdir hooks share the ordinal-pos convention (+1/entry, untouched on EOF) — pos passthrough is safe, and the phase boundary is re-derived per call by re-walk. (4) A negative control (temporarily return EOF after phase 1) is a ~25 s run and proves the regression test discriminates — do it for every overlay-style fix.
- **Adapted:** `vfs::readdir_merged()` (vfs.cpp/vfs.hpp) + both callers + resolve-fallback color probe; `vfs_readdir_lists_mount_points` test (storage 166→167); SIL 3 APPROVED (`report-2026-10-15T06-35-44Z`, 2 S3 notes dispositioned harmless).
- **Style re-surface:** §5 fail-closed on reachable input (shrunk-dir early EOF, truncation guards); §10 named sentinels/bounds; §11 no locks across reschedule (helper holds none; mount table read unlocked like resolve()).

### #271 P2 — rerun builder + shell gate done (issue stays open for P3)

- **Learned:** (1) A shared finalizer tail is a contract: every field it assumes preset (kstack, page_table_, std fds) must be set by ALL callers — the rerun builder missed kstack and faulted on the entry-frame push (convicted via disassembly + register dump, not guessing). (2) Fresh writable pages must be allocated for RW segments before pin copy — the pre-check `virt_to_phys==0 → refuse` correctly caught the missing step with a site-distinct reason (generic reasons hide the site; distinct strings convicted it in one run). (3) Audit-grade teardown accounting: `page_table_=0`-before-destroy leaks the whole fresh space; drops must be epoch-guarded across supersede; doc claims must match implementation (tail returns false WITHOUT teardown).
- **Adapted:** open_std_fds moved into shared tail (single funnel); unmap-without-free + destroy + epoch-guarded drops on all 13 failure paths; memsz cap, orphan-page free, count clamps; fd asserts in test. Re-audit APPROVED after verbatim rejected-patch application.
- **Style re-surface:** site-distinct failure reasons (diagnosable first-try); shared-init-or-diverge (hand-rolled TCB init panics in teardown).

### #271 P1 — retention + outcome tracking done (issue stays open for P2-P3)

- **Learned:** (1) ISR-callable paths need IrqGuard+SpinLock: `report_user_task_end` runs from interrupt context (fault path) yet touches `ElfLoader::lock_` — plain SpinLock deadlocks (task holds lock, ISR preempts, spins forever). Every set-count site reachable from ISR now IrqGuard-masked. (2) Admission failure after `take_completed` adopted retention (task_refs=1) leaves stale `golden[].phys` dangling: destroy frees the phys via the TCB, but retention still points at it (UAF on next use / leak on abandon). Fix: `abandon_retention()` before destroy on the never-added path (zero/zero frees inline). (3) Staging vs retention two-stage (fill at load, adopt at take, abandon-first) keeps ineligible loads from polluting retention; request_load fail-before-free preserves a good retained set when a bad path fails validation.
- **Adapted:** Retention struct (golden+pin+refs+ro baseline), staging/adopt/abandon, outcome enum+hooks, unmap_golden wiring before free_user_pages, shell note_launched after admission, snapshot coverage; tests lifecycle+balance; second audit APPROVED after S1 fixes.
- **Style re-surface:** lock-then-copy staging (bounded walks only, never reschedule inside); outcome tracking via single funnel (report_user_task_end) never UNKNOWN for reported ends.

### #340 — iocd keyboard delivery done P0-P2 (wave 3: 3/4, issue closed)

- **Learned:** (1) Implicit routing (pid-gating) broke 7 legacy tests by starving the kernel ring — explicit ownership flag won on evidence; "simpler" without a switch is wrong when two consumers exist. (2) Daemon decode fires regardless of registration — self-registration would only add a forward-to-self loop; don't register what needs no routing (guard stayed as defense). (3) Tests that flip global routing must collect-then-clear-then-assert + snapshot-cover the flag (abort-leak safety). (4) Shell services can send_sync to daemons (fail-open, no retry) without liveness risk — precedent for future microkernel transitions.
- **Adapted:** P0 daemon, P1 forward+flag, P2 shell helper + interactive default; all APPROVED; pushed, issue closed. MMIO/SERIAL stubs explicitly left for separate issues.
- **Style re-surface:** never both paths (single-ownership invariant asserted, not just commented).

### #271 P0 — VMM RO primitive + load application done (issue stays open for P1-P3)

- **Learned:** (1) aarch64 EL0-RO = AP_USER|AP_RO (both bits) — clearing AP_USER gives no-EL0-access, not RO; verify encodings against the file's own constants, not memory. (2) Namespace-brace accounting: a leftover `} // namespace` silently closed `namespace kernel` — 12 cascade errors from one brace; always re-check block balance after restructuring edits. (3) Test out-param contracts: conditional-write out-params must be documented + never asserted on the negative path (second vacuous-assert catch this session). (4) Big features land phased (P0→P3) with the issue open and R-ticks pending — planner delegation first when no planner type exists (general subagent + schema).
- **Adapted:** RO primitive + inspector + range helper, both load paths, pin test both paths; elf_loader 16/16; APPROVED; pushed.
- **Style re-surface:** shared primitive for identical perms (first run included); trailing TLB flush at every map site.

### #339 — in-tree strtok/strtok_r done (wave 3: 2/4)

- **Learned:** (1) `src/libc` C files NEVER link into the kernel (kernel tests exercise `src/lib/string.hpp`) — a kernel unit test for libc would be undeclared AND unlinked; Ring-3 probe is the only honest vehicle. Check link topology before choosing test placement. (2) Suite-fail triage: tlb p99 assert + ioapic-setup stall + deadline outer TIMEOUT all cleared on identical-binary rerun = timer-sensitive cluster under back-to-back load, not regressions. (3) The stall-between-RUN_PRE-and-first-assert shape points at framework/setup, not the test body (which was 20ms-bounded).
- **Adapted:** strtok + decls, probe block, tls asserts (no new tests/counts); full debug 18/20 + smp/deadline reruns green; APPROVED; pushed, issue closed. scanf/fprintf stay picolibc-only per issue.
- **Style re-surface:** fail-closed NULL inputs; document thread-unsafety where it exists.

### #338 — thread-local errno via TP cell done (wave 3: 1/4)

- **Learned:** (1) No shared-VM threads exist (clone always deep-copies) + x86 Ring-3 cannot rdfsbase without CR4.FSGSBASE — both found by reading before designing; the design fell out (PCID/SMAP probe pattern). (2) Kernel-side translation impossibility arguments cut both ways: document WHY the layer was chosen. (3) crt0 changes move EVERY userspace image: layout-sensitive tests (stack_cookie DATA-padding assumption) flip — the test mirrored the kernel's own skip-zero invariant instead of over-constraining. (4) Full-suite STALLs (hal/smp/testrunner) all cleared standalone — pressure flakes; always rerun before bisecting. (5) Tracked .elf rebuilds must be committed (libc_verify.c.elf); ignored ones (vfsd, errno_probe) rebuild via make. (6) Second owner-commit race handled by clean rebase, never force-push.
- **Adapted:** CR4 BSP+AP, libc TP-errno ×3 archs, crt0 registration, probe+harness, counts; full debug 17/20 + 3 reruns; APPROVED; pushed, issue closed.
- **Style re-surface:** explicit intent, graceful-off CPU probes with loud messages.

### #328 — clone memory-budget charge done (wave 2 complete)

- **Learned:** (1) The global budget is decremented ONLY by create(); PMM allocs attribute to per-task counters instead — read both layers before designing the charge. (2) Reserve only what destroy() releases: over-charging without a matching release monotonically drains the budget (worse than the gap). Symmetry-with-create beat completeness. (3) add_task_err has no budget check (LUB/CPU only) — verified by reading before designing the drain-then-fork test. (4) Aggregate stalls pre-new-test (watchdog 20/145) with identical-binary pass on rerun = TCG flake; Deadline/Proc_elf aggregates flake ~1/3 under host load — always rerun before investigating.
- **Adapted:** clone reserve + rollback symmetry, regression test, rows sched_admission 8→9 / deadline 144→145; APPROVED (3 S3 notes); pushed, issue closed.
- **Style re-surface:** fail-closed nullptr + existing sys_fork -1 mapping (no new error paths).

### Wave 1 verification (#331, #334, #335) — owner-fixed, verified + closed

- **Learned:** (1) Before `issue_write update`, ALWAYS `issue_read get` the full body first — I reconstructed #334/#335 bodies from truncated excerpts and dropped the reporter's fix-hypothesis tails. Closed issues keep evidence comments, but the original text is unrecoverable. (2) `proc_elf` stalls at `loader_fault_dump_gated` (fault-dispatch, zero S-lines) with zero source delta between pass/stall/pass runs = H2 deferred-switch race family flake, not a regression; identical-binary rerun is the correct first move (passed 112/112 on 3rd run). (3) Owner batch-fix commits (`19d55cea1` for 3 issues) shift the role to verify-at-HEAD + close: elf_loader 15/15, task_metering 9/9, deadline 144/144, proc_elf 112/112.
- **Adapted:** all three closed completed with HEAD gate evidence; stall rows recorded with STATUS: TIMEOUT (mandatory history includes abnormal runs).
- **Style re-surface:** test-history rows required for EVERY run including stalls (PASSED: 0 FAILED: 0 + STATUS).

### Wave 0 (#342, #343, #341) — gate hygiene + fail-closeds done

- **Learned:** (1) `make execute-test dump-counts` is broken on x86_64 (mux chardev vs mon:stdio sed — filed #344); capture TCOUNT from stdout instead. (2) A "flag mismatch" fix can hide a second gate: #341's translation was correct but unobservable until the vfsd daemon's create-deny was found by reading — always trace the FULL path (stub→syscall→daemon→backend) before claiming E2E. Kernel-side translation was impossible (bit 0x200 ambiguous across libcs) — translate at the single owner above the boundary. (3) Daemon fd-table calls (fstat) can never succeed for user fds — authorization-only handlers are the correct shape (mirrors read/write). (4) Ring-3 probes that create files are safe under test_isolate (tmpfs reset between tests) but MUST unlink in-test; place early-print markers (512 B capture cap).
- **Adapted:** #342 rows reconciled (APPROVED, zero findings); #343 zero-esz guard + probe (APPROVED); #341 translation + VFS_OPEN_CREATE /tmp policy + fstat fix + E2E (APPROVED, 2 S3 notes). All pushed, issues closed.
- **Style re-surface:** explicit intent ops beat inferred intent (VFS_OPEN_CREATE vs ENOENT-inference); fail honest (ENOSYS) never silent.

### #262 — VFS+daemons+ELF H1–H8 verification pass (9/9 #253 parts done)

- **Learned:** (1) Audit handoffs must match the patch pathspec exactly — citing already-committed files (net.cpp under #260) in a new audit prompt earns a valid S3; generate the patch FIRST, then describe it from `git diff --stat`, never from memory. (2) S4 taxonomy verification = grep raw −1 INSIDE VfsError-typed returns (none found) + confirm boundary enums are actually referenced (vfs.cpp/fat32_fs.cpp use VFS_ERR_*) — internal int conventions with interface docs are compliant, not gaps. (3) History hygiene: awk-captured values carry `\r` from QEMU output — strip before appending, and never invent TIME values; re-run small classes to measure honestly.
- **Adapted:** 14 dup guards; evidence on #262; SIL 3 APPROVED (1 S3 handoff note). All 9 #253 parts complete (#254–#262); #253 itself left open for owner close.
- **Style re-surface:** TOCTOU/snapshot-order/CRC paths frozen by explicit verification, not just intent.

### #261 — drivers+IOMMU H1–H8 verification pass (8/9 #253 parts done)

- **Learned:** (1) Push can be rejected mid-cycle by an owner commit — `git fetch` + inspect first: a README-only fixup rebases cleanly under an untouched scope; never force-push. Record the rebase + new hash on the issue thread (audit reports reference patch content, which is unchanged, but hashes move). (2) Issue class names rot (`ahci_deep` → `drivers_ahci_deep`) — same registry-verification lesson as #258/#260, now third occurrence: treat every issue-quoted class name as unverified until grepped. (3) Driver poll loops already carry their M-1 tick-deadline bounds with rationale — H11.3 verification is read-and-confirm, not rewrite.
- **Adapted:** 9 dup guards + 3 ahci WHY upgrades; evidence + correction comments on #261; SIL 3 APPROVED zero findings.
- **Style re-surface:** probe-rollback order frozen (S2); ISR completion no-block/no-alloc; MMIO-KVA-only discipline untouched.

### #260 — core/libs/services H1–H8 verification pass (last of the ordered sequence)

- **Learned:** (1) "build" is not a test class (like buffer_pool) — verify every class name in test_registry.cpp before running; the issue's class list can be stale. (2) libc/services keep C/POSIX idioms by design: converting userspace −1/errno to kernel enums would BREAK the userspace ABI — H6 layering is a correctness rule, not just style. Vendored third-party loops (libfdt) are out of scope entirely. (3) Load-induced timing FAILs (tlb p99 bound, zombie-ordering) clear on identical-binary rerun like stalls do — record the failing assert + rerun tally as evidence, same standard; nondeterministic hang POINTS across attempts (34/94/0) are the signature of sickness, not code. (4) Tight poll loops self-inflict load (opencode + Terminal rendering topped ps) — back off polling when the host is sick.
- **Adapted:** 8 dup guards + 2 WHY upgrades; evidence on #260; SIL 3 APPROVED zero findings. Ordered sequence #257→#260 complete (7/9 #253 parts done with prior #254–#256; remaining #261/#262 out of the ordered scope).
- **Style re-surface:** user-task fault model ≠ kernel fault model (no ENSURE/locks imported to services); dmesg ring is infra (H7 applies to users, not the ring).

### #259 — interrupts/arch/HAL/timers H1–H8 verification pass

- **Learned:** (1) Audit-mature areas complete H1–H8 with micro-diffs — the review itself is the deliverable; forcing churn would manufacture risk. (2) Suite-first-class core TIMEOUTs are a harness-budget artifact (LTO relink + 129-file TIDY consume the 250 s expect budget before the guest runs), NOT kernel hangs — proven by identical-ISO standalone greens; read `/tmp/jarvis_classruns/<class>.log` head (build lines) vs tail (guest lines) to distinguish build-eaten-budget from guest-hang. (3) Arch-guarded test classes register zero tests off-target (expected-empty, like task_fpu) — verify guards in test_registry.cpp instead of burning watchdog timeouts; aarch64 link is pre-broken (relocation truncated, stash-convicted) — evidence belongs on #327, never inline-fixed under a hygiene gate.
- **Adapted:** 18 dup-`#pragma once` removals (guard-count verified 1/file); evidence on #259 + #327; SIL 3 APPROVED zero findings.
- **Style re-surface:** arch-guard byte-identical discipline; asm read-only (briefing §9); H11.3 bounded-or-mediated for every loop.

### #258 — IPC+caps H1–H8 verification pass (audit-mature subsystem)

- **Learned:** (1) A subsystem can complete H1–H8 with a near-empty diff: prior audit cycles (v0.4.9, #296–#298, #321) had already produced WHY-dense code, zero seams, named sentinels — the honest deliverable is verification + keeps, not churn (same precedent as #227 zero-change audits). (2) H6 must respect layering: int index-or-−1/count/handle-0 internals (cap slots, buffer handles) must NOT be migrated to the syscall ABI enum — slot-0 and count-0 are valid data, and the ABI contract (`encode_user_handle` −1) is documented at its single definition point. Inventory return-shape by TYPE (int/uint64_t/bool/size_t) before judging any literal. (3) `buffer_pool` is not a test class (zero tests registered → QEMU_EXIT) — the pool tests live in `data_structures_buffer_pool`; a wrong class name burns two watchdog timeouts, so verify class names in test_registry.cpp first.
- **Adapted:** 4 dup-`#pragma once` removals; H4 shm div-by-zero filed as #343 (S1 forbids fixing in cleanup); evidence on #258; SIL 3 APPROVED zero findings.
- **Style re-surface:** §10.5 named sentinels already in place (LIST_EMPTY/BUF_INVALID_*); keep ABI vocabulary at the boundary, internal conventions inside.

### #257 — syscall H1–H8 pure refactor (first v0.5.4 cleanup part)

- **Learned:** (1) Raw `-1/0` ABI returns migrate safely in two classes: failure-`-1` → a value-kept all-ones enumerator (`SYS_ERR_GENERIC_FAIL`), status-`0` → `SYS_ERR_OK` — but pid wildcards (`waitpid -1`), UINT64_MAX saturation, byte/record counts, and int-typed VFS-op zeros must be inventoried FIRST (grep by syntactic shape, judge each) or a mechanical replaceAll corrupts sentinels. (2) H2-vs-S1 collisions (canary-latch failure policy, `mark_vfs_touched` with test-side readers) resolve to KEEP + WHY-comment + report — never refactor the seam under a pure-refactor gate. (3) A stall cluster (suite-first-class TIMEOUTs + one standalone hang at a spinner test) under sustained host load convicts environmental via stash control (clean 553/553) + identical-binary green, even when the hang shape matches a known bug's hang — check `pmset -g therm` + desktop hogs (aerial video decode) before bisecting; the suite's later classes going green in the same run further exonerates the diff.
- **Adapted:** `SYS_ERR_GENERIC_FAIL` + file-local `kEio` in-tree; keeps annotated at their sites; H8-decline rationale (per-site cleanup pairing) recorded on #257; evidence comment posted.
- **Style re-surface:** §5 fail-closed preserved (values identical); §10.5 named sentinels; 80-col discipline on the verbose cast idiom (multiline ternaries); dead-artifact asm stays read-only without deletion approval.

### #337 — CheckedPtr count*sizeof overflow rejection

- **Learned:** (1) `count * sizeof(T)` in wrapping uint64 arithmetic can yield 0, and `is_user_range(addr, 0)` early-returns true — so `valid()` accepted an effectively unbounded range. The `count <= LIMIT / sizeof(T)` guard form is constexpr-friendly, freestanding-safe, and keeps `valid()` range-only (the 1 TiB in-range pin still passes). (2) The same wrapping idiom existed in `safe_copy_from_user`/`safe_copy_to_user` — fix every site sharing the idiom, not just the reported one. (3) The `core` aggregate embeds the pointer_isolation group, so enabling one test needs both the fine-row (4→5) and the aggregate (552→553) bump; the `core` run validates both.
- **Adapted:** `count_fits_user_limit<T>` helper + fail-closed guards at all 3 sites (`checked_ptr.hpp`); regression enabled (`#if 1`) + registered; count rows bumped; SIL 3 APPROVED zero findings; issue evidence comment posted.
- **Style re-surface:** §5 fail-closed on reachable input (return false, never ENSURE); §10.5 named bounds (`USER_SPACE_LIMIT`); aggregate/member count-row parity.

### #45 — deterministic boot, final increment (real WCET asserts)

- **Learned:** (1) Raw-counter deltas are host-load-independent (instruction-driven under TCG) — asserting on them, relatively, makes timing tests immune to the #279 wall-dilation class by construction. (2) Some stubs can't become asserts in place: serial self-parse is impossible in-kernel, so T3 was redesigned as a structural check (ordering + fail-closed guards) rather than forced into its pseudocode. (3) Testbed→main merge is routine now (test file + history + report, count rows unchanged, graph chore on top).
- **Adapted:** 3 real boot_deterministic tests (validity+total, per-stage half-budget, structural ordering/OOR guards); #45 closed.
- **Style re-surface:** test-file idiom (JARVIS_ASSERT_FMT + u64 casts, namespaced helpers, include <kernel/kernel.hpp> + <kernel/nexios_config.h>).

### #317 — send_sync reply buffer threaded through the syscall ABI (option b)

- **Learned:** (1) The 4-arg syscall ABI (`__syscall5`: x86 rbx/rcx/rdx/rsi, aarch64 x0-x3, riscv a0-a3) leaves ONE dedicated register free for every defined syscall — x86 `rdi` (trap frame[5]), aarch64 `x4` (frame[4]), riscv `a4` (frame[13]). The kernel handler already receives the full trap frame, so a 5th argument can be threaded with **no entry-stub change** — just read `regs[arch_index]` (verify the frame index per arch: isr_stubs push order; syscall_entry.S sp offsets; OFF_A4). (2) Adding an arg via a free frame register is far cheaper/safer than changing the C-bridge signature (which would touch every handler). (3) The old `sys_send_sync` wrote the reply over the REQUEST buffer with an UNCLAMPED `copy_size = reply.data_size` — a responder could write up to IPC_MAX_MSG_SIZE into a smaller request buffer; the fix both routes the reply to a real `reply_buf` AND clamps the copy to the request size (the checked range). (4) Regression for a userspace-visible ABI change needs a USERSPACE probe (CheckedPtr user-range validation blocks kernel-buffer unit tests): a probe that sends with a DISTINCT reply buffer + self-asserts type/payload/request-intact is genuinely discriminating (fails under the old path). A task that `sys_exit`s is NOT on the zombie list — call `Scheduler::terminate` before `drain_zombie_list` or its Notify/EventGroup/FDs leak (ResourceTracker WARN).
- **Adapted:** `__syscall5r` (libc) + `syscall_arg4(regs)` + rewritten `sys_send_sync`; `userspace/ipcsync-probe.c` + `ipc_send_sync_reply_buffer` test; docs `ipc.md` §2 contract. Report `audits/report-2026-10-10T20-30-14Z.md` (APPROVED).
- **Style re-surface:** SIL 3 fail-closed on user-pointer writes (checked + return-value check + recovery); ABI changes need the retrieval-artifact rule + cross-arch frame-index verification; keep callers contract-agnostic where possible (xmodem passes the request buffer as reply_buf).



### #298 — ipc_timeout wheel assertion raced the watchdog daemon (shared BSP wheel)

- **Learned:** (1) A test that asserts a **global** resource count (`TimerWheel::live_count(0)==0`) is only valid if NO concurrent agent uses that resource. The BSP timer wheel is shared: `watchdogd` (#277, first userspace microservice, landed 2026-10-02) runs a **bounded** receive (`RECV timeout=10`, `userspace/watchdogd.c:207`) in a service loop and legitimately arms the wheel every ~10 ticks — every other daemon uses `timeout=0` (never arms), which is why the assertion held for months then broke. (2) Symptom misdirection: the FAIL surfaced as a **resource LEAK** (`Tasks +1, MsgQueues +1, …`) because the `live_count` assert aborts the test body BEFORE its `terminate_and_drain` cleanup — the leak is a *consequence* of the abort, not the bug. (3) Conviction method that worked: temporarily print the wheel `arm()` callback+context and `IPC::recv_wait_arm` task id → resolved via `addr2line` → the stray arm's context was a boot daemon (`watchdogd`), not the test's receiver. (4) Correct fix = assert the **per-owner** invariant: `task.recv_timeout_armed` clear AND the task's own `TimerWheel::Handle` no longer live (`TimerWheel::is_live`), never a global count. `test_posix_time` used a delta, but a delta is also racy across a task yield (the daemon oscillates 0↔1). (5) Discriminating negative control matters: the bug class is "flag cleared but slot not cancelled", so `is_live(handle)` (not just the flag) is required — verified by two negctrls (flag-left-set and flag-cleared-but-slot-armed) both failing the new asserts.
- **Adapted:** `TimerWheel::is_live(handle)` introspection (test-only); `test_ipc_timeout.cpp` per-owner asserts (`to_assert_recv_arm_reclaimed`) for the 5 daemon-concurrent tests, global counts kept only for the daemon-free stale-fire/full-wheel tests; docs `ipc.md` §6. Report `audits/report-2026-10-10T…Z.md` (APPROVED).
- **Style re-surface:** SIL 3 test isolation on shared kernel resources — assert owner-scoped liveness, not global counts; watch for "leak" reports that are really assert-abort artifacts; instrument→resolve→convict, then revert instrumentation before the patch.



### #311 — 3-arg main envp clobbered by the user-stack "after" canary

- **Learned:** (1) A runelf/exec'd ELF gets an empty env, so a 3-argument `main` reads `envp` derived by crt0 from `rsp` (`lea 16(%rsp,%rdi,8)`). The kernel layout in `setup_user_stack` was `[argc][argv0][envp0]` at the very TOP of the user stack — the SAME top qword used by the v0.4.0 MP-3 stack "after" canary (`canary_install_user_canaries` writes `STACK_VADDR+PAGE_SIZE+STACK_SIZE-8`), which runs AFTER `setup_user_stack` and **overwrote the `envp[0]` NULL terminator** with the canary magic. A 2-arg `main` never reads envp → "clean boot"; a 3-arg main walks a bogus pointer → instant #GP. (2) Diagnostic path that convicted it: a userspace probe with a 3-arg main that hex-dumps argc/argv/envp/envp[0]/rsp → `envp[0] = 0x4e45584943414e5d` ("NEXICAN]" — the canary, little-endian); kernel-side `setup_user_stack` dump showed the layout WAS zeroed → the clobber happens after. When kernel state looks right but user state differs, suspect a later mutation at a colliding address. (3) The fix is a reservation, not a layout change: `sp = stack_top - 8` so args start below the canary slot. (4) Symptom didn't match the ticket's theory ("crt0/iret wrong") — the entry state and frame were correct; always diff the ACTUAL memory against BOTH sides' expectations.
- **Adapted:** `setup_user_stack` reserves the top 8 bytes for the stack canary; probe `userspace/envprobe.c` (3-arg main); regression `loader_runelf_3arg_main_envp` (class `elf_loader`); `docs/specs/memory.md` §3.3. Report `audits/report-2026-10-11T…Z.md` (APPROVED).
- **Style re-surface:** SIL 3 memory-layout invariants — a fixed-location region bracket (canary) must not alias a dynamic layout at the same address; reserve, don't collide. Probe-based verification for user-visible ABI (3-arg main) since kernel unit tests can't drive user-mode reads.



- **Learned:** (1) `TaskControlBlock::clone` copied `period_ticks`/`deadline_ticks`/`remaining_ticks` from the parent unconditionally. The admission gate (`admission_check_cpu_locked`) sums `Σ (budget·10⁶)/period` with `budget = wcet_ticks ?: sporadic max_budget ?: remaining_ticks`; `cmd_runelf` stamps the parent `period=100`, no WCET ⇒ budget = remaining = 100 = **implicit 100 % util**, so an inherited-window child makes the sum 2.0 > bound(2)=0.828 → denied. The filed theory ("expired deadline") was WRONG — `deadline_ticks` only matters via `admission_exempted` (`deadline==0` ⇒ exempt), never in the util sum; "reset the deadline" alone cannot fix it. Always trace an admission denial to the exact numerator/denominator before trusting the ticket's mechanism. (2) Fix must be SCOPED: a blanket "fork children are aperiodic" breaks `admission_fork_denied_no_leak` (kernel forker must still deny). Discriminating on `parent->is_user_` (user child aperiodic/exempt; kernel child inherits) fixes runelf forks and preserves the kernel-fork denial — and matches the loader default (`finalize_loaded_task` already makes user tasks aperiodic) and the aarch64 `fork_marker_smoke`. (3) A "reset to a fresh window" that keeps `period` cannot help when the child is implicit-100 %; the child's BUDGET is the lever, and exempting it (`period/deadline = 0`) is the minimal correct move.
- **Adapted:** `TaskControlBlock::clone` gated the three periodic fields on `parent->is_user_`; regression `admission_fork_from_user_parent_admitted` (asserts child period/deadline/remaining == 0, `admission_exempted`, and a literal `add_task_err` OK); `deadline.md` I-8 addendum; `shell.cpp` comment. Report `audits/report-2026-10-11T…Z.md` (APPROVED).
- **Style re-surface:** SIL 3 admission math is integer-only, gate-scoped (aperiodic/exempt by I-4); never widen a policy gate to fix a caller — scope the change and keep the existing denial contract test green; trace denials to the exact utilization term.



### #301 — aarch64 linker script orphaned the stack markers

- **Learned:** (1) An input section emitted unconditionally but with NO matching output-region/KEEP rule becomes an **orphan** that the linker places ad-hoc near an unrelated section; under `.text` growth the orphan's LMA collides (`section .marker.stack.after LMA overlaps section .data LMA`) → link fails. aarch64 lacked a home for `.marker.stack.*` while x86_64 (`.stack_guard`) and riscv64 kept them. Fix = mirror the KEEP home per arch. (2) Convict/verify with the arch's `readelf -S`: a pre-fix orphan shows as a standalone output section (e.g. `.marker.stack.before` @ just before `.data`); post-fix it folds into its named region. (3) Cross-arch linker-script parity must cover the marker/home regions, not just the load segments — a script that "links green for months" can sit at zero slack and tip on the next ~1 KB. (4) A linker-script change is SIL 3-relevant (audit per #210 precedent). (5) Watch for semantic drift: homing a marker in an EMPTY region (aarch64 `.boot_stack` is empty; the real stack is `_boot_stack` in `.boot.data`) fixes the link but leaves the marker meaningless → file a follow-up rather than silently accept.
- **Adapted:** `linker/linker_aarch64.ld` `.boot_stack` region KEEPs `.marker.stack.before/after`; report `audits/report-2026-10-10T17-37-08Z.md` (APPROVED); follow-up #327 (aarch64 stack-marker semantic placement).
- **Style re-surface:** build-system changes still need the retrieval-artifact rule + SIL 3 audit; verify with a real cross-arch build + `readelf`, not by inspection alone.

### #326 — CONFIG_DEADLINE_MONITOR_TASK guard asymmetry (MONITOR=0 build)

- **Learned:** (1) A `#if CONFIG_X` block that mixes genuinely-X-only symbols with accessors consumed elsewhere makes the X=0 build fail — hoist the shared accessors OUT of the guard; keep only the X-exclusive helpers inside. Here the test-active accessors (`set_test_active`/`is_test_active`/`harness_owns_lifecycle`/`in_test_harness`) + `canary_check_in_scheduler_hooks` were declared inside the monitor guard but used tree-wide. (2) Every symbol gated by `#if CONFIG_X` needs its DECL and DEF under the SAME guard; asymmetric decl/def (e.g. `monitor_task_entry` declared outside, defined inside; `scan_deadlines` likewise) is a latent link break. (3) Monitor-only TEST files that call a guarded test helper must themselves be guarded (bodies + `JARVIS_REGISTER_TEST` lines), keeping the `register_*_tests()` symbol present with an empty body at X=0; helpers that become unused at X=0 need guarding too under `-Werror`. (4) The config-matrix `scripts/run_config_matrix.sh` is STALE (removed `all` class + nonexistent `jarvis_config.h`) — MONITOR=0 is compile-hygiene only, not a live run gate; verify it with a temporary macro flip + `make debug NO_LTO=1`.
- **Adapted:** scheduler.hpp hoist; test.cpp guard removal; 9 test files guarded; `test_expected_counts.hpp` deadline_miss else 3→0. Report `audits/report-2026-10-10T14-25-08Z.md` (APPROVED; iter-1 REJECT = missing retrieval artifacts + residual guard).
- **Style re-surface:** CONFIG-guard symmetry (decl==def==call); `-Werror` unused-function at alternative configs; guard TEST registration + keep register symbols unconditional.

### #299/#280 — pre-suite stall: monitor-wake starves the harness (SIL 3)
- **Learned:** (1) The intermittent pre-suite stall (`[SNAP:SAVE]` then `[DMD] init missed deadline` flood, zero `S:`) is **harness starvation**, not a dead tick and not a stuck dispatcher. Convict it with counters + a state dump: `ticks`/`rate_monotonic_schedule`/`switch_to_task` all advancing (~1/tick) proves ticks + switching are alive; the dump shows `current`=the monitor (prio 127, alternating BLOCKED/RUNNING), init (prio 10) READY+enqueued+physically-queued but never dispatched, `peek_highest()`=watchdogd. (2) The trap: `is_test_active()` gates BOTH the on_tick monitor-wake (`harness_owns_lifecycle`) AND the harness-nonpreempt protection — so before `set_test_active(true)` runs, the harness is unprotected and the prio-127 monitor (woken every tick) starves it before it can mark the suite active → self-sustaining deadlock. **Fix:** in `run_filtered`, inject the TestContext + `set_test_active(true)` BEFORE `ensure_monitor()`/`snapshot_create()` (the snapshot does not capture the TestContext, so the baseline is unchanged). (3) The pre-existing sibling bug (#300) is INDEPENDENT: a task-context `Scheduler::on_tick()`/`handle_interrupt_c` corrupts the APIC tick source → ticks DEAD (beacon/counters freeze); a starvation fix cannot help it. (4) CONFIG-guard asymmetry is a recurring link-break class: a symbol declared inside `#if CONFIG_X` but referenced under `#if CONFIG_DEBUG` links only when both are set — every new reference must carry the declaring guard (this cycle's reject was exactly that; the durable fix, hoisting the test-active accessors out of the monitor guard, is a separate issue since `MONITOR=0` is baseline-broken).
- **Adapted:** `src/lib/test.cpp` reorder; CONFIG_DEBUG stall beacon (one-shot at init-miss 100) + `tick/rms/switch` counters + `diag_ready_probe()` in scheduler.{hpp,cpp}; `docs/specs/test-harness.md` boot flow. Reports `audits/report-2026-10-10T13-05-11Z.md` (APPROVED; 2 REJECT cycles — both CONFIG-guard asymmetry). Validation: monitor-wake-suppressed experiment 16/16; fix 16/16 at load ~8; storage/ipc/servers/capability green.
- **Style re-surface:** precondition ordering (harness marking vs higher-prio per-tick wake sources); CONFIG-guard symmetry on every declared-then-referenced symbol; keep the load-gated reproduction as closing evidence for intermittent defects.

### #318 — ramdisk carve scrub + CRC32 integrity metadata
- **Learned:** (1) "Pristine region reads stale bytes" has two shapes — carve-placement vs mapping/aliasing nondeterminism. Scrub-on-carve fixes the former AND makes the latter detectable (a foreign-frame read won't match a stored CRC / the zero baseline). Decisive test = reboot the SAME image ×N: deterministic-per-image ⇒ carve; varying ⇒ mapping. (2) A read-modify-write FS over IPC needs layered integrity: metadata-region CRC verified at load (cheap) + per-file data CRC verified lazily per cache fill (a full mount scan is O(whole store) chunk round-trips). Verify the freshly-loaded array BEFORE publishing it (`rd_cached==true ⇒ verified`) — publishing first re-opens the stale-byte disclosure race (audit S2). (3) A 96 B dir entry does not divide a 512 B block: the store must SPLIT straddling entries across the boundary; the naive "skip entries not fully in this block" silently drops every entry ≥ index 5 (latent #314 bug). (4) CRC32 covers the LIVE bytes only (padding excluded) and must be recomputed on close — an overwrite at offset 0 breaks incremental byte-append accumulation.
- **Adapted:** `ramdiskd::boot_allocate` HHDM scrub (at carve, not grant); `ramdisk_fs` v2 layout (`crc32`/`flags`/`dir_crc32`) + `rd_serialize_dir_block` + verify-before-publish + `ramdisk_fs_verify_all`/`_extent` seams; `ramdisk_pristine_reads_zero` + `ramdiskfs_crc_detects_corruption`; specs `ramdisk_fs.md` §2/§2.1 + `ramdiskd.md`; removed stray `[TEMP-RDF]` logging. Reports `audits/report-2026-10-10T11-04-14Z.md` (APPROVED; iter-1 REJECT: cache published before verify).
- **Style re-surface:** SIL 3 fail-closed (never expose unverified bytes); no `rd_lock` held across IPC; TEMP-DIAG must be reverted before shipping (the #314 tree kept `[TEMP-RDF]` diagnostics in the block-op hot path); CRC stated as corruption-detection, never security.

### #322 — harness stall retry + STALL verdict
- **Learned:** (1) The stall signature (zero `S:` lines, `RESULT: TIMEOUT`, identical-binary rerun green) is safe to auto-retry ONCE, but the retry must be scoped to the no-progress signatures only — a genuine test FAIL (summary with `failed>0`) and `RESULT: UNKNOWN` must never retry. (2) A "recovered" label must be gated on the FINAL outcome being PASS — a stall followed by a summarised FAIL is not recovered (audit caught `STALL-RECOVERED` being set unconditionally at the stall; fix = `STALL_SEEN` flag set on the stall, `STALL-RECOVERED` assigned only in the PASS arm, and `run_all_classes.sh` also requires `nfail==0`). (3) In a make recipe wrapping `while`/`case`, `break`/`continue` bind to the nearest LOOP (case is not a loop) — retry loops work; `$$` escaping for shell vars is mandatory. (4) The per-class OUTER timeout must cover `attempts × expect-timeout + build`, else a doubled stall truncates into a generic TIMEOUT with no STALL marker.
- **Adapted:** Makefile `_run_test_qemu` per-attempt retry (`TEST_MAX_ATTEMPTS := 2`); `[HOST] STALL-RECOVERED`/`STALL-UNRECOVERED` markers; `scripts/run_all_classes.sh` `STATUS: STALL`/`STALL_RECOVERED` rows + `OUTER_TIMEOUT` 600/1200. Verify: forced `TEST_TIMEOUT_CLASS=2` → STALL-UNRECOVERED, normal classes single-attempt. Report `audits/report-2026-10-10T10-04-12Z.md` APPROVED (iter-1 REJECT: retrieval artifacts + recovered mislabel).
- **Style re-surface:** process/build changes still need the retrieval-artifact rule; machine-readable row format stays AGENTS-compliant; label semantics must match the actual outcome.

### #321 — snapshot IPC-quiescence; duplicate-receipt replay falsified
- **Learned:** (1) A "duplicate IPC delivery across snapshot_restore" theory is cheap to falsify with CONFIG_DEBUG counters: count a distinctive type (RAMDISK_GRANT=403) in `IPC::send` vs receipts in `IPC::recv`, plus a `snapshot_create` scan for any non-empty inbox. Result on the current tree: sends == receipts and 0 non-empty baseline inboxes — the queue byte-replay does NOT occur because the boot grant is consumed before the suite snapshot (daemons prio 20 > init 10; `ramdisk_fs_try_mount` does no IPC). (2) When a bug is non-reproducible, the honest deliverable is a structural guarantee + forward invariant test, NOT a fabricated A/B: a bounded boot settle (drain init's inbox + wait until every `all_tasks()` inbox is empty, ≤500 ticks, `arch::hlt()` to yield to higher-prio daemons) makes the captured baseline IPC-quiescent by construction, plus `ipc_snapshot_baseline_inbox_quiescent`. (3) `snapshot_create` runs entirely under a function-scope `arch::IrqGuard` (IF=0), so a scan and the MemPool capture are atomic on the BSP — sample captured-state invariants there, not before `quiesce_enter`. (4) A priority inversion in reasoning: higher NUMERIC priority wins (`priority_map.hpp`); daemons (20) outrank init (10), which is WHY the baseline is already quiescent.
- **Adapted:** kernel.cpp `init_task_main` settle (CONFIG_DEBUG-gated, re-resolves the task set each pass — no raw TCB pointer across `hlt`); test_isolate detector + `snapshot_baseline_nonempty_inbox_count()`; test_ipc_extended regression; docs `test-harness.md` §2.2/§2.3 + `ipc.md` §1; count rows reconciled (`ipc` stale at 81 → 86). Audit report `audits/report-2026-10-10T09-51-56Z.md` APPROVED (iter-1 REJECTED: missing retrieval artifacts + wrong symbol name).
- **Style re-surface:** SIL 3 — bounded loops with a monotonic deadline; no TCB pointer held across a scheduling point; CONFIG_DEBUG-gate non-production paths; document "preventive guarantee" vs "fix for an observed defect" explicitly.

### #320 — snapshot-restore over-release of capability-registry pins
- **Learned:** (1) Snapshot/restore has a hard ordering rule for anything holding a `KernelObject` pin: release the pins BEFORE `MemPool::restore_pool_data()` byte-rewinds pool block data (recount lives INSIDE the block, so the rewind resets `ref_count_` to baseline B). Releasing after the rewind drops the live pin (L) onto B → premature `dispose()` → freed + 0xDD-poisoned block while the restored CSpace slot still points at it (symptom: `cap::lookup` finds the slot occupied/gen/type/rights-matched, then `slot.obj->acquire()` fails — "slot valid but object dangling"). `FrameUserMap::snapshot_reset()` / `MmioUserMap::snapshot_reset()` / `PagerRegistry::snapshot_reset()` were all mis-ordered; fixed by moving them (arch-neutral) before the PMM/MemPool restore block. (2) A pre-snapshot daemon that maps a capability LAZILY (no pin at snapshot time) and never unmaps is the minimal trigger; live/OVMF boots never run restore, so only the harness exposes it — and it breaks every daemon test after the first mapping. (3) A well-formed registry-pin release must observe `refcount() >= 2` (slot pin + ≥1 owner ref); `<= 1` at release = over-release. Detector is log-only, outside the lock, CONFIG_DEBUG-gated (reads >1 on a freed+poisoned block, so it catches the baseline-cap case but not re-poisoned test caps — diagnostic, not a gate). (4) A test assertion can contradict its own comment and hide a product fix: `test_ramdisk_fs.cpp` "double unlink fails" asserted SUCCESS — match the store's documented contract + VFS convention when auditing tests.
- **Adapted:** test_isolate.cpp "Pin-holding capability registries" block (release before PMM/MemPool restore, arch-neutral); CONFIG_DEBUG over-release detectors in frame_map.cpp/mmio.cpp/pager_registry.cpp; test_ramdisk_fs.cpp double-unlink assertion corrected to `vfs::unlink(path) != 0`; audit report-20261010-090033.md APPROVED; storage 160/160 + capability 148/148 (detector silent); non-blocking S3s (anchor the "registries empty at capture" invariant, exercise arch-neutral reset on aarch64/riscv64) noted on #320.
- **Style re-surface:** §5 fail-closed (map/lookup paths untouched — the fix only reorders teardown); debug instrumentation allocation-free, lock-free at the log site, fully compiled out under `#if defined(CONFIG_DEBUG)`; test assertions must match documented contract + convention.

### #316 — in-tree libc %lu misrender (caller-side, no libc change)
- **Learned:** (1) Garbage `%lu` with stack-address-looking values + correct direct-digit output = caller passed 32-bit to `%lu`: each `%lu` consumes 8 bytes, desyncing `va_arg` for that AND all following conversions. Convict by control probe (correctly-typed args + objdump at both opt levels), never by re-reading the derivation — vsnprintf read correctly all along. (2) When the callee is correct, the fix IS documentation: exact LP64 width contract in-header (`l`=8 B, unflagged=4 B, `ll`-collapses, no `%z`, `%X`-lowercase, 256 B truncation).
- **Adapted:** userspace/printf-probe.c P01–P16 + live target transcript (all exact incl. max-u64); src/libc/stdio.h contract block; basic_lib 22/22; #316 evidence comment.
- **Style re-surface:** int-only TU matching consumer style; static-only, bounded lines (<200 B vs the 256 B printf buffer); header contracts live next to declarations.

### #315 — ramdiskd zeros-after-writes (userspace IPC reply contract)
- **Learned:** (1) OK-results + zero-data across blocks/segments/gaps with a green kernel round-trip = suspect the CLIENT IPC contract before storage: in-tree libc `ipc_send_sync` drops `reply_buf` (never crosses the ABI), the kernel writes the reply over the REQUEST buffer, and readers of a separate local `reply[]` get fresh-stack zeros — result lucks to OK, data permanently zeros. Fixed single-buffer (result `req[0..8)`, payload `req[8..40)`, req ≥ 40 B). (2) E2E serial is multiplexed output: terminal CRLF/tab expansion + `\n`-prefixed trailers + async kernel logs + shared-/dev/tty dual readers corrupt naive window compares — golden patterns must avoid 0x0A/0x09, windowing must account framing, guests must be quiet (drop the HD; OVMF+q35+TCP is the proven path — SeaBIOS+i440fx IDE-CD fails ATA reads code 0009 here). (3) First-rx-of-session flakes are cold-TCG latency (multi-minute stalls are real: zero-S-line + identical-binary green = stall, retry-don't-chase with case-level CAN-abort recovery). (4) Verify edit drafts against `git diff` content before building: a read can misreport one line (copied a phantom `va_list` into stdio.h and broke the build — build caught it, restored byte-identical).
- **Adapted:** xmodem single-buffer + tracked `tools/xmodem276.txt` + verify-276 hardening (SOH block-0, data seqs from 1, corrupt-only-first, CRLF-aware dump, clean golds, cold timeouts, case retry); #317 (contract) + #318 (carve content) filed; full E2E ALL PASS; 3× SIL 3 APPROVED.
- **Style re-surface:** §5 fail-closed preserved on every reachable path (abort propagation, bounded retries); userspace TU comments carry the contract; build green before any audit.

### #275 — ramdiskd microkernel service (16 MiB, block IPC)
- **Learned:** (1) sys_receive copies ONLY msg.data[] and returns type — sender_id is NEVER delivered (debugd.c documents it; vfsd/iocd overlays are latently wrong the same way, with zero live userspace clients to notice). Userspace request/reply protocols MUST pack the reply pid into data[]; verify the copy path before designing any wire format. (2) Snapshot-restore detach_all_objects() nulled daemon CSpaces, orphaning pre-snapshot grants (phantom CNode on next ensure); fixed by preserving cspace_ for daemon-registered tasks (heals watchdog supervision too) — persistent system state vs test fixtures. (3) FrameUserMap regions are 2 MiB: one 16 MiB FrameCap can never map — segment the store (8×2 MiB caps, handles fit one 64 B message exactly). (4) Eager mapping of 4096 pages stalls serving under TCG — map lazily on first use. (5) Stash control is mandatory before deep-diving a suspect regression: pristine tree flaked identically (crash once, 122/122 next), exonerating the change; GDB-paced runs complete what TCG stalls (hal 122/122).
- **Adapted:** data-packed-pid wire (56/40 B), lazy mapping, detach keep_cspace + daemon::is_managed, 6 storage tests (incl. live 16+16+STAT end-to-end), DAEMON+9/+10 catalog rows (INIT space full), docs/specs/ramdiskd.md; #275 evidence comments; full debug 20/20 + release 18/18.
- **Style re-surface:** §5 fail-closed on reachable IPC input (explicit bounds, never ENSURE); §10.5 named geometry; TEMP-DIAG fully reverted grep-verified (userspace + kernel); no new syscalls/locks/IPC primitives.

### #310 — userspace sh PATH + export (follows #274 /bin)
- **Learned:** (1) Static analysis of the user entry contract can be airtight and still wrong: 3-arg main + envp import faulted instantly (#GP) at runelf start despite a verified rsp/rdx derivation — the pristine 2-arg control booted. Convict by control experiment, never by re-reading. (2) runelf tasks share /dev/tty with the kernel shell and `fg` is a stub, so typed input splits randomly between shells; deterministic handoff = `source` a script (loadelf, sleep, runelf, long sleep) and drive the user task while the kernel shell is blocked. (3) `clone()` inherits period/deadline, so fork from a runelf task (period 100, deadline +100 ticks) is admission-denied forever after (~1 s) — pre-existing, filed as #312; the runelf envp gap filed as #311. (4) Bare-name test helpers that skip on missing ELF (`if (!f.data) PASS`) silently lose coverage on layout moves — sweep every bare initrd name (tests, .exp/.py, taskdefs) before moving.
- **Adapted:** sh.c PATH resolver + parent-side export + exec envp passthrough (no import; default PATH always); tests/sh310.txt handoff script; #311 + #312 filed with live evidence; #310 evidence comment (bare-exec proof pending #312).
- **Style re-surface:** int-only matching TU style; static-only buffers with named caps (MAX_ENV/ENV_VAL_LEN inside the 64×256B exec window); fail-closed resolver (−1 → 127); no kernel changes.
- **Learned:** (1) Static analysis of the user entry contract can be airtight and still wrong: 3-arg main + envp import faulted instantly (#GP) at runelf start despite a verified rsp/rdx derivation — the pristine 2-arg control booted. Convict by control experiment, never by re-reading. (2) runelf tasks share /dev/tty with the kernel shell and `fg` is a stub, so typed input splits randomly between shells; deterministic handoff = `source` a script (loadelf, sleep, runelf, long sleep) and drive the user task while the kernel shell is blocked. (3) `clone()` inherits period/deadline, so fork from a runelf task (period 100, deadline +100 ticks) is admission-denied forever after (~1 s) — pre-existing, filed as #312; the runelf envp gap filed as #311. (4) Bare-name test helpers that skip on missing ELF (`if (!f.data) PASS`) silently lose coverage on layout moves — sweep every bare initrd name (tests, .exp/.py, taskdefs) before moving.
- **Adapted:** sh.c PATH resolver + parent-side export + exec envp passthrough (no import; default PATH always); tests/sh310.txt handoff script; #311 + #312 filed with live evidence; #310 evidence comment (bare-exec proof pending #312).
- **Style re-surface:** int-only matching TU style; static-only buffers with named caps (MAX_ENV/ENV_VAL_LEN inside the 64×256B exec window); fail-closed resolver (−1 → 127); no kernel changes.

### #274 — initrd /bin hierarchy + dir vnodes
- **Learned:** (1) `initrd::find` matches directory entries too — a find-first fast path returns bogus size-0 file vnodes once dirs exist; probe dir-ness (bounded readdir scan) BEFORE find. (2) Recipe-only mk changes don't rebuild the cpio — force repack or verification runs the stale image. (3) Runner-startup TCG stalls (zero S: lines post-SNAPSHOT, DMD spam) clear with identical-binary reruns (hal/ui/servers/capability/drivers/random/logging_debug all cleared; GDB-paced run also clears).
- **Adapted:** InitrdDirNode + dir ops (close parity with file close, parent = static root, fail-closed bounded join); taskdefs bin/ daemon paths; 8 test call-sites + .exp/.py burn paths; `vfs_initrd_bin_resolve` test; count rows storage/vfs_core (+2 pre-existing stale: hal, ui); full debug 20/20 + release 18/18.
- **Style re-surface:** §5 fail-closed on reachable input (no ENSURE); §10.5 named bounds (INITRD_MAX_SCAN/SUBDIR/JOINED); §11 no new locks (immutable image).

### #186 — Goal-2 snapshot budget export (v0.5.2 remainder)
- **Learned:** (1) Planner-architected atomicity can be unimplementable: __atomic_load_n needs the field address, but budget_remaining_ is private — the public plain-read accessor + parked-target reasoning (auditor strengthened: consume() only debits t==cur) is the correct construction, not a compromise. (2) Userspace freestanding C defeats host LSP (wrong flags/sysroot) — verify with the real target/host builds, never LSP. (3) Count your own test lengths by hand: 12+8+8+9=37, not 36 — the host oracle caught it only because vectors pin exact bytes.
- **Adapted:** snap_budget_of + 3 sites; stop-reply key:val display (+69 worst-case, ceiling-noted); 2 kernel tests (post-park sampling); extended cproto vectors; #186 stays open for B1/D1.
- **Style re-surface:** §10.5 named bounds (ceiling math in comment); read-only rule verified by absence (no set_priority in debug paths); TCG-relative asserts only.

### #227 — debugd Phase 6 bare-metal audit (verification-only cycle)
- **Learned:** (1) Audit tasks can complete with zero src changes — filings ARE the deliverable; POST-audit then reviews an (almost) empty patch, which is legitimate. (2) `git diff -- .` swallows audits/pending_patch.diff itself — always exclude it (`':!audits/pending_patch.diff'`) or the patch self-references (900+ garbage lines). (3) Verify the "missing" row before assuming loss: test-history appends from parallel runs interleave; `tail` lies, `rg` tells.
- **Adapted:** 6 GAP follow-ups #304–#309 as sub-issues of #227; zero-change POST-227 APPROVED; hal_serial_logic + exc_table evidence rows.
- **Style re-surface:** no code = no style surface; §5 parity verified by reading (dumps must work in release), not by running release.

### #253 — cleanup batch #254/#255/#256 (memory/scheduler/sync pure refactor)
- **Learned:** (1) Zero-S-line TIMEOUTs are TCG virtual-time stalls until proven otherwise: identical-binary rerun green (incl. 148/148 under GDB surveillance) disproves regression more cheaply than a stash rebuild; check host load first (load 7+ during the batch explained a 9-class TIMEOUT cluster). (2) Never pipe make/test output through `head -N` — SIGPIPE kills the build/test mid-run; consume full streams (tail/rg). (3) Debug-green never implies release-green: release-only -Werror (exit_record strncpy, latent since #294) needs an explicit release build before any full gate. (4) Auditor findings can be factually wrong (POST-255 S3 claimed a deleted NOTE standing verbatim at scheduler.cpp:5595-5598) — verify every finding against the file before accepting it. (5) `ps aux | rg PAT` matches the searching shell itself — read PIDs before concluding strays exist.
- **Adapted:** pure-refactor helpers (alloc templates, waiter predicates, reap tail) + shared waiter_tag.hpp; exit_record memset+memcpy fix (d921a8539); evidence comments on #254/#255/#256; full debug+release gates green.
- **Style re-surface:** §10.7 no const_cast (helper signatures over casts); §10.5 named constexprs (kNoPool/kMagic*/IDLE_BAND/NOTIFY_INVALID); §5 fail-closed preserved on every extracted predicate; §11 lock/pairing/generation-tag invariants byte-identical.

### #279 — session discipline failure (redundant pipeline)
- **Learned:** Before starting ANY pipeline, re-read prompts/STATE.md + git log + issue thread: a committed, pushed increment looks exactly like unstarted work if you only read the issue body. I re-ran a full #232 implementation cycle (planner, work-begun post, ~20 edits, host tests) against already-committed code — all edits no-ops, plus a spurious thread comment requiring correction. Cost: a full session segment for zero delta.
- **Adapted:** this entry; correction comments on #232/#279 stand as the audit trail.
- **Style re-surface:** evidence-before-synthesis applies to session state too (git log + STATE.md are cheap local evidence).

### #232 — debugd Phase 4 loop + grants
- **Learned:** (1) A Logger gate consulted from tick paths must be lock-free: an atomic live-counter (updated under the table lock at every transition) beats a locking scan — a spinlock take inside info()/warn() self-deadlocks against a preempted table holder when the tick fires. Count every transition (mint/clear/revoke/drain/reset-zero) or the predicate drifts. (2) Sibling-verb interface extensions force all fakes to grow — one FakeController, mechanical stubs. (3) Disabled taskdef rows still need valid pid fns for validate_all — TU-local cell, no new module.
- **Adapted:** g_live_binding_count + session predicate; DebugLoop header + 7 host tests; disabled debugd row; sel9 + grantor-death disposition.
- **Style re-surface:** §11 (lock-free predicate, no new locks); header-only zero-heap loop; no new syscall numbers.

### #277 — watchdog daemon + pinned-free baseline corruption
- **Learned:** (1) The snapshot pin invariant ("pinned blocks are never recycled") was enforced at pin time but violated by EVERY free path: scan kills, defer_kill/destroy, flush/drain/cleanup_step drains, reap_orphans. First live proof: the daemon's own self-watchdog killed PINNED watchdogd under stress_hrt load → pinned-free WARN → suite wedge. Fix extends the spare/skip rule to all 8 sites (production pins nothing, so zero behavior change there). (2) Straight-line harness code + higher-prio forever task = tick race: a prio-11 spinner preempts the prio-10 harness between add_task and teardown and never yields back — helper tasks that the harness doesn't hlt-wait on must run BELOW harness priority (prio 9). (3) Never read TCB fields after terminate_and_drain (drain frees; 0xDD poison reads nonzero) — assert live-block properties before teardown, slot-reuse properties after. (4) Suite wall outgrows fixed expect windows: deadline 115→139 tests measures up to 185 s vs the 120 s class timeout — TIMEOUT verdicts with complete S: lines mean undersized window, not failure; size from measured walls (moved to the 250 s list).
- **Adapted:** pinned spare/skip in scan + 4 drain paths + destroy + reap can_reap; prio-9 helpers; watchdog.md §7 daemon section; Makefile deadline timeout.
- **Style re-surface:** §11 (no new locks — is_block_pinned is lock-free reads, safe in ISR paths); §5 fail-closed preserved on every expiry path; §6 bounded loops untouched.

### #278 — signal-vs-sleep lost wakeup (owner review)
- **Learned:** (1) sys_kill set pending_signals but never woke wheel-blocked sleepers — park-until-wheel-expiry by design gap, not just test race. (2) IrqGuard is non-nestable: probe+park must drop the guard before reschedule()/set_task_ready() (both take it internally) — close the residual window with a same-dispatch re-check instead. (3) set_task_ready is IrqGuard-only (no scheduler_lock_), hence tick-ISR-safe; the alarm path gets the same wake as sys_kill for free.
- **Adapted:** atomic probe-and-park + post-reschedule re-check in sys_nanosleep; sleeper-only wakes in sys_kill and alarm delivery.
- **Style re-surface:** §11 (guard discipline, deferral-safe wakes); §5 (EINTR path byte-identical, rem math untouched).

### #45 — raw-counter boot time source (T0=0 fail-open found live)
- **Learned:** (1) `Timer::ns()` reads 0 until `Timer::init()` sets the freq var on ALL archs (x86 `tsc_freq_hz_`, aarch64 `counter_freq_hz_`, riscv `timer_freq_hz_`) — any entry-stamp taken through ns()/ns_monotonic() is 0, and a "skip when zero" gate fail-opens forever. Found by live boot, not by tests (no test boots through real calibration). (2) The fix is raw counters with no init dependency (rdtsc/cntpct/rdtime + live-or-constant freq) converted at use with the #16 overflow-split form; zero-freq fails closed. (3) `Logger::debug` is runtime-filtered below the default level — boot milestones must use info/debug_write or they vanish from serial (missing stage table misread as missing marks). (4) Planner stubs happen: a planner returning schema-claims without the document is a non-plan — verify the artifact exists before Phase 2, and say so on the thread.
- **Adapted:** `Timer::raw_counter/raw_counter_freq_hz` ×3 archs; raw entry stamp + raw stage marks in kernel.cpp (ns stamp untouched); watchdog.md §6 handoff note.
- **Style re-surface:** §10.5 named constexprs (no magic freq math); §5 fail-closed zero-freq/invalid (never false-pass); §6 debug/release parity (marks+gate both builds, logging-only divergence).

### #41 — per-task watchdog foundation
- **Learned:** (1) Planner syscall numbers must be verified against the enum tail before writing code (91/92 were correct; trust-but-verify caught nothing here, but the check is cheap). (2) Never compare a per-object arm counter against the global creation sequence — the auditor's S1 was right: unrelated counters fail open and live watchdogs never fire; staleness belongs to disarm-on-teardown + snapshot rewind, not cross-checks. (3) `VnodeOps` slot types are mixed int/int64_t — mirror the sibling (stat) signatures exactly, the compiler (not LSP) arbitrates. (4) The syscall table is positionally built — append in enum order after the DEBUG/RELEASE `#endif` block; fuzz tests derive bounds from MAX_SYSCALL dynamically, so no test churn. (5) Live proof beats gates: a 20M-spin probe outran the 200-tick window (probe too fast, kernel fine) — starve infinitely and watch for the kill line instead.
- **Adapted:** TCB wdog fields + 4-site init + TaskFields rewind; syscalls 91/92 off FAST path; on_tick scan with defer_kill; TIMING+6; read-only /proc nodes; docs/specs/watchdog.md.
- **Style re-surface:** §11 no lock across reschedule (IrqGuard-only handlers, scan under held lock, defer_kill); §5 dedicated -SYS_ERR_* returns (never ENSURE); §12 TCB memset discipline at all sites.

### #234 — dmesg severity + canonical numbering + single renderer
- **Learned:** (1) Map legacy codes by PRODUCER MEANING, not legacy number: 0xDB08/0xDB09 were only ever pushed by the missing-lib/layout sites while their legacy texts said "already/not loading" — number-based mapping preserves the lie plus a severity downgrade (auditor S1, caught iter-1). (2) riscv64 does not fold `__builtin_memcmp` (emits a freestanding-missing `memcmp` libcall) while x86_64 does — manual char compares in tests link everywhere; proven pre-existing via stash-baseline rebuild before fixing. (3) `rg -rn` is the documented footgun (`-r` = replace): it silently empties output — use `rg -n`. (4) Aggregate rows drift tree-wide (core 441-vs-494 etc.): update only the rows your change moves (dmesg 15→20→21, logging_debug 29→35→36), leave pre-existing drift alone. (5) `safe` membership is explicit registration, not class inclusion — new fine-class tests do not leak into the CI gate. (6) Full-taxonomy pass: a per-event push mirroring a per-admission Logger warn floods the ring 1455x/run and breaks fixed-buffer readers — throttle hot checks to first-exceed once-flags (serial keeps every occurrence). (7) Fixed klog/test buffers rot when boot legitimately gains entries (INIT milestones + drift notes overflowed 512): size from measured output with headroom (2048) and document WHY in the doc-block. (8) Canonical-strip dispatch keeps legacy raw pushes byte-identical while new canonical pushes resolve to the same text — additive taxonomy without migration risk.
- **Adapted:** docs/specs/dmesg.md (severity/codebase/base tables, old→new map, producer inventory); dmesg_catalog.hpp record tables; format_dmesg_entry single renderer; elf_loader manual-compare fix.
- **Style re-surface:** §10.5 named constexprs (all bases/offsets, zero magic numbers at push sites); §5 fail-closed (UNKNOWN/ERROR defaults, ENSURE-free); §6 bounded loops + NUL-terminated truncation; §10.3 explicit field init on LogEntry growth.

### #266 — Cap handles: encode at the syscall return boundary
- **Learned:** (1) When producers and consumers disagree, the spec + tests + strictest consumer form a majority: spec defined opaque handles, lookup enforced them, tests encoded manually — only the syscall returns dissented. Align the outlier; never weaken the strict side. (2) Cross-task installs need the DESTINATION's identity in the returned handle (grantee space is where it validates), own-root installs the caller's. (3) Install-then-read races close by writing the out-value inside the occupying lock (defaults keep callers source-compatible); residual staleness must fail closed, stated. (4) Contract changes migrate tests too: every assert treating returns as indices (comparisons, re-encodes, slot removals) moves to decode-aware form with doc-blocks updated together — the failing classes after a contract change enumerate the migration surface; run every consumer family, not just the direct class. (5) Centralize with inline functions, not macros: the failure path stays visible and single-evaluation is guaranteed (macro proposal declined for the shared -1/encode rule).
- **Adapted:** encode_user_handle + gen_out through install/grant/copy/mint/retype; 5 producers encode (dst vs own-root rule); round-trip regression test (returned handle used directly); cspace.md opaque-return contract.
- **Style re-surface:** §5 -1 paths byte-identical; §12.x single-owner untouched; §7 doc-block + code together on contract change; no new locks; debug/release parity.

### #263/#264/#265/#246 — S1 batch: kslot layout, ring race, cspace contract, release-elf
- **Learned:** (1) Capture/restore is a byte contract: advance unconditionally on BOTH sides; a filed one-line fix ("move outside the loop") can pin the offset at zero — verify proposals against the layout, not the symptom. Filed titles routinely misname the mechanism (OOB vs misalignment; UAF vs UB race; exhaustion vs self-consistent tag) — re-derive from code before planning, and disagree with the premise when evidence demands it. (2) Zero-caller code bounds a race's blast radius to diagnostics, but C++ UB still gets fixed — atomic claim, no new locks in creation paths. (3) Truncation can be load-bearing: closed-loop masking (encode/decode/store) plus own-CNode resolution makes the tag self-consistent; naive widening breaks self-lookup — document non-identities, and file adjacent real findings separately instead of smuggling scope. (4) Direct make goals inherit no target-specific flags and no stamp guard — route through flags-correct targets + stamp guards; validating a new build path surfaces latent blockers (arch-specific test asserts, libcall backstops) — fix within breaker budget. (5) Retrieval artifacts go on the thread BEFORE the audit call — a check-7 REJECT with zero code findings still costs a full iteration.
- **Adapted:** task.cpp kslot if/else + ring atomic claim + cspace contract note; Makefile release-elf + release stamp + shared flags; test arch guards; compiler_rt strncpy backstop; raw-vs-encoded follow-up filed as #266.
- **Style re-surface:** §6 bounded loops, no new loops/locks; §11.6 atomics all-or-nothing; §10.3 init; §5 fail-closed preserved on every path.

### #242 — Scheduler wedge: bounded starvation-breaker
- **Learned:** (1) Two never-blocking same-priority spinners deterministically starve ALL lower levels — per-tick RMS + re-queue alternates them while the harness gets zero CPU (proven by GDB snapshots 3001 ticks apart); no race, no corruption, a missing liveness property. Single-spinner survives structurally (alternation), which is why only dual+ hung. (2) Same-priority round-robin would NOT fix it (rotating 11s never select a 10) — the fix must guarantee lower-level progress; deeper nesting keeps classic fixed-priority semantics. (3) Liveness-test windows must span ticks: a fast syscall loop fits between ticks (false PASS); sleeps each needing a fresh dispatch discriminate (TIMEOUT pre-fix, PASS post-fix) — always stash-control (fix-stashed run must hang). (4) `continue &` + `interrupt` fails on the QEMU gdbstub in batch; breakpoint + ignore-count stops are fully synchronous. (5) Spinner-table +3 PMM on grant lifecycles is background noise — compare against sibling tests before hunting leaks.
- **Adapted:** breaker in RMS (counter-only, N=20 fixed, EDF precedence, init/restore reset); contract: scheduler.md §3.3/R4 (1-in-20, 20 ms ceiling).
- **Style re-surface:** §11 (no lock across switch, INV-2 discipline, no priority writes so PI/admission untouched); §6 (O(1) bitmap ops, bounded drop-loop, identical debug/release flow); §10.3/§10.5 (explicit reset owner, named sentinel).

### #252 — APIC EOI-skip via ungated safe_copy recovery
- **Learned:** (1) The x86 fault-recovery check must be vector-gated (#PF/14 only): a tick inside a guarded copy otherwise hijacks recovery + skips EOI, wedging the APIC with ticks dead (aarch64/riscv route from fault dispatchers only). (2) Forensics ladder, each rung decisive in turn: exact artifact + map → addr2line → `-d int` tick histograms → debugcon phase markers → GDB halt → `monitor info lapic` + host CPU%. (3) Frozen APIC counter + ~100% vCPU + IF=1 spins = host virtual-time freeze (retry, don't chase). (4) Never hand-write awk single-quotes inside `bash -c` — escape or generate programmatically.
- **Adapted:** vector-14 gate in handle path; retry-policy note on the Docker issue.
- **Style re-surface:** §5 fail-closed (real #PF still redirects); minimal one-condition diffs; TEMP-DIAG reverted.

### #251 — Ubuntu-only pre-serial triple fault: PIE-GOT slots
- **Learned:** (1) Ubuntu GCC defaults to PIE: GOT codegen under LTO can resolve an extern value-load through a slot past image end → triple fault with zero serial. Match compile (`-fno-pie`) to the `-no-pie` link; search in-tree precedent before theorizing toolchain bugs (riscv64 already had it). (2) Auditor veto is load-bearing: an address-of "fix" can be a semantic inversion — rejected_patch.diff applies verbatim. (3) Container-debug recipe: TEMP-branch artifact upload (exact map+ELF) + `-d int` tick-vector histograms over wall time convict tick-stop.
- **Adapted:** `-fno-pie` on x86_64 CXXFLAGS; aarch64 `-fno-pie` gap filed as follow-up.
- **Style re-surface:** §5 fix at the right layer (per-symbol patching is unbounded, the global flag closes the class); TEMP branches deleted grep-verified.

### #250 — Wake-while-current deferral
- **Learned:** (1) Enforce invariants at the WAKE source, not the dispatch sink: making queued-while-current unrepresentable kills the victim class the evict branch preyed on. (2) "Clear flag before call" is insufficient when the predicate keys on currentness, not the flag — the inner-helper split was the correct construction. (3) set_task_ready must run its liveness guard BEFORE writing READY, or it resurrects zombies; clearing flags without unlinking strands. (4) Post-rescue dispatch legitimately marks tasks RUNNING — assert dispatchability, never exact flags. (5) Snapshot fields capture/restore symmetrically.
- **Adapted:** ready_deferred_ + defer/consume across enqueue/set_task_ready/switch_to_task/set_current/dequeue_ready; evict branch untouched.
- **Style re-surface:** §11.4 (priority only via helper); §10.4 init-list order matches declaration; §10.2 refs/pointers; §5 fail-closed; §6 bounded + parity.

### #249 — Shell wedges READY-but-unqueued, tick-side rescue
- **Learned:** (1) Enqueue-while-current → `next_task` candidate==current evict pops + clears inrq → READY + unqueued + non-current forever; the `inrq==1` orphan detector is blind to it — needs a level-triggered backstop. (2) Sweeps run before the RMS tail, so a rescued task may be RUNNING + dispatch-armed at assert time — assert dispatchability, never exact flags (TaskState READY=0/RUNNING=1). (3) Blindness asserts compare peek against the fixture, not null (idle legitimately sits queued). (4) Live forensics via `nm` addresses when GDB scoping fails; auto-detach (`quit` in batch) leaves the guest running. (5) Per-event warns on a hot path feed back into the reported race — throttle detectors (exact counter + 1st + every Nth).
- **Adapted:** on_tick stranded-READY sweep + rescue counter + throttled warn; backstop citations at notify/shell sites.
- **Style re-surface:** §11.2 (READY implies dispatchable); §11.4 (priority only via helper); §6 bounded loops with named bound + debug/release parity; fail-closed READY-only sweep guard.

### #247 — riscv64 UART input via 16550A MMIO poll
- **Learned:** (1) riscv64 low half is identity-mapped in S-mode: raw-phys MMIO works with zero VMM work (TX stays SBI ecall, RX via mmio_read8; MCR-loopback test pins coherence). (2) `-serial mon:stdio` feeds typed bytes to the monitor, not the guest — deterministic E2E input needs TCP/file serial. (3) Count live JARVIS_REGISTER_TEST lines before bumping expected-count rows.
- **Adapted:** Serial::poll_getchar/getchar per arch; loopback regression tests.
- **Style re-surface:** §10.5 named constexprs (no magic MMIO addrs); §6 bounded loops (zero-wait poll, bounded getchar); §7 full doc-blocks; fail-closed sentinel matching siblings.

### #248 — Shell echo trips async-output detector
- **Learned:** Terminal::serial_putchar is arch-asymmetric by design: x86 bypasses Serial::putchar (raw COM1, uncounted) while non-x86 routes through it (counted) — the shell's own echo is invisible to the async guard on x86 but trips per-keystroke redraws elsewhere. Reason foreground/background write accounting per-arch whenever Terminal and Serial paths differ; redraw-guard changes need an interactive transcript.
- **Adapted:** prompt_serial re-sync after readline's own echo writes.
- **Style re-surface:** redraw-guard heuristics document their TOCTOU window; x86 no-op stores beat ifdefs.

### #245 — aarch64 shell input dead: no UART poll source
- **Learned:** (1) Run the cheapest end-to-end discriminator BEFORE deep GDB (hardened smoke showed bytes rotting in the FIFO — every wheel datum had been normal steady state). (2) A passing E2E probe can be vacuous: expect matched a redrawn prompt from async output — require command output, not UI state. (3) A red `make build` on main from an earlier commit is still your gate to fix. (4) Shared-build + direct `$(KERNEL)` target = LTO-link trap when mixing debug/release — `make clean` on config switches (see #246).
- **Adapted:** Serial::poll_getchar wired into all shell input sites; hardened shell-smoke requiring command output; loopback regression tests.
- **Style re-surface:** auditor reads comments as claims — fix flagged wording immediately; machine-format history rows.

### #244 — Release boot output cleanup
- **Learned:** (1) Quoted-string `-D` macros do not survive generated build rules (extra shell layers strip quotes; fails only on clean rebuilds since `-D` changes bypass `.d` deps) — pp-number + in-code two-level stringify is shell-proof; always verify with a fresh full build. (2) `make build` green means nothing for release-only paths — gate both builds. (3) Test-harness serial asserts only match their own markers; verify gating by grep, never assume.
- **Adapted:** Version::picolibc_string + Makefile early pin; CONFIG_DEBUG-gated boot diags.
- **Style re-surface:** minimal diff on cleanups (flagged lines only); fix auditor comments immediately.

### #237 — debug_attach_rejects orphan race
- **Learned:** (1) A test racing task death can NEVER assert liveness-dependent errnos — determinism by construction (live spinning orphan, parent_id 0) beats timing luck. (2) Retrieval artifacts belong on the issue BEFORE the audit call (check #7 rejects on process alone). (3) remove_child early-returns for non-children, so debug_reap_child is orphan-safe without a special path; detach asserts should print the actual errno.
- **Adapted:** live-spawn orphan case + errno-printing asserts.
- **Style re-surface:** machine-format history rows — verify with the format regex before committing.

### #236 — aarch64 el0 fault trap frame
- **Learned:** (1) The debug slot needs a WRITER on every trap path: el0_sync faults never ran save_all, so handler frames descended from kstack_top straight through the slot — publish freshness and SP alignment can both be fine while content discipline is the whole bug. (2) Stacks grow DOWN: post-save_all pushes land BELOW the slot (safe by geometry). (3) GAS macros must precede use.
- **Adapted:** save_all in el0_sync non-SVC path; slot ELR + stash regression test.
- **Style re-surface:** pre-fix control run (stash, run, pop) is the cheapest flake-vs-regression proof — use it before theorizing.

### #235 — aarch64 debugd residue
- **Learned:** (1) GDB on QEMU beats static analysis for MMU mysteries: census → PTE-watch → TTBR → caller-ID via stack unwind + addr2line. (2) VMM::get_table silently zeroes PTEs whose target is unallocated — ANY freer of a live page kills its mapping on the next walk; heals that re-map mask the freer. (3) Test bugs outnumber kernel bugs in debug suites: poison the VA actually executed, assert park-waits that can be satisfied, reason duty cycles off the ACTUAL stub (spinners never yield). (4) Logger `%d` reads 8 bytes — `%lx` with u64 casts.
- **Adapted:** EL1 recovery check mirroring riscv/x86; direct-VA poison + fault-latch ESR accessor pattern.
- **Style re-surface:** hypothesis + validation plan BEFORE code; TMP-DIAG fully reverted grep-verified; QEMU processes killed after every session.

### #205 — arch_riscv64 gate proof
- **Learned:** (1) Triage by direct read beats summaries for split-count questions. (2) Clone semantics are arch-shaped: x86 zeroes the low half for user isolation, riscv low half IS kernel MMIO/identity (copy L0[0]), aarch64 survives via HHDM aliases. (3) S-mode cannot read M-mode CSRs (illegal-insn) — they can never gate S-mode code; banner-proof replaces them. (4) The RTC is an uptime clock: pin the epoch contract instead of deferring everything.
- **Adapted:** L0[0] clone copy; init_stack by-ref; plic unmask-first; RTC 1970 pin; deregistrations carry follow-up pointers.
- **Style re-surface:** deregistration comments must carry the follow-up issue or the deferral rots.

### #29 — riscv64 production boot path
- **Learned:** (1) Stacked faults need evidence per layer (raw-phys dump, overflow proof, ECAM map, satp MODE slot, snapshot walks) — each proven, none guessed. (2) Stale-TLB masking is real: diag reads need sfence.vma to force walks. (3) `la` + fixed VMA/LMA correspondence is load-bearing: shifting .text LMA breaks the HHDM VMA match silently — restore with load-bearing padding. (4) RV64 LUI sign-extends: zero-extend before unsigned compares. (5) reserve_range without free-list rebuild is void on the fast alloc path. (6) Producers-pass-raw-phys means auditing the consumer, not just the producer.
- **Adapted:** DTB HHDM alias; enlarged boot stack + L1 leaf + PMM reserve + boot-time guard; ECAM + leaves; satp MODE-or; arch-gated snapshot paths.
- **Style re-surface:** TEMP-DIAG fully reverted grep-verified; rejected_patch.diff applies verbatim or hand-apply identical lines; multi-line ASSERT + semicolon-strings break this ld.

### #217 — waitpid wake on fault kill
- **Learned:** (1) Promote-don't-duplicate: an already-running file-static on another arch means promoting to API adds zero new arch risk — the only new surface is the call site. (2) Parity bugs hide in disjunctions: exact-PID vs -1 matching hung wait-any parents on every non-sys_exit death on ALL arches, unnoticed. (3) aarch64 kernel entries must NEVER return (no trampoline) — test entries self-terminate + park. (4) Saved-frame asserts must be captured in-entry: any re-switch overlays slot 0.
- **Adapted:** Scheduler::wake_waiting_parent (+-1 match); fault handler wakes before switch_away; waitpid-parent regression test.
- **Style re-surface:** Rule-5 capture-then-teardown when teardown frees (locals + stack ctx only in asserts).

### #28 — aarch64 production boot path
- **Learned:** (1) The gap was fault POLICY, not mechanism (EL0 work already ran) — untested failure paths, not happy paths, gate "production"; prove them with pre-fix TIMEOUT probes. (2) Never Scheduler::terminate the current task from exception context — state+exit+switch_away_from_terminating, asm applies. (3) TERMINATED TCBs free ~100 ticks later and log deltas — teardown with terminate_err+drain for deterministic zero-delta.
- **Adapted:** el0 fault → TERMINATED/-SIGSEGV + switch; vectors apply-or-park epilogue; fault-terminates regression test.
- **Style re-surface:** extern-C getters for test-observable latches; fault-path comments state what is NOT done and where it goes.

### #215 — stale clone caller conventions
- **Learned:** (1) Untested ≠ dead — classify reachability, not coverage: a branch with no EL0 caller yet is one exec away from failure. (2) Shadow/duplicated constants strike again — single-choke-point helpers beat scattered ifdefs. (3) Cross-arch header changes break the THIRD arch first — always link-check all three arches for shared-header edits.
- **Adapted:** make_synthetic_clone_frame() single helper (per-arch PC/SP); corrected stale comment.
- **Style re-surface:** default args must be USED on all arches ((void) silencers for arch-N/A params).

### #214 + #216 — EL1 fail-stop exposes PCI ECAM rot
- **Learned:** (1) Fail-stop instrumentation EXPOSES latent bugs by design — budget for exposed-bug fallout when landing fail-stop. (2) Ground truth is `info mtree` piped HMP, not code comments (wrong ECAM base + raw-phys VA disease). (3) NEVER hand-compute table indices (self-contradictions); shell/python is authoritative. (4) Duplicate constants across headers: FAR proves which copy is live. (5) panic() is extern-C noreturn callable from asm; Serial::putchar/puts is the only proven lock-free dump; %lx hangs.
- **Adapted:** el1_sync_unexpected (ESR/FAR/latch → C dump + debug panic, release skip preserved); ECAM base + HHDM alias; mapping + range fixes.
- **Style re-surface:** fail-stop over availability for unexpected kernel faults (debug); SError/FIQ deliberately out of scope.

### #210 — no-prefix arch switch double-build
- **Learned:** (1) The TRUE root cause was NOT the stamp: positional debug/release goals ALSO match real targets, so the outer default-ARCH make built a second cross-arch tree — a stamp-only fix would have masked the real bug (the rerun failure proved it). (2) `-n` parses still run $(shell) side effects — never trust dry runs to preserve state; rebuild after. (3) Tracked build blobs (*.elf) churn on every relink and make deletes failed targets — restore before commit.
- **Adapted:** no-op dispatch guard on debug/release goals; goal-aware arch stamp; switch-clean excluding tracked ELFs.
- **Style re-surface:** build-system changes need SIL 3 audit too; document planner deviations.

### #209 — clone frame unreadable: raw-phys VA + silent-skip
- **Learned:** (1) clone()'s kernel-parent fallback used raw phys as VA on non-x86 (unmapped at EL1); EL1-sync default_exception SILENTLY SKIPS faulting insns (elr+=4, no log) — prove via in-kernel probe (top-sp, walk, ESR DFSC, FAR, objdump loop). Map via HHDM on non-x86. (2) Print via Logger::raw_write+print_hex only. (3) `task`/`process` are NOT class names — 0-test runs are harness config errors, investigate before concluding. (4) Verify side-benefit claims (canary installer only runs in the user branch).
- **Adapted:** clone fallback HHDM on non-x86; frame-readback regression test (slots + top-sp); follow-ups filed for handler S3 + caller OOB.
- **Style re-surface:** failure-path leaks contained by snapshot boundary; assert-before-teardown with allocation-free asserts.

### #104 — aarch64 boot + fork prerequisites (open)
- **Learned:** (1) Check HOW a test file's runtime values are produced before trusting them: sizeof() differs between ad-hoc probe flags and the real build (CONFIG_DEBUG ring adds ~64B) — measure with EXACT CXXFLAGS or runtime prints. (2) Push ORDER determines memory order: ascending pushes mirror registers; match the x86 descending convention. (3) Stack-scan unwind output is FICTION (data resembling code); objdump only on the MATCHING binary; QEMU -d int + faulting-CPU prints are the reliable pair. (4) CR3=0x1000 is the normal kernel PML4 (boot tables kept). (5) GDB breakpoints on hot paths suppress timing races — cold-path diagnostics instead. (6) AArch64 default 64K pages pad ELF LOAD segments past the 64KiB tmpfs cap — link user test programs with max-page-size=0x1000. (7) Logger::error hangs on some patterns (%lx, 20-digit %lu) — raw_write+print_hex. (8) Arch stamps flip mid-run — verify which sub-make links. (9) AArch64 linker script needs marker KEEPs mirroring x86_64 (orphan collision once layout shifts). (10) TCG pause/wfi costs ~15ms wall each — tick-aware yields + short bounds + serial heartbeats for long joins.
- **Adapted:** pool size + TCB static_assert; clone indices/descending loop; linker markers; fork-marker program + embed + max-page-size; memory_safety arch-gated bound.
- **Style re-surface:** verify-then-trust auditor S3s (recheck severity claims independently).

### #204 — riscv64/aarch64 test link
- **Learned:** (1) Check for existing patterns FIRST: per-arch test_stubs already stubbed registrations — added stubs collided (multiple-definition) and were reverted. (2) clone() reads fixed reg counts per arch — size test arrays per arch (22/31/37), fixed-size arrays OVER-READ on non-x86. (3) LTO can internalize/GC runtime libcalls at scale — `externally_visible` fixes it; verify with from-scratch (ccache-disabled) rebuilds. (4) NO_LTO link errors are artifacts — always diagnose on the LTO path. (5) clang-tidy cross-arch 'unknown register' errors are non-fatal host-triple noise. (6) Link-green ≠ runs-green — say which explicitly.
- **Adapted:** per-arch tls clone frame + LTO attributes in compiler_rt; redundant stubs reverted.
- **Style re-surface:** test + attribute changes only.

### #185 — riscv64 ECALL dispatch ABI
- **Learned:** (1) Check arg COUNT against the prototype, not just mapping (wrong number reg + dropped arg3 + missing sepc+=4 = three bugs, not one; ECALL is always 4 bytes). (2) The aarch64 stub is the exact pattern to mirror — cross-arch precedent beats reasoning from scratch. (3) riscv S-mode ecalls trap to OpenSBI (M-mode), NOT stvec — no S-mode self-test of dispatch is possible. (4) Overriding a reporter's explicit constraint needs a user decision + filed follow-up, not unilateral action.
- **Adapted:** 6-load dispatch + TRACE-A post-save + sepc+=4; runtime-cookies follow-up filed.
- **Style re-surface:** asm mirror-pattern discipline.

### #202 — hal acpi crash closed as fixed-by-#197
- **Learned:** (1) BISECT FIXES, DON'T ASSUME COVERAGE: only the _in_pml4 flags proved load-bearing (kept the rest as approved defense-in-depth for a real-by-inspection hole). (2) Half 0 in cross-shape asserts is the IDENTITY half (pml4_idx 0) — read the loop before chasing two ghosts. (3) Map/unmap splits are fill-then-flip (atomic store) — the hazard is the PRUNER zeroing live PDEs on stale accounting; fix accounting, not atomicity. (4) A 0/3 → 0/20+ swing closes statistically (p<0.001) even without re-catching the crash; document residual mechanism + reopen criteria honestly. (5) test-history rows are machine-read: no NOTE suffixes; [HOST] elapsed is wrapper overhead — use RESULT test-ms or measured wall deltas.
- **Adapted:** no code changes (tree verified identical to the audited state); close comment carries evidence trail + reopen criteria.
- **Style re-surface:** none (no code).

### #197 cycle 3 — SMP races + snapshot accounting holes
- **Learned:** (1) Quiesce doesn't nest: guard + internal enter/exit unparks the AP mid-window — refcount + outermost-exit + ENSURE + depth accessor + nesting test; audit ALL enter/exit pairings before adding ENSURE. (2) Fake AP-slot + live AP = corruption: freeze (IrqGuard+quiesce) + RAII slot restore (early-return safe) + zombie solitude (pre-drain, assert 0/1). (3) Restore tail must run INSIDE the quiesce window (AP reads torn runqs otherwise) — extend the window over the daemon block. (4) _in_pml4 variants must carry modified-flags or splits persist (info at 0x61000!). (5) PMM rewind untracks mid-class pool pages — post-snapshot overlay (set on alloc, clear on free, apply pre-rebuild); free-path clearing prevents bit-resurrection leaks. (6) CR3=0x1000 is normal (boot PML4 kept). (7) Stack-scan unwind is fiction — objdump on the MATCHING binary + `-d int`. (8) GDB hot-path breakpoints suppress races — cold-path diagnostics (H2 ring, panic dumps). (9) Bisect aggregates by GROUP registration before file-by-file hunting.
- **Adapted:** refcount quiesce + guards + solitude + slot RAII + window extension + _in_pml4 flags + pool overlay + nesting test; cross-shape asserts KEPT STRICT (proven bug-finders).
- **Style re-surface:** §5 fail-closed (ENSURE on unbalanced exit, refusal over orphan); RAII for early-return windows; no asserts inside live windows; TEMP diagnostics reverted before audit.

### #183 — fdt scope-out + static-blob tests, sibling bugfix
- **Learned:** (1) COVERAGE_EXCLUDES ≠ denominator: the flag only controls the exclude-file-list; the denominator comes from ELF symbols — scope-outs apply report-side. Verify tooling claims; planners conflate. (2) NEVER HARDCODE BLOB LAYOUT (one wrong count silently shifts every offset) — builder-recorded offsets make tests self-consistent. (3) Write tests that FIND bugs: the new test proved sibling incoherence the suite had hidden (depth-1 consume + both-directions coverage). (4) Document surprising-but-correct semantics in-test (outermost match).
- **Adapted:** report-side scope-out + filter/exclude plumbing; static-blob tests covering all public entries; coverage.md scope-out contract.
- **Style re-surface:** §5 fail-closed codes (NOTFOUND, not bogus offsets); no digit-terminal test names.

### #199 — cross-arch teardown validation + boot-shape forensics
- **Learned:** (1) LIVE TABLES ARE NOT BOOT-PRISTINE (PCI windows, kstack window, identity aliases, splits, scan remnants) — "exact boot shape" is the WRONG invariant; absent-or-PMM-tracked + round-trip entry-equality is right. (2) VERIFY ARITHMETIC, DON'T MENTAL-MATH (priv slot is 320, not 257). (3) Committed TEMP-DBG = audit S2 — verify removals with the read/grep tool, not shell pipelines. (4) TEST-ORDER DEPENDENCE IS REAL: standalone-green classes can fail inside aggregates (prior tests leave splits + tolerated leaks). (5) Scope hygiene: spin layout-triggered races into their own issue with evidence instead of ballooning.
- **Adapted:** absent-or-tracked shape tests + identity-minimal + HHDM round-trip under IrqGuard + flag semantics; open windows documented in-test.
- **Style re-surface:** §10.6 (no digit-terminal names); §5 fail-closed codes; §11 (IrqGuard RAII; guard-vs-assert-abort verified: record+return unwinds correctly).

### #200 — core QEMU_EXIT, IRQ-atomic HHDM PD surgery
- **Learned:** (1) STASH-BASELINES LIE when the fix is already committed — true baselines need a worktree at the pre-fix commit (+ copy build/picolibc/, worktrees don't share it). (2) QEMU `-d int` is CPU-ground-truth for triple faults (first fault vector/code + APIC tick before). (3) GDB stub + HMP `xp` DEADLOCK (BQL) — GDB high-half reads also fail post-halt. (4) PROBE PRINTS PERTURB: fewer prints preserve crashes better (single-print configs); observation changes outcome. (5) The FAIL line IS evidence (unmap invisibility + RDI/RIP pinpoint). (6) Image-size shifts detonate latent address races — a version revert that doesn't fix proves it; bisect with worktrees. (7) PD surgery is IRQ-critical (snapshot_restore precedent) — test-side IrqGuard across map..restore + exact-VA invlpg; NO production VMM change (split is single-store atomic; guarding alloc paths violates WCET rules).
- **Adapted:** IrqGuard + exact-VA flush in the VMM regression tests (warm-up+rehuge only, timed loop untouched).
- **Style re-surface:** §11 (RAII guards, short IRQ-off sections incl. PMM frees); §10.3 init; §5 fail-closed codes over panics.

### #198 — IRQ controller standardization + early_irq_init contract
- **Learned:** (1) Calibrate idempotency needs label-vs-frequency split: APIC CPUID caps probe after Timer::init, so a cached TSC label must promote on repeat calls; BOOT_ONLY success latches (zero-check first, latch second). (2) GIC polarity: break on the spec value (online==1) with a named bound + ready-query, not on zero. (3) CTL-DISABLE ≠ DISARM on AArch64: TVAL freezes when disabled — zero TVAL first. (4) S-mode CANNOT CLEAR STIP: park mtimecmp at ~0ULL; re-arm relative (now + latched interval), never absolute-counter math. (5) New test files need THREE wirings (register decl + run_group + class entry) PLUS counts row + aggregate membership, or TCOUNT warns. (6) Stale gen.mk: `rm -f mk/cpp-rules.*.gen.mk` after adding .cpps. (7) Baseline-exoneration via stash → new issue; the tree stands exonerated.
- **Adapted:** early_irq_init contract spec + per-arch init with success latches; range guards; fail-closed zero-freq guards; new test class in hal aggregate.
- **Style re-surface:** §5 fail-closed codes, ENSURE-free reachable paths; §10.3 init all statics; §10.5 named constexprs over magic values; §10.6 no digit-terminal names; §1 namespace-scope latch, no function-local statics; §6 bounded polls.

### #75 — picolibc verify program + loader skew fix
- **Learned:** (1) CHUNKED LOADER IGNORED PAGE SKEW: file/page-offset skew misplaces every page of non-aligned segments — the fix term is the load bias (mapped − linked), 0 for aligned. Found via HHDM readback + shifted-slot proof. (2) Driven tests use prio ≥11 (a prio-2 waitee never runs once dispatch demotes the harness; the breaker now guarantees eventual lower progress, but ≥11 keeps dispatch prompt). (3) UART RX bursts overrun the 16 B FIFO mid-burst at tick granularity — capture via tmpfs file for byte-exact output. (4) TMPFS CAP is 64 KiB — strip user ELFs for staging. (5) INITRD is UNMOUNTED in test boot — embed test images via objcopy, don't stage-and-resolve. (6) terminate() RE-ARMS off a dying current task — self-block, never return.
- **Adapted:** chunk-loop skew compensation (skew-0 reduces to old behavior); verify class (tmpfs stdio, prio 11, exit-code rule); strip+objcopy embed (x86_64-guarded, both links).
- **Style re-surface:** §5 dedicated error codes, ENSURE-free staging with unlink on every path; §11.2 BLOCKED+dequeue pairing; §12.4 single-ownership destructors; §12.1 dead-state filter; §10.3/§10.5 init + unique sentinels.

### #74 — TLS on context switch + TLS_SET
- **Learned:** (1) TERMINATE RE-ARMS: terminating off a dying current task clobbers the arm and strands the target — sanctioned exits never return; tests self-block post-arm (BLOCKED + dequeue_ready + hlt, terminate from outside). (2) reschedule() DOES NOT ARM — only need_resched; arming is tick-side. Observe publish through real dispatch. (3) clone(nullptr) FAULTS (unconditional regs[] deref) — pass a synthetic user frame. (4) is_canonical() is x86-48-bit-specific; aarch64 rejects valid high user VAs, riscv64 Sv39 needs an explicit shift check. (5) switch_to_task is file-static — reach publish sites via reschedule()+tick, not direct calls. (6) Same-prio FIFO guarantees the peer runs on the first yield — small explicit bounds beat minute-long retry loops. (7) Only the generic gen.mk is tracked (arch files are ignored scratch) — new .cpp rules go in all three on disk, only the generic commits. (8) x86 epilogue pops ALL GPRs (scratch-safe); riscv must consume into OFF_TP; aarch64 .tls_apply joins at the shared isb.
- **Adapted:** TCB tls_base_ + TLS_SET + per-CPU publish slot + 3-arch epilogue apply; LA57 asymmetry recorded as follow-up.
- **Style re-surface:** §11.1 (lock for scalar store only); §11.2 BLOCKED+dequeue pairing; §11.4 priority via helper, distinct priorities for deterministic arms; §5 dedicated codes, ENSURE-free reachable paths; §10.5 first free ID above task band; §12.1 REAPED+TERMINATED filter on stored refs.

### #73 — picolibc link rule + libos glue
- **Learned:** (1) GNU make takes the FIRST-DEFINED applicable pattern rule — fix hijacks with a static pattern rule + filter-out from the generic source list. (2) Static-pattern prerequisites expand at PARSE time: pin vars must precede the rules include; absolute sysroots break target matching — relative everywhere. (3) picolibc baremetal needs a target libos: _exit + POSIX names + own sbrk (stdlib.o would collide on malloc). (4) picolibc errno is TLS — plain-global stub objects mismatch at link; disable thread-local-storage until real TLS lands. (5) Include sysroot `<sys/time.h>` FIRST for `struct timeval` (picolibc `<time.h>` alone doesn't declare it).
- **Adapted:** static-pattern link rule + glue object rule; file-targets + parse-time pins; build script flags.
- **Style re-surface:** build rules + userspace glue only (no kernel code).

### #72 — picolibc vendored + static sysroot
- **Learned:** (1) `-mgeneral-regs-only` is unusable for hosted-style libc (XMM va_arg); Ring-3 XMM is safe via the kernel FPU context, so keep `-mno-red-zone` and drop regs-only. (2) Meson `install` honors specsdir — confine it to the sysroot or it mutates the host compiler dir. (3) picolibc folds all math into libc.a; empty libm.a is intentional, not a build failure. (4) Managed pythons need install flags + PATH fixes (`--break-system-packages`, versioned bin dir).
- **Adapted:** cross file + build script + Makefile targets + healthcheck probe; vendored-tarball offline discipline with checksum gate.
- **Style re-surface:** host tooling only — confinement (nothing outside repo except user tools).

### release v0.4.10 — Test-Coverage Closure
- **Learned:** (1) A reverted premature release leaves a precise precedent: `git show` on the revert reveals exactly which files a release touches — mirror it to stay surgical. (2) Toolchain skew (local vs committed docs) turns a full regen into structural churn; change the version stamp only, revert the regen. (3) `test-full` auto-appends history rows + SUMMARY lines, but bare `execute-test` does not — single-class runs still need a manual row. (4) Release-gate arithmetic cross-checks from the log (debug sum, release TF_RELEASE subset) — recompute, never trust memory.
- **Adapted:** release + tag + mirror + bump; ROADMAP_done milestone entry; ROADMAP open-items-only collapse.
- **Style re-surface:** no kernel code (version/docs only, no audit); selective `git add` (untracked graphify cache never enters a release commit); never delete without asking.

### #18 — Bounded-wait primitive: wheel-armed receive
- **Learned:** (1) A BLOCKED+dequeued task has no waker by construction — any polled fallback MUST stay RUNNING. (2) Edge-triggered wakeups strand on silent dequeue: level-triggered re-apply (READY+enqueue every tick while armed+timed_out, idempotent guard, never clobber RUNNING) converts any missed wake into ≤1-tick-late delivery. (3) Intermittent full-suite hangs with moving sites need hypothesis-first forensics: serial probes mask Heisenbugs (prints in the tick path shift timing) — test-side bounded dumps don't. (4) Auditor corrective patches can be corrupt — always `git apply --check` first, then implement manually.
- **Adapted:** wheel-arm + inbox-first resume + RUNNING-poll fallback; arm/cancel/fire primitives; TCB timeout slot + TaskFields carry; scheduler tick-tail level trigger; live-clock expiry test chain.
- **Style re-surface:** §5 fail-closed arm/fallback; §11 (no spinlock across reschedule; short ISR sections; ISR try-lock-skip); §11.6 atomics all-or-nothing for tick-observed flags; §12.2 generation-tagged handles; test-first with history row per run.

### #17 — Event-timer wheel: static slot table + generation handles
- **Learned:** (1) An armed-but-never-serviced timer is a silent liveness hole: with a BSP-only tick hook, arm() must fail closed for cpu != 0. (2) Test expiry bases must derive from the LIVE clock: fixed synthetic bases below the suite clock are born-expired (green standalone, flake in `all`). (3) New top-level kernel dirs need no mk changes (auto-glob) but DO need per-arch gen.mk regen. (4) Regenerate pending_patch AFTER final staging via `git diff HEAD`, excluding itself; new-file coverage needs `git add -N` first with a diff-count check.
- **Adapted:** timer_wheel (IrqSpinLockGuard arm/cancel, try-lock-skip + earliest-N + unlock-before-fire, snapshot_reset with generation bump); on_tick hook + callback contract.
- **Style re-surface:** §11 (never reschedules; short sections; ISR try-lock-skip precedent); §12.2 generation-tagged handles; §5 fail-closed arm/cancel; §7 test-first with history row per run.

### #16 — HRT monotonic clock: TickSource + ns_monotonic
- **Learned:** (1) Declared-but-undefined methods link only while unreferenced — filling them can steal shared resources (LAPIC delegation steals the system tick): tests must steal-and-restore before further asserts. (2) Every new `.cpp` needs per-arch gen.mk regen. (3) Cross-arch compile verifies per-TU with `-fsyntax-only` + exact per-arch flags when full builds die on unrelated errors (confirm via stash + rebuild). (4) Calibration idempotency needs label refresh separated from frequency work.
- **Adapted:** resolve_source preference chain + CONFIG knobs; atomics-only monotonic commit; overflow-split conversions with clamp; fail-closed probe (no MMIO until mapped).
- **Style re-surface:** §5 ENSURE-free reachable paths (fail closed, never panic); §10.3/§10.6 init + descriptive names; §11.6 atomics all-or-nothing; test-first with history row per run.

### #95 — ELF shared-object support
- **Learned:** (1) Fixed-address links carry ABSOLUTE values — the correct term is the load bias (mapped − linked), 0 for fixed links; adding the base again double-counts. (2) Shared RO pages have split ownership (cache refcount vs per-pml4 walk): teardown is unmap-then-release everywhere, with the PMM owner bit as backstop turning slips into skips instead of double-frees. (3) Cache-hit reloc needs the image registered in the closure BEFORE applying (same as fresh loads). (4) Cycle detection via in_progress must stay set through dep recursion; depth increments exactly once per level. (5) PMM free deltas are exact only with in-window fixtures AND frozen ticks (IrqGuard is safe: reschedule never halts); pre-window writes read as phantom frees. (6) tmpfs mounts at /tmp, / is read-only initrd — absolute-path fixtures live under /tmp. (7) tmpfs ignores vnode refcounts — document shared exposure instead of fake-pinning races.
- **Adapted:** elf_shared (bias math, teardown protocol, reserve/acquire/release/clear); VMM unmap on 3 arches; TCB lib lists + both drain paths; loader hook states/verbs.
- **Style re-surface:** §4 no raw user writes (HHDM-aliased writes only after walk + range check); §5 fail-closed taxonomy, ENSURE-free reachable paths; §7 test-first (doc-block + impl together); §11 IRQ safety (frozen-tick windows).

### #60 — VAR-17 SMP re-audit: atomic take
- **Learned:** (1) Single-core safety assessments invert under SMP: check-then-clear against a running AP is a fail-dangerous race — the fix is exchange-based take, NOT per-CPU flags (state for a nonexistent writer) and NOT a map_page lock (deadlock risk). (2) No `std::atomic` in this tree (`-nostdinc`) — `__atomic_*` builtins on plain bools; unbuildable planner proposals become the design. (3) Grep accessor users before removing ("unused" claims lie when tests use them) — keep + add alongside. (4) Pre-existing arch asymmetry (riscv sets no flags) → file it, don't scope-creep.
- **Adapted:** take_* accessors + ordering comments; take sites with race comments; take-semantics regression test.
- **Style re-surface:** §5 no ENSURE added without an impossible-invariant claim; fail-closed bool returns; test sanctity (leaves flags clean; unrelated tests untouched).

### #26 — TPR-based interrupt prioritization
- **Learned:** (1) The deferred-switch design RELIES on nested-tick apply for hlt-blocked syscalls — moving the timer vector without updating the `cmp` nested-apply guard in isr_stubs.asm strands the arm and stalls boot; any future timer-vector move must update the guard. Bisect constants via safe-class TIMEOUT + depth/arm ISR probes. (2) New code must use the shared mode-gated MSR path, never raw MSRs (planner addresses can be wrong — the helper exists). (3) Init paths must use `reg_wr` directly (fail-closed setters skip during init). (4) Placement tests asserting queue state race the tick — guard them all with IrqGuard (Rule 2). (5) pending_patch must never contain itself — regenerate after final staging.
- **Adapted:** APIC-TPR contract spec; nested-apply comment documenting the hlt-loop dependency; TprGuard IF=0 ENSUREs; affinity test guards.
- **Style re-surface:** §5 ENSURE for impossible-by-design (constructor asserts, IF=0 contract), reachable bad input returns false; §6 bounded loops, no loops in accessors; test sanctity (doc-block + impl together; strengthen, never weaken).

### #129 — Coverage closure: profiling
- **Learned:** (1) The sampling profiler is NOT coverage/debug-only — it runs unconditionally at boot and records from the timer ISR: production code on the interrupt path in every build. (2) Measure it inside IrqGuard from clear() (clear resets the sequence that makes the rate gate reproducible; the ISR contributes unknown samples otherwise). (3) The rate gate runs over an internal monotonic counter, deliberately NOT over ticks (the harness rewinds ticks and starved the sampler). (4) Ring wrap drops old samples but the running count is unclamped — only dumps clamp. (5) Symbol indices are deterministic (no boot callers → first range is index 0); ranges are half-open; malformed records reject. (6) debugcon-only output is unobservable via Terminal/klog — assert non-destruction only. (7) Restore boot state BEFORE the first assertion (JARVIS_ASSERT returns on failure).
- **Adapted:** sampler test class (rate math, wrap, indices, non-destructive dump, state restore).
- **Style re-surface:** §6 fully bounded loops (malformed records cannot run away); §5 fail closed (reject, never half-register); §11 IRQ safety (frozen measurement); §10.3/§10.5 init + named sentinels; test sanctity (state restoration asserted).

### #128 — Coverage closure: debug dumps
- **Learned:** (1) The test-time logger channel is NOT the UART (debugcon whenever test-active; Serial::write_count frozen) — the klog ring is the documented observation channel: clear → run → read, all in IrqGuard. (2) Per-arch ceiling: only the target arch's guarded definition compiles (10/12 is the max on one arch). (3) Fan-out makes post-mortem coverage cheap (four entry points call all six helpers). (4) Panic-path asserts: rendered (prefix + section markers) + degenerate inputs handled (unknown id → NOT FOUND, never null-deref) + side-effect free (task/count/corruption/IRQ/registers unchanged). (5) The unknown-id branch is the best-specified assertion (short + deterministic).
- **Adapted:** post-mortem dump smoke class; observation-channel caveat in the file header.
- **Style re-surface:** §7 doc-blocks (Runmode/Testidea/Input/Expect/Depends); §6 bounded reads; §11 frozen measurement; §10.5 named sentinels; test sanctity (state restoration, not just crash absence).

### #125 — Coverage closure: shell surface
- **Learned:** (1) Terminal::capture_begin/end is the ONLY sane shell observation channel (16-byte UART FIFO can't show whole outputs). (2) putchar emits pre-fb-gate (framebuffer off ≠ silent capture), but capture no-ops entirely when instance_ is null (no framebuffer at boot). Never nest `>` (redirection uses its own 4 KiB stack buffer — combining clobbers). (3) Classify drive-sweeps by side effect (destructive/blocking/interactive excluded; sleep only with 0 — the arg parser accumulates non-digits into multi-second hangs). (4) Mutating tests undo BEFORE the first assertion (umask/cd/rm/alias/trap; JARVIS_ASSERT returns). (5) Document deviations, never paper them (file the issue; QE reports, never patches production).
- **Adapted:** shell command + framework classes; defect filed as its own issue.
- **Style re-surface:** §7 doc-blocks (defect note lives in Expect); §7.1 self-cleanup; §6 never depend on input arriving; §5 mandatory error codes (mapped strings, never -1); new classes over editing non-stub tests.

### #124 — Coverage closure: vfs devices
- **Learned:** (1) Low coverage is a dead-code signal, not a count signal — files with no opener need their full op contract driven on real root vnodes; no production change required. (2) Blocking devices cover WITHOUT blocking loops (count==0 early return; O_NONBLOCK turns the next iteration into an error return) — a coverage test must never depend on input present. (3) Every lookup pairs with close (or ResourceTracker fails); no post-close deref (UAF); synthetic archives replaced by the boot archive BEFORE the first assertion (early return must not poison later tests). (4) Only / is mounted at boot — tests self-guarantee the mount set (allocation-free remount is Tracker-neutral). (5) get_err returns a pointer INTO the table — capture liveness at get time.
- **Adapted:** devfs/procfs/initrd_fs/vfs_errors classes driving full op contracts.
- **Style re-surface:** §7 test-first + doc-blocks; §7.1 single-owner (close exactly once, unlink before asserting); §6 no input-dependent tests; §5 specific codes, never -1; §10.7/§4 no const_cast, no RT allocation; non-stub tests are read-only.

### #10 — MSI-X Vector Infrastructure
- **Learned:** (1) Keep every kernel-reserved vector excluded in BOTH the allocator AND claim_slot (defense in depth — a hand-crafted vector never sourced from the allocator must still fail): vector 64 is the scheduler tick (an armed slot there swallows every tick = total starvation), 0xFF is spurious. (2) Single-owner must be per-(BDF,entry), not per-vector — one table entry programs exactly one vector; a second create would hijack the first owner's delivery. Bounded static claim registry + snapshot_reset. (3) Device MMIO is NOT PMM — HHDM-map only (floor→ceil so entries never straddle), and probe-once boot premap with cached KVA keeps test-time creates ResourceTracker-clean (test-time maps leak spuriously). (4) pci_find_capability on phantom BDFs loops forever (0xFF reads fake CAP_LIST) — reject non-existent devices first + bound the walk (§6). (5) Release-only boot regressions come from image growth shifting addresses (multiboot info overwritten) — reserve_range boot-critical blobs.
- **Adapted:** MsixCap + claim registry (masked-at-create, rollback, snapshot_reset); kind-gated delivery table (claim window minus reserved); VMM-mapped table programming + premap cache; hardened capability finder; PMM reserve_range; cspace/IRQ specs.
- **Style re-surface:** §11 timer-vector single-owner (never swallow the tick); §12.5 ownership on every state-changing op; §6 bounded loops, no raw phys punning; §5 fail closed on everything; §4 no RT allocation; preprocessor symmetry; test sanctity (boot-vs-test mapping placement).

### #9 — IOMMU DMA Protection Layer
- **Learned:** (1) UEFI-only El Torito ISOs need OVMF for q35 (SeaBIOS can't read them); FAT32 IDE is lost under q35 (SATA-only) — live-variant classes must be disk-free. intel-iommu attaches only to q35 (dynamic sysbus device). (2) Verify register maps against the ACTUAL emulation (or Linux header), not remembered spec — offsets, QI nibble encodings, and IQT/IQH >>4 semantics all differed from first reading. (3) The context-entry TT field is TWO bits (spec §9.3): single-bit treatment makes passthrough entries translate through NULL ASR — cap tests asserting ASR can't catch it; encode per spec. (4) ACPI tables live ABOVE the 128 MiB identity map — map every accessed table page on demand. (5) probe_hardware must CACHE: test isolation rewinds low memory, so re-probes dereference rewound tables.
- **Adapted:** minimal DMAR walker (on-demand mapping, fail-closed); register/descriptor layouts from QEMU/Linux; cached probe + fail-closed enable ordering; q35+OVMF+intel-iommu Makefile variant (disk-free, not in `all`); iommu phase-2 spec.
- **Style re-surface:** §10 register/bit fidelity against actual hardware headers; §5 fail closed on everything malformed; §12.5 never downgrade a translated entry; §11.6 cache what isolation rewinds; hardware-variant classes in the table + variant, never in `all`; preprocessor symmetry.

### #8 — Fine-Grained Hardware I/O Delegation
- **Learned:** (1) A DELTA on closed work needs both pieces (new surface + closure of the old gap), not a new abstraction; reuse the established registry pattern — never a new TCB field (the 4-memset-site sentinel gotcha). (2) Device BAR phys is NOT a PMM page: scope the user-page ENSURE to phys < total_memory (kernel RAM stays guarded, PCI hole maps). (3) Stored-PML4 registries MUST drain before the PML4 frees on EVERY path (exec AND cleanup-inside-if-before-free) — exec swaps without cleanup, cleanup frees before teardown. (4) Fork deep-copy must SKIP non-RAM leaf frames at ALL leaf sites (or the child inherits device access + user-triggerable #PF) — unmapped is fail-closed. (5) Pin registry-referenced capabilities per slot; release OUTSIDE the lock (last release runs dispose → free → re-entrant invalidate).
- **Adapted:** static bounded user-MMIO registry (per-slot pin, pml4-at-map, revalidate-at-clear, snapshot_reset); syscall surface with ledger-reserve-before-grant; VMM scoped ENSURE + non-RAM skip; IOPB ledger; drains on exec + cleanup paths.
- **Style re-surface:** §12 lifecycle (no slot dangles past PML4 teardown on any path); §12.5 owner+gen revalidation on every op; §11.6 exactly-once release; §5 fail closed on everything; §4 static registry + ledger; preprocessor symmetry; real-PML4 fixtures.

### #7 — User-Space IRQ Delivery (NOTIFY mode)
- **Learned:** (1) Modes bind atomically at arm under the slot lock — mode fields are ONLY valid inside that lock scope (same check-then-act TOCTOU discipline as owner revalidation). (2) Validate raw u64 syscall args BEFORE narrowing casts (0x101 wraps into a valid mode) — and probe the wrapping value in a test. (3) Notify::wait() returns immediately (switch applies next tick) — workers spin on BLOCKED then RE-READ via try_wait (the return may be stale). (4) Capture wake-target fields BEFORE clearing them (clearing first nulls the target → waiter BLOCKED forever). (5) Never notify() a destroyed recipient (cleanup destroys Notify before drain runs) — the wakers-own-wakeup contract ends at TERMINATED. (6) Revoked sentinel is 0: it wakes BLOCKED waiters with a defined value.
- **Adapted:** IrqDeliveryMode + NOTIFY branches (arm/isr/release/drain, capture-before-clear); raw arg validation; wait-refuses-NOTIFY in both lock scopes; 0x101 wrap + spin/re-read fixtures; NOTIFY spec paragraph.
- **Style re-surface:** §5 fail closed on reachable bad input, never ENSURE; §11 (slot-lock-only mode access; no spinlock across reschedule; §11.3-shaped blocking); §12.3 wakers own wakeup (release/drain wake, never on destroyed objects); §12.1 dead-state + generation guards; §11.6 capture-before-clear of ISR-shared fields; validate before narrowing.

### #103 — aarch64 deep_copy_user_pages descriptors
- **Learned:** (1) Descriptor encodings differ by LEVEL: L1/L2 blocks are 01, L3 pages are 11, and TABLE pointers are ALSO 11 — x86 idioms collapse to reserved/invalid on aarch64; arch-guard table-pointer sites to PRESENT|TABLE, x86 arm byte-identical. (2) AF/AttrIndx are RES0 in TABLE descriptors — minimal form is PRESENT|TABLE; leaves inherit verbatim. (3) Mid-test asserts skip teardown → LEAK lines are a symptom (assertion too late), not a real leak — put teardown-correctness checks in completing states. (4) Leaf extraction needs the arch 48-bit frame mask (UXN/PXN leak into ~0xFFF). (5) Arch-guard userspace probes (`#if defined(__x86_64__)`); cross-arch builds are the regression net. (6) Out-of-scope smoke requirements become follow-up issues, not ballooned scope.
- **Adapted:** arch-guarded table-descriptor sites; descriptor-validity regression gate (encoding + mask + symmetric free); guarded probe sources.
- **Style re-surface:** guard placement over whole contiguous statements; §5 ENSURE only for impossible invariants (positive gates); test sanctity (pre-fix-failing positive gate, symmetric teardown); minimal diff.

### #91 — ISR_NOERR misclassification
- **Learned:** (1) Per-vector error-code classification is correctness, not style: a CPU-pushed code consumed as RAX + misaligned iretq = triple-fault-class on hosts that raise those vectors. Pin BOTH stub classes to the same frame with assemble-time audits AND gate the classification mask in `all` (frame audits can't catch silent swaps). (2) Reserved-vector fail-stop is a deliberate, auditor-flagged shift (safe: no normal hardware raises them; recovery still wins inside its window). (3) Weak-hook seams give deterministic dispatch probes with exact production parity when unarmed (latch confined to IrqGuard scopes). (4) Compilers fold fault probes (`1/x` → compare, no division) — real probes need inline asm. (5) TCOUNT mismatch is a defect, not noise.
- **Adapted:** ISR_ERR reclassification + frame audits + exported err mask; weak dispatch probe + reserved branch; real-fault probes (inline asm); mask gate in `all`; exception-table audit spec.
- **Style re-surface:** x86-only guards over whole blocks; §5 ENSURE only for impossible invariants (assemble-time, not runtime); weak-symbol discipline (byte-identical unarmed); test sanctity (real faults, exact-set gates); minimal diff.

### #100 — aarch64 runtime bring-up
- **Learned:** (1) Syscall-ABI mismatches are invisible in a "working" boot (every call misrouted, daemons fault-free but nothing delivered) — one short ABI trace at handler entry proves it; anchor the ABI in a comment at the single source of truth. (2) Frame validation needs an arch branch everywhere dispatch guards exist, or candidates get dequeued-then-discarded (stranded READY+inrq=0). (3) Kernel-task frames must resume preemptible (aarch64 I=0, i.e. SPSR 0x345 not 0x3C5) — masked resume spins forever on voluntary reschedules. Symptom: READY + dequeued + block-loop spin while the peer never yields. (4) Dereference page tables ONLY through the HHDM alias (raw phys resolves through the active TTBR0). (5) `& ~0xFFF` is NOT a phys mask on aarch64 — 48-bit DESC masks (UXN/PXN leak; unmasked free silently no-ops = leak).
- **Adapted:** x8 dispatch + ABI comment; ELR/SPSR arch branch; SPSR fix + rationale; HHDM aliases + DESC masks + guarded reclaim + TLB flush; per-arch ECAM default; relocated boot tables; PL031 RTC; init_stack by-ref; walk/GIC/DTB/RTC tests.
- **Style re-surface:** §6.1 no arch leakage across builds; §5 ENSURE only for impossible invariants; §10 const/reference discipline; §12 lifecycle (stranded READY is a lifecycle defect); guard placement (whole groups, byte-identical others); test sanctity (prove systemic wrongness first; doc-blocks together).

### #4 — IOMMU DMA protection (tables)
- **Learned:** (1) Walks that LINK fresh pages must UNWIND them on later failure (linked_entry stack, unlink-then-free deepest-first) — otherwise orphaned empties leak permanently, unreachable by rollback or destroy. (2) Untyped integer fields × fixed encodings escape the type system — validate device<32/function<8 at EVERY indexing site. (3) Full-gate runs are a shared environment (higher-prio boot tasks legitimately win picks) — assert INVARIANTS (>= enqueued prio), never exclusive equality; run in isolation AND in `all`. (4) Hard-coded resource budgets rot — derive bounds from runtime link symbols + slack. (5) Zero-rights lookup passes anything — re-read per-slot rights from the EXACT addressed slot (occupied + identity + gen) under the pin.
- **Adapted:** VT-d layouts single source of truth; bounded domain/root/context tables with unwind/cascade-free/zeroed pages/guards/leaf lock; capability type + revoke-destroys-first; map/unmap syscalls; snapshot_reset; iommu design spec.
- **Style re-surface:** §4 no RT allocation (static tables + zeroed PMM pages); §5 ENSURE only for impossible invariants (exhaustion/validation return); §12.5 ownership on EVERY state-changing op; §12.x single-owner proven disjoint paths; fail-closed default-deny; test sanctity (change non-stub tests only after proving systemic wrongness).

### #2 — IRQ caps + user-space IRQ delivery
- **Learned:** (1) Shared (grantable) caps survive arming-task death and drained slots get REUSED — every stored-index path must revalidate occupied + owner + vector under the slot lock, and check-then-act must be ONE atomic critical section (fold revalidation into arm/release). (2) Kernel blocked-wait = Semaphore::wait shape (register once → dequeue_ready + reschedule → pause-spin on BLOCKED) — never user sti/hlt/cli (IF stays 0 after wake = never descheduled); never re-registering tight loops (lock-held spin livelocks lower wakers). (3) isr_entry gates on armed + vector match (fall-through, single tail EOI) or unarmed claims swallow IRQ lines. (4) PIC restore is stateful — capture the mask at arm, restore exactly. (5) Harness spin-waits re-arm need_resched + hlt (wait_for_termination_safe), not bare pause.
- **Adapted:** IrqCap + owner-tagged delivery table (atomic arm/release, armed-gated isr_entry, drain in cleanup, stateful restore); register/wait syscalls; handler hook; cap_irq class; cspace/IRQ specs.
- **Style re-surface:** §12.5 ownership on every op (auditor caught the split); §11.6 all-or-nothing per index; §12.3 wakers own wakeup (never BLOCKED forever); §11.2 BLOCKED dequeued, lock released first; §5.1 ENSURE only for impossible invariants; deterministic harness waits.

### #1 — Untyped child-split + sub-range carve
- **Learned:** (1) Exhaustion model: the guard is "spent", not "retype-once" — carve transfers [0,carve) to target, [carve,size) to a NEW child; enumerate EVERY post-guard failure path with exact rollback shapes (claim/stretch/install/remove ordering; creator ref ≠ slot ref — a forgotten release leaks block AND pages). (2) Install children with rights|WRITE (caller already held WRITE over the parent — no new authority; without it children are non-retypable). (3) Non-destructive capacity pre-check keeps the parent intact on table-full. (4) First-match slot scans must exclude spent parents at deeper splits. (5) Reject garbage target types explicitly.
- **Adapted:** create_subrange + exhaustion-model retype + shared live bound; retype doc; CAP_RETYPE syscall; doubled test class; cspace exhaustion-model spec.
- **Style re-surface:** §4 MemPool-only, bounded; §5 ENSURE only for impossible invariants; §12.x single-owner/double-free re-audit of every post-claim path; test sanctity (doc-block + impl together on contract change); unique sentinels.

### #3 — MMIO caps + I/O delegation
- **Learned:** (1) THE most dangerous kernel pattern for new per-task fields: create/create_user/clone AND the ELF loader memset the TCB and NEVER run the ctor — a non-zero-default field is silently 0 in every real TCB (made the IOPB permissive = privilege escalation). Set sentinels explicitly after EVERY memset site AND add a mechanical defense that never trusts the value. (2) The TSS I/O bitmap must live INSIDE the TSS segment; default-deny is all-1s + 0xFF terminator (all-zeros = all allowed). (3) Self-grants apply immediately in the handler (not next switch). (4) Ring-3 #GP is untestable from the ring-0 harness — assert bitmap state + layout + bookkeeping instead. (5) Release-only analyzer false positives go on the Makefile exception list. (6) PLANNED vs EXECUTED differ by flag filtering — update counts from the TCOUNT line, not the summary.
- **Adapted:** MmioCap + map/unmap; IOPORT_GRANT syscall; arch IOPB pool + TSSBlock; memset-sentinel fixes at all sites + claimed-slot defense; analyzer exception; cap_mmio class; cspace/config specs.
- **Style re-surface:** §4 no RT allocation (static pool); default-deny as mechanically-enforced baseline; fail-closed exhaustion; test sanctity (state-level assertions where faults are unreachable); document the memset-not-ctor rule wherever a non-zero-default field is added.

### #5 — aarch64 PAN/PXN enablement
- **Learned:** (1) QEMU reports PAN==0, so enablement must be runtime-gated (supported-flag + degraded no-op), never a compile-time assumption, or the build silently ships stubs on the only bootable platform. (2) SCTLR_EL1.PAN is bit 23; PSTATE.PAN is bit 22 of its own sysreg — verify bit numbers, don't trust the roadmap. (3) aarch64 L3 with bits[1:0]=01 is reserved (pages are 11) — a latent boot blocker reachable only once 4K mapping actually runs. (4) Mirror new state without changing consumers (normalize read_rflags to the x86 bit-18 contract; exception entry restores PSTATE so windows survive IRQs). (5) aarch64 boots but PMM OOMs (DTB regions never seeded) — track missing preconditions as bugs, not asserts.
- **Adapted:** SCTLR.PAN detection + init; PAN-sysreg accessors; PXN/UXN closure; L3 fix; arch walk/register tests.
- **Style re-surface:** arch-guard placement (other arches byte-identical); test sanctity (register-level tests only where no fault path exists); ENSURE only for impossible invariants.

### #6 — Pending audits/refactorings
- **Learned:** (1) The reusable kernel-blocking pattern: release the primitive lock BEFORE reschedule/dequeue_ready — holding a spinlock across the deferred switch deadlocks the ISR-side waker; every blocking path mirrors Queue/Notify (scoped waiter-insert, dequeue + reschedule outside, interrupts-disabled rollback). (2) Generation-tag bare TCB pointers (check REAPED + generation; TERMINATED zombies stay valid for PIP). (3) The custom Logger reads ALL integers as uint64_t and mishandles width variants — `%lx` + u64 casts, always. (4) The LSTAR/sysret path was a guaranteed-panic landmine (unwritten GS base) — int $0x80 is the only safe entry. (5) PI restore must use strict `>` or the latch never clears (permanent inflation). (6) Embedded dtors never run under MemPool::free — dispose paths must drain explicitly. (7) `%u` on 64-bit ids leaves garbage high bits.
- **Adapted:** 8 approved phase commits (lock-scopes + dequeue; gen-tags + PIP re-bucket; pool-aware destroy; alloc contracts + frame validation; buffer-pool ownership; dispose drains; driver serialization; LSTAR removal; retry + orphan-drop + SPSC atomics); immutable per-phase reports.
- **Style re-surface:** §5 ENSURE only for impossible invariants (exhaustion → error/retry); §4 no new/delete on RT paths; no spinlock across switches; IrqGuard only for boot/panic/isolation; test sanctity (doc-block + impl together).

### #99 — Multi-arch compile-clean
- **Learned:** (1) Arch-switch clean belongs at PARSE TIME, before `-include $(DEPFILES)` — mid-build clean is too late (stale `.d` says "up to date", object never rebuilds). (2) Legacy generator truncation bugs drop trailing tests silently — only defined-vs-referenced cross-checks find them; dump-counts are the truth. (3) AArch64 ADR can't reach low-VMA symbols from higher-half text — linker absolute symbols (HHDM + low VMA). (4) riscv asm uses `mv`, aarch64 `mov %0, sp`.
- **Adapted:** parse-time arch-stamp check + per-arch gen rules; arch-aware registry stripping; absolute stack symbols; arch guards.
- **Style re-surface:** guard placement (whole groups, byte-identical others); minimal diff; Werror-clean.

### v0.4.2 — Release cycle
- **Learned:** (1) The release gate is debug+release `all` (trace OFF), NOT a clean ISO build — run both gates + history rows. (2) For merges, check the tree delta (`git diff`), not commit count. (3) Release touches exactly: version.hpp (strip -dev), Doxyfile number, README/readme.html, ROADMAP pair, project_structure.txt, regenerated docs. (4) Close the milestone via API after the tag push; re-home leftover trackers. (5) Post-release bump re-adds -dev.
- **Adapted:** release + tag + mirror + bump commits; ROADMAP_done entry; next-milestone ROADMAP.
- **Style re-surface:** version.hpp single source of truth (stage consistent with version string); gates BEFORE bump; trace-OFF verification.

### #101 — Hard RT measurement under QEMU
- **Learned:** (1) Make `$(if …)` SPLITS args on commas — pre-expand comma-bearing flag strings into variables first. (2) QEMU icount is INCOMPATIBLE with the APIC TSC-deadline path (virtual-clock read from non-IO TB) — stays opt-in for fixed QEMU versions. (3) TCG rdtsc advances in COARSE QUANTA — asserts need "≥1 non-zero + magnitude sanity" canaries, never every-sample-nonzero. (4) Bounds must be calibration-derived with headroom (base×8, pathological wedge cap) — never illustrative constants. (5) Fixed-size test registries SILENTLY DROP past capacity (gate still passes) — bump on growth.
- **Adapted:** opt-in icount/trace flags (comma-safe) + trace events file; hard-bound stress class (canary + under-stress bound + zeroed failure counters); registry capacity bumps.
- **Style re-surface:** §11.6 atomics for cross-task flags; §11.1 no spinlock across reschedule/send_sync; §4 no alloc on measured paths; resource discipline (cleanup before asserts, safe joins + drains); relative-over-absolute timing asserts.

### #102 — Scheduler tests with hard-RT assertions
- **Learned:** (1) A READY-spinning high-prio receiver STARVES all lower tasks — latency measurers must GENUINELY block (Semaphore::wait = BLOCKED + dequeue_ready) and sit ABOVE the harness. (2) send_sync roundtrip measures the real deferred-switch wake path. (3) New .cpp files need gen.mk regen (`rm -f` the stale gen file). (4) Registry capacity drops silently — measure TCOUNT with generous capacity, never assume. (5) Latency prio placement: measurer > harness AND woken > measurer; hammerer stays at the bottom.
- **Adapted:** hard-RT scheduler class (canary, IPC wake, semaphore wake, jitter — relative bounds, zeroed fail counters).
- **Style re-surface:** §11.2 BLOCKED dequeued; §11.3 bounded waits (genuine block, never spin); §11.6 cross-task atomics; §4 no measured-path alloc; relative timing asserts; audit TCOUNT, not just PASS/FAIL.

### #92 — Syscall tiered FAST/FULL dispatch
- **Learned:** (1) Constexpr static initializers cannot reference later-declared member functions — namespace-scope free builder functions declared before the class. (2) The issue's FAST list was unimplementable (user-buffer deref = privesc; nonexistent calls) — the audited set is pointer-free syscalls only; verify stated mechanisms against the live path. (3) Relocating checks into hooks creates EXEC-WINDOW false-trips (page-table swap between verify and use) — clear state pre-swap; hot-path hooks get audited against every page_table_ transition. (4) PAUSE/HALT/REBOOT are uncallable in tests (halt the CPU) — verify membership by mask, skip by call. (5) The live entry is `int $0x80` → C — paper asm claims land in C, not dead asm files.
- **Adapted:** tiered dispatch (fast table + mask + full-path canary hooks); exec pre-swap clear; fastpath test class; reconciled spec.
- **Style re-surface:** §6 debug/release symmetry (no #ifdef control-flow divergence); §10.6/§10.7 const correctness, no const_cast; hooks audited against ALL page_table_ transitions; relative latency asserts; registry headroom.

### #93 — FPU/SIMD context hardening
- **Learned:** (1) A latent S1 hid in the #NM handler: in the ARMED-SWITCH window prev == current, so unconditional restore clobbers LIVE registers with stale state — when prev==current, return without restore (FPU analogue of the exec-window finding: hot-path handlers get audited against every deferred-state window). (2) Snapshots reset fpu_owner between tests — arm CR0.TS BEFORE the first FPU op or the test silently exercises the wrong (init) path. (3) Cross-arch compile-clean applies to NEW test files too — guard test bodies + registrations per arch. (4) `-mgeneral-regs-only` rejects `"st"` clobbers — pure memory-operand x87 asm. (5) No `offsetof` in freestanding — `__builtin_offsetof`. (6) #NM enters with IF cleared + regs-only kernel ⇒ non-interruptible swap is hardware-guaranteed; TS-on-switch-away at both publish sites (verified, not assumed).
- **Adapted:** #NM stale-restore guard + depth max; aligned FPU state + generation; clone gen handling; snapshot reset; x86_64-guarded invariants class.
- **Style re-surface:** §6 symmetry (unconditional counters); §4 no #NM-path allocation; §11.2/§12.x deferred windows audited, owner transitions under critical sections; cross-arch guards; memory-operand-only asm.

### #106 — Zero-copy cap shared-memory rings
- **Learned:** (1) Verify CODE against SPEC before assuming a gap — the spec can be the only thing wrong. (2) A kernel task NEVER runs in its own page_table_ (CR3 loads for USER only) — verify mappings via table walks + HHDM phys reads, never VA deref; or run a real user task. (3) Mirror the established registry pattern EXACTLY (claim + acquire under lock, map outside, only the true→false observer releases — double-release is premature dispose → UAF). (4) Map/unmap must be symmetric over count — partial teardown leaves later-freed frames user-mapped (classic asymmetric defect). (5) Release-after-remove is UAF (remove's 1→0 disposes + frees; trailing release touches a freed block). (6) The style checker flags the word "new" in comments — reword. (7) Raw slot indices are NOT cap handles (need encoded cspace/slot/gen).
- **Adapted:** priority-ordered wake spec correction + new SHM design spec; frame registry with multi-page teardown; revoke/dispose closure; map/unmap syscalls; drain hooks; SHM test class.
- **Style re-surface:** §4 no dynamic allocation (static registry + fixed window; mind checker keywords); §12.x single-owner + exactly-once release; symmetric map/unmap; capability = sole authority (lookup + user + revoked); zero-delta tests; non-vacuous asserts (real PML4 + signals).

### #105 Part B — Crash Supervisor notifications
- **Learned:** (1) Post-drain TCB pointers are UAF (reaper frees + poisons) — capture ids BEFORE termination; a test bug that looks like a registry bug. (2) Priority direction matters for fixtures: parked helpers ABOVE the harness starve it when scheduled; helpers that only exist stay LOW + aperiodic. (3) Consume-then-validate loses data on bad pointers — validate user destinations BEFORE consuming. (4) New ResourceTracker fields must join any_leak() or the invariant is unenforced (decorative counter passes while leaking). (5) ISR-reachable lock paths falsify "never in IRQ" doc claims — state the real order. (6) `rg -rn` is the replace flag, never recursive. (7) First-run gate hangs can be QEMU/watchdog flakes — re-verify before bisecting.
- **Adapted:** death-notify design spec (ABI, records, protocol, race policy); validate-before-consume syscalls; cleanup hook; Tracker parity; death test class (pre-death ids, low helpers, drained leftovers).
- **Style re-surface:** §4 static bounded registry, fail-closed; §12.x exactly-once release, drain-before-destroy, gen-tagged pokes; non-blocking supervisor contract; validate-before-consume; honest invariants (any_leak parity); non-vacuous tests.

### #107 — External Pager Protocol
- **Learned:** (1) ISR-acquired locks MUST be try_lock + fail closed (task-context holders can never be rescheduled while the ISR spins = permanent deadlock). (2) Fault-completed cap pins persist in a COMMITTED table until death/revoke/unregister — releasing at consumption lets the client free the pager's frames (ownership violation, double-free). (3) Cross-address-space maps must be ATOMIC against client death (IrqGuard: no tick → no switch → no free mid-map). (4) Validate count ≤ cap count and is_user (oversized maps expose adjacent frames; kernel frames never user-mappable). (5) Size collect-under-lock arrays at the exact worst case (off-by-one = stack overflow S1). (6) invalidate_cap removes ALL matches per slot (swap-with-last + re-examine), never first-only. (7) Ring-3 faults need a real user probe ELF (kernel tasks can't fault in ring 3). (8) Installed caps tear down via slot-remove, never bare release after install (double-release UAF). (9) Accepted baselines stay baselines (harness CNode leak identical across classes — not your leak). (10) Registry-only helpers stay low-priority + aperiodic.
- **Adapted:** bounded-pager design paper (contract, classification, block-in-ISR, fail-closed + kept registration + poison latch, retry-first wake, priority invariant); pager registry + syscalls; #PF hook; drain ordering; invalidate closure; tick watchdog; Tracker counters; user probe ELF; pager test class.
- **Style re-surface:** §4 static bounded registry, fail-closed; §12.x exactly-once pins, drain-before-free, gen-tagged pokes, cross-AS atomicity; validate-before-consume; ISR-non-blocking discipline; honest non-vacuous tests (real probe + PTE proof); Tracker parity.

### #11 — In-Register IPC Fastpath
- **Learned:** (1) Integer bitmasks OVERFLOW past their width (u64 dies at syscall 74+) — widen to __uint128_t and re-check width on every numbering expansion. (2) Verify stated mechanisms against the live path (the "skipped context saves" had no counterpart — the real win is killing the user-buffer round trip + canary walk). (3) Missing peeks get built as single-lock select+check+remove (find_best_index/remove_at shared) — never peek-then-pop (TOCTOU). (4) Extended-asm has no r8–r11 letters — `register … asm("r8")` + generic constraints (`"S"`/`"D"` are valid). (5) Clamp params default to byte-identical behavior; oversized replies stay queued (fail-closed). (6) Blocking handlers mirror the sys_receive block body EXACTLY (WEDGE/dequeue/deadline/sporadic/user-only-hlt) plus wake_sender parity — omitted wakes strand senders.
- **Adapted:** fastpath design paper (budget, syscalls, clamped pop, invariants); widened mask + tables; clamped queue ops; fast handlers; libc wrappers; fastpath test classes.
- **Style re-surface:** validate-at-boundary-once; pointer-free FAST review; single-source constexpr tables; fail-closed oversized (never consume); verbatim block-body duplication; WEDGE block discipline; arch-gated register maps.

### v0.4.3 Test-Coverage Completion
- **Learned:** (1) Private APIs govern test design: live-CPU contracts are reachable (sgdt shows mutated accessed/busy bits — not bugs; i8042 0xD2 injects for real). (2) JARVIS_TEST_PASS() does NOT return — skip guards need explicit return. (3) Test tasks run on a PRIVATE kernel-half PML4 — map/verify/clear on `current_pml4()`, never the boot table. (4) Boot-only derefs (multiboot tags) fault from tests — re-establish identity mapping first. (5) Sub-tick polls make environment-dependent paths unreachable — assert deterministic device-side contracts instead; handshake semaphores get init()ed per test (uninitialized counts eat wakes). (6) sizeof can exceed the largest MemPool class (fail-silent nullptr probes) — precondition-guard allocations.
- **Adapted:** 12 new test classes + counts + MAX_TESTS + QEMU variants; defect-gate guards; filed follow-up issues for found defects.
- **Style re-surface:** §5.1 fail-silent probes need error discipline; §11.3 wake contracts + lost-wakeup diagnosis; §12.x single-owner close semantics; §7 doc-blocks on contract change; honest non-vacuous assertions; history row per run.

### #121 + #122 — Dynamic coverage + profiler fixes
- **Learned:** (1) `-finstrument-functions` reads the caller return address — synthetically-entered functions (context-switch trampoline: no return address above RSP) fault immediately; `no_instrument_function` on such entries (prove with objdump: hook prologue before the fault). (2) Instrumented frames run deeper — raise stack tiers, never the kslot window (bounded mapping can't grow; raising it silently removes fail-closed behavior). (3) Shutdown-time dumps run BEFORE the ACPI exit write (post-exit code is unreachable). (4) Instrumentation + LTO is unusable for coverage; `-g` is mandatory for attribution; per-class relinks shift addresses — union captures by (file, function), never address. (5) Shared capture streams need framing + length + checksum + explicit abort (truncation must never merge silently as "no coverage"). (6) Gate samplers on private monotonic counters — never on counters another subsystem rewinds (harness-rewound ticks starved the profiler to a single sample). (7) One debugcon port per class run (bind exactly one chardev).
- **Adapted:** bounded gcov handler (fail-closed dropped counter, framed dump, per-variant sizing); pre-exit dump placement; trampoline attribute; CONFIG-gated stack tiers + snapshot bound; monotonic sampler counter; per-class coverage targets; coverage design spec.
- **Style re-surface:** §6 fully bounded loops (bounded serial polls, no `while(true)`); §11.6 atomics for ISR-observed counters; §4 static bounded tables, fail-closed overflow (never index 0); §10.6/§5 no dead API; evidence-driven (objdump/counts, never assumed).

### #148 — Threaded-IRQ dispatch wedge
- **Learned:** (1) Audit primitives against their own contract: Notify violated §11.2 three ways (BLOCKED without dequeue, stale return without deschedule, re-entry without reschedule) while siblings complied — the outlier spins wait→handler→wait forever, starving the harness. (2) Fixing one defect deterministically breaks the next masked one (stale-0 return hid a never-delivered kernel write) — evidence-led diagnosis, NOT revert: the regression is a second real defect. (3) Stale-doc discipline: comments documenting primitive internals become defects when the primitive changes — update them in the same diff. (4) Cross-task test flags are atomic on every side.
- **Adapted:** Semaphore-shaped wait (fast-path + dequeue + reschedule + BLOCKED-spin/rollback with self+gen match); kernel-path direct store (null-guarded, CheckedPtr kept for user); reactivated parked e2e (bounded spins, destroy-before-asserts); comment updates without assert changes.
- **Style re-surface:** §11.1–11.3 (lock released first; BLOCKED dequeued; bounded/scheduler-mediated waits); §11.6 atomics all-or-nothing; §12.1/§12.2 filters + gen tags verified; §7 regressions never dismissed, asserts never weakened.

### #25 Phase B — SMP AP bring-up
- **Learned:** (1) SIPI vector V starts the AP at linear V<<12 (vector 0x70 → 0x70000, never 0x7000) — off-by-16x, caught by GDB. (2) 16-bit `jmp far` carries a 16-bit offset — past 64 KiB needs o32; without it the AP lands in IVT/BDA and runs wild (racy resets masquerading as BSP death). (3) 16-bit `lgdt [sym]` truncates base to 24 bits and disp to 16 — above 64 KiB needs a32. (4) Protected mode ENFORCES segment limits — flat 4 GiB descriptors required in trampoline GDTs. (5) IA32_APIC_BASE is PER-CPU: the AP leaves INIT in reset state — init_ap mirrors the BSP mode (MSR writes with x2 disabled #GP; no IDT on AP). (6) PMM allocates low-first — reserve the boot block right after PMM::init; gate copies on the reservation. (7) CPUs write back GDT Accessed bits — mask bit 0 in staged-blob memcmps. (8) Instrumentation can clobber live state (marker `mov al` killed a jump target) — spill live regs around probes. (9) QEMU `file:` serial drops bytes on triple-fault exit — serial variance ≠ code variance; GDB is ground truth. (10) Never map low pages in the active PML4 from tests (per-test accounting leaks) — HHDM alias for reads; boot-cached snapshots, never firmware re-walks.
- **Adapted:** MADT scan; APIC send_ipi/init_ap/enable_local; trampoline (org, self GDT, param block, fixed offsets); bring-up + park + boot-cache modules; CONFIG_MAX_CPUS 8; smp_bringup QEMU flags + blob rules; bring-up test classes.
- **Style re-surface:** §7 hypothesis-first with GDB evidence (disprove before fixing); fail-closed at every gate; no TEST_OUTPUT guessing.

### #150 — Release-only stringop-overflow in test sweep
- **Learned:** (1) Sweep widths must cover max count across callers — a 1-element buffer with 8-wide traffic is a REAL latent stack overwrite that only release `-O2 -fanalyzer` catches; debug never sees it. (2) Exonerate at HEAD with the EXACT release flags + include-set analysis + pristine-tree reproduction before blaming nearby work. (3) `&array` is `T(*)[N]`, not `T*` — `&array[0]` at pointer sites.
- **Adapted:** max-width storage + correct pointer sites.
- **Style re-surface:** fail-closed + evidence-before-synthesis (HEAD reproduction before attribution).

### #25 Phase A — Per-CPU cutover via linker aliases
- **Learned:** (1) Zero-C++-change cutovers exist: linker absolute symbols at per_cpu[0]+offsets keep every decl/test/reader untouched (nm-verified); one owner per arch, no shadow copies. (2) Ground-truth beats plan assumptions (dead code gets deleted, not converged; plans adapt on reader evidence). (3) Flag-set switches need make clean + gen.mk regen (LTO/NO_LTO slim+raw-ld mixing). (4) `%x` truncates to 32 bits in GDB printf — `%llx` for addresses.
- **Adapted:** linker aliases + sync comment; asm access-mode sites; arch guards; percpu init + lapic fill; alias-identity tests; asm checker rule.
- **Style re-surface:** §7 evidence before synthesis (nm + live tests, not assumptions); single-ownership per arch; fail-closed init ordering (GS_BASE before any ISR).

### #149 — safe_copy fault recovery vs GCC
- **Learned:** (1) ROOT CAUSE was codegen, not scheduler: GCC deletes basic blocks unreachable by control flow even when address-taken (`&&label`) — disprove the scheduler theory by GDB FIRST (per protocol, no code touched until evidence redirected). Discriminator: only recover blocks rejoining hot code survive. (2) Fix shape: opaque conditional goto through a volatile-asm value the compiler cannot fold (page-aligned low bits — always zero, fail-closed on spurious); end-of-function placement gets DCE'd (edge must originate from reachable code); gotos must not cross non-vacuous inits (compiler-enforced; hoist or place after last decl). (3) GDB batch hygiene: no every-IRQ breakpoints (stub-link saturation freezes the guest) — run free, then arm observers; `%llx` for addresses; breakpoint iretq from objdump instead of stepi across it.
- **Adapted:** fault-recovery KEEP macro + call sites (KEEP-before-clac, pure-cast hoists, split inits); reactivated strict live tests (kernel-task + Ring-3 e2e).
- **Style re-surface:** §11 no scope change (never-taken edge); §4 no allocation, fail-closed; §7 hypothesis-first with revert discipline; §10.6 descriptive names through restructures.

### #142 + #143 + #144 — Coverage denominator, memory adoption, teardown
- **Learned:** (1) PRODUCTION BUG via SMAP double-write: successful user copies followed by a supervisor-only rewrite panic every successful call (kernel-task tests never see supervisor-only faults) — restructure to if/else; only REAL Ring-3 fixtures find it. (2) Hand-crafted Ring-3 stubs: return slots INSIDE the mapped page; syscall returns are 64-bit (32-bit stores pass vacuous ==0 asserts). (3) Vacuous-pass guard: exit_code==0 is fixture contract (fault-deaths read back as plausible zeros). (4) sys_exit self-termination queues no zombie — teardown terminates explicitly (terminate-iff) + drain or IDs leak and collide. (5) MemPool Pool ctors leave bitmaps uninitialized (BSS-zero hides it in production) — zero stack Pools explicitly; free exactly once; never drive reserve/init_err in tests (permanent / live-table re-init). (6) Flag-set switches need `make clean` (LTO/NO_LTO silent mixing); `make clean` wipes coverage captures — finish sweeps first; first capture after a normal run can boot a stale initrd (verify S: tags).
- **Adapted:** SMAP if/else fixes; Ring-3 probe tests (positional reasoning, 64-bit returns, exit-code contract); VMM/PMM/MemPool error tests; parked stubs with pseudocode + refs; coverage denominator collapse; cherry-picked teardown destroy(); retrieval gates in AGENTS + audit prompt.
- **Style re-surface:** §5 fail-closed user writes, ENSURE only for impossible invariants; §4 no RT allocation (static fixture pages); §6 bounded joins/spins; §7 cleanup-before-assert, doc-blocks on contract change, non-vacuous assertions; §10.6 descriptive names; parked stubs carry refs.

### #21 — Per-task exec-ns metering + SYS_TIMES
- **Learned:** (1) Bill-out at switch-out + tick, stamp-in at switch-in; zero-stamp means stamp-without-billing (never bills creation-to-dispatch) — survives switch machinery with no new lock order (lock-free clock already called from on_tick). (2) Snapshots may intentionally OMIT monotonic clocks (untouched stamp+totals stay mutually consistent = correct lifetime semantics). (3) New-syscall checklist: enum + MAX bump + decl + table entry (never FAST with user buffers) + fuzz auto-covers the range (null-buffer fail-closed asserted free). (4) Per-class fixtures don't share anonymous namespaces (fixed VAs are safe: private PML4s, sequential). (5) Generated per-file rules regen only on Makefile/rules mtime — touch Makefile after adding a test TU. (6) Stamp history rows with `date` output, never hardcoded estimates.
- **Adapted:** exec counters + charge/stamp/read + TIMES syscall; metering class; counts.
- **Style re-surface:** init-order (decl == init-list, -Werror=reorder); TCB memset-zero rule; DRIVEN tests (no fake ticks/field mutation); lower-bound ns asserts (no wall-clock exactness).

### #19 — Deadline-aware preemptive scheduling
- **Learned:** (1) Serial-tick decoupling (TSC time vs on_tick count) + a test-gated kernel trace proved the true fault in 2 runs after ~15 wrong theories — decouple clock domains when hunting hangs. (2) Cap kernel traces + gate on test-set flags (ID reuse misattributes lines); always verify which regime a line belongs to. (3) ROUTING INVARIANT: every enqueue path must route and every reset path must clear BOTH queues — non-empty-head rebuilds double-link into ISR-hanging cycles. (4) Policy changes break choreography tests by phase-dependence/starvation — pin FIXED with doc updates; queue predicates go queue-agnostic. (5) Starvation is CORRECT EDF (forever-earliest hogs); admission owns the guarantee — mechanism now, guarantee next.
- **Adapted:** task policy/deadline links + per-CPU deadline lists + candidate-shared dispatch + DM band + exemptions + snapshot carry + policy setters; EDF test class.
- **Style re-surface:** init-order; TCB memset rule (+ clone no-inherit); DRIVEN tests; doc-block updates with every contract change; bounded spins with watchdog backstop.

### #20 — Enforced admission control
- **Learned:** (1) DEFAULT-FLIP SHRAPNEL: flipping a budget default ON exposes latent fail-closed inversions (zero-budget gates deny EVERYTHING incl. boot) — evidence-chain the OOM to the missing carve-out and fix at ALL gates, not callers. (2) Setup tasks that self-terminate vanish mid-setup — admitted fixtures block on a gate semaphore; terminate+drain at teardown. (3) Admission is create-time ONLY (never wakeup — denying wakes wedges IPC waiters); void add_task stays advisory, the _err variant enforces. (4) Pre-existing raciness that blocks YOUR gate is in scope: conditionalize flaky asserts on actual exhaustion + switch to sanctioned teardown (manual sequences UAF on reaped blocks). (5) TCOUNT actual vs executed vs row diverge — the row tracks TCOUNT actual; release gen.mk needs its own regen.
- **Adapted:** WCET/admission checkers (exempt-before-WCET) + enforced add_task_err + EDF routing fix + deny codes; PMM zero-budget carve-outs; guarded boot budget sizing; fork-via-enforced with destroy-on-deny; conditional racy asserts + sanctioned teardown; gated admission class.
- **Style re-surface:** §5 ENSURE only for impossible (exhaustion returns codes; used codes on every deny path; no defined-but-unused codes); §5.3; §7 hypothesis-first (stash-bisect, SCAN-diag before redesign); test sanctity (doc-block on contract change).

### #22 — Deferrable + background server modes
- **Learned:** (1) Fixed registries SILENTLY DROP past capacity (standalone green + short TCOUNT by exactly the new count) — bump exactly on growth. (2) Plan gaps in eligibility (periodic non-exempt BACKGROUND entering EDF dispatch) close in the eligibility predicate + white-box placement test under IrqGuard. (3) Derive expected numbers from the state machine (true divergence pins at boundaries, not mid-burst). (4) Record watchdog kills as STATUS history rows. (5) New struct fields go LAST with default initializers (mid-struct insertion rebinds positional rows).
- **Adapted:** server-mode enum + branches (activation/completion/exhaustion/top-up) + BG hardwire; taskdef passthrough + validation; admission extraction; new server class.
- **Style re-surface:** §5 ENSURE only impossible (coalesce/ demote/clamp fail-safe under noexcept); §4 same-block reuse, no RT alloc; §7 SCAN-style derivation before test edits; standalone-vs-all split for pre-existing-vs-caused.

### #23 — Partitioned-EDF bounds + migration gate
- **Learned:** (1) Ground the migration vectors: filters can make all but one vector defense-in-depth — read the filter before planning tests; tests pin non-selection, not denial. (2) Variant topology: the same class runs multi-CPU standalone but single-CPU under `all` (masks clamp, meanings change) — branch on cpu_count and verify BOTH variants. (3) Routing probes observe pre-mutation state exactly (no shadow copies) — order operations (position-while-empty, fill, deny). (4) Affinity moves re-route via dequeue/enqueue (bitmap-only moves strand cross-queue tasks; EDF pop self-heals non-READY heads, so BLOCKED placement stays safe).
- **Adapted:** per-CPU admission bounds + delegation; set_affinity_err (clamps → running-refuse → dest probe → EDF-safe move) + void wrapper + error propagation; branched affinity/smp tests.
- **Style re-surface:** §5 reachable denial → codes (keep legacy contracts); §7 verify both variants, never dismiss; §11 lock nesting unchanged, probe under held lock, no IPI wait.

### #24 — Admission bounds verification
- **Learned:** (1) Read-only gates need no new endpoints: stack TCBs (memset discipline, unique ~0 id, never added) through existing public helpers keep one math path. (2) Zero-mutation beats round-trips: failed-reserve proves exhaustion with counters provably untouched (no alloc involvement at boot). (3) Denial is unprovable on empty partitions — split boot-pins from live-class denial and document it instead of forcing. (4) __builtin_memset on non-trivial TCBs trips -Werror=class-memaccess — plain memset is the accepted discipline. (5) Expose minimal named-namespace wrappers for file-local validators (no duplication); journey tests name what existing classes do NOT cover (else the auditor asks why they exist).
- **Adapted:** boot self-test hook (static_assert + runtime range) + validator accessors + verify class.
- **Style re-surface:** value-init before memset; §5 no new codes (reuse; ENSURE nowhere on boot success); §7 overlap analysis per test; record intermittent crashes, never dismiss.

### #64 — AHCI MSI completion ISR
- **Learned:** (1) QEMU-trace over guessing: 6 wrong theories died to one `--trace` run showing synchronous zero-length completion — root causes were a spec-violating header layout (attrs at the PRDTL offset) and a 32-bit write zeroing an adjacent 16-bit register (messaged nowhere). Fix the width, not the logic. (2) Validate the SLOT (address/type), never the COUNT (empty BARs make counts lie). (3) ATAPI shadows: IDENTIFY times out on optical ports by design — skip by signature + scan-continue on zero-sector. (4) Bump MAX_TESTS BEFORE adding tests; reconcile TCOUNT actual vs executed explicitly. (5) Plain guards on ISR-shared locks deadlock the instant the ISR preempts the holder — IrqSpinLockGuard everywhere on ISR-shared state.
- **Adapted:** completion records + ISR entry + MSI enable/disable + wheel-blocked wait + destroy pairing; writew MSI programming; header layout fix; port-signature skips; driver spec flaw closures.
- **Style re-surface:** §11.1 (cli+lock on ISR-shared state, never held across reschedule); §11.2/§11.3 (BLOCKED+dequeue+deferred reschedule); §12 (gen-tagged waiter, teardown drains); §7 stub-first with Runmode/Testidea headers.

### #65 — virtio-blk MSI-X/MSI completion ISR
- **Learned:** (1) NEVER BLOCK THE HARNESS's own task in a wheel wait (its slot is the control plane) — phase timeouts belong in workers. (2) Bound test timeouts to dispatch reality (100ms beats EDF jitter with 10x margin over retired spins). (3) QUEUE_MSIX_VECTOR takes the TABLE ENTRY, not the CPU vector (spec §4.1.4.8) — programming the vector repeats the messaged-nowhere class. (4) AUDIT THE CONSUMER, not just the producer: teardown-set status the waiter never reads is success-shaped error wake — status-sentinel consumption in the wake path + both-interleavings reasoning is checklist. (5) Single-flight preserves exact poll semantics incl. residuals (documented, not fixed) — no slot allocator under single ownership.
- **Adapted:** completion record + ISR entry + IRQ enable/disable (MSI-X→MSI→poll) + wheel-blocked wait + finish split + introspection seams; mock pump; driver spec closure.
- **Style re-surface:** §10.2 nullable out-params as pointers, refs for non-null; §10.6 no short identifiers; §12.3 wakers own wakeup incl. teardown sentinel; diff-patch protocol (verbatim apply, re-gate, re-audit).

### #66 — Bounded-blocking audit closure
- **Learned:** (1) AUDIT ISSUES ARE VERIFICATION-FIRST: most paths need no code (bounded polls accepted per the bounded-or-blocked rule) — scope carve-outs explicitly (no-ISR devices, no-IRQ hardware, boot/pre-ISR) instead of converting. (2) STALE SPEC LIES — audit the prose, not just the code. (3) Composed classes move safe-row counts (MISMATCH warns, not fails) — update the safe row with every block-class addition. (4) NO PARALLEL BUILDS: concurrent makes share the ISO/initrd tree and panic the guest — gates run strictly sequentially.
- **Adapted:** arch serial TX/RX caps + pause + drop sentinel; pauses in port/poll loops; absent-drive early-reject test; driver spec closure (annotations + carve-outs); count rows.
- **Style re-surface:** §6 named-constant caps, pause() in every poll; §10.6 SCREAMING_SNAKE constants; test-header sanctity on new tests.

### #172 — Shell cpuinfo/top monitor
- **Learned:** (1) File-static rings POLLUTE test windows (post-run fixtures never appear in old frames) — labeled reset hooks for tests, production never resets. (2) PRECISION IS A FEATURE BUG: integer EMA below display quantum renders forever-zero — scale internal precision up + per-mille accessors, pinned by a convergence test. Audit advisories beat deferral when the fix is small. (3) NAME columns need padded-needle matching (markers false-positive bare names). (4) Check BOTH period sentinels (0 vs NO_PERIOD), never assume. (5) Never assert ambient names (test env differs from boot) — only fixtures you set.
- **Adapted:** cpuid helpers; running_on_cpu store + loadavg EMA + seams + accessors; read-only zombie snapshot; shell monitor commands + ring + refresh + reset; observability spec.
- **Style re-surface:** §11.6 atomics all-or-nothing; §4 fixed arrays, no alloc on tick/shell paths; §6 bounded refresh + tick deadlines, never while(true); §7 stub-first with unique fixture names; §10.2 nullable out-params as pointers.

### #174 — Shell Notify-nap + 2-CPU release mode
- **Learned:** (1) FREESTANDING HAS NO __cxa_atexit: non-trivial-dtor file-statics fail link — lazy placement-new into alignas storage, never destroyed (shell lives forever). (2) Namespace span bites: mid-file helpers land inside the enclosing namespace;-duplicate fwd-decls link-error — place helpers in the right block first. (3) QEMU BOOT REALITY: UEFI-only El Torito ISOs fail SeaBIOS manual boots silently — replicate test flags (OVMF + boot order); `make release` bakes the CURRENT test-config — always reset it first. (4) Ticks lag wall ~0.6x under TCG — tick-derived windows inflate shares; wall-domain (ns_monotonic) windows are skew-proof; periods stay tick-domain. (5) 2-CPU release boot is clean; IDLE% proves the nap.
- **Adapted:** shell-owned Notify + wheel-timeout nap (cancel-first, fail-closed to pause) at readline/refresh/read; sys/user clamps; wall-ns window; -smp 2 gating for interactive release only.
- **Style re-surface:** §4 static storage + placement new, no RT alloc; §6 bounded nap via wheel deadline; §11 tick-callback notify-only.

### #173 — Structural test-class aggregates
- **Learned:** (1) DEAD TESTS ARE SILENT: built-but-uncalled registrations never execute while comments claim coverage — only defined-vs-referenced cross-checks find them. (2) DUMP-COUNTS ARE THE ONLY TRUTH: hand arithmetic drifts and conditional rows break parsers — reconcile via measured TCOUNT, never math. (3) First-match NAME resolution shadows: duplicate aggregate/fine names make entries unreachable — exact-name dedup. (4) Name-prefix heuristics bite (be-prefix = bench-only skips) — place accordingly. (5) Q35 variants don't survive release (hardcoded DEBUG_ISO) — skip for non-debug builds. (6) FLAKES CLUSTER BY TIMING (position-sensitive, green on re-run) — regrouping moves co-residence; investigate one-at-a-time with hypothesis, never stack fixes on unreproduced flakes. (7) Throwaway parsers lie (greedy matching, branch states) — brace-aware parsing + ground-truth comparison before concluding.
- **Adapted:** single-definition-site group helpers + aggregates + testrunner + rescued fine class; dead stubs removed; dump-count rows; consistency check as coverage (overlap allowed+reported); scripted full-suite runner; gate docs off legacy targets.
- **Style re-surface:** §2 naming (collisions); §3 headers/pragma/include order; §8 formatting; §10.3 initialized vars; history format (TIME always present, one row per run, script appends).

### #76 — POSIX time API surface
- **Learned:** (1) TEST-MASKED ABI SWAPS: identical wrong layouts on both ends pass every in-tree test — new ABI structs need cross-side layout static_asserts or userspace round-trip tests (positional reasoning only). (2) UNSTORED DERIVED STATE: fields read on a different path than their writer get grepped at write time. (3) GEN-0 ROLLBACK TRAP: invalid-sentinel generations silently no-op — rollbacks carry real generations via validated peek accessors. (4) Boundary tests pin the EXACT max value (off-by-one at 999999999). (5) New .cpp files need gen.mk regen. (6) Never expose indices/generations to userspace — resolve fd→object→tag per op with owner+gen revalidation (fork-inherited use fails closed).
- **Adapted:** static-slot time registries (documented lock order); TCB sleep slot + re-apply scan; timerfd vnode (packed tag, ops-close frees); wired syscalls; rerouted libc; Tracker parity; time test class.
- **Style re-surface:** §5 reachable failures → -errno, never ENSURE/panic; §11.1/11.2/11.3 (release-before-block, BLOCKED+dequeue, finite expiry + re-apply); §12.1/12.2/12.3/12.5 (dead-state, gen tags, wakers-own-wakeup, per-op owner checks); §7 stub-first + zero-delta incl. restarts.

### #197 part 1 — SMP drain-spare fix
- **Learned:** (1) GDB-DISPROOF BEATS THEORY: silent traps (entry-poison, terminate-time AP-current) kill hypotheses cheaply; panic-trap backtraces give true shape (free-while-running, not unlink-while-linked). (2) CONTRACT-FIRST FIXES: implement the written contract (refusal + skip-and-retry) instead of inventing policy. (3) Inferior calls perturb execution (birth new faults) — pure-expression traps are perturbation-free. (4) FAKE THE PREDICATE: drive state directly for deterministic wiring tests — live observation flakes on ticks; never enqueue spinning fixtures. (5) Separate mechanisms stay separate cycles (distinct pre-existing items fold as open work, never stack). (6) Register fixtures even when never dispatched (unregistered imbalances embedded tracking).
- **Adapted:** remote-current refusal code; spare-and-retry in drain paths (bounded rotation) + deferred-kill skip; EDF unlink on pops; caller updates; deterministic predicate-driven tests.
- **Style re-surface:** §10.5 sentinel uniqueness; §11.1/§11.3 leaf-only checks, never block in drain (rotate-and-retry); §12.1 TERMINATED ≠ freeable (liveness needs any-current too); §7 stub-first.

### #206 — riscv64 U-mode ECALL M1 closure
- **Learned:** (1) x2-IS-sp ALIASING dominates trap correctness: every `sp` write is an x2 write — verify trap asm by VALUE-FLOW, never shape (silent frame-recording, deterministic post-load faults). (2) SPP IS STALE for nested traps — close nesting at the root with a scratch-free first instruction; return restores via SPIE so ticks survive. (3) MAGIC TRACERS BEAT THEORY: instrument the VALUE (sentinel in the dispatch slot proves the whole path) when theories deadlock. (4) SBI prints from S-mode are nested syscalls (usable for tracing, perturb timing — strip before gates); decoder-verify every hand-assembled byte. (5) TRIAGE DISCIPLINE: hardening assertions beyond acceptance that resist experiments revert to green baseline + filed follow-up — never hold the milestone hostage.
- **Adapted:** shift-codec PTE backend; SPP-discriminated entry; conditional restore; entry gate; decoder-verified probe + regression asserts; follow-ups filed.
- **Style re-surface:** §11.1 entry/exit symmetry; §11.2 frame/slot lifecycle parity; §10.2 scratch discipline is the asm equivalent of refs-over-pointers; §5 reachable exhaustion → errors, impossible → ENSURE (stale reads fail closed).

### #219 — riscv64 sret-restore SIE window
- **Learned:** (1) The RETURN path re-enables what entry closed — audit interrupt state at EVERY trap phase (entry/handler/return/sret), not just entry. (2) One-instruction fixes exist (mask the restored word; return restores via SPIE so ticks pend, never lost — verified delivered). (3) PROVE NEGATIVES WITH TRACES (`-d int` closed-range proof) — cheap deterministic evidence where functional tests only bound regression.
- **Adapted:** single masking instruction in the return block; disasm assertion; closed-range proof (0 hits).
- **Style re-surface:** §11.1 SIE-off spans the return like IrqGuard; §6 bounded deferral, ticks pend never lost.

### #220 — riscv64 post-ECALL x2 transparency
- **Learned:** (1) Park user-sp in a DEAD GPR before any reload (single def-use span, statically auditable via objdump) — any reload destroys it in-register. (2) S-mode x2 reads are VOID (x2==sp there) — only U-mode samples carry information; days of S-mode archaeology measure sp repeatedly. (3) QEMU GDB quirks: VA breakpoints only (physical never fire); `p/x` for full values (`%x` truncates); no hw watchpoints; batch+continue needs MI-async; output buffering eats kills (stdbuf discipline). (4) MAGIC TRACERS first, theories second. (5) TRIAGE COURAGE: revert + follow-up over hostage-holding.
- **Adapted:** 2-line park fix; post-ecall regression assert (decoder-verified bytes); VA-breakpoint trap-hunt script for future use.
- **Style re-surface:** §10.2 aliasing awareness is refs-over-pointers for asm; §7 regression test for every fixed S1; §5 fail-stop reasoning documented, not triggered.

### #221 — riscv64 table-daemon fault + boot stall
- **Learned:** (1) SHARED LOW-HALF TABLES HIJACK SAME-VA IMAGES (last-writer-wins across tasks) — clone-time private deep-copy (leaves by value, USER skipped) fixes the whole family. (2) Held halt-loops ACCUMULATE trap depth — the gate applies timer ticks at depth≥2 unboundedly (each apply abandons exactly its level; tail accounting stays exact). (3) TERMINATE-THEN-SKIP re-faults (d dying task srets into itself) — terminate arms need one-shot force-apply (set incl. early-return, cleared on consume/cancel/clear). (4) STALE ARMS RESUME BLOCKED TASKS — invalidate arms in dequeue_ready. (5) USER WAITPID waits IN-TRAP (hlt-wait + rescan); kernel contract unchanged. (6) Wake slots are arch-specific (riscv return override targets the a0 frame slot, not saved-RAX).
- **Adapted:** L0 deep-copy (+OOM unwind, zeroed tables); trap force-check + depth-gated timer application; force-apply array + sets/clears; arm invalidation; in-trap user wait + rescan; arch wake overrides; conditional orphan + reaper deferral.
- **Style re-surface:** §11.1 never hold across switch-equivalents; §11.2 BLOCKED+dequeue pairing makes invalidation safe; §12.1 TERMINATED≠gone (explicit zombie ownership); §12.4 single alloc/release pair; §5 reachable OOM → guarded return with full unwind.

### #207 — riscv64 gate integration
- **Learned:** (1) POSITIONAL-ARCH SHADOWING: default-valued make vars make `$(or …)` ignore positionals forever — gate entry points need dispatch-matrix dry-run tests. (2) VALIDATE MAPPED ARCHES: pass-through matchers accept garbage — filter against the supported set, fall back loudly. (3) TCG WALL DILATION IS LOAD-BEARING: per-arch timeout tiers at EVERY layer (expect > watchdog; outer > build + run). (4) Shared test-config is CROSS-ARCH STATE — always assert the loaded class in gate logs. (5) SCOPE GATES TO EVIDENCE: wire only green-evidenced classes per arch. (6) Release deferral is a DECISION — say so in the guard + issue, file toolchain separately.
- **Adapted:** per-arch class sets + outer timeouts; arch-aware selection + positional fix; release-guard deferral note; follow-up filed.
- **Style re-surface:** shell `set -u` safety; make `:=` convention match.

### #203 — riscv64 ECALL dispatch runtime proof
- **Learned:** (1) NEVER HAND-TRANSCRIBE MACHINE CODE: correct derivations still mistype — GENERATE bytes programmatically, paste generator output, then DECODE-VERIFY FROM THE FILE (extract, decode each word, diff vs intent). (2) FAULT-FRAME beats stale pointers: instrument the receiving handler (it has the live frame) + translate sepc to print the fetched word — closes cases in one run. (3) MENTAL HEX IS UNRELIABLE — trust code output for encodings, never head-math. (4) RELOAD clobbered regs across calls (ecall returns in a0 — set all documented cookies explicitly every call).
- **Adapted:** cookie-carrying stub (all cookies set, a0 reloaded) + registration; all TEMP-DEBUG reverted (final diff test+history only).
- **Style re-surface:** JARVIS_TEST form; teardown-before-assert; balance asserts.

### #152 — Sv39 snapshot backend
- **Learned:** (1) STATIC_ASSERTS CATCH HEAD-MATH — derive table indices from the FULL VA in code + static_assert the expected value; never offset arithmetic. (2) VERIFY MAPS ARE LIVE BEFORE PROTECTING THEM (dead boot-identity map needs no snapshot) — failed-loudly scope cuts beat forced coverage. (3) No blind x86-shape mirroring (wrong restores armed on alien tables) — the arch codec IS the fix. (4) Pool overwrite-frees are IDEMPOTENT (safe to mirror reclaim). (5) Accepted couplings fail closed (asserts, never silent corruption).
- **Adapted:** exact-match stores + warn; no riscv64 identity; arch-gated x86 restores; Sv39 save/restore; split tests.
- **Style re-surface:** codec idiom reused verbatim; IrqGuard across table surgery.

### #222 — riscv64 CI toolchain + link + layout
- **Learned:** (1) Check local build-system support before assuming toolchain work (the Makefile already had it — the gap was purely the workflow file). Keep arch CI jobs parallel and independent; size TCG timeouts from MEASURED walls + margin. (2) Install AND verify the exact invoked binaries (CC and CXX and objcopy) — partial checks are theater; first runs fail fast and cheap. (3) Same Makefile, same flags, different drivers: distro GCC defaults to PIE (bare-metal drivers don't) — fix at flags level (compile -fno-pie, static link), verify ET_EXEC via readelf; failures arrive in layers, each needing its own evidence pull. (4) Custom linker scripts + distro-injected NOTE orphans derail file-offset assignment (sparse/overlapping output) — DISCARD unneeded sections; CRC-verify byte-identity after. (5) Fix rotting comments in the same patch; the local ceiling (parse + dry-runs) is stated, first push is the real proof.
- **Adapted:** per-arch CI job (toolchain + verify + build + gate + artifact); corrected comments; g++ + build-tool prerequisites; PIE/static flags with mechanism comment, both link paths verified; NOTE discard (CRC-identical).
- **Style re-surface:** none (CI/build metadata).

### #169 + #186 — debugd spec design + audit
- **Learned:** (1) Design audits bite like code audits (multi-S1/S2 first rounds are all load-bearing) — fix every finding; the spec is materially better for rejection. (2) The diff-patch protocol works for prose (verbatim apply, minimal leftover resolution). (3) Spec-first forces dependency honesty (the phase plan falls out of the dependency map, not ambition). (4) Kill "implementation choice" weasel branches in normative text — one exact rule or the counts don't hold.
- **Adapted:** design spec with Doc ID (DRAFT) covering both issues.
- **Style re-surface:** spec-file convention (Doc ID, binding corrections, normative MUSTs).

### #224 — debugd Phase 3 host oracle
- **Learned:** (1) Audit the FIXTURES, not just the code (wrong goldens teach oracles invalid frames — fixtures are spec). (2) Truthful error codes beat empty replies (E-form errno keeps consumers functional). (3) Terminal-state hygiene in incremental parsers (preserve the packet across terminal transitions; restarts work in EVERY state incl. checksum digits). (4) No target libstdc++ exists — reduce "freestanding-clean" to a hosted-header ban + documented runtime prerequisite. (5) Denylists rot (extend, or invert to an allowlist and note it). (6) Derive copy-anchors by copy, verify with git diff (reproduced typos become drive-bys).
- **Adapted:** host oracle + mock transport + test harness (plain + sanitizers + ban); CI step.
- **Style re-surface:** kernel-adjacent idioms kept even outside the style gate.

### #43 — idle-monitor epic (spec + P1a/P2/P4/P5a/P6-thin)
- **Learned:** (1) Tick-path hooks belong in M5 seams: inline tick logic is untestable without driving ticks — seams paid for themselves 3 cycles running. (2) dmesg counting tests need clear-first + IrqGuard windows: the shared SPSC ring races tick DMD traffic and wraps, so live-base deltas are nondeterministic. (3) Never run synthetic-time scans over the shared registry: far-future `now` flags unrelated tasks (daemons) and makes counts environment-dependent — seed flags directly, use real ticks. (4) Reap discipline: TERMINATED-via-handler tasks sit in the tick's deferred queue, not the zombie list — poll for disappearance (or remove+destroy knowing the 0xDD-magic skip protects the pending entry). (5) Live READY helpers dispatch between setup and seam and clear the very flag under test — test helpers stay BLOCKED+registered, never enqueued. (6) defer_kill callers must hold scheduler_lock_ — idle-context callers need the same lock posture as the tick path, not a new lock. (7) Volume-coupled test asserts rot with suite growth (klog 2048 broke at 510 tests) — clear-first + cleanup-before-asserts.
- **Adapted:** idle_monitor.{hpp,cpp} seams (stack/stall/util/mem/escalate) + progress/aggregate/notes; 24 idle_monitor tests; bootstat/monstat shell commands; TIMING+7 + PMM+1 catalog rows.
- **Style re-surface:** integer per-mille (no float), saturating counters, M6 never-kills, fail-closed displays.

### #295 — debugd target runtime ELF (E2E bring-up + audit)
- **Learned:** (1) send_sync pops the OLDEST queued message as its reply with no sender match: any task whose inbox holds a non-reply (grant, stale reply) during send_sync eats it AND fails the call — debugd's write/open authorizes ate its own grant (short-reply authorize fail + grant loss, racy by dispatch order). Filed as #296; the v1 dodge (consume the single grant via plain recv BEFORE any authorizing call) is deterministic only with a single-grant inbox. (2) A BLOCKED-with-no-waker syscall sleeps eternally (BLOCKED is never re-dispatched): tty_read's cooperative block had no UART IRQ handler, waitqueue, or wheel arm anywhere — audit any blocking path for its waker before trusting it; EAGAIN-style poll + caller deadline/yield is the safe shape. (3) `sys_receive` returns msg.type and copies ONLY msg.data — overlaying kernel-Message structs in userspace reads adjacent fields (handle read from priority slot, always 0). (4) Every park must pair with a stop event: the silent tick-park stranded targets (sel2 polls empty forever); the belt latch must be kind 0 (cleanly parked — latching BREAKPOINT wrongly engages continue step-over for a stop no trap produced). Tests observing stops must drain the guaranteed belt event first, in order. (5) Observation discipline for serial hunts: test-mode Logger routes to debugcon (COM1-only runs look dead); file-backed debugcon buffers (flushed at exit, useless live); SeaBIOS prints nothing without a TTY (OVMF always prints); mux stdio needs expect-grade TTY. A "stall" seen through a blind setup is not evidence. (6) Bimodal pass/zero-progress across identical artifacts under varying host load, plus a stash-control that stalls on the clean tree, is load-gating proof — stop bisecting the diff at that point. (7) Rejected patches apply verbatim BUT verify first (`git apply --check` fails once hunks are absorbed — that failure IS the absorption proof); auditors catch real superior bugs (reply-buffer sizing needs worst-case wire math, arch-sized backings need round-up + per-arch widths).
- **Adapted:** nonblocking tty_read; payload-decoded grants; debug_note_requested_park (latch-0 + BP-class event) + detached guard; pre-add handoff_tc + launch_undo (teardown kept for admitted path); grant-arm-before-open; three belt-drain test updates + no-image refusal test; #296 filed; debugd.md §9.4/§9.7 refreshed to the pre-add order.
- **Style re-surface:** §5 fail-closed on every refusal/deny path (no silent fallback, errno named); no new syscalls/errno (denial table preserved); bounded loops + deadline/yield polling; lock-order comments (bind→stop, scheduler→stop); test-file idiom (asserts precede cleanup, counts exact).

### #296 — send_sync reply matching + deadline self-loop fix
- **Learned:** (1) In-kernel direct `IPC::send` does NOT stamp sender_id (only syscall entries + cap path do) — matching on sender requires every in-tree direct sender to stamp truthfully; one test peer stamped the requester's id and starved under matching (old blind pop tolerated it). Audit the stamp convention tree-wide when adding match semantics. (2) A test whose progress needs tick-dispatch of a third party (peer delay, killer) cannot pass reliably on a loaded host — design rendezvous tests on mutual IPC arrivals (deterministic wakes), never on timing; keep a dead-at-entry variant as the deterministic pin and skip (documented, not silent) what needs liveness. (3) A pre-existing test failing after unrelated changes is guilt-by-association until proven — verify via shared-path analysis (none shared → separate issue #298) rather than absorbing it. (4) `DeadlineList::insert` double-insert self-loops the head (live-caught: head→self, spinning walk, zero diagnostics) — remove-before-insert makes the end state correct in every scenario; every cyclic-capable walk needs a trip-count fail-stop (silent wedge → located panic), including `remove()` itself since insert routes through it. (5) Stale QEMUs on a shared :1234 poison GDB evidence across runs (all observations must be single-owner verified); COM1-only runs are blind in test mode; file-backed debugcon buffers until exit.
- **Adapted:** selective-pop trio + match-driven send_sync + ipc.md §2 delta (no ABI change); 4 registered tests + refusal pin restored; #297 filed for the self-loop with the hardening; #298 filed for the wheel-reclaim flake.
- **Style re-surface:** §5 fail-closed preserved on every new branch (oversized-stays-queued, dest-death queue-intact, immediate-false paths); single-lock leaf-only queue ops (no nesting); test-file idiom (bounded waits with hlt, asserts precede cleanup, terminate_and_drain2 for pairs).

### #297 — deadline-list self-loop wedge (live-caught)
- **Learned:** (1) A doubly-linked intrusive list insert that assumes absent-membership self-loops exactly when the task is already head (null links, miss the smaller-than-head branch, re-link self as successor) — and every later insert spins forever with zero diagnostics. Remove-before-insert makes the end state correct in every scenario (count-exact: --size_ on unlink only, ++size_ always). (2) Every cyclic-capable walk needs its own trip-count fail-stop, INCLUDING the helper the fix routes through (remove()'s scan would hang before insert()'s guard — auditor-caught S2). Trip > size_ terminates by pigeonhole even with inflated counts; ENSURE fail-stop converts silent wedge to located panic. (3) Raw-memory GDB reads beat type-driven reads when the toolchain flakes (DWARF/LTO type lookup fails intermittently; numeric addresses from nm + ptype offsets are stable within a build — but rebuilds shift layout, re-resolve per binary).
- **Adapted:** remove-first + bounded insert/remove walks in DeadlineList; tripwire silent since deploy; filed from #280-hunt live evidence.
- **Style re-surface:** fail-stop over hang (ENSURE with location, precedent exists in scheduler paths); defensive invariants over mechanism hunts when the caller search is exhausted.

### #267 — deterministic pre-dispatch cancel (flaky loader_cancel_mid_load)
- **Learned:** (1) reschedule() only arms need_resched — the switch happens on the next tick — so harness yields do NOT bound a higher-prio peer's progress: an 8-page load fits in few tick-slices and 1/6 runs completed inside 3 yields. (2) Poll-until-state does not fix tick-driven races: fast runs jump VALIDATING→DONE between two polls (proven empirically — 1M bounded polls never saw COPYING). The deterministic point is pre-first-dispatch: mask the tick across wake→decision→act and the peer is frozen, so the observed state is provable. (3) Masking across a syscall is defensible only with a cited non-blocking proof (kernel-task vfsd bypass + synchronous spinlock-only path + no reschedule in body); the hang-if-wrong failure mode is loud (TIMEOUT), never a false pass — still record it as fragile and prefer masking pure flag-sets. (4) Zero-S-line stall-unrecovered TIMEOUTs with green identical-binary reruns are host TCG load stalls, not regressions — the guest never booted; my change cannot be guilty when the changed code never executed (cancel test PASSed in the one run that also logged a stall-retry).
- **Adapted:** masked request_load/assert-VALIDATING/request_cancel in loader_cancel_mid_load + in-test coverage disclosure (VALIDATING-checkpoint contract, not per-chunk reclaim); no kernel change; no spec change (test-only).
- **Style re-surface:** §5 fail-closed asserts (OK not NOT_LOADING, IDLE, null take — loud fail on the DONE window); no retries; no new tests (counts exact).

### #273 — Rust no_std user ELF (toolchain, target, shims, E2E)
- **Learned:** (1) Switching ARCH wipes build/ entirely (arch-change clean) — including a 500 MB project-local Rust toolchain; never probe another arch mid-task, reinstall last. (2) `find build -name '*.d'` matches DIRECTORIES: rustup's bash_completion.d broke DEPFILES -include fatally — exclude toolchain/registry homes from shared finds. (3) Nightly custom-target JSON drifts: target-pointer-width is numeric, eh-frame/stack-protector are invalid fields — derive field-for-field from `rustc --print target-spec-json` of the closest builtin, never from memory. (4) Rust asm! forbids rbx (LLVM-reserved) — ABI args in reserved regs need a global_asm trampoline with manual save/restore. Custom JSON targets also need -Z json-target-spec on the cargo line, and build-std needs registry access on first fetch (pin via Cargo.lock + --locked). (5) Read the handlers, not the issue text: KLOG(48) is dmesg-read and PRINT(4) a stub — hello-print is WRITE(13); dmesg subsystem renders USERSPACE, not USER. (6) Keep demo binaries OUT of the committed initrd (ephemeral staging + revert, verified pristine) unless a test pins them.
- **Adapted:** rust-targets/x86_64-nexios.json + user/rust-hello + x86_64-gated make rule with fail-closed bootstrap guard + DEPFILES exclusion + docs/specs/rust-userspace.md; no kernel changes.
- **Style re-surface:** rules hard-set their environment (RUSTUP_HOME/CARGO_HOME/PATH), never inherit dev-machine state; spec cites handler line numbers as ABI truth.

### #271 closed — BSS-only RW pin gap (live-E2E canary trip)
- **Learned:** (1) Silent-skip writers (`canary_write_at`: walk-fail → return) must never pair with an always-armed record: `install_segment_canaries` armed DATA-after from vaddr+memsz while the rerun builder mapped nothing for filesz-0 segments (no pin range) → trip at dispatch. Map the span BEFORE install; len-0 pin ranges copy nothing but still drive map+zero. (2) `in_padding` (slot ≥ vaddr+memsz) is an anti-corruption guard, not a bug: page-aligned memsz deliberately skips DATA-after (slot would overlap the image) — regression images must use the exact live shape (hey: 0x402e80/filesz-0/memsz-0x18), never a tidied page-aligned variant. (3) `register_<group>_tests()` is an explicit list — a new JARVIS_TEST silently runs nowhere until registered; the count bump then fails the gate (PLANNED mismatch convicts it). (4) Storage = pure VFS groups: an elf_loader-only diff cannot execute there — gate-order failure + identical-binary standalone green = flake by mechanism, documented not dismissed.
- **Adapted:** `fill_staging_locked` records len-0 ranges (comment); `rerun_bss_only_data_canary` + counts 24→25/122→123; negative control (skip restored → FAIL) run before the fix commit.
- **Style re-surface:** arm-or-skip symmetry (every record set needs a proven write path); exact-shape regression fixtures; explicit registration lists stay in sync with counts.

### #345 hunt — per-tick daemon-wake starvation + EDF self-pick (tracker open, fix planned)
- **Learned:** (1) Host pipe backpressure freezes the QEMU vCPU inside guest output (GDB: frozen in `Logger::putchar` + unresponsive stub) — but it CANNOT explain silent-spin wedges (a blocked pipe only stops a guest that is writing); fix = file backends, observer lag then only delays matching. (2) GDB `interrupt` STOPS the guest: repeated stop-read-detach cycles measure the observer, not the target (a "wedge" cleared into 6/6 PASS on resume) — one interrupt + full capture + detach, and never GDB-touch a run you intend to time. (3) GDB `call` returns garbage on the live TCG stub (two unmapped pointers); only raw register/memory reads + DWARF-via-GDB-python offsets are trustworthy. (4) Manual QEMU splits serial+debugcon — S-lines/RUN_PRE live in debugcon; watching serial alone fabricates a wedge. Preserve FULL serials (tail-30 loses beacon forensics). (5) Expect verdict "TIMEOUT" on a hot host with cool-host PASS = throughput collapse, not a hang; timeout tiers must be sized from measured walls. (6) A TEMP-DIAG tick beacon (read-only, IRQ-off, ≤1 line/50 ticks) convicted the wedge steady state over ~150k ticks ×6 wedges: Flavor A = per-tick #18 recv-timeout re-wake of watchdogd (`wdwake` 1:1, daemon `recv(timeout=10)`) + prio-20 dispatch + re-block starves prio-10 harness; top-flap 10↔20 pins the #242 breaker at 1; Flavor B = EDF self-pick on a never-yielding AUTO spinner (pick returns current through absolute EDF precedence, bitmap never consulted). Killed en route: aggregate-order leak, frozen ticks, IF, lost-switch, leaked lock, preempt flag, aliasing, dispatch-guard, CPU-index divergence. (7) Beacon fields from different ticks must NOT be compared naively — counters written at RMS-tail report the PREVIOUS tick at the next beacon (off-by-one fooled analysis twice); always align entry-state(N) with RMS-state(N-1) as inter-tick evolution.
- **Adapted:** `QEMU_FLAGS_FILE` + `tools/qemu-file-log.sh` (spawn/eof verdict semantics preserved); hunt log convention `/tmp/jarvis-hunt-<issue>-<TS>.log` + per-iter full serial preservation; fix plan (consume-on-apply → resume-cancel → flap-robust breaker → EDF self-pick guard → supervision sizing) posted on #345.
- **Style re-surface:** disprove before convict (sufficient≠necessary experiments); observer-effect accounting in every hunt loop; TEMP-DIAG reverted grep-verified before ship (zero markers/strings, build green).

### #345 fix — edge-triggered #18 wake + EDF self-pick guard (closed)
- **Learned:** (1) Level-triggered timeout re-apply (every tick while `armed && timed_out`) re-wakes a 10-tick watchdogd every tick (wdwake 1:1) — prio-20 dispatch starves prio-10 harness and pins breaker via top-flap 10↔20; fix = skip duplicate READY+queued re-wakes (already queued) — one wake per expiry, stranded READY (!in_queue) still rescued, generation guard preserved. (2) Absolute EDF precedence lets a never-yielding AUTO task with earliest expired deadline self-pick forever (`edf_preempt_candidate` returned current) — bitmap never consulted; fix = early `if (e == cur) return nullptr` to fall through to bitmap. Both are one-line guards, no new locks/alloc, no debug/release divergence.
- **Adapted:** `scheduler.cpp` #18 loop duplicate-skip + `edf_preempt_candidate` self-pick guard; audit APPROVED; validation: task_watchdog 6/6 ×6, deadline 145/145, proc_elf 123/123 through file-backed harness.
- **Style re-surface:** edge- vs level-triggered wake semantics (one expiry → one wake, not per-tick); current-never-queued and stranded-rescue invariants preserved.

### #336 — ship rust-hello ELF + source in committed initrd
- **Learned:** (1) `execute-test` overwrites `initrd/tests/test-config.txt` with the class filter — any interactive E2E afterwards boots into the test runner (EOF, no prompt) until reset to `none` + cpio rebuild; always `cat` it before an interactive run. (2) Boot-initrd contents are safe to extend: all content tests use synthetic archives + restore, #274 resolves fixed paths, no listing-count asserts — but prove with storage+ui, not by reading. (3) Existence-guarded staging (`if [ -f ... ]`) keeps toolchain-less/arch/clean builds green while shipping more where available.
- **Adapted:** two guarded cp lines in the CPIO rule (/bin/rust-hello.elf + /rust-hello sources); no test changes; no spec change.
- **Style re-surface:** fail-loud beats best-effort in image staging (`&&` chains, missing tree files = make error).

### #363 — VMM re-audit: split, masks, option-B W^X, per-arch codec TUs (closed)
- **Learned:** (1) Pure-move splits verify mechanically: partition into chunks with a walk-up doc-attachment rule, assert tiling (no gaps), per-chunk signature/end/cut-depth checks, and reconstruction equality (concatenated chunks == original bytes) — stronger than review; the compiler then convicts cross-TU `static` deps (file-static helpers used across TUs → shared internal header with external linkage, never per-TU copies which would fork throttle counters). (2) Bulk-token replacement hygiene: `replaceAll` on short hex (`0xFFF`) glues tokens inside longer literals in comments (`kPageOffsetMaskF800...`) — always grep the token class after + review the diff (auditor-convicted S3). (3) NX-shape latent class: test helper `leaf & ~0xFFF` keeps bit 63 once NX exists — production `PAGE_FRAME_MASK` strips it for a reason; test phys-extraction must go through the canonical translation (`virt_to_phys_*`) or the production mask (generalizes the #206 x86-domain lesson to flag-carrying leaves). (4) Flag-day atomicity under `-Werror`: removing the overload turns every missed caller into a build break — enumerate with a depth-aware arg-count scanner (multi-line calls defeat `grep -c`) and classify code-vs-data per site before flipping. (5) HAL reuse needs bit-identity, not similarity: aarch64/riscv `attr_from_flags` add SH/NS + AP policy the VMM map path omits — declined with documented rationale in the header. (6) Arch-build staleness: new TUs need `rm mk/cpp-rules.<arch>.gen.mk` (stale explicit per-file rules); riscv-only breakage hides behind x86 guards (private `TABLE_MASK` in a header `static_assert` — made public with rationale); full-arch link may be pre-broken by another issue (bench `rdtsc`, #362) — validate owned objects, document the rest. (7) `is_table_entry` as the single leaf/table predicate centralizes the arch rule (level-gated); walk shapes stay shared as documented VMM-private residual (snapshot pairing, ENSURE, drop notes — policy, not encoding).
- **Adapted:** 5 concern TUs + `vmm_internal.hpp` + 3 per-arch codec TUs (11 `vmm_arch_*` helpers, guards-in-file); masks (`PAGE_NX`→hal, `kPageOffsetMask/kHuge2M*/kPdptBlockBase*` + asserts); option-B migration (37 test sites + yield stub, overload removed) + `free_priv_window` NX fix; `pml4`-as-root doc note; 3 APPROVED audit reports (`report-2026-10-17T10-16-15Z/10-18-00Z/11-06-57Z`); test-history rows; #365 spun off for the PDPT-mask re-derivation (v0.5.5).
- **Style re-surface:** constexpr helpers over hand-derived constants; fail-closed (NX default, huge-leaf refusal, no silent drops); no CONFIG_DEBUG behavior change; bounded loops preserved; static_assert pins on every new mask; per-site code/data justification for permission changes.

### #366 — VMM audit hardening: flag accessors, contracts, checked unmaps (closed)
- **Learned:** (1) Verify the premise before filing: read-accessors already existed — only the SET path was raw (2 spots + 1 test); the issue shrank from "add accessors" to "add setters + reroute 3 lines" because the tree was read first. (2) A reworked `_err` that delegates to a side-effecting void twin must probe presence FIRST (via the walk-only leaf lookup), or absent-VA calls arm test-touch flags / take the drop-note path for nothing — presence-check-then-delegate keeps the void twin's observable contract exactly. (3) Cap-unmap `_err` twins deliberately skip revoked/IO checks: unmap-after-revoke must work (the revoke path itself unmaps); document the asymmetry vs the map twins. (4) New `_err` APIs need hole-coverage tests at birth: the pre-existing `unmap_page_err(...)=OK` assertion encoded the old always-OK contract and would have masked the semantic change — extend it in the same commit (probe-guarded: never unmap live kernel state to force the OK path; cover OK on scratch PML4s). (5) Count-bump discipline: new test → `test_expected_counts.hpp` fine row + every aggregate row that runs the group (memory_vmm 13→14 AND core 577→578); `validate_class_count` convicts drift via dmesg, `validate_all_consistency` the coverage. (6) Lock-sensitive revoke/teardown paths get fold-and-ignore `(void)` + comment, never logging — Logger would introduce lock-order hazards under the registry/reaper locks; log-and-continue is only for lock-free boot/teardown paths that already log. (7) Test-history rows must carry measured `TIME_ELAPSED_MS` — never estimated walls; re-run with the timing grep rather than filling plausible numbers.
- **Adapted:** `set_hhdm/identity_modified()` + 3 reroutes (grep-verified zero raw stores); 46 per-API Lock/Context/SMP contract notes (merge_kernel_half kept as the canonical example); 4 `_err` unmap twins (INVALID_ADDR/NOT_MAPPED/OK, two-pass mmio, no partial unmap); 9 boot maps → checked with per-site cleanup + PIC fail-open on APIC map failure; cap revoke/drain/teardown fold-and-ignore, ahci/dma log-and-continue, pager rollback untouched; `vmm_unmap_err_not_mapped` test; 2 APPROVED audit reports (`report-2026-10-17T16-09-16Z/16-12-41Z`); #365 stays deferred, no code.
- **Style re-surface:** ENSURE only for impossible-by-design (boot OOM is reachable → return false, never panic — the plan's halt proposal was rejected on §5.1 grounds); fail-closed boot paths (`return 0`/`false` with cleanup); no `_err` propagation through idempotent teardown (fold-and-ignore where the slot/refcount, not the PTE, is authoritative).

### #323 — scheduler.cpp decomposition into 17 thematic sub-TUs (closed)
- **Learned:** (1) Pure-move protocol that survives audit: boundary asserts on first/last lines of every chunk (header numbers shift as strands land — always re-derive), bottom-to-top deletion, byte-identity spot-checks (gone-here/present-there greps). (2) File-static helpers used by moving code go to a shared internal-header home owned by the first consumer strand (current_sp/IRET_* → sched_switch; later strands import via using-decl) — never duplicate per-TU copies. (3) Member-function bodies move with zero lookup risk (member scope travels); free functions (weak handlers) and file-statics need closure analysis — dead CONFIG arms move byte-identical without resolving in the current config (H2_EV using-decl still added for the trace-ON config per audit check 6). (4) A moved second `namespace kernel { using...; }` block must close the outer namespace first, or the compiler reports "not in namespace enclosing" — and an edit that replaces the member's closing `}` with the namespace close silently eats a brace (convicted by build, fixed, brace-count verified). (5) `make execute-test` class names must match the registry exactly: `scheduler` is not a class (`Unknown test class` → zero tests → QEMU_EXIT stall); owning classes were scheduler_core/o1/atomic/preemption, deadline/*, task_*, core for the final gate. (6) Second audit pass is cheap and worth it: first-pass S1s on the switch strand (stranded H2 use, unclosed VMM include) were real latent defects fixed verbatim via rejected_patch.diff.
- **Adapted:** scheduler.cpp 5854 → 234 lines (includes/usings, leftover core-state defs, ISR hook docs); 17 sub-TUs (switch, watchdog, registry, snapshot, snapshot_fields, tick, queue, admission, dispatch, debug, smp, lifecycle, auth, budget, id-table, terminate + snapshot); per-strand retrieval artifacts + independent SIL3 audits (2 REJECTED-first-pass with verbatim fixes, rest APPROVED); test-history rows for every class run incl. the host-load watchdog stall + identical-binary green rerun.
- **Style re-surface:** minimal internal headers exist for pattern consistency even with no shared decls; moved bodies keep original comments (incl. dead-arm rationale); pointer comments only where defs vanish without a trace (shared homes, statics).

### #369 — scheduler post-split hardening (T1–T5, closed)
- **Learned:** (1) Holder-based entry ENSUREs need a per-site take-shape analysis: unconditional blocking take → entry ENSURE (provably safe: any locked caller deadlocks today); conditional take → ENSURE at the take site (set_task_ready taught this via a fired ENSURE on a legal locked-caller path); try_lock-skip paths → NO ENSURE (skip is the design). (2) Stale lock-held comments are load-bearing lies: verify every "caller holds X" claim against brace-parsed scope + runtime holder() before trusting it — the on_tick tail runs unlocked (adopt scope closes ~tick:710) while 6+ comments claimed otherwise. (3) IRQ-window hoists need caller-side reentrancy proof, not callee-side: the T3a dispatch_step hoist was rejected because reschedule() runs unlocked by design so timer ISRs nest fully — proving "no ISR calls X" is insufficient. (4) SwSlot ordering floor holds as designed: zero RELAXED stores anywhere; all 7 RELAXED loads are diagnostics/counter-bumps; per-CPU slots enforce own-CPU writes. (5) C++20 sweep without PGO should be nodiscard + provably-cold unlikely only — skip likely/constexpr-churn without measurement.
- **Adapted:** validate_task_ptr + 30+ entry/take-site ENSUREs across 12 TUs (7 commits, all APPROVED, 2 REJECTED-first-pass with verbatim fixes: switch S1s, T3a S1); canonical lock-order table in scheduler.hpp; corrected 8 stale lock comments; T3 closed as surveyed-minimal (publish windows already minimal; CR0/FPU + release/guard + reschedule rationales documented as do-not-move).
- **Style re-surface:** take-site ENSUREs sit immediately above the guard they protect with an issue-tagged rationale comment; reverted ENSUREs leave a comment explaining why the site is intentionally unguarded.

### #324 — task lifecycle/cleanup audit + teardown trace (T0–T14, closed)
- **Learned:** (1) Audit-only ticks are results, not skips: state the hypothesis and cite file:line evidence for every "verified, no change" verdict (T2/T4–T10). (2) One-shot arms must retain, never rely on re-detection: both KILL arms fire once (watchdog disarms, deadline skips TERMINATED), so a spared deferred-kill is lost until reap — compact-and-retain with a generation guard (Tick 11). The same generation handshake closes lock-free check-then-act races (scan-vs-seam double-fire, Tick 3): skip is always the safe direction. (3) Check-7 needs per-change artifacts: session-start retrieval does not cover later increments — a REJECT on process-only grounds costs a full cycle (Tick 13). (4) Tracer slots compose across registries (bit overlap is fine; folds concatenate), but existing fold tests pin the chain — new steps must preserve every covered path or update the test + counts together. (5) Style-checker English false positive: "free (" trips `free\s*\(` — reword the comment, not the checker. (6) Verify edit placement by reading + building immediately: an indent-mismatched match compiles into misleading-indentation errors, not silent success.
- **Adapted:** teardown trace (SchedTeardownSteps + canonical/reap folds, 7 step sites, 1 test; counts 5→6/578→579); deferred-kill retain + gen guard; reap parent lookup via id_table_find (O(n²) closed); watchdog gen handshake; lifecycle/table-ownership docs; 8 cold-leg [[unlikely]]; span adoption explicitly declined.
- **Style re-surface:** [[unlikely]] on provably-cold legs only (refusal/corruption/stale — never spares/contention); cross-registry expected folds via the constexpr helper; terminal paths as documented prefix values.
### #372 — harness checkout-scoping: per-checkout QEMU tag, scoped pkill, namespaced /tmp (closed)
- **Learned:** (1) A foreign same-UID `pkill -f` kills by cmdline substring — proven live with a marker process; the expanded make recipe shell cmdline itself contains `qemu-system-x86_64 ... -cdrom debug/nexios-rtos.iso` on one logical line, so cross-checkout pkill SIGTERMs recipe shells (own-run pkill is ancestor-spared on macOS; foreign is lethal — explains a `Terminated: 15` with no S-line and an orphaned sub-make completing the build). (2) Prefix trap is real: tag `jarvis` ⊂ `jarvis_test` — every tag match needs a trailing `([^A-Za-z0-9_]|$)` guard in ONE shared ERE var, verified live in both directions (own-tag dies, foreign survives, both ways). (3) Fixed /tmp harness paths (serial/com1/verdict/timestamps) are shared mutable state across checkouts — namespace all of them or verdicts interleave; same for per-class LOGDIR and matrix-runner logs. (4) `make -n` prints fully-expanded recipes — use it to extract verbatim runtime patterns for kill-tests instead of hand-transcribing. (5) Planner missed `run_all_classes.sh:90` blanket pkill — always repo-wide-grep the bug class yourself before implementing; extensions to the plan get flagged explicitly to the auditor.
- **Adapted:** CHECKOUT_TAG + QEMU_TAG_ERE single-definition-point in Makefile; `-name jarvis-<TAG>` on all 3 QEMU spawn paths (test, dump-counts, debug-shell); 3 Makefile pkill sites + run_all_classes.sh + deadline_matrix.sh scoped; 4 logs + 2 timestamp files + LOGDIR + matrix logs namespaced; live scoping proof + core_tracing green; audit APPROVED (`audits/report-2026-10-18T12-33-37Z.md`).
- **Style re-surface:** harness-only change, zero kernel/guest code; single canonical ERE var with prefix-trap comment — no site hand-writes its own pattern; `$$` escaping and ERE anchors verified live, not by reasoning about quoting layers.

### #303 M12 — test_libc_thread.cpp: hosted thread-path tests (open tracker, module ticked)
- **Learned:** (1) nm decides errno routing before any run: image had `T __errno_location` (linked, unused) + `B errno` (used) because picolibc was built WITHOUT `__PICOLIBC_ERRNO_FUNCTION` → plain-global fallback — read the sysroot errno.h + build flags, don't assume the TP cell is live for hosted code. (2) Red runs are evidence: asserting ENOENT on the hosted path failed with eno==0, proving the glue maps no error — that red row (with its fail-path leak-tag artifact) is what justified Branch B, not a setback. (3) A FAIL line's `[LEAK]` tag can be reporting overhead of the failure path itself: identical vehicle path runs leak-free once green — confirm by re-running green before opening a leak hunt. (4) Planner field bindings can be wrong from memory (guessed last_fetch==999; source says (1000,1,1)) — bind every expected value to the exact printf argument order in source. (5) No kernel-visible contract → no kernel-side pin: errno.h is not on the kernel -I path and the kernel treats the TP block as opaque — Ring-3-only scope is the honest shape, flag the dropped pin to the auditor.
- **Adapted:** test_libc_thread.cpp (atomic exact+sweep, errno_mapping honest-subset, errno_fresh_per_dispatch ×2) + errno_probe markers appended at image END (512 B prefix window untouched); proc_elf 131/131, tls 13/13, bench 28/28; audit APPROVED (`audits/report-2026-10-18T12-54-23Z.md`); Threads box ticked in #303 with developer-side leftovers recorded in-body.
- **Style re-surface:** Testidea/Input/Expect/Depends doc-blocks on every test; parse_tag_line is unsigned-only (fold signs in the image, e.g. fd<0 → 1); x86_64 guard idiom copied from test_libc_string.cpp.

### #361 rollback injector tests (fail-first arm(1) + past-end arm(3), closed)
- **Learned:** (1) PMM baselines captured AFTER the device alloc net exactly +2 per cycle (freed device frames) — never +4 for two cycles; recompute from the capture point, not the cycle count (red run caught it, one-attempt fix). (2) Never reuse freed frames for a repeat mapping (daemons may take them under test isolation) — alloc fresh frames for the second pass. (3) Untouched-baseline > cleared-baseline when a boot huge mapping pre-exists: assert virt_to_phys equality before/after, not zero (zero would mistake the baseline for a leak). (4) gh CLI + zsh: never put backticks inside double-quoted --body strings (command substitution); use --body-file with a quoted heredoc.
- **Adapted:** vmm_map_range_rollback_first_fail + _past_end in test_vmm_path.cpp + rows (vmm_path 7, core_tracing 24); core_tracing 24/24 green; audit APPROVED (`audits/report-2026-10-18T13-40-01Z.md`); #361 closed with evidence.
- **Style re-surface:** Mirror the adjacent dev test's teardown exactly (unmap both, restore both boot entries, free both split PTs, flush both VAs); RAII guard for arming; CONFIG_DEBUG trivial-pass idiom + x86_64 registration placement copied from the sibling.

### #367 — aarch64 bench rdtsc + #368 — H2 trace declaration home (closed)
- **Learned:** (1) Cross-arch TU verification without wiping build/: extract the exact compile line via `make -n`, rerun manually with `${=FLAGS}` (zsh needs explicit word-split) + `-DCONFIG_DEBUG` (CpuContext diag members are CONFIG_DEBUG-gated — a missing -D mimics tree rot) + `-D<flag>` under test, output to namespaced /tmp. (2) A TU-local `inline` function does not satisfy an external declaration in another TU — dropping `inline` for external linkage is the fix, proven by full-link build. (3) Flag-gated diagnostic code rots invisibly: the h2 declaration fix exposed two more rotted blocks (stale `kernel::Timer`, missing `task_stack_ptr` include) — a "declaration home" fix must compile ALL user TUs with the flag on, not just resolve the reported symbol.
- **Adapted:** `Timer::raw_counter` migration; `h2_record`/`h2_dump_ring` declared in `ipc_sched_trace.hpp`, defined once in `scheduler_debug.cpp`, `global_state.hpp` includes it; repaired gated blocks.
- **Style re-surface:** declaration home lives with the macro that expands to the call; single definition documented at the declaration site.

### #374 — unified task-ID range gate validate_id (closed)
- **Learned:** (1) A range gate must not live inside the lookup when internal paths legitimately exceed the range: reboot rewinds `next_task_id_` while AP-idle tasks stay live, so `find_task` stays ungated and `validate_id` is advisory for external boundaries only. (2) Exact-ID-match probing is already wrong-task-safe (stale IDs resolve null, never alias) — the gate's value is fail-fast + documented contract, not memory safety.
- **Adapted:** `Scheduler::validate_id()` (sentinel + high-water, probe unchanged); applied at 5 thin-check syscall entries (kill/affinity/times/notify); debug handlers + SwSlots paths deliberately untouched; `validate_id_range_gate` test + count 9→10.
- **Style re-surface:** advisory validators document who must NOT use them (rewound-counter internals) at the declaration.

### #375 — atomic memory-budget counters (closed)
- **Learned:** (1) Flake-vs-regression needs a stash control before any further reasoning: a #GP in an untouched debug-stop path + 120s-elapsed (vs 5s normal) convicted host load, not the atomics — pristine 40/40 + identical-binary 40/40 closed it. (2) A convicted-but-unrelated signature must still be filed (became #379 S2) with the excerpts preserved — the raw serial log is a singleton and control reruns overwrite it; copy it BEFORE the stash run. (3) §11.6 all-or-nothing audit must enumerate every accessor including test-adjacent readers (exit_record reap path, idle_scan_mem cross-CPU scan) — writers-only conversion leaves mixed access.
- **Adapted:** CAS-loop reserve, fetch-add release, atomic charge/credit + saturating ops, atomic snapshot reads; write-once limit documented; TCB/scheduler discipline comments.
- **Style re-surface:** __atomic builtins with explicit ordering (not std::atomic) per tree idiom; boot single-threaded init still uses atomic store for uniformity.

### #377 — privatize TCB page_table_ behind install/swap/clear (closed)
- **Learned:** (1) Privatizing a TCB field breaks strict standard-layout → `offsetof` becomes `-Werror=invalid-offsetof` in tests: derive offsets from live member addresses instead; note the dormant CONFIG_TCB_WRITE_LOG macro shares the rot. (2) Compile-everything is the migration tool: the private field convicts every raw site (including a live test doing the exact #371 shape) — fix writers semantically (install/swap/clear), script reads, then let the build enumerate the rest. (3) Exec is the one legitimate overwrite and must stay swap-then-free: restructuring to free-then-install would expose a dead-root window to the timer ISR.
- **Adapted:** private field + getter/install/swap/clear with ENSUREs; ~70 sites migrated; pcid/buffer_pool exec-simulating tests use swap; two offsetof tests fixed.
- **Style re-surface:** single blessed write path with the invariant (install-onto-zero) enforced where the defect class lived, not documented.

### #378 — VMM walk geometry convergence (closed)
- **Learned:** (1) Convergence stops where policy starts: index computation converges cleanly into arch shims, but split placement/#361 injector/snapshot flags are VMM policy per #363 P4 — parameterizing them would trade straight-line audited code for callback indirection. (2) unmap-via-leaf equivalence must be checked per-arch AND per-side-effect: x86 huge-descend already returned nullptr through get_table's split-guard (matching the new pre-check), and the unconditional #225 flush must be preserved. (3) Exact-PTE tests are the convergence tripwire — they convict bit drift the build cannot.
- **Adapted:** `vmm_arch_walk_levels/indices` in 3 arch TUs; 5 sites share index computation; leaf walk is one level loop; unmap reuses it; map bodies keep policy branches; vmm_internal P4 note updated.
- **Style re-surface:** geometry (arch TU) vs policy (VMM-private skeleton) separation documented at both ends.

### #376 — kernel-half W^X slice 1 (open, epic remainder)
- **Learned:** (1) Verify the blanket claim per-arch before scoping: riscv never sets X and aarch64 sets UXN+PXN in the kernel-leaf path — x86_64 was the sole gap, and boot-established text/HHDM stays RWX everywhere (the actual epic). (2) An NX flip on a shared encoder convicts exact-PTE assertions: split-inherited siblings keep boot flags (no NX) while fresh leaves gain it — assert them separately, never weaken to masks. (3) Enumerate every caller of the flipped path for X-need before flipping (here: all data/MMIO, no executor) — safety argued from the caller list, not the bit.
- **Adapted:** x86 `encode_kernel_leaf` +NX (NXE verified at boot); `kernel_half_runtime_map_nx` inspector test; two exact-PTE tests split into mapped-vs-inherited expectations; per-arch scope correction on the issue.
- **Style re-surface:** permission changes need per-site code/data justification + an inspector test through the arch shim, not bit asserts alone.
### #379 — fail-closed debugger death publish (kill-while-debugged GP, closed)
- **Learned:** (1) intra-cleanup program order can be intact (publish precedes free) while the observed pointer is poisoned — the freer is concurrent/stale, so guards-not-reorder is the fix; verify ordering by reading before theorizing. (2) A white-box poison fixture (0xDD sentinel into the server cache; freed-then-published TCB) pins the guard deterministically where the integration race cannot; the timing-dependent test only proves notification, never the torn state (auditor S3, accepted). (3) `is_valid` + canonical-address + poison guards compose the fail-closed read pattern — same shape as the on_tick SS-race guard; reuse it verbatim.
- **Adapted:** snap_budget_of null/canonical/poison guards (budget 0 fail-closed); publish_death is_valid entry skip; 3 regression tests (kill-while-debugged + poisoned-server + poisoned-TCB); debug_syscall 26→29.
- **Style re-surface:** advisory reads fail closed with budget 0; the DEATH event itself never depends on the advisory read (notification preserved).

### #373 — destroy completes teardown on REAPED-with-resources (closed)
- **Learned:** (1) Track TaskControlBlock::destroy's three shapes explicitly: live → cleanup+remove; REAPED-with-resources (pinned at first destroy) → deferred tail WITHOUT re-remove; REAPED-after-full-teardown → safe no-op — the middle shape is where pinned-then-unpinned leaks hid. (2) remove_task/unregister_task/id_table_remove/all_tasks.remove are all idempotent (double-remove is the NORMAL path: cleanup unregisters, destroy removes after) — so the deferred tail re-running unregister is safe; the real hazard is only double resource teardown. (3) Tracker symmetry for directly-created test TCBs: create() doesn't track_task_add but cleanup() always track_task_removes — add_task under IrqGuard (removed by the same destroy) balances the fixture, otherwise every delta reads -1 (caught red, not a kernel bug).
- **Adapted:** teardown_completed_ flag (Finalize end; false==0 after every memset site) + complete_teardown() tail extraction (verbatim move); three-way destroy branch; pinned test extended with pre-create-baseline zero-delta asserts; destroy-after-teardown noop test.
- **Style re-surface:** extraction over restructuring (verbatim tail move keeps 14-block order + steps identical); state the skipped call (remove_task) with the by-construction reason.

### #303 items 1–2 — hosted errno root cause + TP-wiring (closed)
- **Learned:** (1) A red errno==0 with correct kernel returns means the GLUE misreads the return: `_open` et al. were declared `long` but defined `int` — the missing call-site sign-extension delivered +4294967295 for -1 (the stub epilogue's 32-bit ops zero the upper half), so `ret < 0` never fired while the vehicle's int truncation still read -1. Prove with disassembly (missing `cltq`), not theory. (2) picolibc's errno knob is `-Derrno-function=__errno`, NOT `-Dthread-local-storage=true` (the latter emits PT_TLS and breaks the loader-unchanged gate) — TP locality needs no ELF TLS. (3) map_err's -4095..-1 window decodes GENERIC_FAIL (-1) to EPERM(1): any kernel-failure errno visible pre-mapping is a fail-closed artifact, so the glue overwrites deterministically (open→ENOENT, rwls→EIO) and documents the ENOSYS-trunc supersession.
- **Adapted:** corrected int stub decls + errno sets + `__errno()` (TP+0, static fallback); sysroot rebuilt offline (vendored tarball) with errno-function; vehicle untouched; libc_thread pins eno==ENOENT on the TP cell; `T __errno` resolves, no `B errno`, PT_TLS absent.
- **Style re-surface:** same-source BSS/TP compatibility via sysroot `<errno.h>` (no extern hack); comment the knob that must NOT be flipped.

### #303 item 3 — shell P2 seam + delivery test (closed)
- **Learned:** (1) Moving a function out of an anonymous namespace needs a brace recount, not an assumption: the 111-anon closed at old-203 (not old-169), so a naive cut closed `service::` itself — map `^namespace`/`^}` first, then place the seam after the true anon close. (2) Test the seam through the genuine path (0xD2 + handle_irq + 50-tick spin + seam poll), not around it; ownership restored before asserts so aborts can't leak routing. (3) TF_RELEASE vs JARVIS_TEST follows the file's existing idiom per test shape (daemon-interacting P2 test is debug like cross_boundary's, not release like the serial loopbacks).
- **Adapted:** service::-scope `shell_try_get_key` wrapper (zero-logic) + header decl; P2 test (exactly-once + no ring double-delivery); shell_interaction 19→20, ui 73→74.
- **Style re-surface:** seam = declaration + zero-logic wrapper only; behavior proven by the pre-existing callers running unchanged.

### #303 item 4 — IOCD MMIO policy handler (closed)
- **Learned:** (1) A daemon cannot broker caps without new ABI (no cap transfer over IPC msgs; no client-CNode handle in the daemon) — so the honest split is daemon POLICY (who/which bar index, grant records, unregister lifecycle) + kernel MECHANISM (MmioCaps, drain_task authoritative); document the split, don't fake the grant. (2) The daemon replies to `msg.args[0]`, never to the IPC sender — a test probing with a bogus pid strands its own send_sync (both sides wedge: daemon's reply-send fails fast but the requester never gets a reply). Probe with live pids only; transient register/unregister pairs keep fixtures zero-residue. (3) Stale-graph claims ("no MMIO handler") may mean "stubs only" — read the file; the -1 stubs existed and shaped the handler contract.
- **Adapted:** MAP/UNMAP/UNREGISTER_DEVICE(207) handlers + 8-entry grant table + iocd.hpp protocol doc (incl. args[0] routing caveat); 3 stubs fleshed with live-IPC assertions; driver E2E (daemon grant + real cap + SYS_MMIO_MAP/UNMAP in fixture cspace).
- **Style re-surface:** policy tests assert decisions (0/-1), never PTEs; PTE teardown stays kernel-tested (cap_mmio*, drain_task).

### #376 slice 2 — boot-text W^X harden (closed)
- **Learned:** (1) Never publish NX on a live text page, not even briefly: try-map installs non-exec + flushes internally, so text must install executable (RWX window, safe) before the RO encode — the first boot triple-faulted exactly on the NX window (supervisor I-fetch #PF err=0x11 at the harden lambda). (2) Split fills must carry the FULL permission set or post-harden splits silently re-widen: x86 dropped NX, aarch64 dropped AP bits (its RO check additionally required AP=11, misreporting EL1-only RO — auditor-caught S2); riscv was already complete. (3) Post-harden, HHDM fixture VAs must live above the image range (low 2MiB entries are split, no longer huge) — three vmm tests relocated to idx 16/17/18 with identical-strength asserts, not weakened masks. (4) aarch64 encode_map_leaf kernel+exec must clear UXN+PXN (previously always set — no other caller uses that combination, verified).
- **Adapted:** harden_kernel_half (text RX / rodata RO+NX / data+bss RW+NX, panic on hole/failure, pre-snapshot); x86 kslot NX; aarch64 AP_RO-only RO predicate; boot_wx_pins test; counts page_tables 13, core_memory 120.
- **Style re-surface:** harden = try-map(exec) + encode_ro + text-exec hook + flush per page (uniform across arches via dispatched primitives); identity + non-image HHDM stay RWX by design (documented, not drift).

### #380/#381/#382 — v0.5.4 release-gate triple (closed, audit pending)
- **Learned:** (1) EFER is per-CPU: the AP trampoline set LME only, so any NX kernel page (#376) RSVD-faulted the AP with no IDT yet (#PF e=8 -> #GP -> #DF -> triple -> -no-reboot exit 0, zero S-lines). Convict with QEMU -d int (fault chain + EFER=0x500 NXE-clear) + GDB ap_main tracing, not by re-reading the scheduler. (2) A class with 0 tests exits fast via auto-shutdown, so the harness (debugcon watcher) can only see it if the notice reaches debugcon — Logger routes to COM1 while the suite is inactive; mirror explicitly. (3) QEMU rejects `-mon none` (no such chardev) but accepts `-monitor none`; the file-backend rewrite must use the proven spelling. (4) Python 3.4+ sets CLOEXEC on os.open (PEP 446): a double-fork daemon that dup2s a Python-opened log then execvps runs with CLOSED stdout/stderr — use os.set_inheritable first, or the gate spews EBADF and mangles test-history. (5) Mangled test-history lines from your own broken run may be excised by line (evidence preserved in the gate log); never fabricate replacements — the clean re-run re-records.
- **Adapted:** trampoline LME|NXE (#380, one imm32, no layout shift); run_filtered debugcon mirror (#381); QEMU_FLAGS_FILE -> `-monitor none` (#382); issues closed with evidence + audit-pending note; v0.5.4 released (debug 26/26 1836/1836, release 24/24 89/89).
- **Style re-surface:** BSP/AP register parity asserted at the bring-up site (comment cites both sides); harness-visible notices go to the harness-watched stream; build-system rewrites need a live q35 + empty-class proof, not just base classes.

### #355 slices A–C — console/RTC/input parity (kernel track landed, QE H3–H6 pending)
- **Learned:** (1) QEMU `-kernel` on aarch64 passes NO DTB (GDB-proven x0==0 at higherhalf_entry + full-RAM FDT-magic sweep empty) — DTB-primary designs need an architected slot-table fallback on that path (same board-knowledge class as UART/GIC maps); riscv gets its DTB via a1 and the DTB path works there. (2) QEMU `virt` virtio-mmio exposes the LEGACY (v1) interface (Version reads 1 — GDB-proven on live slots): queue setup is GuestPageSize/Align/PFN with one contiguous chunk, not Ready/addresses; submit/notify/ISR paths are version-independent. (3) `uint32_t addr = pci_make_addr(...)` truncates the aarch64 ECAM HHDM VA to 0x10000004 and faults — FAR-proof by low32 match; the callee takes uint64_t, x86 ports fit either way. (4) PL031 PeriphID carries a revision nibble (QEMU rev 1 → 0x00141031): mask PID2[7:4] for identity. (5) `fdt_first_child` returned NOTFOUND for any parent with properties (QEMU root has cells/compatible first) — skip PROP/NOP to the first BEGIN_NODE. (6) RISC-V `sd` immediate is 12-bit: L3[0x101] needs explicit address arithmetic (0x808 > 2047). (7) Release aarch64 boots silent and never schedules (stuck at vectors, zero reschedules) — pre-VBAR fault or pre-scheduler hang, NOT #355 code by construction (nothing new runs before arch init); filed as #393 under #357 (release gates). Debug E2E is proven both arches; release E2E follows the boot fix.
- **Adapted:** drivers.md §10 (RTC/input/serial contracts) + scheduler.md §11 pointer + BRIEFING gotcha #22; HAL `available()`/`source_name()`; Goldfish latch + mtime loud-fallback; decoder-first ordering at `shell_try_get_key_impl`; H1/H2 test edits (dev-owned, authorized); #391/#392/#393 filed with mechanism proofs.
- **Style re-surface:** single-sourced geometry shared by allocator + fit check (virtio_legacy_used_off); end-contained window checks (base+size, overflow-safe); `#if`-absence (not silent no-op) for release-cut features; test-history rows stamped per-run from log mtimes, never batched.

### #357 slices A–C — framebuffer gap + gates + release-boot H-1 (non-SIL3 review)
- **Learned:** (1) Quoted-string `-D` macros do not survive generated build rules — bare tokens + in-code stringify with a static_assert that fails loudly without the flag (proves the wiring both ways). (2) A `-j4` failure inside a vendored sub-build (picolibc sysroot) is not your Makefile's race — serial green + kernel-side -j safety is the gate; document the vendored limit, don't chase it. (3) Release-only boot death with zero serial + stuck-at-vectors PC means "fault before VBAR": read VBAR_EL1/ESR/FAR directly, then single-step from the entry — but the fastest convict is static: grep the release disassembly of the entry path for SIMD before the FP enable (movi v31 + stur q31 vs zero in debug). (4) `make release` bakes the CURRENT test-config — always reset to `none` first and assert the stamp before QEMU, or you boot the wrong suite silently. (5) Gate lists come from measured rows only; a per-arch `make test-full` census is the membership proof — hand-summed lists rot on the first red class.
- **Adapted:** `build_info.hpp` (constexpr identity + asserts); `TARGET_`→`NEXIOS_` bare-token flags on all five flag sets; `TEST_FULL_/SELFTEST_CLASSES_<arch>` + env-consuming script + baked-config stamp + separate riscv release `.bin`; `docs/specs/test-harness.md` §8 gate matrix + BRIEFING §§6–7; #393 (SIMD-before-fp_enable, Shape-A CPACR fix) with GDB + objdump + E2E evidence; #394 follow-up filed before the gap line references it.
- **Style re-surface:** §5 error policy (loud errors over silent fallbacks); §6 debug/release parity (prove both, gate both); test-history rows measured-only, SUMMARY carries ARCH.

### #354 — PCI ECAM + IRQ/MSI delivery parity, kernel track (audit APPROVED, committed, issue OPEN for QE)
- **Learned:** (1) QEMU `-kernel` never sets x0 for a generic ELF (no ARM64 boot-header magic) — GDB-proven 0x0 with `-dtb` present; the only working aarch64 DTB path is embedding a trimmed dump (boot.S substitutes phys only when x0==0, bootloader always wins). `dumpdtb` pads to 1 MiB with `totalsize` covering padding — trim to `off_strings + size_strings` (FDT field order: size_strings at +32, NOT +36) and patch `totalsize`, else 1 MB bloat or silent truncation. (2) `dtb_from_boot_info()` requires the blob inside a usable region — with the embedded DTB the memory node is skipped by design, so discovery must run AFTER the fallback region is recorded (boot-log "no validated DTB" convicted the ordering live). (3) Logger `%lx` already emits the `0x` prefix (`print_hex`) — a literal `0x%lx` doubles it (virtio_mmio carries the same latent cosmetic bug). (4) dmesg catalog codes collide silently at push time — check `dmesg_catalog.hpp` before picking `+N` (my `+11` duplicated framebuffer's; `+12` + catalog row fixed it; logging_debug 36/36 covers it). (5) Paper gates lose to test reality: planner C28 (compile out BMDMA off-x86) broke 8 green tests live (`BmDmaChannel(0xFF00)` is the engine's mock-port harness) — convict access removals with a live run first, revert verbatim, document why the API stays neutral (MSI fail-closed already bars real devices). (6) claim_slot arch traps: riscv `SYSCALL=2` lies INSIDE the PLIC window so its reject is load-bearing (never x86-gate it), while x86 `entries_` are always populated so an `IDT::has_handler` probe there is vacuous (gate it non-x86 only). (7) GIC/PLIC permit higher-priority same-CPU preemption → `IrqGuard` before the slot lock off-x86 (deadlock otherwise); x86 ISRs run masked, so x86 keeps lock-only (no behavior delta).
- **Adapted:** `d279308a0` (Q3 handoff, iter-3 APPROVED), `4df9be00a` (Slice A ECAM + MSI gates), `021788c9f` (Slice B PLATFORM delivery, D1–D3 verified SAFE), `4dcdb4f7c` (Slice C PRD/cache, C28 rejection confirmed correct); `drivers.md` §5.2 + §2.3, `irq-early-init.md` §7, dmesg catalog `+12`; `tools/trim_dtb.py` (+ `build/virt.dtb` git-ignored QEMU dump, `-dtb` flag kept for intent); per-arch `QEMU_PCI_TEST_DEV=virtio-rng-pci` (non-bound, BAR/MSI-X-cap exercise); #391 also covers the aarch64 `dma_completion_interrupt` instance (history-proven identical signature).
- **Style re-surface:** fail-closed error codes over truncation (CODING_STYLE §5: reachable exhaustion → error, never panic/mask); Thundering tests are read-only — the 8-test spike convicted the planner, not the tests (reverted the code, not the expectations); cross-arch parity ships real-or-documented-equivalent every slice (riscv keeps OpenSBI DTB, aarch64 embeds; riscv fence-only cache is exact under QEMU coherence).

### #389 — cross-CPU TLB shootdown triaged to v0.5.7 (no kernel change, issue OPEN as tracker)
- **Learned:** (1) A caller-less choke point rots: `VMM::tlb_shootdown` has zero callers while unmap/teardown keep parallel local invalidates — the adoption step is cheapest while the hook is still a no-op (behavior-identical, provable), and gets expensive once SMP semantics are in play; track it explicitly or it diverges silently. (2) Single-hart QEMU cannot prove cross-CPU behavior — fan-out code written now would be unprovable dead code; keep the weak default + tracker, build the fan-out where SMP exists to test it (#351). (3) Milestone triage is a valid pipeline outcome for a "future work, NOT this issue's gate" issue: verify premise (graphify + code, no drift), record the hazard, move the milestone, coordinate owners (#351 design, #386 state) — no planner, no diff, no audit when the issue text forbids kernel change.
- **Adapted:** #389 moved v0.5.5 → v0.5.7 Enable Multicore SMP (joins #351 + #386); verification + hazard record posted on the thread; no code, no tests, no audit artifacts (nothing to audit).

### #394 — aarch64/riscv64 framebuffer backend (virtio-gpu + ramfb, kernel track APPROVED, issue OPEN for QE)
- **Learned:** (1) QEMU `-kernel` DTBs carry no `virtio,gpu-device`-style node — discover MMIO transports via `compatible="virtio,mmio"` + DeviceID-register filter (id 16); the #350 DTB helpers + slot fallback both needed zero changes. (2) `VMM::virt_to_phys_err` rejects sub-page addresses — DMA staging must be page-aligned with manual offsets; GDB `used_idx==0 && avail_idx==0` pre-notify convicts phys resolution, not the device. (3) Static BSS has a boot budget: 4 MiB tripped riscv's `kernel_phys_end < 0x80C00000` fail-stop (`!` + scause faults) — big boot buffers belong to PMM (pre-baseline, zero Tracker delta). (4) fw_cfg MMIO: u8 data reads auto-advance; SELECT is BE16; DMA needs the file index in `control[31:16]` (SELECT bit) — selector-register write alone targets file 0 (read-only → ERROR spin); fast-fail on ERROR instead of spinning to timeout. (5) Verify magic constants char-by-char ("QEMU" transposed to "QUMG" passed review, died live — disassembly is the convictor). (6) Per-arch DTB addresses differ (fw-cfg `0x09020000` aarch64 vs `0x10100000` riscv) — dumpdtb per arch, never assume; riscv's sits inside the UART 2MB Device leaf (no new boot mapping). (7) GDB `x/` on Device MMIO misleads (faults/partial reads) — use in-guest Logger evidence for MMIO contents, GDB for control flow.
- **Adapted:** `deffcb09c` (Slice A virtio-gpu, APPROVED), `29a01132a` (Slice B ramfb, APPROVED iter-2 after S2 retrieval cure), `db035736a` (Slice C docs/gates); `drivers.md` §5.3 + BRIEFING §7; `QEMU_FB_GPU`/`QEMU_FB_RAMFB` override topology (GPU-wins-both, ramfb-only, gap-baseline-identical); `ui` 20/54 → 53/54 both arches (single pre-existing cpuinfo), x86 74/74; release boots `fb=virtio-gpu` both arches.
- **Style re-surface:** fail-closed validation at every device boundary (probe/find/backing/DMA each return false independently); TEMP-DIAG fully reverted post-hunt (grep-verified); no uncommitted `build/virt*.dtb` (gitignored QEMU dumps, trimmed+embedded or dumped fresh per run).

### #387 — riscv64 fork FORK-FAILED + folded PLIC/HHDM failures (CLOSED 2026-10-21, kernel+tests, SIL3 APPROVED)
- **Learned:** (1) Fail-closed is ownership-keyed or it misfires: #363's unconditional `return false` tripped on kernel-owned L0[0] MMIO blocks that `clone_kernel_pml4` embeds — convicted by TEMP Serial probe (`lvl=1 l0=0 l1=96 u=0 uf=0`) then reverted verbatim. (2) SBI `Serial::putchar` is the safe TEMP-probe channel in lock-free VMM context (debugcon no-ops off x86; Logger risks locks). (3) `vmm_arch_block_base(entry,huge)` is uniform across arches (riscv ignores `huge`) — use it instead of per-arch phys codecs. (4) Test fixtures must be production-shaped: U=0/user-frame huge entry is a hand-made corner no kernel path creates; the U bit is load-bearing (2M-rounding defeats the frame limb). (5) #349 moved the PLIC driver to S-context but not the tests — M-context reads gave firmware's threshold=7; value 7 = OpenSBI owns context 0. (6) #376 harden splits image-range HHDM blocks — scratch tests must live at verified blocks (L1[8+]; L1[6..7] boot-reserved); `riscv_snapshot_l1` reads LIVE tables. (7) `arch::HHDM_OFFSET` lives in `src/lib/constants.hpp`, not an arch header. (8) LSP evaluates headers in the wrong arch config (x86 HHDM → static_assert "2 == 258") — the build is truth.
- **Adapted:** `vmm_leaf_is_user_owned` + inherit-by-value at 4 block sites; `hal/uart.hpp` + `hal/memmap.hpp` single-source literals (user rule: never hardcode addresses — arch HAL owns them); serial.cpp rerouted; S3 cures post-APPROVED (doc placement, huge=true) re-gated green.
- **Style re-surface:** doc blocks must directly precede their decl (split free_user_pages doc = attachment regression); `huge` flag must match the free path or carry an alignment note; no-hardcode rule proposed for CODING_STYLE.md.

### #383 — deadline budget-fork flake (CLOSED 2026-10-21, test-only, SIL3 APPROVED)
- **Learned:** (1) Repro bar for flakes: identical binary + controlled load (>2 via `yes` procs, uptime-sampled per run) + 3 consecutive runs — 2/3 FAIL mode-2 (line 356, PMM +33 issue signature) convicted the drain→fork window; middle run green on the same binary. (2) Fix shape: move the racy window INSIDE the dispatched task under one IrqGuard (1-vCPU class = system-wide quiescence) with RAII drain (CAS loop kills mode 1, mask kills mode 2) — precedents #267 (tick-mask) + file-local add_task-under-IrqGuard idiom. (3) Always reap `yes` procs after the hunt (lingering load poisons later gates). (4) Stash-prove off-arch failures on unmodified main before claiming pre-existing (aarch64 EL1 + riscv64 unhandled exc after 8/10, both). (5) `TEST_SERIAL_LOG=<path>` env routes the serial log per run — no guessing log paths.
- **Adapted:** `BudgetHold` in test_sched_admission.cpp; strengthened asserts (g_drained/g_remaining_after/g_children_after); release-deadline 0-test runs still prove compile/link.
- **Style re-face:** no dead struct members (`zeroed_` written-never-read — auditor S3); history rows need real measured TIME, never placeholders.

### #304 — x86 post_code + pre-serial checkpoints (CLOSED 2026-10-21, S1 additive, SIL3 APPROVED)
- **Learned:** (1) UEFI-only El Torito ISOs need OVMF pflash on manual QEMU runs (SeaBIOS hangs silently — mirror the harness flags, don't hand-roll). (2) isa-debugcon chardevs give byte-exact host-side proof for write-only ports (0x300 exactly `a1a2a3`; 0x80 by non-zero tail — io_wait floods 0x00, SeaBIOS pre-handoff codes ignored). (3) File-backed chardevs flush only on guest exit — never SIGKILL proof runs; use isa-debug-exit self-exit (release shell ISO has none — capture bytes then kill). (4) LSP "undeclared identifier" on arch asm primitives in HAL headers is wrong-arch evaluation noise — the build is truth.
- **Adapted:** `arch::post_code(u8)` + BootPost 0xA1/0xA2/0xA3; debugd.md §8 implementation note with capture recipe.
- **Style re-face:** history rows must match the `<class> PASSED/FAILED/TIME` schema (auditor S3) — no free-form proof rows.

### #305 — multiboot2 tag6 fail-closed validation (CLOSED 2026-10-21, SIL3 APPROVED iter-2)
- **Learned:** (1) POST the work-begun retrieval comment BEFORE implementing — iter-1 REJECT was procedural check-7 with zero code findings; the comment is part of the deliverable, not ceremony. (2) `mb2_find_tag` bounds the walk but NOT the found tag's own size — consumers must clamp by info-remaining. (3) PMM sizes to `max(base+size)` over BootInfo regions — per-entry overflow rejection is load-bearing, not hygiene. (4) `init` is not a class name (`task_init` is) — check test_registry.cpp:718 mapping before running. Rebuild-after-arch-switch can trip the 120s stall guard — not a guest hang.
- **Adapted:** pure `mb2_mmap_validated_count` (QE-unit-testable); gs getter satisfies the global_state_api style gate (raw `multiboot_info_ptr` is E2).
- **Style re-face:** retrieval comment first, always.

### #306 — aarch64/riscv64 dump_cpu_regs captures (CLOSED 2026-10-21, SIL3 APPROVED)
- **Learned:** (1) Single-asm stack-save capture is mandatory for GPR dumps — multi-block `mov` output regs can clobber unread GPRs. (2) pkv2 unused-function -Werror on riscv: all de-guarded helpers must be consumed by every arm. (3) `exc_table` is x86-only (weak 0-test stub off-x86) — never cite it as off-arch evidence. (4) Release selftest 0/0 on aa64/rv64 proves link+boot only — label rows link-boot, not coverage. (5) `spsr 0x60000345` (EL1h) in the wild confirms sane capture.
- **Adapted:** stp/sd stack-save shapes; IrqGuard-before-CSR discipline; serial-expect markers (x0/elr/spsr/far/ttbr; x1/x31/sp/sepc/stval/satp).
- **Style re-face:** de-guarded helpers need all-arm consumers; array symmetry may leave a documented dead slot (auditor S3, harmless).

### #308 — first-chance raw crash dump + halt (CLOSED 2026-10-21, SIL3 APPROVED)
- **Learned:** (1) x86 trap frame: regs[17]=RIP [18]=CS [19]=RFLAGS [20]=RSP (user only) — CS-gate the SP (x86_fault_sp) or print garbage. (2) aarch64 save_all: x0-29 @[0..239], x30@240, sp_el0@248(=[31]), elr@[32], spsr@[33] — dump_regs was off by one. (3) New .cpp needs `rm mk/cpp-rules.*.gen.mk` rule regen (no rule for target). (4) GAS wants `msr daifset, #2`, not bare `daifset`. (5) Test TUs compile in release: CONFIG_DEBUG seam users must be #if-guarded (+registrations). (6) `daifset`-style park replaces the ELR+=4 skip — silent continuation after kernel faults is a correctness violation.
- **Adapted:** crash_dump module (format/emit/halt + seam); hook bool contract; vectors.S SP arg + park; riscv SP/SATP extension; spec doc updates.
- **Style re-face:** unused static after caller removal = -Werror — attribute or gate immediately; `__attribute__((unused))` preferred over dead-code retention.

### #307 — DMA fail-closed + cache maintenance (CLOSED 2026-10-21, SIL3 APPROVED iter-2)
- **Learned:** (1) replaceAll on identical code blocks can miss a near-duplicate (handle_irq drain had different indentation) — the auditor's S2 caught it; always grep all call sites after a repeated-pattern edit. (2) Invalidate-then-latch with a local is the only safe shape for device-written status bytes (stale 0 == OK). (3) Spin/fast-path used-idx reads stay fence-only (timing) — maintenance belongs at publish + confirmed-completion points. (4) sizeof(type) everywhere, no byte literals — the no-hardcode rule extends to maintenance ranges.
- **Adapted:** bm_prd_addr_ok predicate (QE-unit-testable); AHCI CT/CH clean + read/write/IDENTIFY points; virtio publish/submit/finish/ISR points in store-clean order.
- **Style re-face:** verify repeated-pattern edits with a post-edit grep of every site.
