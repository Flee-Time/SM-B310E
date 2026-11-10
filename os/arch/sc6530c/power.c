/* B310E OS — watchdog, reboot, fault screen.
 *
 * Watchdog in the analog die at 0x82001480: LOAD_LOW +0, LOAD_HIGH +4,
 * CTRL +8 (bit 3 reset enable, bit 0 interrupt, bit 1 run), LOCK +0x20
 * (0xe551 unlocks). 32 kHz. The reboot sequence is the one the Rockbox
 * port verified on the phone (system_reboot): it restarts the boot chain,
 * which brings back the SD menu.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"

#define WDG       0x82001480u
#define WDG_LOCK  (WDG + 0x20)
#define WDG_KEY   0xe551u

/* A reset in hi * 2 s (hi << 16 at 32768 Hz) unless watchdog_stop() comes first */
static void watchdog_arm_hi(uint16_t hi)
{
    adi_write(0x820010e0, 4);
    adi_write(0x820010e4, 2);
    adi_write(0x82001480 + 0x20, 0xe551);
    adi_write(0x82001480 + 8, adi_read(0x82001480 + 8) | 9);
    adi_write(0x82001480, 0);
    adi_write(0x82001480 + 4, hi);
    adi_write(0x82001480 + 8, adi_read(0x82001480 + 8) | 2);
    adi_write(0x82001480 + 0x20, (uint16_t)~0xe551u);
}

/* A reset in 8 s unless watchdog_stop() comes first: for tries that may hang
 * the phone. */
void watchdog_arm(void)
{
    watchdog_arm_hi(4);
}

void watchdog_stop(void)
{
    adi_write(WDG_LOCK, WDG_KEY);
    adi_write(WDG + 8, adi_read(WDG + 8) & ~0xfu);
    adi_write(WDG_LOCK, ~WDG_KEY);
}

#define CHAIN_IRAM 0x4000f800u          /* above sd.c's bounce buffer */
#define CHAIN_TO   0x04000000u          /* where sdboot puts the menu, the menu us */

/* Run another image as if loaded fresh: the boot menu, or this OS again.
 * The image must lie above the copy's end (the heap does). */
void arch_chain(const void *image, uint32_t len)
{
    extern char chain_tramp[], chain_tramp_end[];
    void (*tramp)(const void *, uint32_t, uint32_t) = (void (*)(const void *, uint32_t, uint32_t))CHAIN_IRAM;

    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    dma_quiesce();
    memcpy((void *)CHAIN_IRAM, chain_tramp, (size_t)(chain_tramp_end - chain_tramp));
    dcache_clean_range((void *)CHAIN_IRAM, (size_t)(chain_tramp_end - chain_tramp));
    icache_invalidate_all();
    tramp(image, (len + 3) & ~3u, CHAIN_TO);
    for (;;)
        ;
}

bool arch_chain_fits(const void *image, uint32_t len)
{
    return (uintptr_t)image >= CHAIN_TO + len;
}

void arch_reboot(void)
{
    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    adi_write(0x820010e0, 4);           /* watchdog clocks */
    adi_write(0x820010e4, 2);
    adi_write(WDG_LOCK, WDG_KEY);
    adi_write(WDG + 8, adi_read(WDG + 8) | 9);
    adi_write(WDG, 0x4000);             /* 0.5 s */
    adi_write(WDG + 4, 0);
    adi_write(WDG + 8, adi_read(WDG + 8) | 2);
    adi_write(WDG_LOCK, ~WDG_KEY);
    for (;;)
        arch_wait_for_irq();
}

/* The boot loader's handshake (boot/boot.h): the high byte of analog
 * 0x82001148 keeps its value over the reset; the loader takes a value
 * 0xb0-0xb3 and clears it. The low byte is the stock firmware's restart
 * mode. BOOT_REQ_PATH also leaves the program's path in a block at the top
 * of PSRAM (in our MMU table, dead by then), cleaned from the D-cache so it
 * reaches PSRAM before the reset. */
#define LOADER_HEADER 0x003fc000u       /* NOR: the loader's header */
#define REQ_REG       0x82001148u
#define REQ_BLOCK     0x043fff00u
#define REQ_MAGIC     0x51523342u       /* "B3RQ" */
#define REQ_PATH_LEN  56

bool arch_boot_loader(void)
{
    const volatile uint32_t *h = (const volatile uint32_t *)LOADER_HEADER;

    return h[2] == 0x30313342u && h[3] == 0x544f4f42u;      /* "B310BOOT" */
}

void arch_boot_request(unsigned req, const char *path)
{
    if (req == BOOT_REQ_PATH && path) {
        volatile uint32_t *b = (volatile uint32_t *)REQ_BLOCK;
        uint32_t words[REQ_PATH_LEN / 4] = { 0 }, sum = REQ_MAGIC;

        if (*path == '/')
            path++;
        strncpy((char *)words, path, REQ_PATH_LEN - 1);
        for (int i = 0; i < REQ_PATH_LEN / 4; i++) {
            b[1 + i] = words[i];
            sum += words[i];
        }
        b[1 + REQ_PATH_LEN / 4] = ~sum;
        b[0] = REQ_MAGIC;
        dcache_clean_range((const void *)REQ_BLOCK, 4 * (2 + REQ_PATH_LEN / 4));
    }
    adi_write(REQ_REG, (adi_read(REQ_REG) & 0xffu) | (req & 0xffu) << 8);
}

bool arch_boot_pending(void)
{
    unsigned req = (adi_read(REQ_REG) >> 8) & 0xff;

    return req >= BOOT_REQ_STOCK && req <= BOOT_REQ_PATH;
}

/* Cut the supplies (stock 0x1a448: SET1 = 0x1f, then SET0 = 0x3fff; the
 * last store removes the CPU and RAM supplies). The LEDs and vibrator
 * live in the analog die, which runs from the battery and keeps its
 * state: turn them off first, or the keypad light stays lit (found with
 * Rockbox). Callers flush the filesystem before. */
#define LDO_PD_SET0  0x82001180u
#define LDO_PD_SET1  0x82001184u

void arch_power_off(void)
{
    uint32_t n = 1000000;

    keylight_set(false);
    backlight_set(0);
    vibrator_set(0);
    torch_set(false);
    arch_irq_save();
    REG32(INTC_DISABLE) = 0xffffffffu;
    /* END is down after "held 2 s": cut now and the chip comes straight back
     * up with END held, which the boot loader takes for a power-on to its
     * menu. Display off, wait for it (30 s at most). */
    for (int ms = 30000; ms > 0 && ((adi_read(0x82001900) >> 3) & 1); ms--)
        udelay(1000);
    udelay(50000);
    if (!adi_write(LDO_PD_SET1, 0x1f))
        arch_reboot();
    while ((REG32(0x82000020) & (1u << 9)) && --n)
        ;
    if (!n)
        arch_reboot();
    REG32(LDO_PD_SET0) = 0x3fff;
    for (;;)
        arch_wait_for_irq();
}

/* A data abort that is not the shell's probe (cpu.S, abort mode, interrupts
 * off). A store into the NOR window, which mmu_setup maps read-only (a
 * permission fault there: FSR 0xd, a section's), from a thread (SYS mode)
 * that arch_write_fault says may go on elsewhere: its pc becomes that
 * function, with the address in r0 and the store's pc in r1. Returns the
 * SPSR to go on with, or 0 for the fault screen. */
uintptr_t (*arch_write_fault)(uint32_t addr, uint32_t pc);

__attribute__((target("arm"))) uint32_t arch_data_abort(uint32_t *regs, uint32_t spsr)
{
    uint32_t fsr, far;
    uintptr_t to;

    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(fsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(far));
    if ((spsr & 0x1f) != 0x1f || (fsr & 0xf) != 0xd || far >= PSRAM_BASE || !arch_write_fault)
        return 0;
    if (!(to = arch_write_fault(far, regs[13])))
        return 0;
    regs[0] = far;
    regs[1] = regs[13];
    regs[13] = to & ~1u;
    return to & 1 ? spsr | 0x20 : spsr & ~0x20u;   /* Thumb or ARM */
}

/* ---- the last fault, kept over the reset ---------------------------------
 * The fault screen writes it to FAULT_BLOCK (past the heap, link/os.ld), out
 * of the D-cache, before it draws anything; PSRAM keeps it over the
 * watchdog's reset, and the next boot logs it (arch_fault_report). */
#define FAULT_MAGIC 0x544c4146u         /* "FALT" */

struct fault_rec {
    uint32_t magic, kind, spsr, fsr, far, ticks;
    uint32_t regs[14];                  /* r0-r12, pc */
    char thread[16], where[40];
    uint32_t sum;
};

void (*arch_fault_where)(uint32_t pc, char *out, size_t n);

static uint32_t fault_sum(const struct fault_rec *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x5a17u;

    for (size_t i = 0; i < offsetof(struct fault_rec, sum) / 4; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

__attribute__((target("arm"), noinline)) static void fault_status(uint32_t *fsr, uint32_t *far)
{
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(*fsr));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(*far));
}

static void fault_keep(int kind, const uint32_t *regs, uint32_t spsr)
{
    struct fault_rec *r = (struct fault_rec *)FAULT_BLOCK;
    thread_t *t = kernel_current;

    memset(r, 0, sizeof(*r));
    r->kind = (uint32_t)kind;
    r->spsr = spsr;
    fault_status(&r->fsr, &r->far);
    r->ticks = kernel_ticks();
    memcpy(r->regs, regs, sizeof(r->regs));
    if (t && t->name)
        strncpy(r->thread, t->name, sizeof(r->thread) - 1);
    if (arch_fault_where)
        arch_fault_where(regs[13], r->where, sizeof(r->where));
    r->magic = FAULT_MAGIC;
    r->sum = fault_sum(r);
    dcache_clean_range(r, sizeof(*r));
}

void arch_fault_report(void)
{
    static const char *const names[] = {
        "undefined instruction", "prefetch abort", "data abort", "fiq"
    };
    struct fault_rec *r = (struct fault_rec *)FAULT_BLOCK;
    const uint32_t *x = r->regs;

    if (r->magic != FAULT_MAGIC || r->sum != fault_sum(r))
        return;
    r->thread[sizeof(r->thread) - 1] = r->where[sizeof(r->where) - 1] = 0;
    klog("boot: the last run stopped on a fault, %s, at %lu ms: pc %08lx%s%s%s, cpsr %08lx, "
         "fsr %03lx far %08lx, thread %s", names[r->kind & 3], (unsigned long)r->ticks,
         (unsigned long)x[13], r->where[0] ? " (" : "", r->where, r->where[0] ? ")" : "",
         (unsigned long)r->spsr, (unsigned long)(r->fsr & 0xfff), (unsigned long)r->far,
         r->thread[0] ? r->thread : "?");
    klog("boot: r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx",
         (unsigned long)x[0], (unsigned long)x[1], (unsigned long)x[2], (unsigned long)x[3],
         (unsigned long)x[4], (unsigned long)x[5], (unsigned long)x[6]);
    klog("boot: r7 %08lx r8 %08lx r9 %08lx r10 %08lx r11 %08lx r12 %08lx",
         (unsigned long)x[7], (unsigned long)x[8], (unsigned long)x[9], (unsigned long)x[10],
         (unsigned long)x[11], (unsigned long)x[12]);
    r->magic = 0;
    dcache_clean_range(r, sizeof(*r));
}

/* Undefined instruction, aborts, FIQ: keep the registers for the next boot's
 * log, show them, and restart in 20 s (the watchdog: even if this screen
 * hangs), or at once on END. */
void arch_fault(int kind, const uint32_t *regs, uint32_t spsr)
{
    static const char *const names[] = {
        "undefined instr", "prefetch abort", "data abort", "fiq"
    };
    extern bool console_unlocked;
    static bool in_fault;
    int i;

    if (in_fault)                       /* a fault while reporting one */
        for (;;)
            ;
    in_fault = true;
    fault_keep(kind, regs, spsr);
    watchdog_arm_hi(10);
    console_unlocked = true;            /* no kernel locks from here */
    console_set_color(RGB(255, 255, 255), RGB(160, 0, 0));
    console_clear();
    console_printf("FAULT: %s\n", names[kind & 3]);
    console_printf("pc %08lx cpsr %08lx\n", (unsigned long)regs[13],
                   (unsigned long)spsr);
    for (i = 0; i < 13; i += 2)
        console_printf("r%-2d %08lx r%-2d %08lx\n", i,
                       (unsigned long)regs[i], i + 1,
                       (unsigned long)(i + 1 < 13 ? regs[i + 1] : 0));
    if (((const struct fault_rec *)FAULT_BLOCK)->where[0])
        console_printf("in %s\n", ((const struct fault_rec *)FAULT_BLOCK)->where);
    console_printf("\nRestarting in 20 s, or hold END.\nThe log keeps this.");
    console_flush();
    for (;;) {
        if ((adi_read(0x82001900) >> 3) & 1)   /* END, EIC channel 3 */
            arch_reboot();
        udelay(20000);
    }
}
