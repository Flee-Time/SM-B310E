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
#include "button.h"
#if CONFIG_TUNER
#include "fmradio_i2c.h"
#include "tuner.h"
#include "power.h"
#endif
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
 * playback DMA 20 (normal firmware), keypad 8, USB 25, then user timer 4. Raw
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
#if defined(HAVE_USBSTACK) && !defined(BOOTLOADER)
void USB_UDC(void);   /* usb-drv-sc6530c.c, device controller line 25 */
#endif

/* Stray-IRQ counter: incremented by irq_handler whenever pending holds bits
 * that have no registered handler (23, 20, 8, 25 or 4). Read from a
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
#if defined(HAVE_USBSTACK) && !defined(BOOTLOADER)
        "tst    r0, #0x02000000       \r\n" /* USB IRQ25 */
        "ldrne  r1, =USB_UDC          \r\n"
        "bne    2f                    \r\n"
#endif
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
    REG32(0x8a000000) &= ~(1u << 7);          /* torch off (GPIO7) */
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
#if CONFIG_TUNER
    fmradio_i2c_init();
#endif
}

int system_memory_guard(int newmode)
{
    (void)newmode;
    return 0;
}

/* Debug menu hooks (system.h). */
/* Torch test (Debug > View I/O ports). GPIO7 drives the torch in the
 * stock firmware under QEMU, but did not light it on hardware under
 * Rockbox. This screen shows the live registers, toggles each candidate
 * and writes a register dump (pinmux reads only, never writes). */
extern volatile unsigned torch_hold_count;
#define ANA_LED_CTRL 0x82001220u

