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
    . = 0x200000;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.init_array)
        *(.init_array.*)
    }

    .boot_text : ALIGN(4K)
    {
        KEEP(*(.multiboot_header))
        *(.boot.text)
        *(.boot.rodata)
    } :boot_text

    .boot_data : ALIGN(4K)
    {
        *(.boot.data)
        *(.boot.bss)
    } :boot_data

    . = ALIGN(., 4K);
    boot_vma_end = .;
    phys_lma = boot_vma_end;

    . = 0xFFFF800000000000 + boot_vma_end;

    .text : AT(phys_lma) ALIGN(4K)
    {
        _text_start = .;
        KEEP(*(.marker.text.start))
        *(.text*)
        *(.text.*)
        *(.ltext*)
        *(.ltext.*)
        KEEP(*(.marker.text.end))
        _text_end = .;
    } :text
    phys_lma = ALIGN(phys_lma + SIZEOF(.text), 4K);

    .rodata : AT(phys_lma) ALIGN(4K)
    {
        _rodata_start = .;
        KEEP(*(.marker.rodata.start))
        *(.rodata*)
        *(.rodata.*)
        *(.lrodata*)
        *(.lrodata.*)
        . = ALIGN(8);
        _expected_code_crc = .;
        QUAD(0x0000000000000000)
        KEEP(*(.marker.rodata.end))
        _rodata_end = .;
    } :rodata
    phys_lma = ALIGN(phys_lma + SIZEOF(.rodata), 4K);

    .data : AT(phys_lma) ALIGN(4K)
    {
        _data_start = .;
        KEEP(*(.marker.data.start))
        *(.data*)
        *(.data.*)
        *(.ldata*)
        *(.ldata.*)
        *(.got)
        *(.got.plt)
        /* Static constructor / destructor tables.  The kernel does not run
           these in normal builds (globals are POD or explicitly initialised),
           but -fprofile-arcs registers each translation unit's gcov profile
           through a constructor in .ctors.<priority>.  They are collected
           INSIDE .data (never as their own output section): a separate output
           section would break this segment's constant VMA<->LMA delta, and
           GRUB loads by LMA, so the table would land in unloaded memory. */
        /* MUST be pointer-aligned: the tables are arrays of function
           pointers, and an unaligned base makes every entry a byte-shifted
           fragment of two adjacent pointers (a 2-byte skew is enough to turn
           a valid 0xffff8000_0022xxxx entry into garbage and fault on call). */
        . = ALIGN(16);
        __ctors_start = .;
        KEEP(*(.ctors))
        KEEP(*(.ctors.*))
        __ctors_end = .;
        . = ALIGN(16);
        __dtors_start = .;
        KEEP(*(.dtors))
        KEEP(*(.dtors.*))
        __dtors_end = .;
        KEEP(*(.marker.data.end))
        _data_end = .;
    } :data
    phys_lma = ALIGN(phys_lma + SIZEOF(.data), 4K);

    .bss : AT(phys_lma) ALIGN(4K)
    {
        _bss_start = .;
        KEEP(*(.marker.bss.start))
        *(COMMON)
        *(.bss*)
        *(.bss.*)
        *(.lbss*)
        *(.lbss.*)
        KEEP(*(.marker.bss.end))
        _bss_end = .;
    } :data
    phys_lma = ALIGN(phys_lma + SIZEOF(.bss), 4K);

    .stack_guard : AT(phys_lma) ALIGN(4K)
    {
        _stack_start = .;
        KEEP(*(.marker.stack.before))
        *(.boot_stack)
        KEEP(*(.marker.stack.after))
        _stack_end = .;
    } :data

    kernel_phys_end = ALIGN(phys_lma + SIZEOF(.stack_guard), 4K);
    kernel_virt_end = ALIGN(., 4K);
}