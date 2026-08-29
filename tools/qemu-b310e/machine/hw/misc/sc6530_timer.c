/*
 * Spreadtrum SC6530C system timers (B310E-OS QEMU machine).
 *
 * Todo 14 of .omo/plans/b310e-qemu-machine.md (Wave 3).
 *
 * Two register banks:
 *
 * 1. Timer2 at 0x81000040 (SYS_TIMER2_LOAD etc., kernel/irq.c). The whole
 *    4 KiB timer block at 0x81000000 is mapped so timer0/1 accesses stay
 *    benign (the registers of timer2, the one the B310E-OS kernel uses):
 *      +0x0 LOAD  - countdown reload value (guest writes 26000 = 1 ms
 *                   at the 26 MHz timer clock)
 *      +0x8 CTL   - 0xc0 = enable/run, 0 = stop (bits 7:6)
 *      +0xc INT   - bit0 = IRQ output enable (write 1),
 *                   bit1 = underflow/done status (the stock OS's one-shot
 *                   delay timer polls it - dump PSRAM 0x4010888),
 *                   bit2 = pending flag (readable),
 *                   write bit3 = clear pending (write 9 = clear + keep
 *                   enabled, the kernel's sys_tick_isr verbatim)
 *    A periodic 1 ms ptimer (26 MHz countdown, limit = LOAD) raises the
 *    SysBus IRQ output; the line stays asserted (level) until the guest
 *    clears the source at +0xc - the INTC has no pending-clear, the
 *    peripheral ISR owns source-clearing (see sc6530_intc.c).
 *
 *    Guest contract (kernel/irq.c sys_timer_start, lines 149-174):
 *      MEM4(+8)   = 0;          ctl off while configuring
 *      MEM4(+0)   = 26000;      load (1 ms @ 26 MHz)
 *      MEM4(+0xc) = 1;          int enable
 *      MEM4(+8)   = 0xc0;       ctl: enable
 *    ISR (kernel/sched.c sys_tick_isr, lines 57-65):
 *      if (MEM4(+0xc) & 4u) MEM4(+0xc) = 9;   clear + keep enabled
 *
 * 2. Sys-timer at 0x81003000 (SC6530_SYS_TIMER per include/arch.h):
 *      +0x4 SYS_CNT0 - free-running 1 ms counter (the stock PBL delay
 *                      loops busy-wait on it advancing; the counter must
 *                      move or the guest spins)
 *      +0x8 SYS_CTL  - store+echo control register: the stock OS's
 *                      Syscnt_Init RMW chain (dump 0x69350, SDK
 *                      syscnt_drv.c: REG32(SYS_CTL) &= ~BIT_0; |= BIT_3,
 *                      which writes 9) reads the register back between
 *                      writes - echo keeps that chain stable
 *      +0xc SYS_MS   - 1 ms monotonic counter with bit 0 write-echo: the
 *                      same 0x69350 loop does REG32(SYS_MS) &= ~1, so the
 *                      bit-0 write must survive while the counter keeps
 *                      counting
 *    These three addresses were benign-table entries in todo-12's
 *    sc6530_aux catch-all; this device now shadows them at a higher
 *    priority (b310e.c maps at B310E_REGION_PRIORITY 1) and reproduces
 *    the same semantics (the aux entries are dead code from here on).
 *
 * All qemu_log lines carry the guest PC where applicable; the timer2
 * INT read/write lines are the todo-14 acceptance evidence (the guest's
 * pending check "& 4" and the write-9 clear).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/cpu.h"          /* current_cpu (sc6530_timer_guest_pc) */
#include "target/arm/cpu.h"       /* ARM_CPU() cast (same helper) */
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_SC6530_TIMER "sc6530_timer"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530TimerState, SC6530_TIMER)

/* Timer2 register offsets (block base 0x81000000, timer2 at +0x40). */
#define SC6530_TIMER2_LOAD      0x40
#define SC6530_TIMER2_CTL       0x48
#define SC6530_TIMER2_INT       0x4c

/* The SC6530 timer clock is 26 MHz; the kernel loads 26000 = 1 ms. */
#define SC6530_TIMER2_FREQ      26000000u
#define SC6530_TIMER2_DEFLOAD   26000u

/* Sys-timer register offsets (bank base 0x81003000). */
#define SC6530_SYSTIMER_CNT0    0x04
#define SC6530_SYSTIMER_CTL     0x08
#define SC6530_SYSTIMER_MS      0x0c

#define SC6530_TIMER_BLOCK_SIZE 0x1000
#define SC6530_SYSTIMER_SIZE    0x1000

