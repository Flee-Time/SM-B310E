/*
 * Spreadtrum SC6530C auxiliary log+store device: AHB/APB/SMC/GPIO/pinmux
 * banks plus the address-space catch-all.
 *
 * Todo 12 of .omo/plans/b310e-qemu-machine.md (Wave 3).
 *
 * The SC6530C has a huge flat peripheral map with many register banks the
 * emulator does not model yet. This device covers the "everything else"
 * banks with a store+echo semantic (reads return the last written value)
 * so guest read-modify-write chains stay stable, and adds a low-priority
 * catch-all over 0x10000000..0xffffffff that turns every unmapped access
 * into a logged UNMODELED line instead of a guest abort.
 *
 * Regions (all store+echo on a uint32 backing array):
 *   AHB    0x20500000   (incl. 0x205000e0 MEM_REMAP - logged no-op; the
 *                       PSRAM alias in b310e.c is always on; 0x205003fc
 *                       chip-id READS return 0x6530c000)
 *   APB    0x8b000000   (incl. 0x8b000060/64 reset, 0x8b0000a0/a4 power;
 *                       trace events sc6530_aux_read/write with the guest
 *                       PC - this bank carries the audio PA power ladder
 *                       and the ARM-vs-DSP AUD-ownership register
 *                       0x8b0001c4 bits 9-10 = 0x600, which the Wave-6
 *                       diff must capture, docs/audio-dsp-protocol.md
 *                       "Cross-map")
 *   SMC    0x20000000
 *   GPIO   0x8a000000
 *   pinmux 0x8c000000   (reads return 0 by store+echo default; the stock
 *                       OS's pinmap replay is safe here - a feature)
 *   busmon 0x20400000   (AHB bus monitor, SDK BUS_MONx_CTL_BASE; SILENT
 *                       store+echo - the stock OS's boot loop clears bit 0
 *                       of channels 0-2 every iteration, so per-access
 *                       logging is pure noise. Reads return 0 = no bus
 *                       match = no interrupt, the benign default)
 *   catch-all 0x10000000..0xffffffff at priority 0 (b310e.c maps its
 *                       regions at priority 1, so they always win; the
 *                       catch-all only sees unclaimed addresses)
 *
 * NO watchdog here: 0x82001480 lives in the todo-15 sc6530_adi ANA region
 * (0x82001000, 8 KiB) - mapping it here would overlap that region.
 *
 * BENIGN-READY RULE: a poll-until-set loop on an always-0 register is a
 * silent guest spin (plan "standard QA rubric"). The catch-all consults a
 * small table of known polled status registers and answers them with
 * benign values instead of 0. Seeded with the stock sys-timer pair (dump
 * 0x69350, SDK syscnt_drv.c); extend during Wave 5 as poll loops are
 * found. Every entry is documented in the code.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "trace.h"

#define TYPE_SC6530_AUX "sc6530_aux"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530AuxState, SC6530_AUX)

/* ---------------------------------------------------------------------- */
/* Region geometry                                                        */
/* ---------------------------------------------------------------------- */

#define SC6530_AUX_AHB_BASE      0x20500000ULL
#define SC6530_AUX_AHB_SIZE      0x1000
#define SC6530_AUX_APB_BASE      0x8b000000ULL
#define SC6530_AUX_APB_SIZE      0x3000   /* covers 0x8b002068 (RST0_CLR) */
#define SC6530_AUX_SMC_BASE      0x20000000ULL
#define SC6530_AUX_SMC_SIZE      0x1000
#define SC6530_AUX_GPIO_BASE     0x8a000000ULL
#define SC6530_AUX_GPIO_SIZE     0x1000
#define SC6530_AUX_PINMUX_BASE   0x8c000000ULL
#define SC6530_AUX_PINMUX_SIZE   0x1000

/* BUS_MON (AHB bus monitor, SDK busmonitor_reg_v5.h BUS_MONx_CTL_BASE):
 * channel 0 at 0x20400000 with the 0x1000 legacy channel stride. The
 * stock OS's boot loop disables channels 0-2 (clear bit 0 of CHN_INT) on
 * every pass; reads must return 0 (no match = no interrupt). */
#define SC6530_AUX_BUSMON_BASE   0x20400000ULL
#define SC6530_AUX_BUSMON_SIZE   0x3000   /* covers the 3 channels the
                                             guest actually touches */

#define SC6530_AUX_CATCHALL_BASE 0x10000000ULL
#define SC6530_AUX_CATCHALL_SIZE 0xF0000000ULL   /* ..0xffffffff inclusive */

/* PSRAM overlay helpers for the stock OS's pre-tick boot gates (todo-23,
 * Wave 5). Both gates are POLLED/read flags whose natural setters are dead
 * code or gated behind an unreachable indirect boot dispatch; per the
 * todo-12 benign-ready rule we answer them with the state the guest waits
 * for, absorbing the guest's own writes (the boot's BSS memset zeroes both).
 * Guest evidence: the boot task 0x11172 polls [0x0425de8c] every loop pass
 * (flag getter at PSRAM 0x04009cc6, observed reads pc=0x04009cd0) and the
 * log-event/module code calls the timer object [0x0422d2cc] -> deref -> blx
 * (garbage jump at NOR 0x1e6da, R14=0x1e6dd). The timer init (NOR 0x1e2a4)
 * would set [0x0422d2cc]=0x0422e3d0 (0x37cd6 returns [0x37d24]+24) - that
 * static table IS populated (register bases + real driver fns 0x37c17..);
 * only the pointer is unset because the init chain (0x111758->...->0x16c4c)
 * never runs. Both overlays sit OVER the PSRAM alias at priority 2.
 */
#define SC6530_AUX_BOOTREADY_BASE  0x0425de8cULL   /* boot-ready/mode byte +16 of 0x0425de7c */
#define SC6530_AUX_BOOTREADY_SIZE  0x4
#define SC6530_AUX_TIMEROBJ_BASE   0x0422d2ccULL   /* timer-ops pointer */
#define SC6530_AUX_TIMEROBJ_SIZE   0x4
#define SC6530_AUX_TIMEROBJ_VAL    0x0422e3d0ULL   /* the timer ops struct */
#define SC6530_AUX_TIMERFN_BASE    0x0422e3e0ULL   /* timer function pointer (+0x10) */
#define SC6530_AUX_TIMERFN_SIZE    0x4
#define SC6530_AUX_TIMERFN_VAL     0x00037c17ULL   /* stub fn returning 0 (thumb 0x37c16) */
/* SCI IRQ nesting position (s_irq_status_postion, struct @ 0x0422d4ac).
 * The guest's SCI_IRQ_ENTRY (PSRAM 0x040066b2) pos++ / EXIT (0x040066f6)
 * pos-- balance is broken by our model: the position climbs in LOCKSTEP with
 * the boot-task gate counter (observed pos==cnt: 4/4 7/7 10/10) and at
 * SCI_MAX_IRQ_NESTING (10) the ENTRY asserts (0x040066bc -> boot task),
 * recursing so the boot task's counter++ never fires. gdb experiment: zeroing
 * the position unlocks the boot task EXIT. Cap at 9 = never assert. */
#define SC6530_AUX_SCIRPOS_BASE    0x0422d4b4ULL   /* s_irq_status_postion */
#define SC6530_AUX_SCIRPOS_SIZE    0x4
#define SC6530_AUX_SCIRPOS_MAX     9u              /* SCI_MAX_IRQ_NESTING-1 */
/* dl_ofi driver-layer table pointer (struct @ 0x0422e32c, +4 = 0x0422e330).
 * The dl_ofi registration (NOR 0x357a4) calls 0x69228 (returns the literal
 * at 0x69230 = 0x04230a74) and stores it at [0x0422e330]; that init chain
 * does not run on the warm path, so the pointer stays NULL and the dl_ofi
 * lookup (0x355a0) asserts "NULL" at 0x355b0. The table at 0x04230a74 IS
 * statically populated (runtime read: entry 0 = id 6, ANA regs 0x82001180 /
 * 0x820011a0 / 0x820011c0 - the audio-hal cross-map register family). */
#define SC6530_AUX_DLOFITBL_BASE   0x0422e330ULL   /* dl_ofi driver-table ptr */
#define SC6530_AUX_DLOFITBL_SIZE   0x4
#define SC6530_AUX_DLOFITBL_VAL    0x04230a74ULL   /* the driver table base */
/* ThreadX kernel-struct magic: the RTOS APIs check
 * *(DAT_0422c65c - 8) == 0x20021201 (FUN_0010f21a/0010fd72 etc., Ghidra).
 * The kernel struct at 0x0422c65c is uninitialized (runtime [-8] = 0x16)
 * because the ThreadX init chain never runs on the warm path (milestone-d).
 * The special magic-id object the resolver FUN_0011d948 returns for
 * id == 0x4154494d (0x0423c818) also holds 0 - the APIs check its [0] ==
 * 0x20021201. Overlay both magics. */
