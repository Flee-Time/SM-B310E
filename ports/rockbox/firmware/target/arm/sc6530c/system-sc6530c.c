/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Michael Sevakis
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
#include "kernel.h"
#include "system.h"
#include "panic.h"
#include "cpu.h"
#include "gcc_extensions.h"
#include "action.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "pcm-internal.h"
#include "audio-target.h"
#include <stdio.h>

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/system-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/system-s3c2440.c)
 *
 * The B310E boot menu has already done full chip init (PLL, SDRAM, IRAM,
 * power gating, keypad, SDIO) before branching to this image, so
 * system_init() is deliberately minimal. HARD SAFETY: no 0x8c000000
 * pinmux writes here (they hang the phone), no clock changes.
 */

/* ---- SC6530 interrupt controller (see firmware/export/sc6530c.h) ------ */
#define REG32(a) (*(volatile uint32_t *)(a))
#define INT_PENDING_REG  0x80000004
#define INT_DISABLE_REG  0x8000000C
#define TIMER_IRQ_MASK   (1 << 23)   /* 1 ms system timer 2 -> line 23 */

/* ---- bounded ADI mailbox (fpdoom/B310E-OS drivers/keypad.c pattern) ----
 * The ANA/analog die registers (0x8200xxxx, incl. the watchdog) are
 * written through this FIFO bridge. Budgeted so a wedged bridge degrades
 * to a no-op instead of hanging the cooperative scheduler. */
#define ADI_RD_CMD      0x82000018
#define ADI_RD_DATA     0x8200001C
#define ADI_FIFO_STS    0x82000020
#define ADI_FIFO_FULL   (1 << 9)
#define ADI_FIFO_EMPTY  (1 << 8)
#define ADI_BUDGET      1000000u

static uint32_t adi_read(uint32_t addr)
{
    uint32_t a = 0, n = ADI_BUDGET;

    REG32(ADI_RD_CMD) = addr & 0xfff;
    while ((a = REG32(ADI_RD_DATA)) >> 31)  /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)   /* FIFO full */
        if (--n == 0) return;
    REG32(addr) = val;
    n = ADI_BUDGET;
    /* fpdoom-exact: wait until FIFO_EMPTY is SET (write drained) */
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

/* Best-effort disarm of the SC6530 watchdog (WDG 0x82001480, ADI). The
 * menu path that launches us (USB fdl / menu UI) normally never arms it,
 * but a NOR-chain boot or a prior reboot may have left it counting — a
 * running watchdog would reset us ~0.5 s after entry. Mirror of the
 * B310E-OS menu_reboot() sequence (arch/diag_menu_main.c), inverted. */
static void watchdog_stop(void)
{
    adi_write(WDG_BASE + 0x20, 0xe551);        /* LOCK: unlock */
    {
        uint32_t ctrl = adi_read(WDG_BASE + 8);
        adi_write(WDG_BASE + 8, ctrl & ~0xFu); /* CTRL: clear enable/start */
    }
    adi_write(WDG_BASE + 0x20, ~0xe551u);      /* LOCK: relock */
}

/* ---- IRQ dispatch -------------------------------------------------------
 * Read masked status at 0x80000000 and dispatch only owned lines: tick 23,
 * playback DMA 20 (normal firmware), keypad 8, then user timer 4. Raw
 * status at +4 includes disabled lines and must not dispatch DMA while the PCM lock
 * masks it. Each handler acknowledges its own peripheral, preserving
 * unrelated interrupt enables. INT_ENABLE +8 is a full R/W mask and
 * INT_DISABLE +12 clears selected enables; neither acknowledges a source.
 */
void TIMER23(void);   /* kernel-sc6530c.c (strong, the kernel tick) */
void TIMER0(void);    /* timer-sc6530c.c (strong, the user timer) */
void KEYPAD(void);    /* button-sc6530c.c, capture/ack matrix edges */
#ifndef BOOTLOADER
void DMA(void);       /* pcm-sc6530c.c, paced stereo bank completion */
#endif

/* Stray-IRQ counter: incremented by irq_handler whenever pending holds bits
 * that have no registered handler (23, 20, 8 or 4). Read from a
 * debugger / the debug menu to confirm the stray-line source after a
 * session (a non-zero count = stray edges were seen and safely skipped). */
volatile uint32_t s_stray_irq_count = 0;

