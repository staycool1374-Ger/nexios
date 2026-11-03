.section .text
.globl trap_vector
.globl syscall_entry
.type trap_vector, @function
.type syscall_entry, @function

# Must be 4-byte aligned for stvec Direct mode (low 2 bits = 0)
.balign 4
.equ SAVE_SIZE, 296

# Register save area offsets (from sp) — up here because the entry swap
# path (issue #206 E1) references OFF_* before the classic save block:
#   0-240:   x1-x31 (GPRs)
#   248: sepc
#   256: sstatus
#   264: scause
#   272: stval
#   280: return value (for syscalls)
#   (total SAVE_SIZE = 296)
.equ OFF_RA,   0
.equ OFF_SP,   8
.equ OFF_GP,   16
.equ OFF_TP,   24
.equ OFF_T0,   32
.equ OFF_T1,   40
.equ OFF_T2,   48
.equ OFF_S0,   56
.equ OFF_S1,   64
.equ OFF_A0,   72
.equ OFF_A1,   80
.equ OFF_A2,   88
.equ OFF_A3,   96
.equ OFF_A4,   104
.equ OFF_A5,   112
.equ OFF_A6,   120
.equ OFF_A7,   128
.equ OFF_S2,   136
.equ OFF_S3,   144
.equ OFF_S4,   152
.equ OFF_S5,   160
.equ OFF_S6,   168
.equ OFF_S7,   176
.equ OFF_S8,   184
.equ OFF_S9,   192
.equ OFF_S10,  200
.equ OFF_S11,  208
.equ OFF_T3,   216
.equ OFF_T4,   224
.equ OFF_T5,   232
.equ OFF_T6,   240
.equ OFF_SEPC, 248
.equ OFF_SSTAT, 256
.equ OFF_SCAUSE, 264
.equ OFF_STVAL, 272
.equ OFF_RET,  280

trap_vector:
syscall_entry:
    # Clear SIE first (uses no registers): no nested trap can interleave
    # entry past this point — a tick *before* it restarts entry pristinely.
    # See the SIE note below (before `csrr t0, sstatus`) for rationale.
    csrci sstatus, 2
    # Issue #206 (E1): land on the current task's own kernel stack.
    # sscratch is a pure one-word spill (never read for value); the swap
    # source is the published scheduler_load_kstack_top slot.  S-mode
    # traps (sp already a kernel stack incl. boot stack) save directly;
    # U-mode traps (user sp) swap to the slot's top.  Discrimination is by
    # SPP (no sp-sign heuristics: boot phys and user VAs share the low
    # range), using sscratch itself as the spill so no GPR is lost.
    csrrw t0, sscratch, t0   // spill entry-t0; t0 = stale (discarded)
    # SIE is already clear (csrci at function top, before any state change):
    # no nested trap can interleave entry, so the SPP branch below always
    # sees hardware-fresh state.  Rationale (issue #206): a nested trap
    # reads a STALE SPP (the outer trap's trapped-from marker) and would
    # take the wrong entry path over the live frame (nested S-tick with
    # SPP=0 clobbered OFF_SP).  sret restores SIE from SPIE (U-ticks keep
    # working); S-mode callers keep their trapped SIE.  Closes #219 too.
    csrr t0, sstatus
    andi t0, t0, 0x100       // SPP bit: 1 = S-mode, 0 = U-mode
    beqz t0, 41f
    # from_s: recover entry-t0 (spill still intact — nothing read it).
    csrr t0, sscratch
    addi sp, sp, -SAVE_SIZE
    j 42f
41:
    # from_u: sscratch holds user-t0; x2 holds the user sp; t0 holds the
    # discardable SPP==0 test result (dead).  x2 IS sp: reloading sp below
    # would destroy the user sp before its OFF_SP save (issue #206 S1 —
    # the slot recorded the frame base).  Park user-sp in dead t0 first.
    mv t0, x2                # t0=user_sp (parked; x2 dies at sp reload)
    la sp, scheduler_load_kstack_top
    ld sp, 0(sp)             # sp=kstack top (user sp safe in t0)
    addi sp, sp, -SAVE_SIZE
    # Park user-t0 via swap (csrrw): t1 = user-t0, sscratch = user-t1.
    # A plain csrr here would clobber user-t1 (x6) before its save.
    csrrw t1, sscratch, t1
    sd t1, OFF_T0(sp)
    sd x1, OFF_RA(sp)
    sd t0, OFF_SP(sp)        # F[1]=user_sp (t0 intact: nothing above wrote t0)
    sd x3, OFF_GP(sp)
    sd x4, OFF_TP(sp)
    # Reload user-t1 from the swap slot into its own save slot.
    csrr t1, sscratch
    sd t1, OFF_T1(sp)
    j 44f
