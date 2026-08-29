/*
 * B310E-OS — kernel/irq.c
 *
 * IRQ infrastructure (wave): exception vector installation, per-mode
 * stacks, IRQ dispatch, and the minimal MMU page table that maps the
 * high-vector page (0xffff0000). Everything register/asm is ported
 * VERBATIM from fpdoom (Unlicense / public domain):
 *
 *   - int_vectors   arch/vectors.s  (asmcode.s:35-82)
 *   - set_mode_sp   arch/vectors.s  (asmcode.s:29-33)
 *   - enable_mmu    arch/mmu.s      (asmcode.s:84-92, +TLB/I-cache flush)
 *   - get_ram_size  entry.c:84-94
 *   - MMU table     entry.c:193-217 + syscode.c sys_prep_vectors:995-1020
 *   - handler table syscode.c sys_set_handlers:1059-1075
 *
 * NO interrupt source is enabled in this wave: IRQs stay masked at the
 * CPSR level (start.s msr cpsr_c,#0xdf). The next wave enables the 1 ms
 * system timer and a preemptive scheduler on top of this infrastructure.
 *
 * Host builds (#ifndef HOST_TEST) compile the hardware/asm paths out; the
 * pure dispatch logic (irq_line_from_pending / irq_dispatch_for_test)
 * stays testable and the API links.
 */

#include "irq.h"
#include "printk.h"
#include "../arch/chip.h"

/* ---- fixed SC6530C addresses (fpdoom, verified — see learnings.md) ----- */
#define VECTOR_ADDR     0x40019000u  /* CHIPRAM exception vectors          */
#define VECTOR_L2       (VECTOR_ADDR + 0x400u)  /* coarse page table for
                                                   the 0xffff0000 page    */
#define MODE_STACK_SP   0x40022000u  /* IRQ/SVC/ABT/UND banked stacks      */
#define CHIPRAM_BASE    0x40000000u  /* on-chip SRAM                       */
#define TTB_SIZE        0x4000u      /* 16 KB = 4096 1 MB-section entries  */

/* NO SDRAM_BASE #define here. The PSRAM window is runtime-dependent: the
 * image is linked at 0x14000000 (SC6531E's native window) but runs at
 * 0x04000000 (SD readbin, MEM_REMAP=0) or 0x34000000 (USB ram, MEM_REMAP=1)
 * on the SC6530 — a compile-time constant cannot cover both, and a bare
 * 0x14000000 is NOT a valid SC6530 address (writes there stall the AHB).
 * irq_init() derives the window from __image_start, a linker symbol whose
 * references ARE patched by arch/start.s apply_reloc (diff = runtime - link),
 * exactly like fpdoom entry.c:119-120 (fw_addr + 0x04000000). */

#ifndef HOST_TEST
/* SC6530 interrupt controller (fpdoom usbio.c:287-291 reads pending at
 * 0x80000004 for SC6530; enable at 0x80000008). NOTE: there is NO write
 * register at 0x8000000C — that address is INT_DISABLE/mask, and the
 * peripheral ISR clears its own source (see irq_handler). */
#define INT_PENDING     0x80000004u
#define INT_ENABLE      0x80000008u
#endif

#define IRQ_LINE_MAX    32

/* Registered C handlers, indexed by SC6530 interrupt line (0..31). */
static void (*s_handlers[IRQ_LINE_MAX])(void);

/* ---- asm entry points (arch/vectors.s, arch/mmu.s) ---------------------- */
extern uint8_t int_vectors[], int_vectors_end[];
extern void set_mode_sp(int mode, uint32_t sp);
extern void enable_mmu(uint32_t *ttb, uint32_t domain);
extern void invalidate_tlb_mva(uint32_t mva);
extern void invalidate_icache(void);
extern void clean_invalidate_dcache(void);

/* ---- pure dispatch logic (host-testable) -------------------------------- */

int irq_line_from_pending(uint32_t pending)
{
    int line = 0;

    if (pending == 0)
        return -1;
    while ((pending & 1u) == 0) {
        pending >>= 1;
        line++;
    }
    return line;
}

int irq_dispatch_for_test(uint32_t pending)
{
    int line = irq_line_from_pending(pending);

    /* ONLY dispatch a pending line that has a registered handler. The
     * SC6530 INTC pending register (0x80000004) reflects DISABLED lines
     * too (fpdoom checks pending bit 25 for USB without ever enabling it,
     * usbio.c:287-291), so an unhandled peripheral line appears here the
     * moment its source asserts. If such a line sits BELOW a registered
     * one (e.g. the keypad/EIC lines vs the 1ms timer on line 23), the
     * naive lowest-line dispatch returns without running the timer ISR —
     * the timer source is never acked, the ENABLED line stays pending and
     * the IRQ re-enters forever: the "1ms tick + key activity" hard freeze
     * seen on hardware (2026-08-21; immune to every task-level mitigation
     * because the storm lives in the IRQ path, not a task). */
    while (line >= 0 && s_handlers[line] == NULL) {
        pending &= ~(1u << line);
        line = irq_line_from_pending(pending);
    }
    if (line < 0)
        return -1;
    s_handlers[line]();
    return line;
}