struct Sc6530TimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion timer2_iomem;     /* 0x81000000 (timer2 regs at +0x40) */
    MemoryRegion systimer_iomem;   /* 0x81003000 */

    ptimer_state *ptimer;
    qemu_irq irq;

    uint32_t load;       /* SYS_TIMER2_LOAD countdown reload value */
    uint32_t ctl;        /* SYS_TIMER2_CTL (0xc0 = run) */
    bool int_enable;     /* SYS_TIMER2_INT bit0: IRQ output enabled */
    bool pending;        /* SYS_TIMER2_INT bit2: tick fired, not cleared */

    uint32_t sys_ctl;    /* sys-timer +0x8: last written value (echo) */
    uint32_t sys_ms_b0;  /* sys-timer +0xc: bit-0 write echo */
};

static uint32_t sc6530_timer_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

static void sc6530_timer_update_irq(Sc6530TimerState *s)
{
    qemu_set_irq(s->irq, s->pending && s->int_enable);
}

static void sc6530_timer_tick(void *opaque)
{
    Sc6530TimerState *s = opaque;

    /* The tick latches the pending flag and asserts the IRQ output; the
     * line stays up until the guest clears the source (write-9 to +0xc).
     * The ptimer keeps running periodically from its load value. */
    s->pending = true;
    qemu_log("sc6530_timer: tick expired (pending=1)\n");
    sc6530_timer_update_irq(s);
}

static void sc6530_timer2_run(Sc6530TimerState *s)
{
    /* Start (or restart) the periodic countdown: limit = LOAD at the
     * 26 MHz timer clock -> 26000 = 1 ms. ptimer_run(..., 0) is the
     * periodic mode (musicpal's mv88w8618_timer pattern). */
    ptimer_transaction_begin(s->ptimer);
    ptimer_set_limit(s->ptimer,
                     s->load ? s->load : SC6530_TIMER2_DEFLOAD, 1);
    ptimer_set_freq(s->ptimer, SC6530_TIMER2_FREQ);
    ptimer_run(s->ptimer, 0);
    ptimer_transaction_commit(s->ptimer);
}

static void sc6530_timer2_stop(Sc6530TimerState *s)
{
    ptimer_transaction_begin(s->ptimer);
    ptimer_stop(s->ptimer);
    ptimer_transaction_commit(s->ptimer);
}

/* ---------------------------------------------------------------------- */
/* Timer2 block (0x81000000, timer2 at +0x40)                             */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_timer2_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    Sc6530TimerState *s = opaque;
    uint64_t val = 0;

    switch (offset) {
    case SC6530_TIMER2_LOAD:
        val = s->load;
        break;
    case SC6530_TIMER2_CTL:
        val = s->ctl;
        break;
    case SC6530_TIMER2_INT:
        /* bit0 = IRQ enable, bit1 = underflow/done status (the stock OS's
         * one-shot delay timer waits for it - dump PSRAM 0x4010888 does
         * lsls r5,r5,#30 / bpl, i.e. polls bit 1), bit2 = pending flag
         * (our os.bin's sys_tick_isr checks & 4). Both 1 and 2 latch on
         * the tick. */
        val = (s->int_enable ? 1u : 0u) | (s->pending ? 6u : 0u);
        qemu_log("sc6530_timer: INT read val=0x%" PRIx64 " pc=0x%08"
                 PRIx32 "\n", val, sc6530_timer_guest_pc());
        break;
    default:
        /* Unknown offsets in the timer block: benign (read 0). */
        break;
    }
    return val;
}

static void sc6530_timer2_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    Sc6530TimerState *s = opaque;
    uint32_t pc = sc6530_timer_guest_pc();

    switch (offset) {
    case SC6530_TIMER2_LOAD:
        s->load = value;
        qemu_log("sc6530_timer: timer2 LOAD write val=0x%" PRIx64
                 " pc=0x%08" PRIx32 "\n", value, pc);
        if (s->ctl & 0xc0) {
            sc6530_timer2_run(s);   /* reload while running */
        }
        break;
    case SC6530_TIMER2_CTL:
        s->ctl = value;
        qemu_log("sc6530_timer: timer2 CTL write val=0x%" PRIx64
                 " pc=0x%08" PRIx32 "\n", value, pc);
        if (value & 0xc0) {
            sc6530_timer2_run(s);
        } else {
            sc6530_timer2_stop(s);
        }
        break;
    case SC6530_TIMER2_INT:
        /* bit0 = IRQ enable, bit3 = clear pending (write 9 = both; the
         * kernel's sys_tick_isr writes exactly 9). The pending bit stays
         * readable independent of the enable state. */
        qemu_log("sc6530_timer: INT write val=0x%" PRIx64 " pc=0x%08"
                 PRIx32 "\n", value, pc);
        s->int_enable = (value & 1u) != 0;
        if (value & 8u) {
            s->pending = false;
        }
        sc6530_timer_update_irq(s);
        break;
    default:
        /* Unknown offsets: absorb (the catch-all convention). */
        break;
    }
}

