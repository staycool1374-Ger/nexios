// Issue #354 (Q3): embedded QEMU virt DTB for aarch64.
//
// QEMU `-kernel` loads a generic ELF (no ARM64 Linux boot-header magic), so
// no DTB arrives in x0. This object embeds QEMU's own dumped virt DTB
// (build/virt.trimmed.dtb, totalsize-trimmed by the Makefile rule), 8-aligned
// for the in-tree libfdt's direct u32 dereferences. boot.S passes its
// physical address to higherhalf_entry only when x0 == 0; a bootloader DTB
// always wins. The kernel uses the embedded blob for device discovery only
// and ignores its memory node by design (dump-time -m goes stale; RAM sizing
// stays on the proven fallback). aarch64-only: this TU builds solely under
// arch/aarch64 (mk/rules.mk SRC_S_ARCH); riscv64 keeps its OpenSBI DTB.

.section .rodata.virt_dtb, "a", @progbits
.balign 8
.globl _binary_virt_dtb_start
_binary_virt_dtb_start:
.incbin "build/virt.trimmed.dtb"
.globl _binary_virt_dtb_end
_binary_virt_dtb_end:
