/*
 * B310E-OS — arch/main.c
 *
 * C entry point (called from arch/start.s after SVC32 mode, stack, bss
 * clear). Wave 8: the real boot path —
 *
 *     sc6530_chip_init()  ->  kmem_init(__bss_end, 2 MiB)
 *     ->  module_register(usb_debug/lcd/keypad)  ->  module_init_all()
 *     ->  task_create(banner, keypad)  ->  sched_start()   (never returns)
 *
 * The temporary direct LCD/USB hooks of waves 4-6 are gone: the drivers
 * now come up through the kernel module framework, and the demo tasks run
 * under the cooperative scheduler.
 *
 * Register sequence copied verbatim from fpdoom's init_sc6530.h (Unlicense,
 * public domain). Pin mux is intentionally NOT done here: fpdoom replays the
 * stock firmware's pinmap (0x8cxxxxxx writes) which we don't have yet.
 *
 *   SAFETY RULE: NEVER write 0x8c0002a4 = 0x231 (UART-TX pinmux) — it
 *   HANGS the Samsung B310E (fpdoom syscode.c:189-197 workaround).
 *
 * Compiled -mthumb -ffreestanding; no libc, no startup files.
 */

#include "os.h"
#include "chip.h"
#include "../drivers/lcd.h"
#include "../drivers/led.h"
#include "../drivers/keypad.h"
#include "../drivers/usb_debug.h"
#include "../drivers/audio.h"
#include "../app/demo.h"

/* End of the zeroed BSS (link/os.ld) — the kernel heap starts right after
 * the loaded image in SDRAM. */
extern char __bss_end[];

/* Kernel heap pool size: ~2 MiB. We have 4 MB SDRAM at 0x34000000 and the
 * image + bss is ~50 KB; 2 MiB covers task stacks (2 KiB each), kernel
 * objects and future app buffers, leaving ~2 MB headroom. */
#define KMEM_POOL_SIZE 0x200000u   /* 2 MiB */

/* Module descriptors (defined in drivers/lcd.c, led.c, keypad.c,
 * usb_debug.c). Registered explicitly, in this order, BEFORE
 * module_init_all() so init order == registration order: usb_debug FIRST
 * (the fdl_ack handshake and kprintf channel must be live ASAP — if the
 * LCD still fails, the user still sees the banner diagnostics on
 * libc_server), then led, then lcd, then keypad. */
extern const module_t usb_debug_module, led_module, lcd_module, keypad_module,
                      audio_module;

/* CPU freq -> 208 MHz (fpdoom init_sc6530.h:44-55, verbatim). */
static void sc6530_init_freq(void)
{
    uint32_t a;

    a = MEM4(0x8b000040);
    MEM4(0x8b000040) = a |= 4;
    MEM4(0x8b000040) = (a & ~3) | 1;    /* 208 MHz */
    DELAY(100);
}

/* SMC / SDRAM controller init @ 0x20000000. MUST run from on-chip IRAM:
 * reconfiguring the SDRAM controller while the CPU is executing from
 * SDRAM (0x34000000) hangs the bus — the observed hardware failure
 * (spd_dump streams os.bin, then `timeout reached`, black screen).
 *
 * fpdoom runs this exact routine from IRAM too (SMC_INIT_BUF, entry.c:11;
 * asmcode.s:474-533). The routine is position-independent, so we copy it
 * verbatim from the image into IRAM and call it there. */
#define SMC_INIT_BUF (0x40000000u + 0xa000u - 1024u)   /* 0x40009c00 */

extern int sc6530_init_smc_asm[1];      /* arch/smc_init.s */
extern int sc6530_init_smc_asm_end[1];

static void sc6530_init_smc(void)
{
    int *p = sc6530_init_smc_asm;
    int n = sc6530_init_smc_asm_end - p; /* word count = (end - start) */
    int *d = (int *)SMC_INIT_BUF, *f = d;

    do *d++ = *p++; while (--n);
    ((void (*)(void))(uintptr_t)f)();
}

/* ADI enable + reset, analog init (fpdoom init_sc6530.h:57-66, verbatim). */
static void sc6530_init_adi(void)
{
    MEM4(0x8b0000a0) = 1 << 24;        /* ADI enable        */
    MEM4(0x8b000060) = 1 << 19;        /* ADI reset         */
    DELAY(100);
    MEM4(0x8b000064) = 1 << 19;
    /* init analog */
    MEM4(0x82000000) &= ~(1 << 4);
    MEM4(0x82000004) = 0x55000;
}