42:
    sd x5, OFF_T0(sp)
    sd x1, OFF_RA(sp)
    sd x2, OFF_SP(sp)
    sd x3, OFF_GP(sp)
    sd x4, OFF_TP(sp)
    sd x6, OFF_T1(sp)
44:
    # Shared saves continue (x7-x31; x2 holds the pre-trap sp value which
    # OFF_SP preserves for the sret restore)
    sd x7, OFF_T2(sp)
    sd x8, OFF_S0(sp)
    sd x9, OFF_S1(sp)
    sd x10, OFF_A0(sp)
    sd x11, OFF_A1(sp)
    sd x12, OFF_A2(sp)
    sd x13, OFF_A3(sp)
    sd x14, OFF_A4(sp)
    sd x15, OFF_A5(sp)
    sd x16, OFF_A6(sp)
    sd x17, OFF_A7(sp)
    sd x18, OFF_S2(sp)
    sd x19, OFF_S3(sp)
    sd x20, OFF_S4(sp)
    sd x21, OFF_S5(sp)
    sd x22, OFF_S6(sp)
    sd x23, OFF_S7(sp)
    sd x24, OFF_S8(sp)
    sd x25, OFF_S9(sp)
    sd x26, OFF_S10(sp)
    sd x27, OFF_S11(sp)
    sd x28, OFF_T3(sp)
    sd x29, OFF_T4(sp)
    sd x30, OFF_T5(sp)
    sd x31, OFF_T6(sp)

    # Save CSRs
    csrr t0, sepc
    sd t0, OFF_SEPC(sp)
    csrr t0, sstatus
    sd t0, OFF_SSTAT(sp)
    csrr t0, scause
    sd t0, OFF_SCAUSE(sp)
    csrr t0, stval
    sd t0, OFF_STVAL(sp)

    # TRACE prints removed (issue #206: they interleave every Logger print
    # under ticks and were slated for removal before acceptance).

    # Increment isr_nesting_depth (matches x86_64: wrap entire handler)
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    addi t1, t1, 1
    sd t1, 0(t0)

    # Load scause
    ld t0, OFF_SCAUSE(sp)

    # Check if interrupt (bit 63 set — negative when signed)
    bge t0, zero, 1f

    # === Interrupt ===
    # Call handle_plic_trap(scause, sepc, sp)
    ld a0, OFF_SCAUSE(sp)
    ld a1, OFF_SEPC(sp)
    mv a2, sp
    call handle_plic_trap
    j 5f

1:  # === Exception ===
    li t1, 8                   # ECALL from U-mode
    beq t0, t1, 6f
    li t1, 9                   # ECALL from S-mode
    beq t0, t1, 6f
    # Other exceptions: SPP-gated dispatch (issue #206 M2) — U-mode faults
    # terminate the task, S-mode faults panic. Pass frame + sstatus.
    mv a0, sp
    csrr a1, sstatus
    call riscv64_exception_dispatch

6:  # Syscall (ECALL): syscall_handler(num, a0, a1, a2, a3, regs)
    # §2 ABI (issue #185; docs/specs/syscall-abi-picolibc.md): number rides
    # in user a7, args in a0-a3 (src/libc/syscall.h __syscall5) — mirrors
    # the aarch64 stub (x8=num, x0-x3=args). C args go in a0-a5.
    # sepc advances past the 4-byte ecall so sret does not re-trap.
    ld t0, OFF_SEPC(sp)
    addi t0, t0, 4
    sd t0, OFF_SEPC(sp)
    ld a0, OFF_A7(sp)          # number = user a7
    ld a1, OFF_A0(sp)          # arg0
    ld a2, OFF_A1(sp)          # arg1
    ld a3, OFF_A2(sp)          # arg2
    ld a4, OFF_A3(sp)          # arg3
    mv a5, sp                  # regs pointer
    call syscall_handler
    sd a0, OFF_A0(sp)         # store return value into saved a0 slot