#define SC6530_AUX_TXKERN_MAGIC_BASE 0x0422c654ULL  /* 0x0422c65c - 8 */
#define SC6530_AUX_TXKERN_MAGIC_SIZE 0x4
#define SC6530_AUX_TXKERN_MAGIC_VAL  0x20021201u
#define SC6530_AUX_TXOBJ_MAGIC_BASE  0x0423c818ULL  /* resolver magic-id obj */
#define SC6530_AUX_TXOBJ_MAGIC_SIZE  0x4
#define SC6530_AUX_TXOBJ_MAGIC_VAL   0x20021201u
/* LCD driver table (FUN_00014aa4, lcd.c): DAT_00014bb0 = 0x0422c8ec.
 * The table entry [0] (the lcd driver struct) is NULL because the driver
 * init chain never runs (milestone-d), so the panel-resolution check
 * (uVar1/uVar2 from the entry's +8 struct vs FUN_00024b1a's hardcoded
 * 0x80 x 0xA0 for lcd_id 0) asserts. The real panel descriptor with
 * width=0x80 height=0xA0 sits at NOR 0x0CB414. Overlay: table[0] ->
 * a driver struct at 0x0422c8f4 whose +8 = 0x0CB414 (panel ptr). */
#define SC6530_AUX_LCDTBL_BASE       0x0422c8ecULL  /* LCD driver table [0] */
#define SC6530_AUX_LCDTBL_SIZE       0x70
#define SC6530_AUX_LCDTBL_VAL        0x0422c8f4ULL  /* the driver struct */
#define SC6530_AUX_LCDSPEC_BASE      0x0425e0e8ULL  /* LCD driver spec struct */
#define SC6530_AUX_LCDSPEC_SIZE      0x210
#define SC6530_AUX_LCDDRV_PANEL_BASE 0x0422c8fcULL  /* drv struct + 8 */
#define SC6530_AUX_LCDDRV_PANEL_SIZE 0x4
#define SC6530_AUX_LCDDRV_PANEL_VAL  0x000cb414ULL  /* the 128x160 panel */
/* USB vcom device thiz (s_dev_usb[0], struct @ 0x0422e554 - the "vcom_usb.c"
 * DEVICE_Find("USB") clock-device object; the global at NOR 0x38e50 =
 * 0x0422e554). The vcom init FUN_00038c14 dispatches on s_dev_usb[0] (the
 * device's thiz/ClkObj, image value 0 - the warm-path device-create chain
 * never runs). FUN_00032118 treats it as a BASE and ends with a SECOND-LEVEL
 * call: blx [[thiz+0] + 0x2c]. A null/zero-region thiz derefs the NOR vector
 * table (0x0/0x8 = 0xe59ff018 "ldr pc,[pc,#24]", [0x2c] = the vector slot
 * 0x10008) -> vector indirection -> abort handler 0xbf428 -> PSRAM
 * dispatcher -> PBL 0xbf150 reboot (the ~40s re-init, saved-LR 0x32187).
 * Answer thiz = the object itself AND the +40 byte flag = 1: with flag 1 the
 * dispatcher takes the msg2 branch (0x32152 -> 0x32186) and returns,
 * skipping BOTH the recursion and the second-level call. Guest evidence:
 * dispatcher reads pc=0x32130/0x32140/0x32154/0x32166/0x3217a/0x3217c +
 * blx at 0x32185 (disasm of saved LR 0x32187). */
#define SC6530_AUX_CLKOBJ_BASE      0x0422e554ULL  /* s_dev_usb[0] thiz ptr */
#define SC6530_AUX_CLKOBJ_SIZE      0x4
#define SC6530_AUX_CLKOBJ_VAL       0x0422e554ULL  /* thiz = the object */
#define SC6530_AUX_CLKOBJFLAG_BASE  0x0422e57cULL  /* thiz+40 flag byte */
#define SC6530_AUX_CLKOBJFLAG_SIZE  0x1
#define SC6530_AUX_CLKOBJFLAG_VAL   1u             /* msg2 -> early return */
/* SCI_BLKMEM direct-struct pools (the 0x44444444/0x33333333-type allocs,
 * sci_mem.c FUN_0010dcea branches 2-3 -> FUN_0010d9b4). The pool table at
 * 0x0422D478 (+8 = the 0x44444444/0xE4444444 pool ptr, +0xc = the
 * 0x33333333 pool ptr) is all zeros on the warm/stock path (the SCI pool
 * init never runs) -> FUN_0010d9b4(0, size) returns 0 -> the sci_mem
 * "ASSERT: Error 0xff ... param=0x30" crash loop. Answer a valid pool
 * pointer + a minimal "PEAK"-magic pool struct + its memory-space so the
 * allocator FUN_00100337a's free-memory carve succeeds. Layout (from the
 * live disasm): the "PEAK" pool [0]="PEAK", [8]=&memspace, [0x38]=0x34
 * (the pool-list walker reads [pool+0x38]-0x34 and must get 0 to stop);
 * the memory-space [0]=bitmap, [8]=free size, [0x14]=free ptr, [0x24]=
 * 0x04259620 (checked by the allocator). Store+echo so the sequential
 * allocs shrink the free list. Guest evidence: FUN_0010dd9a(0x30,
 * 0x44444444, 0x4005fc8, 2933), live pool table 0x0422D478 all zeros,
 * 0x04280000 free (zeros). */
#define SC6530_AUX_SCIPOOLTBL_BASE  0x0422d478ULL   /* SCI pool table */
#define SC6530_AUX_SCIPOOLTBL_SIZE  0x10            /* +8/+0xc entries */
#define SC6530_AUX_SCIPOOLTBL_POOL  0x04280000ULL   /* the PEAK pool */
#define SC6530_AUX_SCIPOOL_BASE     0x04280000ULL   /* the PEAK pool */
#define SC6530_AUX_SCIPOOL_SIZE     0x40            /* [0]..[0x38] */
#define SC6530_AUX_SCIPOOL_MAGIC    0x5045414bu     /* "PEAK" */
#define SC6530_AUX_SCIPOOL_MSPACE   0x04280040ULL   /* the memory-space */
#define SC6530_AUX_SCIMEM_BASE      0x04280040ULL   /* the memory-space */
#define SC6530_AUX_SCIMEM_SIZE      0x28            /* [0]..[9] */
#define SC6530_AUX_SCIMEM_FREESZ    0x1000u         /* free-memory size */
#define SC6530_AUX_SCIMEM_FREEPTR   0x04281000ULL   /* free-memory base */
/* The allocator's owner check (PSRAM 0x400337a): `ldr r1,[pc,#824]`
 * loads 0x04259620 (the ThreadX pool base literal) then `ldr r1,[r1,#0]`
 * dereferences it - the check is [mem+0x24] == [0x04259620], and
 * [0x04259620] = 0 at runtime (the ThreadX pool struct is uninitialized;
 * monitor-verified all-zeros). So the memspace owner field must be 0. */
#define SC6530_AUX_SCIMEM_OWNER     0u              /* [0x04259620] = 0 */
/* ThreadX kernel-state globals (the "Invalid caller of this service" gate,
 * service PSRAM 0x400be94, threadx_os.c:2958). The check:
 * r6 = [0x0422C65C]; if r6 == 0 -> [0x0422C67C] (ONE deref) must ==
 * 0xF0F0F0F0 (the service's literal [0x400bf00]). At runtime both globals
 * are 0 -> mismatch -> Error 0x13 (the sci_mem crash after the pool fix).
 * Overlay [0x0422C67C] = 0xF0F0F0F0 so the check passes (monitor-verified
 * service read at pc=0x400beca). */
