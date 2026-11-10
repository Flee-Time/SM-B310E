/* B310E OS — SC6530C hardware layer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_ARCH_H
#define OS_ARCH_H

/* ---- memory map (link/os.ld); plain numbers, the assembler uses them --- */
#define PSRAM_BASE    0x04000000
#define PSRAM_SIZE    0x00400000
#define TTB_ADDR      0x043fc000    /* PSRAM end - 16 KB */
#define LCD_WIDTH     128
#define LCD_HEIGHT    160
#define FRAME_ADDR    0x043f2000    /* TTB - 128 * 160 * 2 */
#define FAULT_BLOCK   0x043f1f00    /* the last fault, kept over a reset */
#define CACHE_LINE    32

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

/* ---- interrupt controller ---------------------------------------------- */
#define INTC_STATUS   0x80000000u   /* masked pending */
#define INTC_RAW      0x80000004u
#define INTC_ENABLE   0x80000008u   /* read/write mask */
#define INTC_DISABLE  0x8000000cu   /* write ones to mask (not an ack) */

#define IRQ_TIMER0    4
#define IRQ_KEYPAD    8
#define IRQ_DSP       13
#define IRQ_DMA       20
#define IRQ_TIMER2    23
#define IRQ_USB       25

void irq_register(unsigned line, void (*handler)(void));
void irq_enable_line(unsigned line);
void irq_disable_line(unsigned line);
void irq_dispatch(void);            /* called from the IRQ vector */
extern volatile uint32_t irq_stray_count;
extern volatile uint32_t irq_count;
extern volatile uint32_t irq_line_count[32];

/* ---- CPU (arch/sc6530c/cpu.S) ------------------------------------------ */
uint32_t arch_irq_save(void);       /* masks IRQ, returns the old CPSR */
void arch_irq_restore(uint32_t cpsr);
bool arch_in_irq(void);             /* IRQ or SVC (scheduler) context */
void arch_yield(void);              /* svc: run the scheduler */
void arch_start_first(void) __attribute__((noreturn));
void arch_wait_for_irq(void);       /* CP15 wait-for-interrupt */
void arch_thread_trampoline(void);  /* first frame of every thread */

void dcache_clean_range(const void *addr, size_t len);
void dcache_flush_range(void *addr, size_t len);   /* clean + invalidate */
void dcache_clean_all(void);
void icache_invalidate_all(void);

void udelay(unsigned us);           /* busy wait, scaled to the CPU clock */

/* CPU clock (arch/sc6530c/clock.c): level 0 26 MHz, 1 104 MHz, 2 208 MHz */
#define CLOCK_LEVELS 3
void clock_init(void);
bool clock_scalable(void);          /* false: unknown boot clock, left alone */
int clock_level(void);
int clock_mhz(void);
int clock_level_mhz(int level);
void clock_set_level(int level);
void clock_pll_stop(void);           /* at 26 MHz only; the idle loop's */
void clock_pll_need(void);           /* interrupts off: running and locked */
uint32_t clock_pll_starts(void);
extern uint32_t adi_read_errors;     /* reads that never came back right */
void clock_account(void);           /* irqs off: count the time at this level */
uint32_t clock_level_ms(int level); /* ms at that level since start */

/* ---- analog die through the ADI mailbox (arch/sc6530c/adi.c) ----------- */
uint32_t adi_read(uint32_t addr);
bool adi_write(uint32_t addr, uint32_t value);

/* ---- tick timer (arch/sc6530c/timer.c) --------------------------------- */
#define TIMER_HZ 26000000u
void timer_init(unsigned hz, void (*tick)(void));
uint32_t timer_count(void);         /* the tick's 26 MHz down-counter */
uint32_t timer_cycles(void);        /* 26 MHz cycles since start (irqs off) */
void timer_skip(uint32_t cycles);   /* time that passed with the timer stopped */

/* Chip sleep (arch/sc6530c/sleep.c) */
#define SLEEP_DCXO_ID 0x1130c000u   /* this phone's analog die */
uint32_t sleep_analog_id(void);
void sleep_chip_init(void);         /* at boot: nothing playing; it waits */
void sleep_stop(int level);         /* 0 the system may stop, 1 the core only */
void sleep_idle(void);              /* the idle loop's wait, interrupts off */
void sleep_idle_enable(bool on);    /* off: a plain wait for interrupt */
extern uint32_t sleep_idle_count;   /* system sleeps the idle loop took */
uint32_t sleep_counter_ms(void);    /* runs on through a sleep */
void sleep_timer_start(uint32_t ms);
uint32_t sleep_timer_stop(void);
void timer_restart(uint32_t cycles);    /* a new period from now (irqs off) */
uint32_t timer_load(void);          /* its reload value */
uint32_t timer_elapsed(uint32_t start);  /* cycles since a timer_count() */

/* ---- power (arch/sc6530c/power.c) -------------------------------------- */
void watchdog_arm(void);            /* a reset in 8 s */
void watchdog_stop(void);
void arch_reboot(void) __attribute__((noreturn));
/* A thread's store into the NOR window (read-only in the MMU): where the thread
 * goes on, called as fn(address, pc) in its own mode (loader.c: an app's, to its
 * end). Called in abort mode, interrupts off; returns the function (bit 0 set
 * for Thumb), or 0 for the fault screen. */
extern uintptr_t (*arch_write_fault)(uint32_t addr, uint32_t pc);
/* A fault's pc in words for the record (loader.c: "Blood+0x1234"); NULL: none */
extern void (*arch_fault_where)(uint32_t pc, char *out, size_t n);
/* The boot after a fault: the record the fault screen kept, to the log */
void arch_fault_report(void);
/* Copy image to 0x04000000 and enter it, MMU and caches off (from IRAM). */
void arch_chain(const void *image, uint32_t len) __attribute__((noreturn));
bool arch_chain_fits(const void *image, uint32_t len);   /* lies above the copy */
void arch_power_off(void) __attribute__((noreturn));
/* The B310E boot loader (boot/) in NOR, and a request it takes after the
 * next reset (arch_reboot): BOOT_REQ_MENU, BOOT_REQ_OS, or BOOT_REQ_PATH
 * with the card path of a payload (payloads/...). */
#define BOOT_REQ_STOCK 0xb0
#define BOOT_REQ_MENU  0xb1
#define BOOT_REQ_OS    0xb2
#define BOOT_REQ_PATH  0xb3
bool arch_boot_loader(void);
void arch_boot_request(unsigned req, const char *path);
bool arch_boot_pending(void);       /* a request waits for the next reset */

#endif /* __ASSEMBLER__ */
#endif