5:  # === Return path / context switch check ===
    # Apply at outermost nesting (depth == 1). Nested timer ticks
    # (depth == 2, SYSCALL+timer) MUST also apply: a task blocked in a
    # syscall hlt-loop publishes its voluntary arm and only a depth-1
    # epilogue could otherwise apply it — the hlt-loop never reaches one,
    # so without this the arm strands (RMS sees the pending arm and returns
    # early every tick: total stall). Mirrors the isr_stubs.asm 0xE0-depth-2
    # rule on x86_64 (issue #221). Depth >= 2 covers single-hlt shapes
    # (task syscall-hlt + tick) and deeper steady-state stacks (init's
    # daemon-wait + several daemon recv-hlts + tick: live depth 4
    # observed — any depth cap reintroduces the stall for N blocked
    # daemons). This is accounting-sound: each apply abandons exactly
    # its own trap level (the tail dec below drops it), while held
    # outer frames resume intact with their own decs on exit. Frames
    # live on their own tasks' kstacks, so deep nesting cannot overflow
    # a single stack. Deeper-than-1 non-timer sources still skip. The existing `sd sp` save below is
    # already correct for the nested case: sp is the nested frame base and
    # resuming there completes the nested return back to the hlt-loop, so
    # no save-side adjustment is made (cf. outer-frame reasoning).
    # Issue #221: terminate-driven arms force-apply (one-shot). A
    # terminating task's epilogue must apply its switch-away arm even
    # when the depth/source gate below would skip it — sret back into
    # the dead task re-faults with the task TERMINATED (double-fault
    # park, IRQs masked). Set by switch_away_from_terminating; cleared
    # here on consume (save_rsp_to==0 still skips below, harmlessly).
    la t0, scheduler_force_apply
    ld t1, 0(t0)
    beqz t1, check_depth
    sd zero, 0(t0)
    j apply
check_depth:
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    li t2, 1
    beq t1, t2, apply
    li t2, 2
    blt t1, t2, 7f
nested_timer:
    # Depth >= 2: allow the s_timer nested tick only (scause[63]=1,
    # cause 5); all other nested sources preserve the arm and skip.
    ld t0, OFF_SCAUSE(sp)
    bge t0, zero, 7f
    andi t0, t0, 0x7ff
    li t1, 5
    bne t0, t1, 7f
apply:
    # H2 generation-lock (mirrors x86_64 isr_stubs .depth_ok): capture the
    # publishing generation BEFORE touching the arm. A concurrent publisher
    # superseding or half-overwriting the pair between capture and apply
    # must not be applied (half-written RSP/CR3 pair).
    # H2 generation-lock per-hart (mirrors TLS fix 353-374): idx=(tp-base)>>12
    mv t2, tp
    la t1, arch_per_cpu_base
    ld t1, 0(t1)
    sub t2, t2, t1
    srli t2, t2, 12
    li t1, 8
    bltu t2, t1, 99f
    # Issue #440: foreign tp (U-mode task with unset TLS — exec zeroes it
    # and only TLS_SET restores it — or pre-percpu-init zero) falls back to
    # slot 0 instead of skipping: user tasks run ONLY on logical CPU 0
    # (enqueue_ready_inner CPU0 clamp), the BSP is always logical CPU 0,
    # and APs never run user tasks, so slot 0 is the rightful slot.
    # Without this, no deferred switch from user context EVER applies on
    # rv64 (the arm sits forever while the parked task spins in hlt-wait).
    # Revisit if APs ever run user tasks (post-C1 SMP).
    li t2, 0
99: slli t2, t2, 3
    la t3, scheduler_switch_generation
    add t3, t3, t2
    ld t3, 0(t3)
    # Check scheduler_save_rsp_to per-hart
    la t0, scheduler_save_rsp_to
    add t0, t0, t2
    ld t0, 0(t0)
    beqz t0, 7f
    # Verify the generation is unchanged per-hart
    la t1, scheduler_switch_generation
    add t1, t1, t2
    ld t1, 0(t1)
    bne t3, t1, gen_skip

    # Save current SP per-hart (pointer to save area)
    la t1, scheduler_save_rsp_to
    add t1, t1, t2
    ld t0, 0(t1)
    sd sp, 0(t0)

    # H2 apply-side liveness + ownership re-check (mirrors x86_64
    # scheduler_validate_pending_switch): the arm can outlive its trap and
    # the target can be terminated + freed + recycled in between (issue
    # #385: resume onto a recycled stack + sret into an unmapped yield
    # stub with stale current). Refuse to resume onto freed/foreign
    # memory. C call with 64 bytes reserved below the frame: only sp is
    # live here (ABI-preserved); every GPR is reloaded from the frame by
    # restore_all below; ra is stashed to match the on_context_switch
    # call convention used further down.
    addi sp, sp, -64
    sd ra, 0(sp)
    call scheduler_validate_pending_switch
    ld ra, 0(sp)
    addi sp, sp, 64
    # a0 = 1 apply / 0 abort (the validator already invalidated the atoms
    # and undid the publish side effects on abort).
    beqz a0, abort_switch

    # Re-derive per-hart offset: t2 (x7) is caller-saved and the
    # scheduler_validate_pending_switch call above may clobber it
    # (x86 preserves r11 via push/pop around the call).
    mv t2, tp
    la t1, arch_per_cpu_base
    ld t1, 0(t1)
    sub t2, t2, t1
    srli t2, t2, 12
    li t1, 8
    bltu t2, t1, 98f
    # Issue #440: foreign-tp fallback, same as the apply gate above
    # (tp is unchanged by the C call above — reserved, never clobbered).
    li t2, 0