/* IRAM (CHIPRAM 0x40000000) enable — required before using that region
 * (fpdoom entry.c:190). */
static void sc6530_init_iram(void)
{
    MEM4(0x8b0001a0) |= 7 << 19;       /* iram(1..3) enable */
}

/* Peripheral power gating (fpdoom syscode.c:77-81 pattern; SC6530C: AHB
 * power-on set 0x20500060, APB power-on set 0x8b0000a0). OR in the bits we
 * need — 0x8b0000a0 already holds 1<<24 (ADI enable) from sc6530_init_adi,
 * do not clobber it. */
static void sc6530_init_power(void)
{
    /* AHB: enable the blocks the drivers use */
    MEM4(0x20500060) |= 0x40;          /* LCM enable   (syscode.c:507)  */
    MEM4(0x20500060) |= 0x1000;        /* LCDC enable  (syscode.c:718)  */
    MEM4(0x20500060) |= 0x400;         /* SDIO0 enable                   */

    /* APB: keypad + GPIO_D */
    MEM4(0x8b0000a0) |= 0x80040;       /* keypad       (syscode.c:887)  */
    MEM4(0x8b0000a0) |= 0x800080;      /* GPIO_D       (syscode.c:160)  */
}

/* Full SC6530C init sequence (order is critical — see learnings.md).
 * IRAM enable is FIRST: the SMC routine is copied into IRAM (0x40009c00)
 * and executed there, so IRAM must be writable before sc6530_init_smc. */
void sc6530_chip_init(void)
{
    sc6530_init_iram();
    sc6530_init_freq();
    sc6530_init_smc();
    sc6530_init_adi();
    sc6530_init_power();

    /* PIN MUX — INTENTIONALLY OMITTED.
     *
     * fpdoom extracts the pin map from the stock firmware and replays
     * 0x8cxxxxxx pin writes at early chip init (syscode.c:186-219
     * pin_init). We extracted the table (dump_firmware.bin @ 0xc6ef0, 149
     * {addr,val} pairs) and tried led_pinmap_apply() twice: (1) from the
     * banner task — froze the LCD (mux flipped under live peripherals);
     * (2) from sc6530_chip_init() at boot, fpdoom's exact position —
     * STILL HANGS. So the B310E has ANOTHER 0x8c landmine besides
     * 0x8c0002a4 (fpdoom's sc6530_fix only skips that one). The pinmap
     * hypothesis for the torch is FALSIFIED; the drivers set the specific
     * registers they need, and the keypad/LCD/USB controllers work
     * without any 0x8c writes (proven in waves 4-6 hardware tests).
     *
     * >>> NEVER write 0x8c0002a4 = 0x231 (UART-TX pinmux) — it HANGS the
     *     B310E. And per the above, don't replay the whole stock pinmap
     *     either — the B310E hangs on at least one other entry. <<<
     */

}

/* FIFO-gated backlight marker (the ADI mailbox, led.c led_adi_write
 * pattern) — bisects a boot freeze on the panel when the LCD is not up
 * yet. level = 0x40 | (duty-1): 0x42 dim (~10%), 0x5f full bright.
 *
 * MONOTONIC levels (boot_mark sequence below): each stage writes a HIGHER
 * level than the last, so the freeze stage = the highest level the panel
 * reached. The old BRIGHT/DIM alternation was unreadable ("mid brightness,
 * no change" — the frozen STARTING screen). */
#define BOOT_MARK_CHIP    0x46u   /* chip init done   — ~20% */
#define BOOT_MARK_MODULES 0x4eu   /* modules done     — ~45% */
#define BOOT_MARK_IRQ     0x56u   /* irq/MMU done     — ~70% */
#define BOOT_MARK_SCHED   0x5fu   /* scheduler about  — 100% */

void boot_mark(uint16_t level)
{
    uint32_t n = 1000000u;

    while (MEM4(0x82000020) & 0x200u)       /* FIFO_FULL */
        if (--n == 0u) return;
    MEM4(0x82001220) = 0x1100u | level;     /* backlight, flash-off bit */
    n = 1000000u;
    while (!(MEM4(0x82000020) & 0x100u))    /* wait FIFO_EMPTY */
        if (--n == 0u) return;
}

