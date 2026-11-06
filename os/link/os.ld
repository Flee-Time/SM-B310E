/* B310E OS link map. The SD boot menu copies the raw image to the PSRAM
 * base and jumps to its first word in ARM state.
 *
 *   0x04000000  vectors, code, data, bss (stacks, the vector page table)
 *   __heap_*    kernel heap: everything up to the frame buffer
 *   0x043f2000  LCD frame buffer, 128 x 160 x 2 bytes (FRAME_ADDR)
 *   0x043fc000  MMU translation table, 16 KB (TTB_ADDR)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)

MEMORY
{
    PSRAM (rwx) : ORIGIN = 0x04000000, LENGTH = 0x3f2000
}

SECTIONS
{
    .text :
    {
        KEEP(*(.vectors))          /* must be first: the menu jumps here */
        *(.text.start)
        *(.text .text.*)
        *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
    } > PSRAM

    .rodata : { *(.rodata .rodata.*) } > PSRAM

    .data : { *(.data .data.*) } > PSRAM

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(1024);
        __l2_vectors = .;          /* coarse page table for 0xfff00000 */
        . += 1024;
        . = ALIGN(8);
        . += 1024; __exc_stack_top = .;   /* undefined/abort dumps */
        . += 256;  __fiq_stack_top = .;
        . += 2048; __irq_stack_top = .;   /* interrupt dispatch */
        . += 4096; __svc_stack_top = .;   /* boot, then the scheduler */
        . = ALIGN(4);
        __bss_end = .;
    } > PSRAM

    __heap_start = ALIGN(32);
    __heap_end = 0x043f2000;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