98: slli t2, t2, 3
    # Load new SP per-hart (points to save area of next task)
    la t1, scheduler_load_rsp_from
    add t1, t1, t2
    ld sp, 0(t1)

    # Clear scheduler_save_rsp_to per-hart
    la t1, scheduler_save_rsp_to
    add t1, t1, t2
    sd zero, 0(t1)

    # Issue #206 (E1 note): sscratch is a pure entry spill, not a stack
    # pointer — the trap entry reads the kstack top from
    # scheduler_load_kstack_top directly, so nothing is maintained here.

    # Call scheduler_on_context_switch()
    addi sp, sp, -16
    sd ra, 0(sp)
    call scheduler_on_context_switch
    ld ra, 0(sp)
    addi sp, sp, 16

    # Re-derive per-hart offset: scheduler_on_context_switch may
    # clobber t2 (caller-saved); recompute from tp (reserved).
    mv t2, tp
    la t0, arch_per_cpu_base
    ld t0, 0(t0)
    sub t2, t2, t0
    srli t2, t2, 12
    li t0, 8
    bltu t2, t0, 97f
    # Issue #440: foreign-tp fallback, same as the apply gate above.
    li t2, 0
97: slli t2, t2, 3
    # Check SATP switch per-hart (FULL SATP via context_id_tag)
    la t0, scheduler_load_cr3_from
    add t0, t0, t2
    ld t0, 0(t0)
    beqz t0, 7f
    csrw satp, t0
    sfence.vma
    la t0, scheduler_load_cr3_from
    add t0, t0, t2
    sd zero, 0(t0)
    j 7f

gen_skip:
    # Torn publish: a concurrent publisher superseded the pair between
    # capture and apply. Record the skip and leave the atoms untouched
    # (the superseding arm — including its TLS slot — stays in place for
    # the next trap). Jump past the TLS consume so the pending TLS value
    # is not stolen. a0/a1 are reloaded from the frame by restore_all.
    mv a0, t3
    la t1, scheduler_switch_generation
    add t1, t1, t2
    ld a1, 0(t1)
    addi sp, sp, -64
    sd ra, 0(sp)
    call scheduler_record_skip
    ld ra, 0(sp)
    addi sp, sp, 64
    j 8f

abort_switch:
    # The validator refused the arm (stale target or foreign RSP) and
    # already invalidated the atoms + undid the publish side effects.
    # Rebind anything the arm side redirected, then return to the
    # current task. sp still points at the current frame (nothing was
    # loaded). The TLS slot was cleared by the validator, so falling
    # into the shared consume is a no-op.
    addi sp, sp, -64
    sd ra, 0(sp)
    call scheduler_abort_switch_fixup
    ld ra, 0(sp)
    addi sp, sp, 64
    j 7f

7:  # Issue #74: consume the TLS slot into the trap-frame TP save area.
    # Both SATP paths converge here. The slots are per-hart
    # ([CONFIG_MAX_CPUS], indexed by this_cpu() in C++): index by the
    # CURRENT hart here — a hardcoded slot 0 lets an AP timer tick steal
    # the BSP's pending TLS update AND clobber the AP's own tp with a
    # user VA (after which cpu_index() misderives to 0 and the AP
    # impersonates the BSP; issue #351 S3 triage). Derive the index from
    # tp itself (the per_hart base): idx = (tp - base) >> 12.
    # Issue #440: a foreign tp (U-mode task with unset TLS — exec zeroes
    # it, TLS_SET restores it later — or pre-percpu-init zero) falls back
    # to slot 0 instead of skipping: user tasks run ONLY on logical CPU 0
    # (enqueue_ready_inner CPU0 clamp) and the BSP is always logical CPU 0,
    # so slot 0 is the rightful slot (the old skip wedged every deferred
    # switch from user context). A direct tp write would die at the
    # OFF_TP restore below, and no sfence is needed for tp. Revisit if APs
    # ever run user tasks (post-C1 SMP).
    mv   t0, tp                       # current per_hart base (pre-consume)
    la   t1, arch_per_cpu_base
    ld   t1, 0(t1)                    # &per_cpu[0] (C-linkage anchor)
    sub  t0, t0, t1                   # byte offset into per_cpu[]
    srli t0, t0, 12                   # logical index (4 KiB pages)
    li   t1, 8                        # CONFIG_MAX_CPUS (range guard)
    bltu t0, t1, 96f                  # in range: use computed index
    li   t0, 0                        # foreign tp: slot 0 (see above)
96: slli t0, t0, 3                    # slot byte offset
    la   t1, scheduler_load_tls_from
    add  t0, t0, t1                   # &slots[current hart]
    ld   t1, 0(t0)
    sd   zero, 0(t0)
    beqz t1, 8f
    sd   t1, OFF_TP(sp)
8:  # Decrement nesting depth
    la t0, isr_nesting_depth
    ld t1, 0(t0)
    addi t1, t1, -1
    sd t1, 0(t0)

    # Restore CSRs
    ld t0, OFF_SSTAT(sp)
    # Issue #219: run the whole return with SIE=0 (clear bit 1 here); sret
    # restores SIE from SPIE, so ticks merely pend (~50 insns) instead of
    # nesting over the live frame mid-restore. SPP/SPIE preserved.
    andi t0, t0, -3
    csrw sstatus, t0
    ld t0, OFF_SEPC(sp)
    csrw sepc, t0

    # Restore all GPRs except x2/x5/x6 (x2 IS sp: restored conditionally
    # below; x5/x6 restore after E2 so no restored register is reused as
    # scratch — reusing t0 after its restore clobbered pre-trap t0).
    # OFF_SP preserves x2's entry value (the user sp for U-mode traps).
    ld x1, OFF_RA(sp)
    ld x3, OFF_GP(sp)
    ld x4, OFF_TP(sp)
    ld x7, OFF_T2(sp)
    ld x8, OFF_S0(sp)
    ld x9, OFF_S1(sp)
    ld x10, OFF_A0(sp)
    ld x11, OFF_A1(sp)
    ld x12, OFF_A2(sp)
    ld x13, OFF_A3(sp)
    ld x14, OFF_A4(sp)
    ld x15, OFF_A5(sp)
    ld x16, OFF_A6(sp)
    ld x17, OFF_A7(sp)
    ld x18, OFF_S2(sp)
    ld x19, OFF_S3(sp)
    ld x20, OFF_S4(sp)
    ld x21, OFF_S5(sp)
    ld x22, OFF_S6(sp)
    ld x23, OFF_S7(sp)
    ld x24, OFF_S8(sp)
    ld x25, OFF_S9(sp)
    ld x26, OFF_S10(sp)
    ld x27, OFF_S11(sp)
    ld x28, OFF_T3(sp)
    ld x29, OFF_T4(sp)
    ld x30, OFF_T5(sp)
    ld x31, OFF_T6(sp)

    # Restore sp last
    addi sp, sp, SAVE_SIZE
    # Restore t0 first via x6 as scratch (x6 still holds handler garbage,
    # so it is safe to reuse; t0 becomes live after its load).
    addi x6, sp, -SAVE_SIZE
    ld x5, OFF_T0(x6)
    # Issue #206 (E2): x2 IS the sp register, so it restores conditionally.
    # S-mode: sp is already correct (frame popped); the slot holds a stale
    # entry value that must NOT overwrite it — skip x2.  U-mode (SPP==0):
    # sp must become the user stack pointer from OFF_SP.  x6 is dead until
    # its restore below, so it serves as scratch.
    # (E2 debug print removed: printing inside the trap return is reentered
    # by ticks and corrupts Logger state.)
    csrr x6, sstatus
    andi x6, x6, 0x100
    bnez x6, 40f
    addi x6, sp, -SAVE_SIZE
    ld x2, OFF_SP(x6)
    # U-path: x2 IS sp, so sp is user_sp from here on — never recompute
    # the frame base from sp again (that faulted: user_sp-296 is not the
    # frame). x6 still holds the base: load t1 directly, then sret.
    ld x6, OFF_T1(x6)
    sret
40:
    # S-path: x2 (hence sp) untouched, so the sp recompute is valid here.
    addi x6, sp, -SAVE_SIZE
    ld x6, OFF_T1(x6)
    sret
