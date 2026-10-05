ENTRY(_loader_entry)

SECTIONS
{
    . = 0;

    .text : ALIGN(4)
    {
        KEEP(*(.text._loader_entry))
        *(.text*)
        *(.rodata*)
    }

    .data : ALIGN(8)
    {
        *(.data*)
    }

    .bss : ALIGN(8)
    {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