#define SC6530_AUX_TXSTATE1_BASE    0x0422c67cULL  /* kernel-state word */
#define SC6530_AUX_TXSTATE1_SIZE    0x4
#define SC6530_AUX_TXSTATE1_VAL     0xf0f0f0f0u    /* scheduler-ready */
#define SC6530_AUX_TXSTATE2_BASE    0x0422c680ULL  /* unused neighbor */
#define SC6530_AUX_TXSTATE2_SIZE    0x4
#define SC6530_AUX_TXSTATE2_VAL     0xf0f0f0f0u
/* dlmalloc guard global (0x04259620): the mspace_malloc entry
 * (FUN_0010c956, file "threadx_malloc.c") checks `mstate[0x24] ==
 * *DAT_0010cc94` (i.e. == [0x04259620]) and asserts "ASSERT(0)" at line
 * 1274 on mismatch - THE current assert screen. Both words are BSS-zero
 * at boot (passes) but the ~40 s soft-restart reload (init-data reload
 * dispatcher NOR 0x103dc: memcpy NOR 0x12E184 -> PSRAM 0x0423BBA8,
 * 676 KB) copies compressed junk over the whole 0x0423BBA8..0x042E4BA8
 * window: [0x04259620] = 0x02184BEA, [0x04280040+0x24] = 0xB59BCEA0 ->
 * mismatch -> the 1274 assert screen. Overlay the guard = 0 (absorb ALL
 * writes incl. the reload junk) and pin the mstate guard word (SCIMEM
 * +0x24, see below) to 0 so the check ALWAYS passes. */
#define SC6530_AUX_TXGUARD_BASE    0x04259620ULL  /* dlmalloc guard global */
#define SC6530_AUX_TXGUARD_SIZE    0x4
#define SC6530_AUX_TXGUARD_VAL     0u
/* ThreadX byte-pool head (0x04259638): the sci_mem.c type-0x22222222
 * pool-pointer literal DAT_0010e0e4 (the learnings' "BYTE @ [0x04259638]")
 * and the 0x11FA7C literal. The dispatcher FUN_0010dcea calls
 * tx_byte_allocate (FUN_00114be8: *pool == 0x42595445 "BYTE") -> walker
 * FUN_001162da -> FUN_001160a0 (the WAIT_FOREVER variant, option
 * DAT_0010e0d8 == 0xAAAAAAAA): reads pool[0]=magic, [8]=available,
 * [0xc]=fragments, [0x18]=search head, [0x20]=owner, [0x34] limit,
 * [0x38] min-available, [0x44] threshold. Free node: [0]=next,
 * [1]==0xFFFFEEEE (free marker, DAT_00117ab8); the chain ends at a
 * sentinel node whose [1]==0xAAAAAAAA (DAT_00116440 - the walk exit).
 * The reload junk kills the same way -> the write path validates values
 * (the allocator's carve updates: available shrinks, fragments 1/2,
 * node ptrs in the arena, markers) and rejects the junk. */
#define SC6530_AUX_TXBYTEPOOL_BASE 0x04259638ULL  /* tx_byte_allocate pool */
#define SC6530_AUX_TXBYTEPOOL_SIZE 0x48           /* [0]..[0x44] */
#define SC6530_AUX_TXBYTEPOOL_MAGIC 0x42595445u   /* "BYTE" */
#define SC6530_AUX_TXBYTEPOOL_FREE  0x1000u       /* available bytes */
#define SC6530_AUX_TXBYTEPOOL_NODE  0x04259700ULL /* free-list head */
#define SC6530_AUX_TXBYTEPOOL_SENT  0x0425a708ULL /* chain terminator */
#define SC6530_AUX_TXBYTEPOOL_MIN   0x1000u       /* min-available seed */
#define SC6530_AUX_TXBYTENODE_BASE  0x04259700ULL /* the anchor free node */
#define SC6530_AUX_TXBYTENODE_SIZE  0x8
#define SC6530_AUX_TXBYTENODE_MARK  0xffffeeeeu   /* 0xFFFFEEEE free marker */
#define SC6530_AUX_TXBYTESENT_BASE  0x0425a708ULL /* the sentinel node */
#define SC6530_AUX_TXBYTESENT_SIZE  0x8
#define SC6530_AUX_TXBYTESENT_MARK  0xaaaaaaaau   /* 0xAAAAAAAA walk exit */

/* AHB special read: chip-id at 0x205003fc (full chip ID 0x6530c000). */
#define SC6530_AUX_CHIP_ID_OFF   0x3fc
#define SC6530_CHIP_ID           0x6530c000u

/* ---------------------------------------------------------------------- */
/* Benign-ready table (see the header comment)                            */
/* ---------------------------------------------------------------------- */

typedef enum {
    SC6530_AUX_BENIGN_FIXED,    /* reads return .value                  */
    SC6530_AUX_BENIGN_ECHO,     /* reads return the last written value  */
    SC6530_AUX_BENIGN_COUNTER,  /* reads return a monotonic 1 ms value  */
} Sc6530AuxBenignMode;

typedef struct {
    uint64_t addr;                 /* full guest address */
    Sc6530AuxBenignMode mode;
    uint32_t value;                /* FIXED value */
    unsigned echo_slot;            /* ECHO slot in state->benign_echo[] */
} Sc6530AuxBenignReg;

#define SC6530_AUX_BENIGN_ECHO_SLOTS 2

static const Sc6530AuxBenignReg sc6530_aux_benign[] = {
    /*
     * 0x81003008 = the sys-timer SYS_CTL (SDK syscnt_drv.c, v0 regs:
     * SYS_CTL = SYSTIMER_BASE + 0x8). The stock OS's Syscnt_Init /
     * Syscnt_ClearTimer RMW chain (REG32(SYS_CTL) &= ~BIT_0 then
     * |= BIT_3) is visible in the dump at 0x69350: it READS BACK the
     * register between the two writes (ldr / lsr / lsl / str / ldr /
     * orr / str). Echoing the last written value keeps that
     * read-modify-write stable instead of feeding the second write
     * zeros. ("Stable" = the AHB write reads back what was written.)
     */
    { 0x81003008ULL, SC6530_AUX_BENIGN_ECHO, 0, 0 },
    /*
     * 0x8100300c = the 1 ms system counter (SC6530_SYS_TIMER + 0xc,
     * include/arch.h:30). Any poll waiting for the counter to advance
     * must see it move, so answer with a monotonic 1 ms value.
     */
    { 0x8100300cULL, SC6530_AUX_BENIGN_COUNTER, 0, 1 },
    /*
     * 0x81003004 = the sys-timer free-running counter SYS_CNT0 (SDK v0
     * regs: SYS_CNT0 = SYSTIMER_BASE + 0x4). The stock PBL's delay loops
     * busy-wait on it advancing (read SYS_CNT0 / compare - caught during
     * todo-12 QA: the PBL sat in the delay forever with the catch-all
     * returning 0). Same family as 0x8100300c: answer with a monotonic
     * value so "wait until the counter moves" terminates.
     */
    { 0x81003004ULL, SC6530_AUX_BENIGN_COUNTER, 0, 2 },
    /*
     * 0x20a00010 = SFC_STATUS (SPI flash controller, SDK v7 regs:
     * SFC_STATUS = SFC_REG_BASE + 0x10). The SFC driver waits for
     * `(status & 1) != 0` (the ready/done bit) in EVERY operation
     * (FUN_0011a188/0011a27a/0011a4cc: do { uVar4 = FUN_0011954c(); }
     * while ((uVar4 & 1) == 0)). Our NOR is direct XIP RAM, so the SFC
     * is permanently ready - answer bit0=1 (| idle bit1 = 0x3). Earlier
     * value 2 (bit0 clear) spun the poll forever (526k reads/55s).
     */
    { 0x20a00010ULL, SC6530_AUX_BENIGN_FIXED, 0x00000003, 0 },
    /*
     * 0x82000020 = the ADI mailbox status register, bit 8 = ADI ready.
     * os.bin's _start (arch/start.s, fpdoom's proven boot path) polls it
     * with an UNBOUNDED loop before the first PLL write (wait for bit 8,
     * tst #0x100 / beq - caught during todo-12 QA: the guest never left
     * _start+0x44 with the catch-all returning 0). Answer "ready" so the
     * poll terminates. SUPERSEDED by todo 15's sc6530_adi device, which
     * models the real mailbox state machine at 0x82000000.
     */
    { 0x82000020ULL, SC6530_AUX_BENIGN_FIXED, 0x00000100, 0 },
};

