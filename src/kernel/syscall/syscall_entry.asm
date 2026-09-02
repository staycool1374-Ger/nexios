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

; ============================================================================
; DEAD CODE (P7, issue #6) — NOT ASSEMBLED on any architecture.
;
; The LSTAR/sysret fastpath was removed: MSR_KERNEL_GS_BASE was never written,
; so the entry swapgs left GS base 0 and `mov [gs:0],rsp` faulted on phys 0 —
; a guaranteed kernel panic on any ring-3 `syscall` (0F 05).  The sole live
; syscall path is `int $0x80` (trap gate isr_128, GS-free).  The fastpath
; redesign lives in docs/specs/syscall-fastpath.md (v0.4.3).  This file is
; retained as a commented historical artifact pending explicit approval to
; delete (mk/rules.mk filters it out of SRC_ASM_GENERIC unconditionally).
; ============================================================================

extern syscall_handler

global syscall_entry
syscall_entry:
    swapgs
    mov [gs:0x00], rsp
    mov rsp, [gs:0x08]

    push 0
    push 0xFFFFFFFF80000000
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

    mov rdi, rax
    mov rsi, rbx
    mov rdx, rcx
    mov rcx, rdx
    mov r8, rsi
    mov r9, rdi

    call syscall_handler

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

    mov rsp, [gs:0x00]
    swapgs
    o64 sysret