void irq_handler(void) __attribute__((interrupt ("IRQ"), naked));
void irq_handler(void)
{
    asm volatile (
        "sub    lr, lr, #4            \r\n"
        "stmfd  sp!, {r0-r3, ip, lr}  \r\n"
        "mov    r0, #0x80000000       \r\n" /* masked INT_STATUS */
        "ldr    r0, [r0, #0x00]       \r\n"
        "tst    r0, r0                \r\n"
        "beq    3f                    \r\n" /* nothing pending */
        "ldr    r1, =0x00800000       \r\n" /* TIMER_IRQ_MASK (1<<23) */
        "tst    r0, r1                \r\n"
        "ldrne  r1, =TIMER23          \r\n"
        "bne    2f                    \r\n"
#ifndef BOOTLOADER
        "tst    r0, #0x00100000       \r\n" /* DMA IRQ20 */
        "ldrne  r1, =DMA              \r\n"
        "bne    2f                    \r\n"
#endif
        "tst    r0, #0x00000100       \r\n" /* keypad IRQ8 */
        "ldrne  r1, =KEYPAD           \r\n"
        "bne    2f                    \r\n"
        "ldr    r1, =0x00000010       \r\n" /* TIMER0_MASK (1<<4) */
        "tst    r0, r1                \r\n"
        "ldrne  r1, =TIMER0           \r\n"
        "bne    2f                    \r\n"
        /* Enabled source without a registered handler: count it. Do not
         * acknowledge a peripheral we do not own. */
        "ldr    r1, =s_stray_irq_count \r\n"
        "ldr    r2, [r1]                \r\n"
        "add    r2, r2, #1              \r\n"
        "str    r2, [r1]                \r\n"
        "b      3f                      \r\n"
        "2:                           \r\n"
        "mov    lr, pc                \r\n"
        "bx     r1                    \r\n"
        "3:                           \r\n"
        "ldmfd  sp!, {r0-r3, ip, pc}^ \r\n"
    );
}

/* ---- power / reset ----------------------------------------------------- */

void system_reboot(void)
{
    /* Disarm IRQs, then arm the SC6530 watchdog for a 0.5 s reset
     * (B310E-OS menu_reboot, arch/diag_menu_main.c). */
    disable_interrupt(IRQ_FIQ_STATUS);
    REG32(INT_DISABLE_REG) = 0xFFFFFFFF;
    /* SC6530 analog and RTC watchdog clock SET aliases; fpdoom's
     * SC6530 reset path and stock clock code use these, not 0x1040. */
    adi_write(0x820010e0, 4);
    adi_write(0x820010e4, 2);
    adi_write(WDG_BASE + 0x20, 0xe551);
    {
        uint32_t ctrl = adi_read(WDG_BASE + 8);
        adi_write(WDG_BASE + 8, ctrl | 9);
    }
    adi_write(WDG_BASE, 0x4000);              /* LOAD_LOW  (0.5 s @ 32k) */
    adi_write(WDG_BASE + 4, 0);               /* LOAD_HIGH */
    {
        uint32_t ctrl = adi_read(WDG_BASE + 8);
        adi_write(WDG_BASE + 8, ctrl | 2);    /* CTRL: start */
    }
    adi_write(WDG_BASE + 0x20, ~0xe551u);     /* LOCK: relock */
    for (;;)
        ;
}

void system_exception_wait(void)
{
    REG32(INT_DISABLE_REG) = 0xFFFFFFFF;
    while (1)
        ;
}

#ifdef BOOTLOADER
void system_prepare_fw_start(void)
{
    tick_stop();
    disable_interrupt(IRQ_FIQ_STATUS);
    REG32(INT_DISABLE_REG) = 0xFFFFFFFF;
}
#else /* BOOTLOADER */
void system_prepare_fw_start(void)
{
    /* Not used outside the bootloader; keep the symbol for the vector
     * table branch. */
}
#endif /* BOOTLOADER */

/* ---- init -------------------------------------------------------------- */

/* FIFO-gated keylight boot marker (crt0.S): level 0-15; level 0xf = full
 * brightness proves main()'s init() reached system_init(). */
extern void b310e_boot_mark(unsigned level);

void system_init(void)
{
    /* BOOT MARKER (level 0xf = full bright): main() was reached. A hang
     * before this point decodes by the crt0 keylight levels (1 dim / 4
     * MMU / 8 pre-main). */
    b310e_boot_mark(0xf);

    /* Mask every interrupt line (the crt0 already did this, but repeat for
     * safety after the initial INTC writes). The tick ISR enables line 23
     * via INT_ENABLE when tick_start() runs. */
    REG32(INT_DISABLE_REG) = 0xFFFFFFFF;

    watchdog_stop();
}

int system_memory_guard(int newmode)
{
    (void)newmode;
    return 0;
}

/* Debug menu hooks (system.h). */
bool dbg_ports(void)
{
    return false;
}

