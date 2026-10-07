/* Real SC6530C crt0 supplies vectors, cache/MMU setup and exception stacks. */
ENTRY(start)
SECTIONS {
    . = 0x04000000;
    .vectors : { KEEP(*(.vectors*)) KEEP(*(.init.text)) }
    .text : { *(.text*) *(.icode*) }
    .rodata : { *(.rodata*) *(.irodata*) }
    .data : { *(.data*) *(.idata*) }
    .stack (NOLOAD) : { . = ALIGN(8); stackbegin = .; . += 8192; stackend = .; }
    .bss (NOLOAD) : { . = ALIGN(32); _edata = .; *(.bss*) *(.ibss*) *(COMMON) . = ALIGN(4); _end = .; }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) }
}
