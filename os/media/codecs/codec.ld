/* B310E OS codec module (os/codec.h): one image linked at 0, the
 * codec_module_t first, then code, read-only data and data; bss follows
 * (not in the file). Linked with -q so that mkapp.py sees the relocations.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
ENTRY(codec_module)

SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.codec_module))
        *(.text .text.* .icode .icode.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.* .irodata .irodata.*)
    }
    .data : ALIGN(4) {
        *(.data .data.* .idata .idata.*)
        . = ALIGN(4);
    }
    __image_end = .;
    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.* .ibss .ibss.* COMMON)
        . = ALIGN(8);
    }
    __bss_end = .;
    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*) *(.init_array*) *(.fini_array*)
        *(.got*) *(.plt*) *(.interp) *(.dynamic*)
    }
}