void irq_register(int line, void (*fn)(void))
{
    if (line < 0 || line >= IRQ_LINE_MAX)
        return;
    s_handlers[line] = fn;
}

/* ---- next-wave hook: 1 ms system-timer ISR ------------------------------ */
__attribute__((weak)) void sys_tick_isr(void)
{
    /* The preemptive scheduler overrides this and calls
     * irq_register(SYS_TIMER_IRQ_LINE, sys_tick_isr). */
}

/* ---- 1 ms system-timer (timer 2 @ 0x81000040, IRQ line 23) --------------
 * fpdoom lcd_mono.h:288-327 (Unlicense): LCD_TIMER 2, LCD_TIMER_ADDR =
 * 0x81000000 | (2/3)<<12 | (2%3)<<5 = 0x81000040; timer 2 runs on the
 * 26 MHz clock (comment: "2,5 freq: 26M"); 1 ms load = 26000.
 *   ctl @ +8  = 0xc0 (enable)
 *   load @ +0 = 26000
 *   int  @ +0xc = 1 (enable); ISR clears by writing 9 (bit3 clr + bit0 en)
 * IRQ line: fpdoom maps timer index j -> SC6530 line: j = i+4 = 6, and
 * `if (_chip == 3 && j == 6) j = 23` -> line 23.
 * Chip-level clock enable + reset (lcd_mono.h:296-308):
 *   0x8b0000a0 = 0x410000 (enable, i<3); 0x8b000060 = 0x2000 (reset) x2.
 * We OR (never clobber) the shared power/ADI bits in 0x8b0000a0. */

#define SYS_TIMER2_ADDR  0x81000040u
#define SYS_TIMER2_LOAD  26000u
#define SYS_TIMER_IRQ_LINE 23u

/* IRQ mask helpers: arch/vectors.s on ARM; no-ops on the host (the host
 * scheduler has no IRQ to mask). */
#ifndef HOST_TEST
void irq_enable(void);  /* arch/vectors.s */
void irq_disable(void); /* arch/vectors.s */
#else
void irq_enable(void) { }
void irq_disable(void) { }
#endif

void sys_timer_start(void)
{
#ifndef HOST_TEST
    /* clock enable + reset — fpdoom lcd_mono.h:296-308, VERBATIM:
     * clock gate is a WRITE (0x410000 for timer 2, chip != 1);
     * reset goes to 0x8b000060 then t += 4 -> 0x8b000064. The old code
     * wrote 0x2000 to 0x8b000060 twice (stuck in reset -> timer never
     * runs -> sched_ticks() never advances -> the torch sweep never
     * fired on hardware). */
    MEM4(0x8b0000a0) = 0x410000u;
    MEM4(0x8b000060) = 0x2000u;
    DELAY(1000);
    MEM4(0x8b000064) = 0x2000u;
    DELAY(1000);

    /* IRQ line + timer config (lcd_mono.h:310-327): INT_IRQ_ENABLE is a
     * WRITE (1 << j) — the timer is the only IRQ source we use. */
    MEM4(INT_ENABLE) = 1u << SYS_TIMER_IRQ_LINE;
    MEM4(SYS_TIMER2_ADDR + 8) = 0;          /* ctl off while configuring */
    MEM4(SYS_TIMER2_ADDR + 0) = SYS_TIMER2_LOAD;
    MEM4(SYS_TIMER2_ADDR + 0xc) = 1;        /* int enable */
    MEM4(SYS_TIMER2_ADDR + 8) = 0xc0;       /* ctl: enable */

    irq_register(SYS_TIMER_IRQ_LINE, sys_tick_isr);
#endif
}

/* Suspend/resume the 1 ms tick IRQ line at the INT controller (not just
 * the CPU I bit). The SD probe runs with the line fully disabled because
 * a constantly-asserting (pending) timer line during SDIO init kills the
 * phone on hardware: diag-sdmmu (timer never armed) completes the whole
 * SD stack, os.bin (timer armed + firing, CPU I bit masked but the line
 * still asserting into the INT controller) dies at sdio_init stage 5.
 * The timer hardware keeps free-running; only the IRQ delivery is off. */
void sys_timer_pause(void)
{
#ifndef HOST_TEST
    MEM4(INT_ENABLE) = 0;
#endif
}