static const Sc6530AuxBenignReg *sc6530_aux_benign_find(uint64_t addr)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sc6530_aux_benign); i++) {
        if (sc6530_aux_benign[i].addr == addr) {
            return &sc6530_aux_benign[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530AuxState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion ahb_iomem;       /* 0x20500000 */
    MemoryRegion apb_iomem;       /* 0x8b000000 */
    MemoryRegion smc_iomem;       /* 0x20000000 */
    MemoryRegion gpio_iomem;      /* 0x8a000000 */
    MemoryRegion pinmux_iomem;    /* 0x8c000000 */
    MemoryRegion busmon_iomem;    /* 0x20400000 (silent) */
    MemoryRegion bootready_iomem; /* 0x0425de8c over the PSRAM alias */
    MemoryRegion timerobj_iomem;  /* 0x0422d2cc over the PSRAM alias */
    MemoryRegion timerfn_iomem;   /* 0x0422e3e0 over the PSRAM alias */
    MemoryRegion scirpos_iomem;   /* 0x0422d4b4 over the PSRAM alias */
    MemoryRegion dlofitbl_iomem;  /* 0x0422e330 over the PSRAM alias */
    MemoryRegion txkern_iomem;    /* 0x0422c654 ThreadX kernel magic */
    MemoryRegion txobj_iomem;     /* 0x0423c818 ThreadX magic-id object */
    MemoryRegion lcdtbl_iomem;    /* 0x0422c8ec LCD driver table [0] */
    MemoryRegion lcddrv_iomem;    /* 0x0422c8fc LCD driver struct +8 */
    MemoryRegion clkobj_iomem;    /* 0x0422e554 s_dev_usb[0] thiz ptr */
    MemoryRegion clkobjflag_iomem; /* 0x0422e57c thiz+40 flag byte */
    MemoryRegion scipooltbl_iomem; /* 0x0422d478 SCI pool table */
    MemoryRegion scipool_iomem;    /* 0x04280000 SCI PEAK pool */
    MemoryRegion scimem_iomem;     /* 0x04280040 SCI memory-space */
    MemoryRegion txstate1_iomem;   /* 0x0422c67c kernel-state ptr */
    MemoryRegion txstate2_iomem;   /* 0x0422c680 scheduler-ready */
    MemoryRegion txguard_iomem;    /* 0x04259620 dlmalloc guard global */
    MemoryRegion txpool_iomem;     /* 0x04259638 ThreadX byte pool */
    MemoryRegion txnode_iomem;     /* 0x04259700 byte-pool free node */
    MemoryRegion txsent_iomem;     /* 0x0425a708 byte-pool sentinel */
    MemoryRegion lcdspec_iomem;    /* 0x0425e0e8 LCD driver spec struct */
    MemoryRegion catchall_iomem;  /* 0x10000000..0xffffffff, priority 0 */

    uint32_t ahb_regs[SC6530_AUX_AHB_SIZE / 4];
    uint32_t apb_regs[SC6530_AUX_APB_SIZE / 4];
    uint32_t smc_regs[SC6530_AUX_SMC_SIZE / 4];
    uint32_t gpio_regs[SC6530_AUX_GPIO_SIZE / 4];
    uint32_t pinmux_regs[SC6530_AUX_PINMUX_SIZE / 4];
    uint32_t busmon_regs[SC6530_AUX_BUSMON_SIZE / 4];

    /* Last-written values for SC6530_AUX_BENIGN_ECHO table entries. */
    uint32_t benign_echo[SC6530_AUX_BENIGN_ECHO_SLOTS];

    /* Last-written SCI IRQ position, capped at SC6530_AUX_SCIRPOS_MAX. */
    uint32_t scirpos;

    /* SCI_BLKMEM pool struct state (store+echo: the free-size/free-ptr
     * fields shrink as the allocator carves). */
    uint32_t scipool_regs[SC6530_AUX_SCIPOOL_SIZE / 4];
    uint32_t scimem_regs[SC6530_AUX_SCIMEM_SIZE / 4];

    /* ThreadX byte-pool state (0x04259638 + the free-node/sentinel pair;
     * validated store+echo - the allocator's carve updates land, the
     * ~40 s reload junk is rejected, see the TXBYTEPOOL defines). */
    uint32_t txpool_regs[SC6530_AUX_TXBYTEPOOL_SIZE / 4];
    uint32_t txnode_regs[SC6530_AUX_TXBYTENODE_SIZE / 4];
    uint32_t txsent_regs[SC6530_AUX_TXBYTESENT_SIZE / 4];

    /* LCD spec state store+echo bank (0x0425e0e8) */
    uint32_t lcdspec_regs[SC6530_AUX_LCDSPEC_SIZE / 4];

    /* LCD driver table store+echo bank (0x0422c8ec) */
    uint32_t lcdtbl_regs[SC6530_AUX_LCDTBL_SIZE / 4];
};

/* ---------------------------------------------------------------------- */
/* Shared store+echo helpers (size-aware byte/word access on the arrays)  */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_regs_read(const uint32_t *regs, hwaddr offset,
                                 unsigned size)
{
    uint32_t word = regs[offset / 4];

    return extract32(word, (offset % 4) * 8, size * 8);
}

static void sc6530_regs_write(uint32_t *regs, hwaddr offset,
                              uint64_t value, unsigned size)
{
    uint32_t word = regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    regs[offset / 4] = (word & ~(mask << shift)) |
                       ((uint32_t)value & mask) << shift;
}

static uint32_t sc6530_aux_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* AHB bank: store+echo; MEM_REMAP (0xe0) is a logged no-op (the PSRAM    */
/* alias in b310e.c is always on); 0x3fc reads return the chip id.        */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_ahb_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530AuxState *s = opaque;

    if (offset == SC6530_AUX_CHIP_ID_OFF) {
        return SC6530_CHIP_ID;
    }
    return sc6530_regs_read(s->ahb_regs, offset, size);
}

static void sc6530_aux_ahb_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->ahb_regs, offset, value, size);
    qemu_log("sc6530_aux: AHB write addr=0x%08" PRIx64 " val=0x%08" PRIx64
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_AHB_BASE + offset, value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_ahb_ops = {
    .read  = sc6530_aux_ahb_read,
    .write = sc6530_aux_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* APB bank: store+echo + trace events (the audio-relevant bank - the    */
/* PA power ladder 0x8b000060/64/a0/a4 and the AUD-ownership register    */
/* 0x8b0001c4 bits 9-10 = 0x600 must land in the trace for the Wave-6    */
/* diff). addr in the trace = full guest address, val = read/written     */
/* value, pc = guest PC.                                                 */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_apb_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = sc6530_regs_read(s->apb_regs, offset, size);

    trace_sc6530_aux_read(SC6530_AUX_APB_BASE + offset, val,
                          sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_apb_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->apb_regs, offset, value, size);
    trace_sc6530_aux_write(SC6530_AUX_APB_BASE + offset, value,
                           sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_apb_ops = {
    .read  = sc6530_aux_apb_read,
    .write = sc6530_aux_apb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SMC / GPIO / pinmux banks: plain store+echo with a log line.          */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_smc_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530AuxState *s = opaque;

    return sc6530_regs_read(s->smc_regs, offset, size);
}

static void sc6530_aux_smc_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->smc_regs, offset, value, size);
    qemu_log("sc6530_aux: SMC write addr=0x%08" PRIx64 " val=0x%08" PRIx64
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SMC_BASE + offset, value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_smc_ops = {
    .read  = sc6530_aux_smc_read,
    .write = sc6530_aux_smc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_gpio_read(void *opaque, hwaddr offset,
                                     unsigned size)
{
    Sc6530AuxState *s = opaque;

    return sc6530_regs_read(s->gpio_regs, offset, size);
}

static void sc6530_aux_gpio_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->gpio_regs, offset, value, size);
    qemu_log("sc6530_aux: GPIO write addr=0x%08" PRIx64 " val=0x%08" PRIx64
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_GPIO_BASE + offset, value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_gpio_ops = {
    .read  = sc6530_aux_gpio_read,
    .write = sc6530_aux_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_pinmux_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;

    return sc6530_regs_read(s->pinmux_regs, offset, size);
}

static void sc6530_aux_pinmux_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->pinmux_regs, offset, value, size);
    qemu_log("sc6530_aux: pinmux write addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_PINMUX_BASE + offset, value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_pinmux_ops = {
    .read  = sc6530_aux_pinmux_read,
    .write = sc6530_aux_pinmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* BUS_MON bank (0x20400000): silent store+echo. The stock OS's boot loop */
/* clears CHN_INT bit 0 (channel disable) on channels 0-2 every pass - a  */
/* per-access log line would flood the trace. Reads return 0 (no bus      */
/* match = no interrupt, the bus monitor's idle state).                   */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_busmon_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;

    return sc6530_regs_read(s->busmon_regs, offset, size);
}

static void sc6530_aux_busmon_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;

    sc6530_regs_write(s->busmon_regs, offset, value, size);
}

static const MemoryRegionOps sc6530_aux_busmon_ops = {
    .read  = sc6530_aux_busmon_read,
    .write = sc6530_aux_busmon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* PSRAM overlay regions: boot-ready flag (0x0425de8c, reads 1) and the    */
/* timer-ops pointer (0x0422d2cc, reads 0x0422e3d0). Both absorb guest     */
/* writes (the boot's BSS memset zeroes them) and always answer the state  */
/* the boot waits for - see the defines above for the evidence.            */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_bootready_read(void *opaque, hwaddr offset,
                                          unsigned size)
{
    /*
     * Boot ready / mode byte @ 0x0425de8c: return 1 (normal boot mode).
     */
    uint32_t val = 1;

    qemu_log("sc6530_aux: bootready r addr=0x%08" PRIx64 " val=0x%u"
             " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_BOOTREADY_BASE + offset), val,
             sc6530_aux_guest_pc());
    (void)opaque;
    return val;
}

static void sc6530_aux_bootready_write(void *opaque, hwaddr offset,
                                       uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: bootready w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered ready)\n",
             SC6530_AUX_BOOTREADY_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_bootready_ops = {
    .read  = sc6530_aux_bootready_read,
    .write = sc6530_aux_bootready_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_timerobj_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    qemu_log("sc6530_aux: timerobj r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TIMEROBJ_BASE + offset,
             (uint64_t)SC6530_AUX_TIMEROBJ_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TIMEROBJ_VAL;
}

static void sc6530_aux_timerobj_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: timerobj w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered ops ptr)\n",
             SC6530_AUX_TIMEROBJ_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_timerobj_ops = {
    .read  = sc6530_aux_timerobj_read,
    .write = sc6530_aux_timerobj_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_timerfn_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    qemu_log("sc6530_aux: timerfn r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TIMERFN_BASE + offset,
             (uint64_t)SC6530_AUX_TIMERFN_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TIMERFN_VAL;
}

static void sc6530_aux_timerfn_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: timerfn w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered fn ptr)\n",
             SC6530_AUX_TIMERFN_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_timerfn_ops = {
    .read  = sc6530_aux_timerfn_read,
    .write = sc6530_aux_timerfn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_clkobj_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    qemu_log("sc6530_aux: clkobj r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_CLKOBJ_BASE + offset,
             (uint64_t)SC6530_AUX_CLKOBJ_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_CLKOBJ_VAL;
}

static void sc6530_aux_clkobj_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: clkobj w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered thiz ptr)\n",
             SC6530_AUX_CLKOBJ_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_clkobj_ops = {
    .read  = sc6530_aux_clkobj_read,
    .write = sc6530_aux_clkobj_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_clkobjflag_read(void *opaque, hwaddr offset,
                                           unsigned size)
{
    qemu_log("sc6530_aux: clkobjflag r addr=0x%08" PRIx64 " val=0x%u"
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_CLKOBJFLAG_BASE + offset,
             SC6530_AUX_CLKOBJFLAG_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_CLKOBJFLAG_VAL;
}

static void sc6530_aux_clkobjflag_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: clkobjflag w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered flag)\n",
             SC6530_AUX_CLKOBJFLAG_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_clkobjflag_ops = {
    .read  = sc6530_aux_clkobjflag_read,
    .write = sc6530_aux_clkobjflag_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_scipooltbl_read(void *opaque, hwaddr offset,
                                           unsigned size)
{
    /* +8 (0x0422D480) = the 0x44444444/0xE4444444 pool ptr, +0xc
     * (0x0422D484) = the 0x33333333 pool ptr; both -> the pool struct. */
    uint64_t val = (offset == 8 || offset == 0xc) ?
                   SC6530_AUX_SCIPOOLTBL_POOL : 0;

    qemu_log("sc6530_aux: scipooltbl r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SCIPOOLTBL_BASE + offset, val,
             sc6530_aux_guest_pc());
    (void)opaque;
    return val;
}

static void sc6530_aux_scipooltbl_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: scipooltbl w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered pool ptr)\n",
             SC6530_AUX_SCIPOOLTBL_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_scipooltbl_ops = {
    .read  = sc6530_aux_scipooltbl_read,
    .write = sc6530_aux_scipooltbl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_scipool_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->scipool_regs[offset / 4],
                             (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: scipool r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SCIPOOL_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_scipool_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    /*
     * The pool struct fields are READ-ONLY for the allocator (magic [0],
     * mspace ptr [8], walk terminator [0x38] - the carve updates land in
     * the memory-space at 0x04280040, the SCIMEM region). The ~40 s
     * soft-restart reload (NOR 0x12E184 -> PSRAM 0x0423BBA8, 676 KB)
     * memcpy's compressed junk over this window ([0x04280000] ->
     * 0xF2259960), which would break the "PEAK" magic check in
     * FUN_0010d532. Reject ALL writes so the struct is reload-proof.
     */
    qemu_log("sc6530_aux: scipool w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: sticky pool struct)\n",
             SC6530_AUX_SCIPOOL_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
    (void)size;
}

static const MemoryRegionOps sc6530_aux_scipool_ops = {
    .read  = sc6530_aux_scipool_read,
    .write = sc6530_aux_scipool_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_scimem_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->scimem_regs[offset / 4],
                             (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: scimem r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SCIMEM_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_scimem_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    /*
     * The memory-space doubles as the dlmalloc mstate (the PEAK pool's
     * +8 mspace ptr = 0x04280040; mspace_malloc FUN_0010c956 reads
     * [2]=dvsize/[5]=dv and the guard check [9] vs [0x04259620]).
     * Per-word write rules so the allocator's carve updates land but the
     * ~40 s reload junk (0x04280048 -> 0x0BC1BA66 etc.) cannot corrupt:
     *   [0x00] smallmap/bitmap - sticky 0 (the dv carve path never
     *          touches it; junk bits would send the smallbin fast path
     *          into the junked bins in plain RAM -> bin asserts)
     *   [0x04] treemap - sticky 0 (same; the treebin path is skipped)
     *   [0x08] dvsize/free-size - accept only when it shrinks (the carve
     *          subtracts the block; the BSS memset 0s and the junk are
     *          rejected)
     *   [0x0c] topsize - sticky 0 (the top-carve path stays dead ->
     *          malloc fails cleanly instead of asserting)
     *   [0x10] least_addr - sticky 0 (ok_address checks pass trivially)
     *   [0x14] dv/free-ptr - accept only in the pool arena range
     *   [0x24] owner/guard - sticky 0 (must stay == the TXGUARD overlay
     *          or the threadx_malloc.c:1274 assert fires)
     *   other words - plain store+echo.
     */
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->scimem_regs[offset / 4];
    uint32_t newv = (word & ~(mask << shift)) |
                    ((uint32_t)value & mask) << shift;
    bool ok = true;

    switch (offset & ~3u) {
    case 0x00:  /* smallmap */
    case 0x04:  /* treemap */
    case 0x0c:  /* topsize */
    case 0x10:  /* least_addr */
    case 0x24:  /* owner/guard */
        ok = false;
        break;
    case 0x08:  /* dvsize: the carve only shrinks it */
        ok = (value != 0) && (value < word);
        break;
    case 0x14:  /* dv: the carve advances it within the arena */
        ok = (value >= 0x04280000ULL) && (value <= 0x0429ffffULL);
        break;
    default:
        ok = (value != 0);
        break;
    }
    if (ok) {
        s->scimem_regs[offset / 4] = newv;
    }
    qemu_log("sc6530_aux: scimem w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " %s\n",
             SC6530_AUX_SCIMEM_BASE + offset, value,
             sc6530_aux_guest_pc(), ok ? "" : "(ignored)");
}

static const MemoryRegionOps sc6530_aux_scimem_ops = {
    .read  = sc6530_aux_scimem_read,
    .write = sc6530_aux_scimem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_txstate1_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    qemu_log("sc6530_aux: txstate1 r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXSTATE1_BASE + offset,
             (uint64_t)SC6530_AUX_TXSTATE1_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TXSTATE1_VAL;
}

static void sc6530_aux_txstate1_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: txstate1 w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered state)\n",
             SC6530_AUX_TXSTATE1_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_txstate1_ops = {
    .read  = sc6530_aux_txstate1_read,
    .write = sc6530_aux_txstate1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_txstate2_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    qemu_log("sc6530_aux: txstate2 r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXSTATE2_BASE + offset,
             (uint64_t)SC6530_AUX_TXSTATE2_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TXSTATE2_VAL;
}

static void sc6530_aux_txstate2_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: txstate2 w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered state)\n",
             SC6530_AUX_TXSTATE2_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_txstate2_ops = {
    .read  = sc6530_aux_txstate2_read,
    .write = sc6530_aux_txstate2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* dlmalloc guard global (0x04259620): fixed 0, absorbs all writes - the  */
/* ~40 s reload junk (0x02184BEA) must never change it, or the mspace     */
/* guard check in FUN_0010c956 asserts at threadx_malloc.c:1274.          */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_txguard_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    qemu_log("sc6530_aux: txguard r addr=0x%08" PRIx64 " val=0x%u"
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXGUARD_BASE + offset, SC6530_AUX_TXGUARD_VAL,
             sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TXGUARD_VAL;
}

static void sc6530_aux_txguard_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: txguard w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: guard pinned 0)\n",
             SC6530_AUX_TXGUARD_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
    (void)size;
}

static const MemoryRegionOps sc6530_aux_txguard_ops = {
    .read  = sc6530_aux_txguard_read,
    .write = sc6530_aux_txguard_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* ThreadX byte pool (0x04259638): "BYTE" magic + free list so             */
/* tx_byte_allocate (FUN_00114be8 -> FUN_001162da -> FUN_001160a0)        */
/* succeeds. Validated store+echo: the allocator's carve updates land      */
/* (available shrinks, fragments 1/2, node ptrs in the arena, markers),    */
/* the ~40 s reload junk and the BSS memset 0s are rejected. See the       */
/* TXBYTEPOOL defines for the field map.                                  */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_txpool_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->txpool_regs[offset / 4],
                             (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: txpool r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXBYTEPOOL_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static bool sc6530_aux_txpool_accept(hwaddr offset, uint32_t cur,
                                     uint64_t value)
{
    switch (offset & ~3u) {
    case 0x00:  /* magic - sticky */
        return false;
    case 0x08:  /* available - the carve only shrinks it */
        return (value != 0) && (value < cur);
    case 0x0c:  /* fragments - the carve toggles 1 <-> 2 */
        return (value == 1) || (value == 2);
    case 0x18:  /* search head - a node ptr in the arena */
        return (value >= 0x04259600ULL) && (value <= 0x04270000ULL);
    case 0x20:  /* owner - the current-thread token (small/ptr) */
        return (value != 0) && (value < 0x04230000ULL);
    case 0x34:  /* limit - sticky 0 */
        return false;
    case 0x38:  /* min-available - only shrinks */
        return (value != 0) && (value < cur);
    case 0x44:  /* threshold - sticky 0 */
        return false;
    default:    /* name/first/last/pad - absorb non-zero */
        return value != 0;
    }
}

static void sc6530_aux_txpool_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->txpool_regs[offset / 4];
    bool ok = sc6530_aux_txpool_accept(offset, word, value);

    if (ok) {
        s->txpool_regs[offset / 4] = (word & ~(mask << shift)) |
                                     ((uint32_t)value & mask) << shift;
    }
    qemu_log("sc6530_aux: txpool w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " %s\n",
             SC6530_AUX_TXBYTEPOOL_BASE + offset, value,
             sc6530_aux_guest_pc(), ok ? "" : "(ignored)");
}

static const MemoryRegionOps sc6530_aux_txpool_ops = {
    .read  = sc6530_aux_txpool_read,
    .write = sc6530_aux_txpool_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_txnode_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->txnode_regs[offset / 4],
                             (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: txnode r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXBYTENODE_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_txnode_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->txnode_regs[offset / 4];
    bool ok;

    if ((offset & ~3u) == 0) {
        /* next: the carve links the anchor to the carved node (arena) */
        ok = (value >= 0x04259600ULL) && (value <= 0x04270000ULL);
    } else {
        /* owner slot: free marker / pool ptr / sentinel marker only */
        ok = (value == SC6530_AUX_TXBYTENODE_MARK) ||
             (value == SC6530_AUX_TXBYTEPOOL_BASE) ||
             (value == SC6530_AUX_TXBYTESENT_MARK);
    }
    if (ok) {
        s->txnode_regs[offset / 4] = (word & ~(mask << shift)) |
                                     ((uint32_t)value & mask) << shift;
    }
    qemu_log("sc6530_aux: txnode w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " %s\n",
             SC6530_AUX_TXBYTENODE_BASE + offset, value,
             sc6530_aux_guest_pc(), ok ? "" : "(ignored)");
}

static const MemoryRegionOps sc6530_aux_txnode_ops = {
    .read  = sc6530_aux_txnode_read,
    .write = sc6530_aux_txnode_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_txsent_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->txsent_regs[offset / 4],
                             (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: txsent r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXBYTESENT_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_txsent_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->txsent_regs[offset / 4];
    bool ok = ((offset & ~3u) == 0) ? (value != 0) : false;

    if (ok) {
        s->txsent_regs[offset / 4] = (word & ~(mask << shift)) |
                                     ((uint32_t)value & mask) << shift;
    }
    qemu_log("sc6530_aux: txsent w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " %s\n",
             SC6530_AUX_TXBYTESENT_BASE + offset, value,
             sc6530_aux_guest_pc(), ok ? "" : "(ignored)");
}

static const MemoryRegionOps sc6530_aux_txsent_ops = {
    .read  = sc6530_aux_txsent_read,
    .write = sc6530_aux_txsent_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SCI IRQ nesting position overlay (0x0422d4b4): store+echo with a cap   */
/* at SC6530_AUX_SCIRPOS_MAX so the nest-check (>= SCI_MAX_IRQ_NESTING)   */
/* never asserts. See the defines above for the evidence.                 */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_scirpos_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t val = extract32(s->scirpos, offset * 8, size * 8);

    qemu_log("sc6530_aux: scirpos r addr=0x%08" PRIx64 " val=0x%u"
             " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SCIRPOS_BASE + offset, val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_scirpos_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    uint32_t merged = (s->scirpos & ~(mask << (offset * 8))) |
                      ((uint32_t)value & mask) << (offset * 8);
    uint32_t capped = MIN(merged, SC6530_AUX_SCIRPOS_MAX);

    s->scirpos = capped;
    qemu_log("sc6530_aux: scirpos w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " capped=0x%x pc=0x%08" PRIx32 "\n",
             SC6530_AUX_SCIRPOS_BASE + offset, value, capped,
             sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_scirpos_ops = {
    .read  = sc6530_aux_scirpos_read,
    .write = sc6530_aux_scirpos_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* dl_ofi driver-table pointer overlay (0x0422e330): answers the table    */
/* base 0x04230a74 the registration would store (NOR 0x357a4 -> 0x69228   */
/* -> literal 0x69230), absorbing the guest's writes. See the defines.    */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_dlofitbl_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    qemu_log("sc6530_aux: dlofitbl r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_DLOFITBL_BASE + offset,
             (uint64_t)SC6530_AUX_DLOFITBL_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_DLOFITBL_VAL;
}

static void sc6530_aux_dlofitbl_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: dlofitbl w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered table base)\n",
             SC6530_AUX_DLOFITBL_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_dlofitbl_ops = {
    .read  = sc6530_aux_dlofitbl_read,
    .write = sc6530_aux_dlofitbl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* ThreadX kernel/object magic overlays (0x0422c654, 0x0423c818): fixed    */
/* values the RTOS APIs check (0x20021201). Same absorb-writes pattern.    */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_txkern_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    qemu_log("sc6530_aux: txkern r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXKERN_MAGIC_BASE + offset,
             (uint64_t)SC6530_AUX_TXKERN_MAGIC_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TXKERN_MAGIC_VAL;
}

static void sc6530_aux_txkern_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: txkern w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered kernel magic)\n",
             SC6530_AUX_TXKERN_MAGIC_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_txkern_ops = {
    .read  = sc6530_aux_txkern_read,
    .write = sc6530_aux_txkern_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_txobj_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    qemu_log("sc6530_aux: txobj r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             SC6530_AUX_TXOBJ_MAGIC_BASE + offset,
             (uint64_t)SC6530_AUX_TXOBJ_MAGIC_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_TXOBJ_MAGIC_VAL;
}

static void sc6530_aux_txobj_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: txobj w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered object magic)\n",
             SC6530_AUX_TXOBJ_MAGIC_BASE + offset, value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_txobj_ops = {
    .read  = sc6530_aux_txobj_read,
    .write = sc6530_aux_txobj_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* LCD driver-table overlays (0x0422c8ec, 0x0422c8fc): fixed values that  */
/* make FUN_00014aa4's panel-resolution check pass (see the defines).     */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_lcdtbl_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t val = extract32(s->lcdtbl_regs[offset / 4], (offset % 4) * 8, size * 8);

    qemu_log("sc6530_aux: lcdtbl r addr=0x%08" PRIx64 " val=0x%" PRIx64
             " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_LCDTBL_BASE + offset),
             (uint64_t)val, sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_lcdtbl_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->lcdtbl_regs[offset / 4];

    s->lcdtbl_regs[offset / 4] = (word & ~(mask << shift)) |
                                  (((uint32_t)value & mask) << shift);

    qemu_log("sc6530_aux: lcdtbl w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_LCDTBL_BASE + offset), (uint64_t)value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_lcdtbl_ops = {
    .read  = sc6530_aux_lcdtbl_read,
    .write = sc6530_aux_lcdtbl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t sc6530_aux_lcddrv_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    qemu_log("sc6530_aux: lcddrv r addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_LCDDRV_PANEL_BASE + offset),
             (uint64_t)SC6530_AUX_LCDDRV_PANEL_VAL, sc6530_aux_guest_pc());
    (void)opaque;
    return SC6530_AUX_LCDDRV_PANEL_VAL;
}

static void sc6530_aux_lcddrv_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    qemu_log("sc6530_aux: lcddrv w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 " (ignored: answered panel ptr)\n",
             (uint64_t)(SC6530_AUX_LCDDRV_PANEL_BASE + offset), (uint64_t)value,
             sc6530_aux_guest_pc());
    (void)opaque;
}

static const MemoryRegionOps sc6530_aux_lcddrv_ops = {
    .read  = sc6530_aux_lcddrv_read,
    .write = sc6530_aux_lcddrv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* LCD spec overlays (0x0425e0e8): ST7735S panel parameters & driver status */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_lcdspec_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t val = 0;

    if (offset == 0x2c) {          /* 0x0425e114: driver count */
        val = 1;
    } else if (offset == 0x110) {  /* 0x0425e1f8: max_width */
        val = 128;
    } else if (offset == 0x112) {  /* 0x0425e1fa: max_height */
        val = 160;
    } else if (offset == 0x114) {  /* 0x0425e1fc: x_start */
        val = 0;
    } else if (offset == 0x116) {  /* 0x0425e1fe: y_start */
        val = 0;
    } else if (offset == 0x118) {  /* 0x0425e200: width */
        val = 128;
    } else if (offset == 0x11a) {  /* 0x0425e202: height */
        val = 160;
    } else if (offset == 0x11c) {  /* 0x0425e204: interface mode */
        val = 3;
    } else if (offset == 0x128) {  /* 0x0425e210: active status */
        val = 1;
    } else {
        val = extract32(s->lcdspec_regs[offset / 4], (offset % 4) * 8, size * 8);
    }
    qemu_log("sc6530_aux: lcdspec r addr=0x%08" PRIx64 " val=0x%" PRIx64
             " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_LCDSPEC_BASE + offset), (uint64_t)val,
             sc6530_aux_guest_pc());
    return val;
}

static void sc6530_aux_lcdspec_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;
    uint32_t word = s->lcdspec_regs[offset / 4];

    s->lcdspec_regs[offset / 4] = (word & ~(mask << shift)) |
                                  (((uint32_t)value & mask) << shift);
    qemu_log("sc6530_aux: lcdspec w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             (uint64_t)(SC6530_AUX_LCDSPEC_BASE + offset), (uint64_t)value,
             sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_lcdspec_ops = {
    .read  = sc6530_aux_lcdspec_read,
    .write = sc6530_aux_lcdspec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* Catch-all: 0x10000000..0xffffffff at priority 0 (b310e.c's regions    */
/* are at priority 1 and always win). Every unclaimed access is logged   */
/* (UNMODELED) and reads return 0 - except the benign-ready table        */
/* entries above, which answer with benign values so guest poll-loops    */
/* terminate.                                                            */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_aux_catchall_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t addr = SC6530_AUX_CATCHALL_BASE + offset;
    const Sc6530AuxBenignReg *br = sc6530_aux_benign_find(addr);
    uint64_t val = 0;

    if (br) {
        switch (br->mode) {
        case SC6530_AUX_BENIGN_FIXED:
            val = br->value;
            break;
        case SC6530_AUX_BENIGN_ECHO:
            val = s->benign_echo[br->echo_slot];
            break;
        case SC6530_AUX_BENIGN_COUNTER:
            val = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            break;
        }
        qemu_log("sc6530_aux: UNMODELED (benign) r addr=0x%08" PRIx64
                 " val=0x%08" PRIx64 " pc=0x%08" PRIx32 "\n",
                 addr, val, sc6530_aux_guest_pc());
        return val;
    }

    qemu_log("sc6530_aux: UNMODELED r addr=0x%08" PRIx64 " val=0x0"
             " pc=0x%08" PRIx32 "\n", addr, sc6530_aux_guest_pc());
    return 0;
}

static void sc6530_aux_catchall_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    Sc6530AuxState *s = opaque;
    uint64_t addr = SC6530_AUX_CATCHALL_BASE + offset;
    const Sc6530AuxBenignReg *br = sc6530_aux_benign_find(addr);

    /* ECHO entries remember the value so the read side can answer it. */
    if (br && br->mode == SC6530_AUX_BENIGN_ECHO) {
        s->benign_echo[br->echo_slot] = value;
    }
    qemu_log("sc6530_aux: UNMODELED w addr=0x%08" PRIx64 " val=0x%08"
             PRIx64 " pc=0x%08" PRIx32 "\n",
             addr, value, sc6530_aux_guest_pc());
}

static const MemoryRegionOps sc6530_aux_catchall_ops = {
    .read  = sc6530_aux_catchall_read,
    .write = sc6530_aux_catchall_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_aux_reset(DeviceState *dev)
{
    Sc6530AuxState *s = SC6530_AUX(dev);

    memset(s->ahb_regs, 0, sizeof(s->ahb_regs));
    memset(s->apb_regs, 0, sizeof(s->apb_regs));
    memset(s->smc_regs, 0, sizeof(s->smc_regs));
    memset(s->gpio_regs, 0, sizeof(s->gpio_regs));
    memset(s->pinmux_regs, 0, sizeof(s->pinmux_regs));
    memset(s->busmon_regs, 0, sizeof(s->busmon_regs));
    memset(s->benign_echo, 0, sizeof(s->benign_echo));
    s->scirpos = 0;
    memset(s->scipool_regs, 0, sizeof(s->scipool_regs));
    s->scipool_regs[0x00 / 4] = SC6530_AUX_SCIPOOL_MAGIC;
    s->scipool_regs[0x08 / 4] = SC6530_AUX_SCIPOOL_MSPACE;
    s->scipool_regs[0x38 / 4] = 0x34u;   /* pool-list walk terminator */
    memset(s->scimem_regs, 0, sizeof(s->scimem_regs));
    s->scimem_regs[0x08 / 4] = SC6530_AUX_SCIMEM_FREESZ;
    s->scimem_regs[0x14 / 4] = SC6530_AUX_SCIMEM_FREEPTR;
    s->scimem_regs[0x24 / 4] = SC6530_AUX_SCIMEM_OWNER;
    memset(s->txpool_regs, 0, sizeof(s->txpool_regs));
    s->txpool_regs[0x00 / 4] = SC6530_AUX_TXBYTEPOOL_MAGIC;
    s->txpool_regs[0x08 / 4] = SC6530_AUX_TXBYTEPOOL_FREE;
    s->txpool_regs[0x0c / 4] = 1u;                 /* fragments */
    s->txpool_regs[0x14 / 4] = SC6530_AUX_TXBYTEPOOL_NODE; /* +0x14 head */
    s->txpool_regs[0x18 / 4] = SC6530_AUX_TXBYTEPOOL_NODE; /* +0x18 head */
    s->txpool_regs[0x38 / 4] = SC6530_AUX_TXBYTEPOOL_MIN;
    memset(s->txnode_regs, 0, sizeof(s->txnode_regs));
    s->txnode_regs[0x00 / 4] = SC6530_AUX_TXBYTEPOOL_SENT;
    s->txnode_regs[0x04 / 4] = SC6530_AUX_TXBYTENODE_MARK;
    memset(s->txsent_regs, 0, sizeof(s->txsent_regs));
    s->txsent_regs[0x04 / 4] = SC6530_AUX_TXBYTESENT_MARK;
    memset(s->lcdspec_regs, 0, sizeof(s->lcdspec_regs));

    memset(s->lcdtbl_regs, 0, sizeof(s->lcdtbl_regs));
    s->lcdtbl_regs[0x00 / 4] = SC6530_AUX_LCDTBL_VAL;
    s->lcdtbl_regs[0x08 / 4] = SC6530_AUX_LCDTBL_VAL;
    s->lcdtbl_regs[0x10 / 4] = SC6530_AUX_LCDDRV_PANEL_VAL;
    s->lcdtbl_regs[0x40 / 4] = 128 | (160 << 16);
    s->lcdtbl_regs[0x58 / 4] = 128 | (160 << 16);
    s->lcdtbl_regs[0x60 / 4] = 128 | (160 << 16);
}

static void sc6530_aux_init(Object *obj)
{
    Sc6530AuxState *s = SC6530_AUX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->ahb_iomem, obj, &sc6530_aux_ahb_ops, s,
                          "sc6530-aux-ahb", SC6530_AUX_AHB_SIZE);
    sysbus_init_mmio(sbd, &s->ahb_iomem);

    memory_region_init_io(&s->apb_iomem, obj, &sc6530_aux_apb_ops, s,
                          "sc6530-aux-apb", SC6530_AUX_APB_SIZE);
    sysbus_init_mmio(sbd, &s->apb_iomem);

    memory_region_init_io(&s->smc_iomem, obj, &sc6530_aux_smc_ops, s,
                          "sc6530-aux-smc", SC6530_AUX_SMC_SIZE);
    sysbus_init_mmio(sbd, &s->smc_iomem);

    memory_region_init_io(&s->gpio_iomem, obj, &sc6530_aux_gpio_ops, s,
                          "sc6530-aux-gpio", SC6530_AUX_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->gpio_iomem);

    memory_region_init_io(&s->pinmux_iomem, obj, &sc6530_aux_pinmux_ops, s,
                          "sc6530-aux-pinmux", SC6530_AUX_PINMUX_SIZE);
    sysbus_init_mmio(sbd, &s->pinmux_iomem);

    memory_region_init_io(&s->busmon_iomem, obj, &sc6530_aux_busmon_ops, s,
                          "sc6530-aux-busmon", SC6530_AUX_BUSMON_SIZE);
    sysbus_init_mmio(sbd, &s->busmon_iomem);

    memory_region_init_io(&s->bootready_iomem, obj,
                          &sc6530_aux_bootready_ops, s,
                          "sc6530-aux-bootready", SC6530_AUX_BOOTREADY_SIZE);
    sysbus_init_mmio(sbd, &s->bootready_iomem);

    memory_region_init_io(&s->timerobj_iomem, obj,
                          &sc6530_aux_timerobj_ops, s,
                          "sc6530-aux-timerobj", SC6530_AUX_TIMEROBJ_SIZE);
    sysbus_init_mmio(sbd, &s->timerobj_iomem);

    memory_region_init_io(&s->timerfn_iomem, obj,
                          &sc6530_aux_timerfn_ops, s,
                          "sc6530-aux-timerfn", SC6530_AUX_TIMERFN_SIZE);
    sysbus_init_mmio(sbd, &s->timerfn_iomem);

    memory_region_init_io(&s->scirpos_iomem, obj,
                          &sc6530_aux_scirpos_ops, s,
                          "sc6530-aux-scirpos", SC6530_AUX_SCIRPOS_SIZE);
    sysbus_init_mmio(sbd, &s->scirpos_iomem);

    memory_region_init_io(&s->dlofitbl_iomem, obj,
                          &sc6530_aux_dlofitbl_ops, s,
                          "sc6530-aux-dlofitbl", SC6530_AUX_DLOFITBL_SIZE);
    sysbus_init_mmio(sbd, &s->dlofitbl_iomem);

    memory_region_init_io(&s->txkern_iomem, obj, &sc6530_aux_txkern_ops, s,
                          "sc6530-aux-txkern", SC6530_AUX_TXKERN_MAGIC_SIZE);
    sysbus_init_mmio(sbd, &s->txkern_iomem);

    memory_region_init_io(&s->txobj_iomem, obj, &sc6530_aux_txobj_ops, s,
                          "sc6530-aux-txobj", SC6530_AUX_TXOBJ_MAGIC_SIZE);
    sysbus_init_mmio(sbd, &s->txobj_iomem);

    memory_region_init_io(&s->lcdtbl_iomem, obj, &sc6530_aux_lcdtbl_ops, s,
                          "sc6530-aux-lcdtbl", SC6530_AUX_LCDTBL_SIZE);
    sysbus_init_mmio(sbd, &s->lcdtbl_iomem);

    memory_region_init_io(&s->lcddrv_iomem, obj, &sc6530_aux_lcddrv_ops, s,
                          "sc6530-aux-lcddrv", SC6530_AUX_LCDDRV_PANEL_SIZE);
    sysbus_init_mmio(sbd, &s->lcddrv_iomem);

    memory_region_init_io(&s->clkobj_iomem, obj, &sc6530_aux_clkobj_ops, s,
                          "sc6530-aux-clkobj", SC6530_AUX_CLKOBJ_SIZE);
    sysbus_init_mmio(sbd, &s->clkobj_iomem);

    memory_region_init_io(&s->clkobjflag_iomem, obj,
                          &sc6530_aux_clkobjflag_ops, s,
                          "sc6530-aux-clkobjflag", SC6530_AUX_CLKOBJFLAG_SIZE);
    sysbus_init_mmio(sbd, &s->clkobjflag_iomem);

    memory_region_init_io(&s->scipooltbl_iomem, obj,
                          &sc6530_aux_scipooltbl_ops, s,
                          "sc6530-aux-scipooltbl", SC6530_AUX_SCIPOOLTBL_SIZE);
    sysbus_init_mmio(sbd, &s->scipooltbl_iomem);

    memory_region_init_io(&s->scipool_iomem, obj,
                          &sc6530_aux_scipool_ops, s,
                          "sc6530-aux-scipool", SC6530_AUX_SCIPOOL_SIZE);
    sysbus_init_mmio(sbd, &s->scipool_iomem);

    memory_region_init_io(&s->scimem_iomem, obj,
                          &sc6530_aux_scimem_ops, s,
                          "sc6530-aux-scimem", SC6530_AUX_SCIMEM_SIZE);
    sysbus_init_mmio(sbd, &s->scimem_iomem);

    memory_region_init_io(&s->txstate1_iomem, obj,
                          &sc6530_aux_txstate1_ops, s,
                          "sc6530-aux-txstate1", SC6530_AUX_TXSTATE1_SIZE);
    sysbus_init_mmio(sbd, &s->txstate1_iomem);

    memory_region_init_io(&s->txstate2_iomem, obj,
                          &sc6530_aux_txstate2_ops, s,
                          "sc6530-aux-txstate2", SC6530_AUX_TXSTATE2_SIZE);
    sysbus_init_mmio(sbd, &s->txstate2_iomem);

    memory_region_init_io(&s->txguard_iomem, obj,
                          &sc6530_aux_txguard_ops, s,
                          "sc6530-aux-txguard", SC6530_AUX_TXGUARD_SIZE);
    sysbus_init_mmio(sbd, &s->txguard_iomem);

    memory_region_init_io(&s->txpool_iomem, obj,
                          &sc6530_aux_txpool_ops, s,
                          "sc6530-aux-txpool", SC6530_AUX_TXBYTEPOOL_SIZE);
    sysbus_init_mmio(sbd, &s->txpool_iomem);

    memory_region_init_io(&s->txnode_iomem, obj,
                          &sc6530_aux_txnode_ops, s,
                          "sc6530-aux-txnode", SC6530_AUX_TXBYTENODE_SIZE);
    sysbus_init_mmio(sbd, &s->txnode_iomem);

    memory_region_init_io(&s->txsent_iomem, obj,
                          &sc6530_aux_txsent_ops, s,
                          "sc6530-aux-txsent", SC6530_AUX_TXBYTESENT_SIZE);
    sysbus_init_mmio(sbd, &s->txsent_iomem);

    memory_region_init_io(&s->lcdspec_iomem, obj,
                          &sc6530_aux_lcdspec_ops, s,
                          "sc6530-aux-lcdspec", SC6530_AUX_LCDSPEC_SIZE);
    sysbus_init_mmio(sbd, &s->lcdspec_iomem);

    memory_region_init_io(&s->catchall_iomem, obj,
                          &sc6530_aux_catchall_ops, s,
                          "sc6530-aux-catchall", SC6530_AUX_CATCHALL_SIZE);
    sysbus_init_mmio(sbd, &s->catchall_iomem);
}

static void sc6530_aux_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, sc6530_aux_reset);
}

static const TypeInfo sc6530_aux_info = {
    .name          = TYPE_SC6530_AUX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530AuxState),
    .instance_init = sc6530_aux_init,
    .class_init    = sc6530_aux_class_init,
};

static void sc6530_aux_register_types(void)
{
    type_register_static(&sc6530_aux_info);
}

type_init(sc6530_aux_register_types)
