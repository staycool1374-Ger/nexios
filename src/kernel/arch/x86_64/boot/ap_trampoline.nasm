; NexIOS RTOS — SMP AP trampoline (issue #25, Phase B3)
;
; Position-locked 16-bit startup blob for application processors.  Assembled
; with `nasm -f bin` and `org 0x70000`; the BSP copies the raw bytes to
; physical 0x70000 and wakes each AP with SIPI vector 0x70
; (CS:IP = 0x7000:0x0000, linear 0x70000).  Self-contained: carries its
; own GDT (null + 32-bit code + 64-bit code + data) and reads all handoff
; state from the param block at offset 0x800 (filled by the BSP AFTER
; the copy).  Total size must stay below 0x800 bytes (asserted by the
; smp_bringup test class via _binary_ap_trampoline_size).
;
; Stages: 16-bit real mode -> 32-bit protected -> 64-bit long mode ->
; far-jump to ap_main (high virtual address, shared kernel PML4).
; Interrupts stay disabled throughout (cli at entry, no sti, no IDT load).

org 0x70000
bits 16

ap_entry16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000              ; temp real-mode stack (free low RAM)
    ; NOTE: 32-bit address-size override (0x67) is MANDATORY here: the
    ; GDT lives at org-absolute 0x707xx, which does not fit the default
    ; 16-bit displacement (it would truncate to 0x07xx and load a garbage
    ; GDT -> #GP -> triple fault).  With a32, DS:disp32 = 0:0x707xx.
    lgdt [dword gdt_ptr]        ; absolute (org-locked) address
    mov eax, cr0
    or eax, 1                   ; PE: enter protected mode
    mov cr0, eax
    ; NOTE: o32 (0x66) is MANDATORY: a 16-bit far jump carries a 16-bit
    ; offset (prot32 at 0x7031 would truncate to 0x31 and land in the
    ; IVT/BDA — wild execution -> triple fault).  The 0x66 prefix makes
    ; it ptr16:32 with the full 32-bit offset.  NASM `dword' forces it.
    jmp dword 0x08:prot32       ; far jump flushes the prefetch queue

bits 32
prot32:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, 0x20                ; PAE
    mov cr4, eax
    mov eax, [pml4_phys]        ; BSP-filled kernel PML4 physical address
    mov cr3, eax
    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or eax, 0x100               ; LME: enable long mode
    wrmsr
    mov eax, cr0
    or eax, 0x80000000          ; PG: enable paging -> long mode active
    mov cr0, eax
    jmp 0x10:long64

bits 64
long64:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [stack_top]        ; BSP-filled AP park stack (high VA)
    mov rcx, 0xC0000101         ; GS_BASE: AP's PerCpu page (high VA)
    mov rax, [percpu_va]
    mov rdx, rax
    shr rdx, 32
    wrmsr
    mov rdi, [logical_id]       ; arg0 for ap_main
    mov esi, [lapic_id]         ; arg1 (32-bit LAPIC ID)
    mov rax, [ap_entry]         ; high VA of ap_main (shared PML4)
    jmp rax                     ; ap_main never returns (hlt park)
.hang:
    hlt
    jmp .hang

align 8
; GDT lives at the FIXED offset TRAMPOLINE_GDT_OFF (0x700, see hal/smp.hpp)
; so tests can address it without tracking code size.  `times' fails the
; build if the stage code ever overflows into it (fail-closed layout).
times 0x700 - ($ - $$) db 0
gdt64:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF       ; 32-bit protected code, selector 0x08
    dq 0x00209A0000000000       ; 64-bit code, selector 0x10
    dq 0x00CF92000000FFFF       ; flat 4 GiB data, selector 0x18
                                ; (N.B.: 32-bit protected mode ENFORCES
                                ; segment limits — a zero-limit data
                                ; descriptor #GPs on first use.  Long
                                ; mode ignores limits, which is why the
                                ; boot GDT gets away with limit 0.)
gdt_ptr:
    dw gdt_ptr - gdt64 - 1
    dd gdt64                    ; absolute low address (org-locked)

; ─── Param block at offset 0x800 (BSP fills after copying the blob) ──────────
; C++ side (hal/smp.hpp) mirrors these offsets as TRAMPOLINE_PARAM_OFF + k*.
times 0x800 - ($ - $$) db 0
pml4_phys:  dq 0                ; +0x00 kernel PML4 physical address
stack_top:  dq 0                ; +0x08 AP stack top (high VA, grows down)
percpu_va:  dq 0                ; +0x10 &per_cpu[logical] (high VA)
ap_entry:   dq 0                ; +0x18 ap_main virtual address
logical_id: dq 0                ; +0x20 logical CPU index
lapic_id:   dd 0                ; +0x28 LAPIC ID
            dd 0                ; +0x2C padding