static const MemoryRegionOps sc6530_timer2_ops = {
    .read = sc6530_timer2_read,
    .write = sc6530_timer2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* Sys-timer bank (0x81003000)                                            */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_systimer_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    Sc6530TimerState *s = opaque;
    uint64_t ms;

    switch (offset) {
    case SC6530_SYSTIMER_CNT0:
        /* Free-running 1 ms counter: the stock PBL delay loops busy-wait
         * on it advancing, so it must always move. */
        return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    case SC6530_SYSTIMER_CTL:
        /* Store+echo: the stock Syscnt_Init RMW chain (dump 0x69350)
         * reads this register back between writes. */
        return s->sys_ctl;
    case SC6530_SYSTIMER_MS:
        /* 1 ms monotonic counter, bit 0 write-echo: the same 0x69350
         * chain does REG32(SYS_MS) &= ~1, so bit 0 is guest-writable
         * while the counter keeps counting. */
        ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        return (ms & ~1ull) | s->sys_ms_b0;
    default:
        return 0;
    }
}

static void sc6530_systimer_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    Sc6530TimerState *s = opaque;
    uint32_t pc = sc6530_timer_guest_pc();

    switch (offset) {
    case SC6530_SYSTIMER_CNT0:
        /* Counter: absorb writes. */
        break;
    case SC6530_SYSTIMER_CTL:
        s->sys_ctl = value;
        qemu_log("sc6530_timer: sys-ctl write val=0x%" PRIx64 " pc=0x%08"
                 PRIx32 "\n", value, pc);
        break;
    case SC6530_SYSTIMER_MS:
        /* Echo bit 0 (the &= ~1 write), keep the counter counting. */
        s->sys_ms_b0 = value & 1u;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sc6530_systimer_ops = {
    .read = sc6530_systimer_read,
    .write = sc6530_systimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_timer_reset(DeviceState *dev)
{
    Sc6530TimerState *s = SC6530_TIMER(dev);

    sc6530_timer2_stop(s);
    s->load = SC6530_TIMER2_DEFLOAD;
    s->ctl = 0;
    s->int_enable = false;
    s->pending = false;
    s->sys_ctl = 0;
    s->sys_ms_b0 = 0;
    sc6530_timer_update_irq(s);
}

static void sc6530_timer_init(Object *obj)
{
    Sc6530TimerState *s = SC6530_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sysbus_init_irq(sbd, &s->irq);

    s->ptimer = ptimer_init(sc6530_timer_tick, s, PTIMER_POLICY_LEGACY);
    s->load = SC6530_TIMER2_DEFLOAD;

    memory_region_init_io(&s->timer2_iomem, obj, &sc6530_timer2_ops, s,
                          "sc6530-timer2", SC6530_TIMER_BLOCK_SIZE);
    sysbus_init_mmio(sbd, &s->timer2_iomem);

    memory_region_init_io(&s->systimer_iomem, obj, &sc6530_systimer_ops, s,
                          "sc6530-systimer", SC6530_SYSTIMER_SIZE);
    sysbus_init_mmio(sbd, &s->systimer_iomem);
}

static void sc6530_timer_finalize(Object *obj)
{
    Sc6530TimerState *s = SC6530_TIMER(obj);

    ptimer_free(s->ptimer);
}

static const VMStateDescription sc6530_timer_vmsd = {
    .name = "sc6530_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(ptimer, Sc6530TimerState),
        VMSTATE_UINT32(load, Sc6530TimerState),
        VMSTATE_UINT32(ctl, Sc6530TimerState),
        VMSTATE_BOOL(int_enable, Sc6530TimerState),
        VMSTATE_BOOL(pending, Sc6530TimerState),
        VMSTATE_UINT32(sys_ctl, Sc6530TimerState),
        VMSTATE_UINT32(sys_ms_b0, Sc6530TimerState),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 system timer (timer2 + sys-timer)";
    device_class_set_legacy_reset(dc, sc6530_timer_reset);
    dc->vmsd = &sc6530_timer_vmsd;
}

static const TypeInfo sc6530_timer_info = {
    .name = TYPE_SC6530_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530TimerState),
    .instance_init = sc6530_timer_init,
    .instance_finalize = sc6530_timer_finalize,
    .class_init = sc6530_timer_class_init,
};

static void sc6530_timer_register_types(void)
{
    type_register_static(&sc6530_timer_info);
}

type_init(sc6530_timer_register_types)
