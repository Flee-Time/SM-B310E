/* B310E OS app: one image linked at 0, app_info first, then code, read-only
 * data and data; bss follows (not in the file). Linked with -q so that
 * mkapp.py sees the relocations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
ENTRY(app_info)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.app_info))
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.*)
    }
    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    }
    __image_end = .;
    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
    }
    __bss_end = .;
    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*) *(.init_array*) *(.fini_array*)
        *(.got*) *(.plt*) *(.interp) *(.dynamic*)
    }
}