bool dbg_hw_info(void)
{
    static const struct { const char *name; uint32_t addr; } regs[] = {
        { "UID15", 0x20102038 }, { "UID16", 0x2010203c },
        { "L CFG", 0x201010c8 }, { "R CFG", 0x20101088 },
        { "L IRQ", 0x201010cc }, { "R IRQ", 0x2010108c },
        { "L SRC", 0x201010d0 }, { "R SRC", 0x20101090 },
        { "DMA IRQ", 0x20100010 }, { "INT MASK", 0x80000008 },
        { "VBC", 0x82003018 }, { "DAC", 0x8a00200c },
        { "DA GATE", 0x8a002000 }, { "OWNER", 0x8b0001c4 },
        { "ADI FIFO", 0x82000020 },
    };
    uint32_t values[ARRAYLEN(regs)];
    struct sc6530_audio_debug info;
    bool saved = false;
    unsigned page = 0;
    lcd_setfont(FONT_SYSFIXED);
    for (;;)
    {
        lcd_clear_display();
        lcd_puts(0, 0, page == 1 ? "B310E analog" : page == 2 ? "B310E jack/gain" : "B310E audio");
        lcd_putsf(0, 1, "Playing: %d", pcm_is_playing());
        sc6530_audio_debug(&info);
        for (unsigned i = 0; i < ARRAYLEN(regs); i++)
        {
            /* Read only known control/status registers. VBC data ports
             * and unverified DSP shared addresses are deliberately absent. */
            values[i] = REG32(regs[i].addr);
            if (page == 0)
                lcd_putsf(0, i + 2, "%s %08lx", regs[i].name, (unsigned long)values[i]);
        }
        if (page == 1)
            for (unsigned i = 0; i < SC_AUDIO_ANALOG_COUNT; i++)
                lcd_putsf(0, i + 2, "%s %04x%s", sc_audio_analog_regs[i].name,
                          info.analog[i], (info.valid & (1u << i)) ? "" : " ERR");
        if (page == 2)
        {
            lcd_putsf(0, 3, "EIC DATA %08lx", (unsigned long)info.headset_data);
            lcd_putsf(0, 4, "EIC MASK %08lx", (unsigned long)info.headset_mask);
            lcd_putsf(0, 5, "APB CLK  %08lx", (unsigned long)info.headset_clocks);
            lcd_putsf(0, 7, "Headset: %d", !(info.headset_data & 1));
            lcd_putsf(0, 8, "Speaker: %d", info.speaker);
            lcd_putsf(0, 10, "Volume: %d/10 dB", info.volume);
            lcd_putsf(0, 11, "PCM: %d/10 dB", info.digital_volume);
            lcd_putsf(0, 12, "HP GAIN: %04x", info.analog[12]);
        }
        lcd_putsf(0, 17, "PCM %u/%u", info.peak[0], info.peak[1]);
        lcd_puts(0, 18, saved ? "Saved audio log" : "Center: save log");
        lcd_puts(0, 19, "Menu: change page");
        lcd_update();
        int action = get_action(CONTEXT_STD, HZ / 4);
        if (action == ACTION_STD_CANCEL)
            break;
        if (action == ACTION_STD_MENU)
            page = (page + 1) % 3;
        if (action == ACTION_STD_OK)
        {
            int fd = open(ROCKBOX_DIR "/audio-b310e.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
            {
                saved = fdprintf(fd, "B310E audio tick=%ld playing=%d\n",
                                 current_tick, pcm_is_playing()) >= 0;
                saved &= fdprintf(fd, "codec_initialized=%d adi_failed=%d speaker=%d volume_tenth_db=%d banks=%lu pcm_peak_L=%u pcm_peak_R=%u\n",
                                  info.initialized, info.adi_failed, info.speaker,
                                  info.volume, (unsigned long)info.banks, info.peak[0], info.peak[1]) >= 0;
                saved &= fdprintf(fd, "pcm_volume_tenth_db=%d headset_data=%08lx headset_mask=%08lx apb_clocks=%08lx\n",
                                  info.digital_volume, (unsigned long)info.headset_data,
                                  (unsigned long)info.headset_mask, (unsigned long)info.headset_clocks) >= 0;
                for (unsigned i = 0; i < ARRAYLEN(regs); i++)
                    saved &= fdprintf(fd, "%s %08lx=%08lx\n", regs[i].name,
                                      (unsigned long)regs[i].addr,
                                      (unsigned long)values[i]) >= 0;
                for (unsigned i = 0; i < SC_AUDIO_ANALOG_COUNT; i++)
                    saved &= fdprintf(fd, "%s %08lx=%04x valid=%d\n",
                                      sc_audio_analog_regs[i].name,
                                      (unsigned long)sc_audio_analog_regs[i].addr,
                                      info.analog[i], !!(info.valid & (1u << i))) >= 0;
                saved = (close(fd) == 0) && saved;
            }
        }
    }
    lcd_setfont(FONT_UI);
    return false;
}

/* Busy-wait, calibrated for 208 MHz. Deliberately a plain iteration loop:
 * the SC6530 system timer (0x8100300c) is avoided in drivers (the B310E-OS
 * sdio.c lesson — a timer read mid-SDIO can hang the phone), so no SoC
 * timer dependency here. ~208 cycles/µs at ~5 cycles/iteration. */
void udelay(unsigned int usecs)
{
    volatile unsigned int n = usecs * 42;

    while (n--)
        ;
}
