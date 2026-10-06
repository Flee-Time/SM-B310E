/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Karl Kurbjun
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/app.lds
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/app.lds)
 *
 * Rockbox is linked FIXED at 0x04000000 (the SC6530 PSRAM window,
 * MEM_REMAP=1). The boot menu streams the image to this exact address and
 * branches to it — NO relocation, NO -pie. The image must therefore fit in
 * DRAMSIZE (see below) and the whole 4 MB budget is:
 *
 *   image + audiobuf  (DRAMSIZE)      -> ENDAUDIOADDR
 *   codec buffer      (CODEC_SIZE)    -> ENDADDR
 *   plugin buffer     (PLUGIN_BUFFER_SIZE) -> LCD_FRAME_ADDR
 *   framebuffer       (LCD_BUFFER_SIZE)    -> TTB_BASE_ADDR
 *   translation table (TTB_SIZE)      -> 0x08000000
 *
 * The LCDC DMA reads FRAME (LCD_FRAME_ADDR); the crt0 identity MMU makes
 * the CPU see the same physical address.
 */

ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/sc6530c/crt0.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#include "cpu.h"
#define DRAMSIZE (MEMORYSIZE * 0x100000) - PLUGINSIZE - CODECSIZE - LCD_BUFFER_SIZE - TTB_SIZE

#define DRAMORIG 0x04000000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    DRAM : ORIGIN = DRAMORIG, LENGTH = DRAMSIZE
}

SECTIONS
{
    /* The image is already at its load address (0x04000000) when the menu
     * branches to it — loadaddress == DRAMORIG, nothing to copy.
     */
    loadaddress = 0x04000000;

    .vectors DRAMORIG :
    {
        _vectorstart = .;
        KEEP(*(.vectors*));
        *(.init.text)
        . = ALIGN(0x4);
    } > DRAM

    .text :
    {
        _textstart = .;
        *(.text*)
        *(.icode*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
    } > DRAM

    _textcopy = LOADADDR(.text);

    .rodata :
    {
        *(.rodata*)
        *(.irodata*)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        *(.idata*)
        . = ALIGN(0x4);
    } > DRAM

    /DISCARD/ :
    {
        *(.eh_frame)
    }

    _initdata_end =.;

    .stack (NOLOAD) :
    {
       *(.stack)
       stackbegin = .;
       . += 0x2000;
       stackend = .;
    } > DRAM

    .bss (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(.ibss*)
        *(COMMON)
        . = ALIGN(0x4);
       _end = .;
    } > DRAM

    .audiobuf (NOLOAD) :
    {
    	. = ALIGN(4);
        _audiobuffer = .;
        audiobuffer = .;
    } > DRAM

    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
        audiobufend = .;
        _audiobufend = .;
    } > DRAM

    .codec ENDAUDIOADDR  (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    }

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
