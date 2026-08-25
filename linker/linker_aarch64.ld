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

    . = 0x40080000;

    .boot_text : ALIGN(4K)
    {
        *(.boot.text)
        *(.boot.rodata)
    } :boot_text

    .boot_data : ALIGN(4K)
    {
        *(.boot.data)
        *(.boot.bss)
    } :boot_data

    .boot_stack : ALIGN(4K)
    {
        _stack_start_low = .;
        *(.boot_stack)
        _stack_end_low = .;
    } :boot_data

    . = ALIGN(., 4K);
    boot_vma_end = .;
    phys_lma = boot_vma_end;

    /* Higher-half aliases of the boot stack bounds.  After MMU enable the
     * kernel runs via TTBR1_EL1 and the live boot-stack SP is
     * HHDM_OFFSET + low-VMA address (boot.S adds 0xFFFF800000000000).
     * The scheduler's is_boot_stack_rsp() range-checks the live RSP against
     * these, so they must be the higher-half alias.  Defined as plain linker
     * absolute symbols (no section), so references materialise as immediates
     * and never emit an AArch64 ADR_PREL_PG_HI21 relocation against a low-VMA
     * section from higher-half .text. */
    _stack_start = 0xFFFF800000000000 + _stack_start_low;
    _stack_end = 0xFFFF800000000000 + _stack_end_low;

    . = 0xFFFF800000000000 + phys_lma;

    .text : AT(phys_lma) ALIGN(4K)
    {
        _text_start = .;
        *(.vectors)
        *(.text*)
        *(.text.*)
        _text_end = .;
    } :text
    phys_lma = ALIGN(phys_lma + SIZEOF(.text), 4K);

    .rodata : AT(phys_lma) ALIGN(4K)
    {
        _rodata_start = .;
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
        *(.data*)
        *(.data.*)
        *(.got)
        *(.got.plt)
        _data_end = .;
    } :data
    phys_lma = ALIGN(phys_lma + SIZEOF(.data), 4K);

    .bss : AT(phys_lma) ALIGN(4K)
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        *(.bss.*)
        _bss_end = .;
    } :data

    kernel_phys_end = ALIGN(phys_lma + SIZEOF(.bss), 4K);
    kernel_virt_end = ALIGN(., 4K);
}
