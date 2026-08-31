; NexIOS RTOS — Development Roadmap / Kernel Core
; Copyright (C) 2026 Arnold Hasshold
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <https://www.gnu.org/licenses/>.

extern handle_interrupt_c
extern scheduler_save_rsp_to
extern scheduler_load_rsp_from
extern scheduler_load_cr3_from
extern scheduler_load_kstack_base
extern scheduler_load_kstack_top
extern scheduler_switch_generation
extern scheduler_kernel_cr3
extern scheduler_next_task_id
extern scheduler_on_context_switch
extern scheduler_validate_pending_switch
extern scheduler_record_skip
extern scheduler_abort_switch_fixup
extern scheduler_diag_pre_save
extern scheduler_diag_depth_skip
extern scheduler_diag_rsp_abort
extern isr_nesting_depth
extern irq_entry_tsc

; Uniform-frame discipline (exception-table-audit.md §3.2): every vector
; reaches isr_common with an 8-byte vector + 8-byte error slot at [rsp]/[rsp+8].
; NOERR vectors push a synthetic 0 so the frame is byte-identical to ERR
; vectors (which get the CPU-pushed error code).  The %assign audits below
; pin each macro's push depth: if the two classes ever diverge, the build
; fails instead of silently producing a misaligned iretq frame.
%assign isr_frame_bytes_noerr 0
%assign isr_frame_bytes_err 0
%assign isr_noerr_count 0
%assign isr_err_count 0
%assign isr_err_mask_0_31 0

%macro ISR_NOERR 1
global isr_%1
isr_%1:
    push 0
    push %1
    jmp isr_common
%assign isr_frame_bytes_noerr isr_frame_bytes_noerr + 16
%assign isr_noerr_count isr_noerr_count + 1
%endmacro

%macro ISR_ERR 1
global isr_%1
isr_%1:
    push %1
    jmp isr_common
%assign isr_frame_bytes_err isr_frame_bytes_err + 8
%assign isr_err_count isr_err_count + 1
%assign isr_err_mask_0_31 isr_err_mask_0_31 | (1 << %1)
%endmacro

ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_ERR   28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

; Assemble-time frame audit (exception-table-audit.md §3.1): both classes must
; yield an identical 16-byte stub frame (NOERR = synthetic 0 + vec, ERR =
; 8-byte CPU error code + vec).  Pins each macro's push depth so a future edit
; that breaks uniformity fails the build.
%if isr_frame_bytes_noerr != isr_noerr_count * 16
%error "ISR_NOERR stub frame changed — uniform-frame invariant violated"
%endif
%if isr_frame_bytes_err != isr_err_count * 8
%error "ISR_ERR stub frame changed — uniform-frame invariant violated"
%endif

section .text
isr_common:
    inc qword [rel isr_nesting_depth]

    ; Capture TSC at ISR entry — rdtsc clobbers RAX and RDX
    push rax
    push rdx
    rdtsc
    shl rdx, 32
    or  rax, rdx
    mov [rel irq_entry_tsc], rax
    pop rdx
    pop rax

    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, [rsp + 15*8]
    mov rsi, [rsp + 16*8]
    mov rdx, [rsp + 17*8]
    mov rcx, rsp
    mov r8, [rel irq_entry_tsc]

    call handle_interrupt_c

    cli
    mov rax, [rel scheduler_save_rsp_to]
    test rax, rax
    jz .restore

    ; Perform context switch at any nesting depth ≤ 2 (normal = 1, SYSCALL+timer = 2).
    ; Deeper nesting (≥ 3) indicates a bug — skip to avoid stack corruption.
    cmp qword [rel isr_nesting_depth], 2
    jbe .depth_ok
    ; H2 depth-skip audit (cold: fires only when a pending apply is skipped due
    ; to excessive ISR nesting).  Preserve every GPR .restore pops.
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call scheduler_diag_depth_skip
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    jmp .restore
.depth_ok:

    ; Generation-lock: capture the generation that published this deferred-switch
    ; pair (load_rsp_from / load_cr3_from).  RCX is preserved across the
    ; diagnostic call below (the push/pop block includes RCX).
    mov rcx, [rel scheduler_switch_generation]

    ; Diagnostic: check RSP before saving (preserve caller-saved regs)
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call scheduler_diag_pre_save
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rax

    ; Verify the deferred switch is still the one we captured.  If the
    ; generation advanced, a concurrent publisher superseded/partially
    ; overwrote this pair — applying it would use a half-written RSP/CR3 pair
    ; (the H2 race).  Skip the apply and leave the atoms untouched: a
    ; superseding publisher's own arm is in place, and a stale arm is ignored
    ; by the next ISR's fresh generation check.
    cmp rcx, [rel scheduler_switch_generation]
    je .gen_ok
    ; H2 ring: record the generation-skip (captured vs current) before
    ; abandoning the apply.  Cold path.  Preserve every GPR that .restore pops.
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov rdi, rcx
    mov rsi, [rel scheduler_switch_generation]
    call scheduler_record_skip
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    jmp .restore
.gen_ok:
    mov rax, [rel scheduler_save_rsp_to]
    test rax, rax
    jz .restore

    mov [rax], rsp
    ; Hold the old RSP for the apply-side abort (RBX is restored by the
    ; .restore pops below, so clobbering it here is safe).
    mov rbx, rsp

    ; Apply-side liveness + ownership re-check (H2, IF=0): the arm side
    ; validated the target's frame at publish time, but the arm can survive
    ; past its ISR (nested-ISR depth guard / generation-skip) and the target
    ; task can be terminated + freed in between (IF=1) or the published RSP can
    ; drift from its CURRENT kernel stack (snapshot restore / free+reuse).
    ; Verify the target still exists (id_table_) AND the published RSP lies
    ; within its live kernel stack (or the harness boot stack) BEFORE loading
    ; it — otherwise the iretq would resume on freed/foreign memory (the [H2W]
    ; orphan displacement).  Runs with IF=0, so no task-context removal can
    ; interleave.  RAX is the C return value (1=apply, 0=abort) and is NOT in
    ; the preserve set; the original RAX sits deeper in the ISR frame.  RBX is
    ; callee-saved and still holds the abort RSP.
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call scheduler_validate_pending_switch
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    test rax, rax
    jz .abort_switch

    mov rsp, [rel scheduler_load_rsp_from]
    mov qword [rel scheduler_load_rsp_from], 0
    mov qword [rel scheduler_save_rsp_to], 0

    ; Apply-side RSP-owner check (H2): the deferred switch must resume the
    ; dispatched task ON ITS OWN kernel stack.  If the loaded RSP is outside
    ; [scheduler_load_kstack_base, scheduler_load_kstack_top) — a stale/foreign
    ; value from a split-phase or nested-ISR switch — refuse to iretq onto it.
    ; Restore the original RSP and fall through to .restore (iretq back to the
    ; current task); the dropped switch is harmless and retried next tick.
    mov rcx, [rel scheduler_load_kstack_base]
    mov rdx, [rel scheduler_load_kstack_top]
    cmp rsp, rcx
    jb .h2_rsp_abort
    cmp rsp, rdx
    jae .h2_rsp_abort
    jmp .rsp_owner_ok
