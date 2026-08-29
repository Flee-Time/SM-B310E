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
 * The SC6530 INTC has no INTOFFSET register: read INT_PENDING and dispatch
 * only the lines we own — line 23 (the 1 ms tick, TIMER23) and line 4 (the
 * user timer, TIMER0); 23 wins when both are set. ANY OTHER pending bit is
 * a stray: the SC6530 pending register (0x80000004) reflects DISABLED lines
 * too, so a peripheral edge (SDIO DMA, keypad/EIC, USB) asserts its bit the
 * moment the source fires even though the line is masked. Strays are SKIPPED
 * — never panic, never ack a line we don't own (0x8000000C is INT_DISABLE,
 * not an acknowledge, and acking an unhandled line would starve the tick).
 * This mirrors the proven B310E-OS kernel fix (kernel/irq.c
 * irq_dispatch_for_test: "ONLY dispatch a pending line that has a registered
 * handler"). The stray bit stays pending and is re-seen harmlessly on the
 * next tick; if the source persists it can never wedge the tick because the
 * dispatch order never skips 23/4 to run a stray handler.
 */
void TIMER23(void);   /* kernel-sc6530c.c (strong, the 1 ms tick) */
void TIMER0(void);    /* timer-sc6530c.c (strong, the user timer) */

/* Stray-IRQ counter: incremented by irq_handler whenever pending holds bits
 * that are neither the tick (23) nor the user timer (4). Read from a
 * debugger / the debug menu to confirm the stray-line source after a
 * session (a non-zero count = stray edges were seen and safely skipped). */
volatile uint32_t s_stray_irq_count = 0;

void irq_handler(void) __attribute__((interrupt ("IRQ"), naked));
void irq_handler(void)
{
    asm volatile (
        "sub    lr, lr, #4            \r\n"
        "stmfd  sp!, {r0-r3, ip, lr}  \r\n"
        "mov    r0, #0x80000000       \r\n" /* INT_PENDING */
        "ldr    r0, [r0, #0x04]       \r\n"
        "tst    r0, r0                \r\n"
        "beq    3f                    \r\n" /* nothing pending */
        "ldr    r1, =0x00800000       \r\n" /* TIMER_IRQ_MASK (1<<23) */
        "tst    r0, r1                \r\n"
        "ldrne  r1, =TIMER23          \r\n"
        "bne    2f                    \r\n"
        "ldr    r1, =0x00000010       \r\n" /* TIMER0_MASK (1<<4) */
        "tst    r0, r1                \r\n"
        "ldrne  r1, =TIMER0           \r\n"
        "bne    2f                    \r\n"
        /* Stray pending line(s) only — no registered handler (the SC6530
         * pending register reflects DISABLED lines). Skip, never panic:
         * count it for debugging and return. The timer ISRs ack their OWN
         * sources (TIMER2_INT/TIMER0_INT = 9); the INTC is never touched. */
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
    REG32(INT_DISABLE_REG) = 0xFFFFFFFF;
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

/* Debug menu hooks (system.h): return false = "no extra info to show". */
bool dbg_ports(void)
{
    return false;
}

bool dbg_hw_info(void)
{
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
