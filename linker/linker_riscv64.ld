ENTRY(_start)

PHDRS
{
    boot_text PT_LOAD FLAGS(5);
    boot_data PT_LOAD FLAGS(6);
    text      PT_LOAD FLAGS(5);
    rodata    PT_LOAD FLAGS(4);
    data      PT_LOAD FLAGS(6);
}

SECTIONS
{
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.init_array)
        *(.init_array.*)
    }

    /* Physical load address (QEMU virt RAM starts at 0x80000000) */
    . = 0x80200000;

    /* Boot code stays identity-mapped for early MMU init */
    .boot : ALIGN(4K)
    {
        _boot_start = .;
        *(.text.boot)
        *(.text.boot.*)
        . = ALIGN(4K);
        *(.boot.bss)
        _boot_end = .;
    } :boot_text

    . = ALIGN(., 4K);
    boot_vma_end = .;
    phys_lma = boot_vma_end;

    /* Higher-half kernel virtual base.
     * .text loads at LMA 0x80207000 (0x7000 into the 2MB page at PA 0x80200000).
     * The boot page table maps PA 0x80200000 → VA 0xFFFFFFC080200000 (2MB page),
     * so .text at PA 0x80207000 is accessible at VA 0xFFFFFFC080207000.
     * Must match: VMA = HHDM_base + (LMA & 0x1FFFFF).
     */
    . = 0xFFFFFFC080207000;

    .text : AT(phys_lma) ALIGN(4K)
    {
        _text_start = .;
        *(.text*)
        *(.text.*)
        _text_end = .;
    } :text
    phys_lma = ALIGN(phys_lma + SIZEOF(.text), 4K);

    .rodata : AT(phys_lma) ALIGN(4K)
    {
        _rodata_start = .;
        *(.srodata*)
        *(.srodata.*)
        *(.rodata*)
        *(.rodata.*)
        . = ALIGN(8);
        _expected_code_crc = .;
        QUAD(0x0000000000000000)
        _rodata_end = .;
    } :rodata
    phys_lma = ALIGN(phys_lma + SIZEOF(.rodata), 4K);

    .data : AT(phys_lma) ALIGN(4K)
    {
        _data_start = .;
        *(.sdata*)
        *(.sdata.*)
        *(.data*)
        *(.data.*)
        *(.got)
        *(.got.plt)
        . = ALIGN(4096);
        _data_end = .;
    } :data
    phys_lma = ALIGN(phys_lma + SIZEOF(.data), 4K);

    .bss : AT(phys_lma) ALIGN(4K)
    {
        _bss_start = .;
        *(COMMON)
        *(.sbss*)
        *(.sbss.*)
        *(.scommon)
        _stack_start = .;
        KEEP(*(.marker.stack.before))
        *(.boot_stack)
        *(.boot_stack.*)
        KEEP(*(.marker.stack.after))
        _stack_end = .;
        *(.bss*)
        *(.bss.*)
        _bss_end = .;
    } :data

    phys_lma = ALIGN(phys_lma + SIZEOF(.bss), 4K);

    kernel_phys_end = phys_lma;
    kernel_virt_end = ALIGN(., 4K);
}