.h2_rsp_abort:
    ; H2 asm RSP-owner abort audit (cold): dump the rejected RSP and the kstack
    ; range so we can confirm whether the freeze is this silent abort.  RBX
    ; still holds the original RSP.  Preserve every GPR .restore pops.
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    call scheduler_diag_rsp_abort
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    jmp .abort_switch
.rsp_owner_ok:

    ; Context switch complete — update current_index_ to the next task
    push rax
    push rcx
    push rdx
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    call scheduler_on_context_switch
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rax

    ; Reset nesting depth — the old task may have had a pending SYSCALL
    ; (depth=1) that was absorbed by the context switch.  The new task
    ; must start from a clean depth so its own ISRs nest correctly.
    mov qword [rel isr_nesting_depth], 1

    ; Load the next task's CR3.  If no CR3 was published for this switch
    ; (kernel/harness context) or it was consumed, fall back to the static
    ; kernel PML4 so the harness can never resume on a stale user CR3 from a
    ; previous task (the H2 freeze path).
    mov rax, [rel scheduler_load_cr3_from]
    test rax, rax
    jnz .load_cr3
    mov rax, [rel scheduler_kernel_cr3]
    test rax, rax
    jz .restore
.load_cr3:
    mov cr3, rax
    mov qword [rel scheduler_load_cr3_from], 0
    jmp .restore

.abort_switch:
    ; The deferred switch's load RSP is outside the dispatched task's kernel
    ; stack (stale/foreign pair — H2).  Abort: restore the original RSP, clear
    ; the pending-switch atoms so the next tick publishes fresh, and iretq back
    ; to the current task.  RBX still holds the original RSP.
    mov qword [rel scheduler_save_rsp_to], 0
    mov qword [rel scheduler_load_cr3_from], 0
    mov qword [rel scheduler_next_task_id], -1
    mov rsp, rbx

    ; Rebind TSS.RSP0 to the continuing task's kernel stack: the arm side may
    ; have set it to the aborted next-task's stack (user-task dispatch).  A
    ; subsequent ring-3→ring-0 transition would otherwise push its iretq frame
    ; onto a freed/foreign stack.  Preserve every GPR that .restore pops below
    ; (including RBX) across the call — RSP already points at the ISR frame.
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push rbx
    call scheduler_abort_switch_fixup
    pop rbx
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

.restore:
    ; NOTE: do NOT clear scheduler_save_rsp_to here.  If we reach .restore via
    ; jne .restore (nested ISR, depth != 1), the outer ISR's pending context
    ; switch must be preserved.  The successful switch path at line 117 already
    ; clears it, so any other path must leave it for an outer ISR epilogue.
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    dec qword [rel isr_nesting_depth]
    iretq

global __isr_vector
__isr_vector:
%assign i 0
%rep 256
    dq isr_%+i
%assign i i+1
%endrep

; Bitmask of the 0-31 vectors declared ISR_ERR (CPU-pushed error code).
; Consumed by the exc_table test class (err_macro_consistency) so a future
; misclassification of any vector is caught as a test failure.
section .rodata
global __isr_vectors_err_mask
__isr_vectors_err_mask:
    dq isr_err_mask_0_31