/* ---- stock-spy boot-time reader -------------------------------------
 * When the user tests tools/stock-spy (boots the STOCK firmware with ADI
 * and UART hooks that log every ANA write + debug char into a PSRAM ring),
 * the capture survives a warm reboot. This os.bin boots afterwards and
 * dumps that ring over libc_server — no second spd_dump read_mem needed.
 *
 * Ring layout (tools/stock-spy/read-spy.ps1 — the payload was loaded at
 * physical 0x352F0800 while MEM_REMAP=1, so it is visible here at the SAME
 * VA 0x352F0800 — a plain volatile read, no MMU setup required):
 *   +0x0000  hook code (not dumped)
 *   +0x0400  ring1: 512 x {u32 addr, u32 value} (every ADI write, in order)
 *   +0x1400  ring2: 128 x {u32 0x8b0001c4, 0x8b000060, 0x8b0000a0,
 *                        0x8b000160} (snapshots on audio-ANA writes)
 *   +0x1D00  ring3: captured UART debug chars (the stock OS's trace/assert)
 *   +0x1F00  header: tick, ring1_idx, ring2_idx, timer_base, keypad_idle,
 *                    hold_start, magic "1SPY", rebooted, ring3_idx
 *
 * The magic is cleared at the end so a later boot (without a fresh
 * stock-spy capture) does not re-dump stale data. */
#define SPY_BASE        0x352F0800u
#define SPY_RING1       0x0400u
#define SPY_RING2       0x1400u
#define SPY_RING3       0x1D00u
#define SPY_HEADER      0x1F00u
#define SPY_MAGIC       0x59505331u    /* "1SPY" */

static void spy_dump(void)
{
    volatile uint32_t *h =
        (volatile uint32_t *)(SPY_BASE + SPY_HEADER);

    if (h[6] != SPY_MAGIC)
        return;

    /* The host must be reading EP3 before any kprintf: this runs BEFORE
     * sched_start(), so no task polls EP2 for libc_server's HOST_CONNECT
     * yet, and an unread TX times out -> s_link_down -> every frame drops.
     * usb_debug_poll() drains EP2 + sends the "connected" greeting; walk
     * it for a bounded ~10 s or until the host arrives. Each poll call is
     * a few USB register reads (~100 cycles on the hot path): 0x40000000
     * trivial iterations ≈ 10 s @ 208 MHz (stockram shim calibration), so
     * ~0x01400000 poll calls ≈ 10 s. */
    uint32_t n = 0x01400000u;
    while (!usb_debug_host_connected() && --n != 0)
        usb_debug_poll();
    if (!usb_debug_host_connected())
        return;                          /* no host: keep the capture for the
                                          * next boot instead of dropping it */

    uint32_t tick  = h[0];
    uint32_t r1idx = h[1];
    uint32_t r2idx = h[2];
    uint32_t r3idx = h[8];
    uint32_t rebooted = h[7];

    kprintf("spy: capture found (tick=%u r1=%u r2=%u r3=%u rebooted=%u)\n",
            (unsigned)tick, (unsigned)r1idx, (unsigned)r2idx,
            (unsigned)r3idx, (unsigned)rebooted);
    if (tick == 0u)
        kprintf("spy: WARNING ring1 never filled — patch may not be live\n");

    if (r1idx > 512u) r1idx = 512u;
    for (uint32_t i = 0u; i < r1idx; i++) {
        volatile uint32_t *e =
            (volatile uint32_t *)(SPY_BASE + SPY_RING1 + i * 8u);
        kprintf("spy: [%3u] 0x%08x = 0x%08x\n",
                (unsigned)i, (unsigned)e[0], (unsigned)e[1]);
    }

    if (r2idx > 128u) r2idx = 128u;
    for (uint32_t i = 0u; i < r2idx; i++) {
        volatile uint32_t *e =
            (volatile uint32_t *)(SPY_BASE + SPY_RING2 + i * 16u);
        kprintf("spy2: [%3u] own=0x%08x p60=0x%08x pa0=0x%08x dsp=0x%08x\n",
                (unsigned)i, (unsigned)e[0], (unsigned)e[1],
                (unsigned)e[2], (unsigned)e[3]);
    }

    if (r3idx > 255u) r3idx = 255u;
    if (r3idx > 0u) {
        kprintf("spy3: \"");
        for (uint32_t i = 0u; i < r3idx; i++) {
            volatile uint8_t *c =
                (volatile uint8_t *)(SPY_BASE + SPY_RING3 + i);
            uint8_t ch = *c;
            if (ch >= 0x20u && ch <= 0x7eu)
                kputc((char)ch);
            else if (ch == 10u || ch == 13u)
                kputc((char)ch);
            else
                kputc('?');
        }
        kprintf("\"\n");
    }

    h[6] = 0u;
}