void sys_timer_resume(void)
{
#ifndef HOST_TEST
    MEM4(INT_ENABLE) = 1u << SYS_TIMER_IRQ_LINE;
#endif
}

/* ---- fatal exception handlers (vector+0x24 / +0x28 slots) --------------- */

void def_data_except(uint32_t fsr, uint32_t far, uint32_t pc, uint32_t cpsr)
{
#ifndef HOST_TEST
    /* r0 bit 8 set by the prefetch-abort stub marks IFSR (fpdoom). cpsr
     * is the faulting CPSR (from SPSR_abt) — shows the mode/T/I state at
     * the fault. */
    kprintf("exception: FSR=0x%03x, FAR=0x%08x, PC=0x%08x, CPSR=0x%08x\n",
            (unsigned)(fsr & 0x1ff), (unsigned)far, (unsigned)pc,
            (unsigned)cpsr);
    for (;;)
        ;
#endif
}

void undef_handler(uint32_t pc)
{
#ifndef HOST_TEST
    kprintf("exception: undefined instruction, PC=0x%08x\n", (unsigned)pc);
    for (;;)
        ;
#endif
}

/* ---- IRQ dispatcher (installed at vector+0x20) -------------------------- */

void irq_handler(void)
{
#ifndef HOST_TEST
    uint32_t pending = MEM4(INT_PENDING);
    (void)irq_dispatch_for_test(pending);

    /* NO INT_CLEAR (0x8000000C) write here. That register is the SC6530
     * INT_DISABLE / mask, not an acknowledge: the stock kernel references
     * only 0x80000008 (INT_ENABLE, 4 refs in the kern region) and never
     * writes 0x8000000C; fpdoom's timer ISR clears the timer's OWN source
     * register (lcd_mono.h timer+0xc = 9) which deasserts the INTC pending
     * bit, and its irq_handler never touches 0x8000000C. Writing 1<<line
     * here after the first timer fire MASKED line 23 -> the 1 ms tick fired
     * exactly once then stopped (sched_ticks stuck at 1 on hardware,
     * learnings 2026-08-21). The peripheral ISR owns source-clearing. */
#endif
}

/* ---- RAM size probe (fpdoom entry.c:84-94 verbatim) ----------------------
 * "dcache must be disabled for this function": D-cache is effectively off
 * until enable_mmu flips the M bit, so the probes reach SDRAM directly.
 * Leaves the detected size stored at MEM4(addr) — fpdoom reads it back in
 * restrict_mem (syscode.c:1025). */
static uint32_t get_ram_size(uint32_t addr)
{
    uint32_t size = 1 << 20;    /* start from 2MB */
    uint32_t v1 = 0x12345678;

    do {
        if (size >> 27)
            break;
        size <<= 1;
        MEM4(addr + size) = v1;
        MEM4(addr) = size;
    } while (MEM4(addr + size) == v1);
    return size;
}

/* ---- MMU page table build (fpdoom entry.c:193-217 verbatim structure) ---
 * A 1 MB-section table: identity low 64 MB, full-access device sections
 * up to 0x900fffff (covers every SC6530 peripheral), nothing above, plus
 * cacheable/bufferable bits on CHIPRAM + the SDRAM window + the 16 MB NOR
 * firmware window, and the high-vector 0xffff0000 page mapped onto the
 * CHIPRAM vectors via a coarse page table (fpdoom sys_prep_vectors). */
