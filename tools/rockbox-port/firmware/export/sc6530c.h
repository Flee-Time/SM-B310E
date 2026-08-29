/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Marcoen Hirschberg
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
#ifndef __SC6530C_H__
#define __SC6530C_H__

/*
 * B310E-OS Rockbox port — b310e/export/sc6530c.h
 * (GPLv2, Rockbox-derived; modeled on Rockbox's export/s3c2440.h)
 *
 * SoC-level constants for the Spreadtrum SC6530C (ARM926EJ-S, ARMv5TE).
 * Register bases below are the B310E-OS ground truth (include/arch.h,
 * drivers/{lcd,keypad,sdio,led,audio}.c + kernel/irq.c — fpdoom-derived,
 * HW-verified). Rockbox is linked FIXED at 0x04000000 (the 4 MB PSRAM
 * window, MEM_REMAP=1) and runs WITHOUT the MMU's address translation in
 * M1 — the crt0.S identity TTB maps VA == PA, so FRAME is both the CPU and
 * the LCDC DMA's view of the framebuffer.
 *
 * Memory layout inside the 4 MB window (see app.lds):
 *   [0x04000000 .. ENDAUDIOADDR)   image + audiobuf (DRAMSIZE)
 *   [ENDAUDIOADDR  .. ENDADDR)     codec buffer  (CODEC_SIZE)
 *   [ENDADDR       .. LCD_FRAME_ADDR) plugin buffer (PLUGIN_BUFFER_SIZE)
 *   [LCD_FRAME_ADDR .. TTB_BASE_ADDR) framebuffer (LCD_BUFFER_SIZE)
 *   [TTB_BASE_ADDR .. 0x08000000)  translation table (TTB_SIZE)
 */

#define CACHEALIGN_BITS (5)
#define CACHEALIGN_SIZE (1 << CACHEALIGN_BITS)

#define LCD_BUFFER_SIZE (128*160*2)
#define TTB_SIZE (0x4000)
/* must be 16Kb (0x4000) aligned */
#define TTB_BASE_ADDR   (0x04000000 + (MEMORYSIZE*1024*1024) - TTB_SIZE)
#define LCD_FRAME_ADDR  (TTB_BASE_ADDR - LCD_BUFFER_SIZE)

#define TTB_BASE   ((unsigned long *)TTB_BASE_ADDR) /* End of memory */
#define FRAME      ((unsigned short *)LCD_FRAME_ADDR) /* Right before TTB */

/* ---- interrupt controller (kernel/irq.c) -------------------------------
 * The SC6530 INTC has NO INTOFFSET register: read pending, lowest set bit
 * with a registered handler wins. 0x8000000C is INT_DISABLE (a mask), NOT
 * an acknowledge — the peripheral ISR clears its own source. */
#define INT_PENDING (*(volatile unsigned long *)0x80000004)
#define INT_ENABLE  (*(volatile unsigned long *)0x80000008)
#define INT_DISABLE (*(volatile unsigned long *)0x8000000C)
#define TIMER_IRQ_MASK (1 << 23)   /* 1 ms system timer 2 -> line 23 */

/* ---- 1 ms system timer (timer 2 @ 0x81000040, 26 MHz, load 26000 = 1 ms)
 * ctl @ +8 = 0xc0 (enable), load @ +0, int @ +0xc = 1 (enable); the ISR
 * ACKs by writing 9 (bit3 clr + bit0 en) to +0xc when bit 2 is pending
 * (kernel/irq.c sys_timer_start / kernel/sched.c sys_tick_isr). */
#define SYS_TIMER2_ADDR  0x81000040
#define SYS_TIMER2_LOAD  26000

/* ---- peripherals (B310E-OS ground truth) ------------------------------ */
#define ADI_BASE      0x82000000 /* ADI mailbox (clock/analog bridge)     */
#define ANA_BASE      0x82001000 /* analog die registers                  */
#define WDG_BASE      0x82001480 /* watchdog (ADI)                        */
#define EIC_BASE      0x82001900 /* external interrupt ctrl (END/power)   */
#define VBC_BASE      0x82003000 /* voice band codec (8 kHz DA path)      */
#define KEYPAD_BASE   0x87000000 /* keypad matrix controller              */
#define SDIO_BASE     0x20700000 /* SDIO0 controller                      */
#define LCM_BASE      0x20800000 /* parallel DBI LCM controller           */
#define LCDC_BASE     0x20d00000 /* LCD display controller (DMA)          */
#define AHB_BASE      0x20500000 /* AHB power/reset gates                 */
#define APB_BASE      0x8b000000 /* APB power/peripheral ctl              */

#endif /* __SC6530C_H__ */