static bool torch_dump(const char *what)
{
    int fd = open(ROCKBOX_DIR "/torch-b310e.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return false;
    bool ok = fdprintf(fd, "== %s tick=%ld holds=%u torch=%d\n", what, current_tick,
                       torch_hold_count, torch_get()) >= 0;
    ok &= fdprintf(fd, "ANA 82001220=%04lx 82001224=%04lx 82001240=%04lx\n",
                   (unsigned long)adi_read(ANA_LED_CTRL),
                   (unsigned long)adi_read(0x82001224),
                   (unsigned long)adi_read(0x82001240)) >= 0;
    for (unsigned bank = 0; bank < 8; bank++) {
        uint32_t b = 0x8a000000u + bank * 0x80u;
        ok &= fdprintf(fd, "GPIO%u D=%04lx M=%04lx R=%04lx IE=%04lx\n", bank,
                       (unsigned long)REG32(b), (unsigned long)REG32(b + 4),
                       (unsigned long)REG32(b + 8), (unsigned long)REG32(b + 0x18)) >= 0;
    }
    {
        uint16_t c[8];
        for (int at_start = 1; at_start >= 0; at_start--) {
            sc6530_rec_debug(c, at_start);
            ok &= fdprintf(fd, "CODEC %s +00=%04x +40=%04x +5c=%04x +60=%04x "
                           "+64=%04x +70=%04x +ac=%04x 1164=%04x\n",
                           at_start ? "recstart" : "now", c[0], c[1], c[2], c[3],
                           c[4], c[5], c[6], c[7]) >= 0;
        }
        ok &= fdprintf(fd, "VBC +10=%08lx +18=%08lx +80=%08lx owner=%08lx "
                       "DP +00=%08lx +18=%08lx route=%d\n",
                       (unsigned long)REG32(0x82003010), (unsigned long)REG32(0x82003018),
                       (unsigned long)REG32(0x82003080), (unsigned long)REG32(0x8b0001c4),
                       (unsigned long)REG32(0x8a002000), (unsigned long)REG32(0x8a002018),
                       sc6530_rec_route()) >= 0;
    }
#if CONFIG_TUNER
    {
        struct rtc6211_dbg_info fm;
        rtc6211_dbg_info(&fm);
        ok &= fdprintf(fd, "FM present=%d N=%d powered=%d nor_tables=%d pin17c=%08lx regs",
                       rtc6211_get(RADIO_PRESENT), fm.chip_n, tuner_powered(), fm.nor_tables,
                       (unsigned long)REG32(0x8c00017c)) >= 0;
        for (int i = 0; i < 16; i++)
            ok &= fdprintf(fd, " %04x", fm.regs[i]) >= 0;
        ok &= fdprintf(fd, "\n") >= 0;
    }
#endif
    ok &= fdprintf(fd, "APB 8b000000=%08lx 8b0000a0=%08lx\n",
                   (unsigned long)REG32(0x8b000000), (unsigned long)REG32(0x8b0000a0)) >= 0;
    for (uint32_t off = 0; off < 0x300; off += 0x20) {
        ok &= fdprintf(fd, "PIN %03lx:", (unsigned long)off) >= 0;
        for (uint32_t i = 0; i < 0x20; i += 4)
            ok &= fdprintf(fd, " %05lx", (unsigned long)(REG32(0x8c000000u + off + i) & 0xfffff)) >= 0;
        ok &= fdprintf(fd, "\n") >= 0;
    }
    return (close(fd) == 0) && ok;
}

bool dbg_ports(void)
{
    uint32_t led_saved = adi_read(ANA_LED_CTRL);
    int ana_mode = 0;              /* 0 untouched, 1 0x8800/0x4400, 2 clear 0x1100 */
    const char *status = torch_dump("open") ? "dump saved" : "dump FAILED";

    lcd_setfont(FONT_SYSFIXED);
    for (;;)
    {
        uint32_t led = adi_read(ANA_LED_CTRL);
        lcd_clear_display();
        lcd_puts(0, 0, "Torch test");
        lcd_putsf(0, 2, "GPIO7 torch: %s", torch_get() ? "ON" : "off");
        lcd_putsf(0, 3, "Hold-0 toggles: %u", torch_hold_count);
        lcd_putsf(0, 4, "B0 D%04lx M%04lx", (unsigned long)REG32(0x8a000000),
                  (unsigned long)REG32(0x8a000004));
        lcd_putsf(0, 5, "B0 R%04lx", (unsigned long)REG32(0x8a000008));
        lcd_putsf(0, 6, "LED_CTRL %04lx", (unsigned long)led);
        lcd_putsf(0, 7, "ANA mode %d", ana_mode);
        lcd_puts(0, 9, "1: GPIO7 on/off");
        lcd_puts(0, 10, "3: LED 8800 on/off");
        lcd_puts(0, 11, "5: LED ~1100 on/off");
        lcd_putsf(0, 12, "7: VBC+80 bits30 %s", sc6530_rec_route() ? "on" : "off");
        lcd_puts(0, 14, "9: save dump");
        lcd_puts(0, 15, "Back: exit");
        lcd_puts(0, 17, status);
        lcd_update();

        int button = button_get_w_tmo(HZ / 4);
        if (button & (BUTTON_REL | BUTTON_REPEAT))
            continue;
        if (button == BUTTON_BACK)
            break;
        if (button == BUTTON_1) {
            torch_set(!torch_get());
            status = torch_get() ? "GPIO7 high" : "GPIO7 low";
        } else if (button == BUTTON_3 || button == BUTTON_5) {
            int mode = button == BUTTON_3 ? 1 : 2;
            ana_mode = ana_mode == mode ? 0 : mode;
            if (ana_mode == 1)
                adi_write(ANA_LED_CTRL, (led_saved & ~0x4400u) | 0x8800u);
            else if (ana_mode == 2)
                adi_write(ANA_LED_CTRL, led_saved & ~0x1100u);
            else
                adi_write(ANA_LED_CTRL, led_saved);
            status = ana_mode ? "LED_CTRL changed" : "LED_CTRL restored";
        } else if (button == BUTTON_7) {
            sc6530_rec_set_route(!sc6530_rec_route());
            status = "VBC+80 toggled";
        } else if (button == BUTTON_9) {
            status = torch_dump("key 9") ? "dump saved" : "dump FAILED";
        }
    }
    if (ana_mode)
        adi_write(ANA_LED_CTRL, led_saved);
    lcd_setfont(FONT_UI);
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