static void mmu_build(uint32_t *ttb, uint32_t ram_addr, uint32_t ram_size,
                      uint32_t fw_addr)
{
    const uint32_t ro_domain = 1u << 5;     /* domain 1 (DACR 0x57 client) */
    const uint32_t cb = 3u << 2;            /* cached=1, buffered=1         */
    unsigned i;

    /* zero the whole table first: everything above 0x90100000 is then
     * "no access" (fpdoom entry.c:203), including the unmapped tail of
     * the high-vector 1 MB region. */
    for (i = 0; i < 0x1000; i++)
        ttb[i] = 0;

    /* 0x00000000-0x03ffffff: identity sections (fpdoom entry.c:199-200) */
    for (i = 0; i < 0x40; i++)
        ttb[i] = i << 20 | ro_domain | 0x12;

    /* 0x04000000-0x900fffff: full access (AP 11), device (no CB) —
     * covers SDRAM, CHIPRAM and all peripherals (fpdoom entry.c:201-202) */
    for (; i < 0x901; i++)
        ttb[i] = i << 20 | 3u << 10 | 0x12;

    /* CHIPRAM + SDRAM window: cacheable/bufferable (fpdoom entry.c:204-206) */
    ttb[CHIPRAM_BASE >> 20] |= cb;
    for (i = 0; i < (ram_size >> 20); i++)
        ttb[ram_addr >> 20 | i] |= cb;

    /* NOR/firmware window around fw_addr: cached, 16 MB (fpdoom
     * entry.c:208-211 — "for faster search"). */
    for (i = 0; i < 16; i++) {
        uint32_t j = (fw_addr >> 20) + i;
        ttb[j] = j << 20 | ro_domain | cb | 0x12;
    }

    /* High-vector page 0xffff0000 -> the CHIPRAM vectors, via a 4 KB
     * coarse page table (fpdoom syscode.c:1006-1014 verbatim):
     *   ttb[0xfff]  = L1 coarse-page descriptor pointing at VECTOR_L2
     *   L2 entry    = small page (type 2), CB, physical VECTOR_ADDR
     * The L2 index for 0xffff0xxx is 0xf0 (byte offset 0x3c0). */
    {
        uint32_t l2 = VECTOR_L2;
        unsigned j;

        for (j = 0; j < 0x400; j += 4)
            MEM4(l2 + j) = 0;
        MEM4(l2 + 0x3c0) = VECTOR_ADDR | cb | 2;
        ttb[0xfff] = l2 | ro_domain | 0x11;
    }
}

/* Byte copy for the vector table (no libc in the kernel). */
static void vectors_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    while (n--)
        *dst++ = *src++;
}

/* ---- boot the IRQ infrastructure ---------------------------------------- */

void irq_init(void)
{
#ifndef HOST_TEST
    extern char __image_start[];
    uint32_t image_addr = (uint32_t)(uintptr_t)__image_start; /* runtime
                                        image base — the linker symbol's
                                        references are patched by the .rel
                                        relocation in arch/start.s, so this
                                        reads 0x04000000 (SD) or 0x34000000
                                        (USB) at runtime, NOT the 0x14000000
                                        link constant. */
    uint32_t fw_addr  = image_addr & 0xf0000000u;  /* NOR XIP base        */
    uint32_t ram_addr = fw_addr + 0x04000000u;     /* PSRAM window         */
    uint32_t ram_size = get_ram_size(ram_addr);
    uint32_t *ttb = (uint32_t *)(ram_addr + ram_size - TTB_SIZE);
    uint8_t *p = (uint8_t *)VECTOR_ADDR;
    uint32_t n = (uint32_t)(int_vectors_end - int_vectors);

    /* 1. Page table at the top of SDRAM (fpdoom entry.c:195). */
    mmu_build(ttb, ram_addr, ram_size, fw_addr);

    /* 2. Copy the exception stubs to CHIPRAM 0x40019000 (fpdoom
     *    sys_prep_vectors:1001). Writes go straight to memory: D-cache
     *    is effectively disabled until enable_mmu flips the M bit. */
    vectors_copy(p, int_vectors, n);

    /* 3. Banked stacks for the exception modes at 0x40022000 (fpdoom
     *    sys_prep_vectors:1002-1005). The kernel itself stays in SYS mode
     *    on its existing stack (set_mode_sp returns to SYS/0xdf). */
    set_mode_sp(0xd2, MODE_STACK_SP);   /* sp_IRQ */
    set_mode_sp(0xd3, MODE_STACK_SP);   /* sp_SVC */
    set_mode_sp(0xd7, MODE_STACK_SP);   /* sp_ABT */
    set_mode_sp(0xdb, MODE_STACK_SP);   /* sp_UND */

    /* 4. Install the C handler pointers into the vector-table slots
     *    (fpdoom sys_set_handlers:1062-1072). Done BEFORE enable_mmu so
     *    the writes are in physical memory before caches go live.
     *    &fn is the interworking address (bit 0 = Thumb) — the stubs'
     *    `blx lr` enters the Thumb handler and returns to the ARM stub. */
    MEM4(VECTOR_ADDR + 0x20) = (uint32_t)(uintptr_t)&irq_handler;
    MEM4(VECTOR_ADDR + 0x24) = (uint32_t)(uintptr_t)&def_data_except;
    MEM4(VECTOR_ADDR + 0x28) = (uint32_t)(uintptr_t)&undef_handler;

    /* 5. Switch the MMU on (TTBR + DACR 0x57, caches flushed). High
     *    vectors (c1 bit 13) were already set by start.s. The D-cache
     *    clean+invalidate mirrors fpdoom sys_prep_vectors:1018 — belt and
     *    braces for any lines dirtied since enable_dcache() while the
     *    window was being rebuilt. */
    clean_invalidate_dcache();
    enable_mmu(ttb, 0x57);
    invalidate_tlb_mva(0xffff0000u);
    invalidate_icache();
#endif
}