void main(void)
{
    sc6530_chip_init();
    boot_mark(BOOT_MARK_CHIP);      /* chip init done — ~20% */

    /* D-cache: enable right after chip init (freq/SMC/ADI/IRAM/power done,
     * no DMA active yet). clean_dcache() in lcd_show()/lcd_show_bounded()
     * keeps the LCDC DMA coherent with the CPU-written framebuffer.
     * NOTE: on ARM926EJ-S the C bit is overridden while the MMU is off
     * (TRM DDI0198D Table 4-3: "DCache effectively disabled... noncachable,
     * nonbufferable"), so this is a documented no-op until a future wave
     * adds MMU page tables — MMU bit 0 stays clear here (no TTBR, enabling
     * it would fault on the next access). If hardware ever shows a
     * regression, drop this call; the rest of the wave stands alone. */
    enable_dcache();

    /* Boot marker: recognizable magic in IRAM scratch so later waves can
     * verify this init ran ("B310E-OS v0.4"). 0x40004000 is FDL1's former
     * residence — free once it has jumped us into RAM. */
    MEM4(0x40000000 + 0x4000) = 0xB310E051;

    /* 1. Kernel heap: the bump pool over SDRAM right after the image.
     *    Backs task stacks (2 KiB each) and kernel objects. */
    kmem_init(__bss_end, KMEM_POOL_SIZE);

    /* 2. Register ALL modules, then run their inits in that order.
     *    usb_debug first: its fdl_ack unblocks spd_dump and its kputc
     *    channel carries all subsequent diagnostics to libc_server. */
    module_register(&usb_debug_module);
    module_register(&led_module);
    module_register(&lcd_module);
    module_register(&keypad_module);
    module_register(&audio_module);
    if (module_init_all() != 0)
        kprintf("boot: module init FAILURES\n");
    boot_mark(BOOT_MARK_MODULES);   /* modules done — ~45% */

    /* ---- stock-spy reader: dump PSRAM ring data if present ---- */
    spy_dump();

    /* 3. IRQ infrastructure + MMU — LAST in the pre-kernel init. The whole
     *    polled system above (chip init, heap, modules incl. the LCD banner
     *    and the USB channel) is already up, so a fault inside the vector/
     *    MMU setup cannot take an otherwise-working boot down with it.
     *    irq_init() copies the exception stubs to CHIPRAM, sets the
     *    IRQ/SVC/ABT/UND stacks, builds the page table (high vectors at
     *    0xffff0000 mapped) and ENABLES the MMU; it never returns to the
     *    polled path. No IRQ source is enabled yet (IRQs stay masked —
     *    start.s msr cpsr_c,#0xdf); the next wave enables the timer tick. */
    irq_init();
    boot_mark(BOOT_MARK_IRQ);       /* irq/MMU done — ~70% */

    /* 4. Demo tasks. They do not run until sched_start() below, so every
     *    module init above is guaranteed complete before any task prints
     *    (USB kputc override live) or draws (LCD up). */
    if (task_create("banner", demo_banner_task, NULL, 0) < 0)
        kprintf("boot: task 'banner' create failed\n");
    if (task_create("keypad", demo_keypad_task, NULL, 0) < 0)
        kprintf("boot: task 'keypad' create failed\n");
    /* SD boot-path probe (Rockbox groundwork): key-triggered (DIAL), runs
     * the full sdboot FAT chain once and exits. Validates the FAT32 layer
     * on hardware inside os.bin (diag-sd already proved the SDIO/MBR half). */
    if (task_create("sd", demo_sd_task, NULL, 0) < 0)
        kprintf("boot: task 'sd' create failed\n");

    /* 4. Run the cooperative scheduler forever. Never returns. */
    boot_mark(BOOT_MARK_SCHED);     /* scheduler about to run — 100% */
    sched_start();
}
