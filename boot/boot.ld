/* B310E boot loader link map.
 *
 * The image sits in NOR from 0x3fc000 (the boot vector, NOR word 0x20,
 * points there) and runs in place through the NOR window at 0x0. At a
 * power-on the flash controller decodes only 4 MiB: reads from 0x400000
 * up come back wrong until its size register (SFC 0x20a00200) says 8 MiB,
 * as the stock PBL sets it (NOR 0x10354). v1.1 drew its font, linked
 * above 0x400000, as solid boxes; v1.2 tried the 0x30000000 window, which
 * did not read the NOR at boot either. So:
 *
 *   .early   the header, the start-up, the boot decision and the helpers
 *            it calls: below 0x400000 (asserted). Chaining the stock never
 *            touches the rest.
 *   .text    everything else; start.S sets the flash size to 8 MiB and
 *            checks a marker at the image's end before running any of it
 *            (the stock firmware if it reads wrong).
 *   IRAM 0x40000000..0x40004000   .data and .bss (the menu path only),
 *                 where fpdoom's sdboot kept its .bss on this phone. From
 *                 0x4000c000 or so IRAM did not hold the loader's data on
 *                 the phone (v1.4: FatFs's volume table read back 0, "FAT
 *                 error 12"; gfx.c's corner table drew square corners).
 *   IRAM 0x40004000..0x40009000   the stack (inside sdboot's old stack)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)

NOR_LOAD = 0x003fc000;

MEMORY
{
    NOR  (rx)  : ORIGIN = 0x003fc000, LENGTH = 64K
    IRAM (rwx) : ORIGIN = 0x40000000, LENGTH = 16K
}

SECTIONS
{
    .early :
    {
        KEEP(*(.head))
        *(.early .early.*)
        . = ALIGN(4);
    } > NOR
    ASSERT(ADDR(.early) + SIZEOF(.early) <= 0x400000,
           ".early must stay below NOR 4 MiB")

    .text :
    {
        *(.text .text.*)
        *(.glue_7) *(.glue_7t) *(.vfp11_veneer) *(.v4_bx)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        KEEP(*(.tail))                  /* last: start.S checks it */
    } > NOR

    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > IRAM AT > NOR
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        KEEP(*(.bss.status))
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > IRAM

    ASSERT(__bss_end <= 0x40003f00, ".data and .bss must stay below the early save words")
    __stack_top = 0x40009000;
    lcd_back = 0x043f2000;          /* B310E OS gfx.c draws here: FRAME_ADDR */
    __image_size = LOADADDR(.data) + SIZEOF(.data) - NOR_LOAD;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
