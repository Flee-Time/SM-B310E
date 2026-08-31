/*
 * Spreadtrum SC6530C (Samsung SM-B310E) machine emulation.
 *
 * The SC6530C is an ARM926EJ-S (ARMv5TE, Thumb-1) feature-phone SoC with
 * ~4 MB PSRAM, 8 MB NOR XIP flash, 256 KiB IRAM and a DSP core that is NOT
 * emulated (faked at the interface level - see docs/audio-dsp-protocol.md
 * "DSP-fake-spec").
 *
 * Modeled on QEMU v11.1.0 hw/arm/musicpal.c (the surviving ARM926EJ-S +
 * custom-PIC template): CPU via cpu_create(), memory regions added to the
 * system memory, MachineClass via DEFINE_MACHINE_EXTENDED with
 * arm_machine_interfaces. No arm_load_kernel: the initial PC is set by a
 * reset handler (see b310e_reset), mirroring the raw-PC start style of
 * machines that bypass the kernel loader.
 *
 * Memory map (todo 11 of .omo/plans/b310e-qemu-machine.md):
 *
 *   0x00000000  8 MiB   NOR XIP (read-only; writes logged, not stored)
 *                       contents from -drive if=none,id=nor
 *   0x04000000  4 MiB   PSRAM alias (always-on; MEM_REMAP writes are logged
 *                       no-ops - see todo 12's sc6530_aux)
 *   0x10000000  128 KiB DSP shared RAM (DSP-fake spec section 1: weighted
 *                       base; SC6530-class SDK default). The DSP download
 *                       control struct the guest actually uses sits at
 *                       base + 0xFE0 (runtime [0x0422E598] = 0x10000FE0);
 *                       the 0x20-byte fake control windows map there.
 *   0x30000000  128 KiB DSP shared RAM alias (defensive; SC6531EFM-family
 *                       value - the provisional 0x30000000 NOR alias from
 *                       the original plan is DROPPED, see
 *                       docs/audio-dsp-protocol.md "DSP-fake-spec" section 1)
 *   0x34000000  4 MiB   PSRAM (the machine RAM, default_ram_id b310e.psram)
 *   0x40000000  256 KiB IRAM
 *
 * SUBREGION PRIORITY RULE: every region mapped here uses priority
 * B310E_REGION_PRIORITY (1). Todo 12's catch-all will map
 * 0x10000000..0xffffffff at priority 0 (memory_region_add_subregion, the
 * default), so it can never shadow any of the regions above - the catch-all
 * only sees addresses no real region claims.
 *
 * Device call sites for todos 12-19 are listed as TODO comments in
 * b310e_init(); this file deliberately creates no device yet (b310e.c maps
 * memory + CPU + boot only).
 *
 * Boot modes (-M b310e,boot-mode=warm|stock|ours, default warm).
 * NOTE: the property is named "boot-mode", not "boot" - MachineClass already
 * owns a generic typed "boot" property (BootConfiguration, hw/core/machine.c)
 * and object_class_property_add would abort on the duplicate (asserted on
 * v11.1.0 during -M machine enumeration). Semantics below are unchanged:
 *   warm  - PBL warm-boot fast path: write the 0xFE519C04 magic into PSRAM
 *           0x04000000 (the alias; vestigial - nothing on this path reads it)
 *           and start at 0x10000 (the stock main-OS entry, the path proven
 *           by tools/stockram).
 *   stock - full boot: start at 0x0 (vector table -> PBL 0x46e4 -> full
 *           init path), magic NOT written.
 *   ours  - load -drive if=none,id=os at 0x34000000 and start at
 *           0x34000010 (our os.bin entry; start.s runs in ARM state, so the
 *           PC is even/bit0=0).
 *   rockbox - load -drive if=none,id=os at 0x34000000 and start at
 *           0x34000000 (Rockbox's own vector table — the boot menu branches
 *           there in ARM state). ALSO sets CP15 SCTLR V=1 (high vectors):
 *           the real boot chain runs through our menu, whose start.s leaves
 *           V=1, and Rockbox's crt0 builds an identity MMU + a 0xffff0000
 *           high-vector map SPECIFICALLY because V=1 — without it the tick
 *           IRQ fetches from 0x00000000 (the NOR vector table / zeros) and
 *           Rockbox hangs in kernel_init.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/arm/machines-qom.h"
#include "monitor/qdev.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "qemu/error-report.h"
#include "target/arm/cpu.h"

/* ---------------------------------------------------------------------- */
/* Addresses and sizes (see the map comment above)                        */
/* ---------------------------------------------------------------------- */

#define B310E_NOR_BASE           0x00000000ULL
#define B310E_NOR_SIZE           (8 * MiB)

#define B310E_PSRAM_ALIAS_BASE   0x04000000ULL
#define B310E_PSRAM_BASE         0x34000000ULL
#define B310E_PSRAM_SIZE         (4 * MiB)

#define B310E_DSP_SHARE_BASE     0x10000000ULL
#define B310E_DSP_SHARE_ALIAS    0x30000000ULL
#define B310E_DSP_SHARE_SIZE     (128 * KiB)

#define B310E_IRAM_BASE          0x40000000ULL
#define B310E_IRAM_SIZE          (256 * KiB)

#define B310E_INTC_BASE          0x80000000ULL

/* sc6530_adi (todo 15) region bases - keep in sync with
 * hw/misc/sc6530_adi.c (region geometry there). The ADI mailbox is at
 * 0x82000000, the ANA analog bank at 0x82001000 (8 KiB: codec regs +
 * WDG 0x82001480 + EIC 0x82001900) and the VBC audio FIFO at
 * 0x82003000 (a SEPARATE region - a single 0x2000 ANA region would end
 * at 0x82002fff and miss the VBC base). */
#define B310E_ADI_MAILBOX_BASE   0x82000000ULL
#define B310E_ADI_ANA_BASE       0x82001000ULL
#define B310E_ADI_VBC_BASE       0x82003000ULL

/* sc6530_timer (todo 14): timer2 regs at 0x81000040 (SYS_TIMER2_LOAD etc.,
 * kernel/irq.c - the 4 KiB region covers the whole timer block so
 * timer0/1 accesses stay benign) + the sys-timer counter bank at
 * 0x81003000 (SYS_TIMER_MS per include/arch.h). */
#define B310E_TIMER_BASE         0x81000000ULL
#define B310E_SYSTIMER_BASE      0x81003000ULL

/* sc6530_aux (todo 12) bank bases - keep in sync with
 * hw/misc/sc6530_aux.c (region geometry there). The catch-all is
 * intentionally mapped at priority 0 (see B310E_REGION_PRIORITY). */
#define B310E_AUX_AHB_BASE       0x20500000ULL
#define B310E_AUX_APB_BASE       0x8b000000ULL
#define B310E_AUX_SMC_BASE       0x20000000ULL
#define B310E_AUX_GPIO_BASE      0x8a000000ULL
#define B310E_AUX_PINMUX_BASE    0x8c000000ULL
#define B310E_AUX_BUSMON_BASE    0x20400000ULL
#define B310E_AUX_CATCHALL_BASE  0x10000000ULL
/* Aux PSRAM overlay regions (todo-23: the boot-ready flag 0x0425de8c and
 * the timer-ops pointer 0x0422d2cc) sit OVER the PSRAM alias (0x04000000)
 * and must beat it, so they map one priority level above
 * B310E_REGION_PRIORITY (same level as the DSP subregions). */
#define B310E_AUX_BOOTREADY_BASE 0x0425de8cULL
#define B310E_AUX_TIMEROBJ_BASE  0x0422d2ccULL
#define B310E_AUX_TIMERFN_BASE   0x0422e3e0ULL
#define B310E_AUX_SCIRPOS_BASE   0x0422d4b4ULL
#define B310E_AUX_DLOFITBL_BASE  0x0422e330ULL
#define B310E_AUX_TXKERN_BASE    0x0422c654ULL
#define B310E_AUX_TXOBJ_BASE     0x0423c818ULL
#define B310E_AUX_LCDTBL_BASE    0x0422c8ecULL
#define B310E_AUX_LCDDRV_BASE    0x0422c8fcULL
#define B310E_AUX_CLKOBJ_BASE    0x0422e554ULL
#define B310E_AUX_CLKOBJFLAG_BASE 0x0422e57cULL
#define B310E_AUX_SCIPOOLTBL_BASE 0x0422d478ULL
#define B310E_AUX_SCIPOOL_BASE    0x04280000ULL
#define B310E_AUX_SCIMEM_BASE     0x04280040ULL
#define B310E_AUX_TXSTATE1_BASE   0x0422c67cULL
#define B310E_AUX_TXSTATE2_BASE   0x0422c680ULL
#define B310E_AUX_TXGUARD_BASE    0x04259620ULL
#define B310E_AUX_TXPOOL_BASE     0x04259638ULL
#define B310E_AUX_TXNODE_BASE     0x04259700ULL
#define B310E_AUX_TXSENT_BASE     0x0425a708ULL
#define B310E_AUX_LCDSPEC_BASE    0x0425e0e8ULL

/* sc6530_lcdc (todo 16): the LCDC display controller @ 0x20d00000 + the
 * LCM DBI controller @ 0x20800000 (both devices live in hw/misc/
 * sc6530_lcdc.c; the QemuConsole is 128x160, console index 0). */
#define B310E_LCDC_BASE          0x20d00000ULL
#define B310E_LCM_BASE           0x20800000ULL

/* sc6530_keypad (todo 17): the matrix controller @ 0x87000000. The EIC
 * END key (0x82001900 bit 3) lives INSIDE the todo-15 ANA region - the
 * keypad device holds a QOM link to the ADI device and raises the bit
 * through the ANA bank (no second mapping at 0x82001900). */
#define B310E_KEYPAD_BASE        0x87000000ULL

/* sc6530_dsp (todo 18): the DSP fake's 7 four-byte APB subregisters over
 * the todo-12 aux APB bank (0x8b000000) + two 0x20-byte control windows
 * at the base of BOTH share-mem candidate bases (0x10000000 / 0x30000000
 * - the sharemem RAM regions above are already mapped at
 * B310E_REGION_PRIORITY; the windows sit one level higher and forward to
 * the same RAM). Addresses per docs/audio-dsp-protocol.md "DSP-fake-spec"
 * 2c; keep in sync with hw/misc/sc6530_dsp.c (region geometry there). */
#define B310E_DSP_INT_STS0_BASE    0x8b000140ULL
#define B310E_DSP_INT_SET_CLR0_BASE 0x8b000160ULL
#define B310E_DSP_RST0_SET_BASE    0x8b001068ULL
#define B310E_DSP_RST0_CLR_BASE    0x8b002068ULL
#define B310E_DSP_MCU_CTL0_BASE    0x8b0001a0ULL
#define B310E_DSP_DSP_CTL0_BASE    0x8b0001c0ULL
#define B310E_DSP_PERI_CTL0_BASE   0x8b0001c4ULL
/* SC6530C ground truth (2026-08-27): the DSP soft reset is b16 of the
 * APB_EB0 SET/CLR pair 0x8b000060/0x64 (stock FUN_00067fc8), NOT the
 * SDK's RST0 0x8b001068/0x2068. Keep in sync with hw/misc/sc6530_dsp.c. */
#define B310E_DSP_APB_EB0_SET_BASE 0x8b000060ULL
#define B310E_DSP_APB_EB0_CLR_BASE 0x8b000064ULL
/* The guest's DSP download control struct lives at sharemem base + 0xFE0
 * (runtime [0x0422E598] = 0x10000FE0); the DOWNLOAD handshake itself runs
 * at base + 0 (stock FUN_0003aa76, 2026-08-27). Both areas get windows. */
#define B310E_DSP_SHARE_DL_BASE    0x10000000ULL
#define B310E_DSP_SHARE_DL_ALIAS   0x30000000ULL
#define B310E_DSP_SHARE_CTL_BASE   0x10000fe0ULL
#define B310E_DSP_SHARE_CTL_ALIAS  0x30000fe0ULL
#define B310E_DSP_SHARE_CTL_SIZE   0x20

/* sc6530_usb (todo 19): the USB controller register bank @ 0x20300000
 * (0x1000 - EP0/2/3 ctrl + status + setup regs) + the FIFO window
 * @ 0x20380000 (0x100: EP0 FIFO +0, EP3 +8, EP2 OUT FIFO +0x10 - the
 * 0x20380010 reads our os.bin's usb_debug_poll/wait_connect make).
 * Keep in sync with hw/misc/sc6530_usb.c (region geometry there). */
#define B310E_USB_BASE            0x20300000ULL
#define B310E_USB_FIFO_BASE       0x20380000ULL

/* sc6530_sdio (todo 19): SDIO0 @ 0x20700000 (0x1000 - dma/arg/resp/ctrl/
 * int regs). Keep in sync with hw/misc/sc6530_sdio.c (region geometry). */
#define B310E_SDIO_BASE           0x20700000ULL

/*
 * All B310E regions are mapped with this priority; the todo-12 catch-all
 * (0x10000000..0xffffffff) must be mapped at a LOWER priority (0).
 */
#define B310E_REGION_PRIORITY    1

/* The DSP fake's subregions must beat BOTH the todo-12 aux APB bank
 * (0x8b000000, priority 1) and the sharemem RAM regions (priority 1). */
#define B310E_DSP_REGION_PRIORITY (B310E_REGION_PRIORITY + 1)

/* Device type names (no machine include/ dir: the type strings are
 * declared in the device .c files - keep these in sync with them). */
#define TYPE_SC6530_INTC         "sc6530_intc"
#define TYPE_SC6530_AUX          "sc6530_aux"
#define TYPE_SC6530_ADI          "sc6530_adi"
#define TYPE_SC6530_TIMER        "sc6530_timer"
#define TYPE_SC6530_LCDC         "sc6530_lcdc"
#define TYPE_SC6530_LCM          "sc6530_lcm"
#define TYPE_SC6530_KEYPAD       "sc6530_keypad"
#define TYPE_SC6530_DSP          "sc6530_dsp"
#define TYPE_SC6530_USB          "sc6530_usb"
#define TYPE_SC6530_SDIO         "sc6530_sdio"

/* Boot constants */
#define B310E_WARM_MAGIC         0xFE519C04u   /* PBL warm-boot magic */
#define B310E_WARM_PC            0x00010000u   /* stock main-OS entry */
#define B310E_STOCK_PC           0x00000000u   /* vector table */
#define B310E_OURS_PC            0x34000010u   /* os.bin entry (ARM state) */
#define B310E_ROCKBOX_PC         0x34000000u   /* Rockbox vector table */

#define TYPE_B310E_MACHINE MACHINE_TYPE_NAME("b310e")
OBJECT_DECLARE_TYPE(B310EMachineState, B310EMachineClass, B310E_MACHINE)

struct B310EMachineClass {
    /*< private >*/
    MachineClass parent_class;
};

struct B310EMachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/

    /* -M b310e,boot=warm|stock|ours (default "warm") */
    char *boot_mode;

    /* -M b310e,hold-end=on (default off): hold the EIC END key from reset */
    bool hold_end;

    ARMCPU *cpu;

    /* SC6530 INTC (todo 13): kept for the timer's IRQ wiring below. */
    DeviceState *intc;

    /* NOR XIP backing: zero-filled until -drive if=none,id=nor is loaded */
    uint8_t *nor;
    MemoryRegion nor_iomem;       /* 8 MiB @ 0x0, write-logging ops */

    MemoryRegion psram_alias;     /* 4 MiB @ 0x04000000 (always on) */
    MemoryRegion iram;            /* 256 KiB @ 0x40000000 */
    MemoryRegion dsp_sharemem;    /* 128 KiB @ 0x10000000 */
    MemoryRegion dsp_alias;       /* 128 KiB @ 0x30000000 */
};

/* ---------------------------------------------------------------------- */
/* NOR XIP: reads return the loaded bytes; writes are LOGGED and dropped  */
/* (XIP is read-only RAM - the guest's flash/NV writes must not silently  */
/* disappear from the log; a plain read-only RAM region would drop them). */
/* ---------------------------------------------------------------------- */

static uint32_t b310e_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

static uint64_t b310e_nor_read(void *opaque, hwaddr offset, unsigned size)
{
    B310EMachineState *s = opaque;
    uint64_t val = 0;

    memcpy(&val, s->nor + offset, size);
    return val;
}

static void b310e_nor_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    /* Logged, not stored: NOR is read-only XIP in this model. */
    qemu_log("b310e: NOR write @0x%" PRIx64 " size=%u val=0x%" PRIx64
             " pc=0x%08" PRIx32 " (XIP read-only: logged, not stored)\n",
             (uint64_t)offset, size, value, b310e_guest_pc());
}

static const MemoryRegionOps b310e_nor_ops = {
    .read  = b310e_nor_read,
    .write = b310e_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ---------------------------------------------------------------------- */
/* Boot PC (reset handler: runs AFTER the CPU's own reset, so the value   */
/* sticks - the reset container resets children in addition order and the */
/* CPU is realized before qemu_register_reset() is called here).          */
/* ---------------------------------------------------------------------- */

static void b310e_reset(void *opaque)
{
    B310EMachineState *s = opaque;
    MachineState *machine = MACHINE(s);
    uint32_t pc = B310E_STOCK_PC;

    if (strcmp(s->boot_mode, "warm") == 0) {
        /* Warm-boot fast path magic at the PSRAM base (alias 0x04000000). */
        stl_le_p(memory_region_get_ram_ptr(machine->ram), B310E_WARM_MAGIC);
        pc = B310E_WARM_PC;
    } else if (strcmp(s->boot_mode, "ours") == 0) {
        pc = B310E_OURS_PC;
    } else if (strcmp(s->boot_mode, "rockbox") == 0) {
        /* Rockbox's crt0 builds its high-vector map BECAUSE the real boot
         * chain leaves CP15 V=1 (our menu's start.s); the QEMU CPU resets
         * with V=0, so set the SCTLR V bit here (post-CPU-reset, it sticks)
         * to model the real chain — otherwise the tick IRQ fetches from
         * 0x00000000 and Rockbox hangs in kernel_init. */
        s->cpu->env.cp15.sctlr_el[1] |= (1u << 13);
        pc = B310E_ROCKBOX_PC;
    } else {
        pc = B310E_STOCK_PC; /* magic NOT written: full PBL init path */
    }

    cpu_set_pc(CPU(s->cpu), pc);
    qemu_log("b310e: reset boot=%s pc=0x%08" PRIx32 "\n", s->boot_mode, pc);
}

/* ---------------------------------------------------------------------- */
/* Boot-mode property: -M b310e,boot-mode=warm|stock|ours (default warm). */
/* Named "boot-mode" because "boot" is taken by the generic MachineClass   */
/* BootConfiguration property (hw/core/machine.c).                         */
/* ---------------------------------------------------------------------- */

static char *b310e_get_boot(Object *obj, Error **errp)
{
    B310EMachineState *s = B310E_MACHINE(obj);

    return g_strdup(s->boot_mode ? s->boot_mode : "warm");
}

static void b310e_set_boot(Object *obj, const char *value, Error **errp)
{
    B310EMachineState *s = B310E_MACHINE(obj);

    if (strcmp(value, "warm") != 0 && strcmp(value, "stock") != 0 &&
        strcmp(value, "ours") != 0 && strcmp(value, "rockbox") != 0) {
        error_setg(errp, "b310e: invalid boot mode '%s' "
                   "(expected warm|stock|ours|rockbox)", value);
        return;
    }
    g_free(s->boot_mode);
    s->boot_mode = g_strdup(value);
}

/* Hold-END property: -M b310e,hold-end=on holds the EIC power-button
 * (END key, 0x82001900 bit 3) from reset, mirroring a physically held
 * key - the user HW finding: the stock OS's boot waits for END held. */
static bool b310e_get_hold_end(Object *obj, Error **errp)
{
    return B310E_MACHINE(obj)->hold_end;
}

static void b310e_set_hold_end(Object *obj, bool value, Error **errp)
{
    B310E_MACHINE(obj)->hold_end = value;
}

/* ---------------------------------------------------------------------- */
/* Machine init                                                           */
/* ---------------------------------------------------------------------- */

static void b310e_init(MachineState *machine)
{
    B310EMachineState *s = B310E_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    MemoryRegion *address_space_mem = get_system_memory();
    BlockBackend *blk;
    DeviceState *adi_dev;   /* the ADI device: todo 17's keypad links to it */
    int64_t len;

    if (machine->ram_size != mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be %s", sz);
        g_free(sz);
        exit(EXIT_FAILURE);
    }

    if (!s->boot_mode) {
        s->boot_mode = g_strdup("warm");
    }

    /* CPU: ARM926EJ-S (mc->default_cpu_type), created like musicpal. */
    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    /* PSRAM: the machine RAM at 0x34000000 (machine->ram is auto-created
     * because default_ram_id is set). */
    memory_region_add_subregion_overlap(address_space_mem, B310E_PSRAM_BASE,
                                        machine->ram, B310E_REGION_PRIORITY);

    /* Always-on PSRAM alias at 0x04000000: the stock OS and the PBL see
     * the same RAM through this window (MEM_REMAP on the real chip only
     * switches windows; here both are live, and the 0x205000e0 MEM_REMAP
     * register is a logged no-op in todo 12's sc6530_aux). */
    memory_region_init_alias(&s->psram_alias, OBJECT(machine),
                             "b310e.psram-alias", machine->ram,
                             0, B310E_PSRAM_SIZE);
    memory_region_add_subregion_overlap(address_space_mem,
                                        B310E_PSRAM_ALIAS_BASE,
                                        &s->psram_alias,
                                        B310E_REGION_PRIORITY);

    /* NOR XIP at 0x0: read-only with write logging. */
    s->nor = g_malloc0(B310E_NOR_SIZE);
    memory_region_init_io(&s->nor_iomem, OBJECT(machine), &b310e_nor_ops, s,
                          "b310e.nor", B310E_NOR_SIZE);
    memory_region_add_subregion_overlap(address_space_mem, B310E_NOR_BASE,
                                        &s->nor_iomem, B310E_REGION_PRIORITY);

    /* IRAM at 0x40000000 (vectors/stack live here on the real chip).
     * NOTE: the RAM-region owner must be NULL (not OBJECT(machine)) -
     * memory_region_register_ram asserts that a RAM owner is a DeviceState
     * or NULL (system/memory.c), and MachineState is not a device. */
    memory_region_init_ram(&s->iram, NULL, "b310e.iram",
                           B310E_IRAM_SIZE, &error_fatal);
    memory_region_add_subregion_overlap(address_space_mem, B310E_IRAM_BASE,
                                        &s->iram, B310E_REGION_PRIORITY);

    /* DSP shared RAM: one region at BOTH candidate bases per
     * docs/audio-dsp-protocol.md "DSP-fake-spec" section 1 (the
     * 0x30000000 NOR alias is DROPPED - the real chip cannot have both). */
    memory_region_init_ram(&s->dsp_sharemem, NULL,
                           "b310e.dsp-sharemem", B310E_DSP_SHARE_SIZE,
                           &error_fatal);
    memory_region_add_subregion_overlap(address_space_mem,
                                        B310E_DSP_SHARE_BASE,
                                        &s->dsp_sharemem,
                                        B310E_REGION_PRIORITY);
    memory_region_init_alias(&s->dsp_alias, OBJECT(machine),
                             "b310e.dsp-sharemem-alias", &s->dsp_sharemem,
                             0, B310E_DSP_SHARE_SIZE);
    memory_region_add_subregion_overlap(address_space_mem,
                                        B310E_DSP_SHARE_ALIAS,
                                        &s->dsp_alias,
                                        B310E_REGION_PRIORITY);

    /* NOR contents from -drive if=none,id=nor (missing drive -> zero-filled
     * with a warning). Load the first min(8 MiB, filesize) bytes. */
    blk = blk_by_name("nor");
    if (blk) {
        len = blk_getlength(blk);
        if (len <= 0) {
            warn_report("b310e: cannot read NOR drive length");
        } else {
            size_t n = MIN(len, (int64_t)B310E_NOR_SIZE);

            if (blk_pread(blk, 0, (int64_t)n, s->nor, 0) < 0) {
                warn_report("b310e: failed to read NOR drive");
            } else {
                qemu_log("b310e: NOR XIP loaded %zu bytes from "
                         "-drive if=none,id=nor\n", n);
            }
        }
    } else {
        warn_report("b310e: no NOR drive (-drive if=none,id=nor); "
                    "XIP at 0x0 is zero-filled");
    }

    /* boot=ours / boot=rockbox: load -drive if=none,id=os at 0x34000000.
     * Rockbox is an 836 KiB raw ARM image (vectors at offset 0) — well
     * within the 4 MiB PSRAM window, so the existing MIN(len, PSRAM_SIZE)
     * load path handles it unchanged. */
    if (strcmp(s->boot_mode, "ours") == 0 ||
        strcmp(s->boot_mode, "rockbox") == 0) {
        blk = blk_by_name("os");
        if (blk) {
            len = blk_getlength(blk);
            if (len <= 0) {
                warn_report("b310e: cannot read os drive length");
            } else {
                size_t n = MIN(len, (int64_t)B310E_PSRAM_SIZE);

                if (blk_pread(blk, 0, (int64_t)n,
                              memory_region_get_ram_ptr(machine->ram),
                              0) < 0) {
                    warn_report("b310e: failed to read os drive");
                } else {
                    qemu_log("b310e: os.bin loaded %zu bytes at 0x%"
                             PRIx64 "\n", n, (uint64_t)B310E_PSRAM_BASE);
                }
            }
        } else {
            warn_report("b310e: boot=%s but no os drive "
                        "(-drive if=none,id=os); PSRAM stays zero-filled",
                        s->boot_mode);
        }
    }

    /*
     * Device call sites (todos 12-19 of .omo/plans/b310e-qemu-machine.md).
     * Each device is created here as a SysBusDevice and mmio-mapped with
     * memory_region_add_subregion_overlap(..., B310E_REGION_PRIORITY) so
     * the todo-12 catch-all never shadows it:
     *
     *   DONE (todo 12) sc6530_aux: log+store for AHB 0x20500000, APB
     *        0x8b000000, SMC 0x20000000, GPIO 0x8a000000, pinmux
     *        0x8c000000 + the low-priority (0) catch-all over
     *        0x10000000..0xffffffff (see below).
     *   DONE (todo 13) sc6530_intc @ 0x80000000, sysbus IRQ 0 ->
     *        qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ).
     *   DONE (todo 14) sc6530_timer: timer2 @ 0x81000040 (block base
     *        0x81000000) + sys-timer @ 0x81003000, 1 ms ptimer -> INTC
     *        line 23 (see the creation block below).
     *   DONE (todo 15) sc6530_adi: ADI mailbox @ 0x82000000 + ANA
     *        0x82001000 (8 KiB, incl. WDG 0x82001480 + EIC 0x82001900)
     *        + VBC 0x82003000 (0x100) + sc6530_ana_read/write trace
     *        events (see below).
     *   DONE (todo 16) sc6530_lcdc @ 0x20d00000 + sc6530_lcm
     *        @ 0x20800000 + QemuConsole 128x160 (see below).
     *   DONE (todo 17) sc6530_keypad @ 0x87000000 + EIC END key
     *        (0x82001900 bit 3 raised through the ADI's ANA bank via
     *        the keypad's 'adi' link - see below; HMP sendkey drives
     *        the matrix through the QEMU generic input layer).
 *   DONE (todo 18) sc6530_dsp: APB subregisters 0x8b000140/160/
 *        0x8b001068/0x8b002068/0x8b0001a0/0x8b0001c0/0x8b0001c4 as
 *        4-byte higher-priority subregions over the aux APB region;
 *        the share-mem regions are already mapped above (see below).
 *   DONE (todo 19) sc6530_usb @ 0x20300000 (+ FIFO window 0x20380000)
 *        + sc6530_sdio @ 0x20700000: log+store no-ops ("host never
 *        connected" / "no card" - see below).
 */

    /* SC6530 aux (todo 12): log+store banks for AHB 0x20500000, APB
     * 0x8b000000, SMC 0x20000000, GPIO 0x8a000000, pinmux 0x8c000000 +
     * the SILENT bus-monitor bank 0x20400000 (the stock boot loop clears
     * its channel enables every pass - see hw/misc/sc6530_aux.c) mapped
     * at B310E_REGION_PRIORITY + the catch-all over
     * 0x10000000..0xffffffff at priority 0 (UNMODELED log + read 0,
     * benign-ready table for known polled status registers - see
     * hw/misc/sc6530_aux.c). The catch-all MUST stay at priority 0: it
     * only sees addresses no B310E_REGION_PRIORITY region claims.
     * NOTE: no watchdog here - 0x82001480 lives in the todo-15 ANA
     * region (sc6530_adi @ 0x82001000). */
    {
        DeviceState *aux_dev = qdev_new(TYPE_SC6530_AUX);
        SysBusDevice *aux_sbd = SYS_BUS_DEVICE(aux_dev);

        sysbus_mmio_map_overlap(aux_sbd, 0, B310E_AUX_AHB_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 1, B310E_AUX_APB_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 2, B310E_AUX_SMC_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 3, B310E_AUX_GPIO_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 4, B310E_AUX_PINMUX_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 5, B310E_AUX_BUSMON_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 6, B310E_AUX_BOOTREADY_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 7, B310E_AUX_TIMEROBJ_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 8, B310E_AUX_TIMERFN_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 9, B310E_AUX_SCIRPOS_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 10, B310E_AUX_DLOFITBL_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 11, B310E_AUX_TXKERN_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 12, B310E_AUX_TXOBJ_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 13, B310E_AUX_LCDTBL_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 14, B310E_AUX_LCDDRV_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 15, B310E_AUX_CLKOBJ_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 16, B310E_AUX_CLKOBJFLAG_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 17, B310E_AUX_SCIPOOLTBL_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 18, B310E_AUX_SCIPOOL_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 19, B310E_AUX_SCIMEM_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 20, B310E_AUX_TXSTATE1_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 21, B310E_AUX_TXSTATE2_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 22, B310E_AUX_TXGUARD_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 23, B310E_AUX_TXPOOL_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 24, B310E_AUX_TXNODE_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 25, B310E_AUX_TXSENT_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 26, B310E_AUX_LCDSPEC_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(aux_sbd, 27, B310E_AUX_CATCHALL_BASE, 0);
        sysbus_realize_and_unref(aux_sbd, &error_fatal);
    }

    /* SC6530 INTC @ 0x80000000 (todo 13): 32 input lines, output SysBus
     * IRQ 0 wired directly to the ARM926 CPU's IRQ line (no GIC - the ARM
     * CPU exposes plain GPIO lines: ARM_CPU_IRQ = 0, target/arm/cpu-qom.h).
     * Created manually (not sysbus_create_simple) and mmio-mapped with
     * sysbus_mmio_map_overlap at B310E_REGION_PRIORITY so the todo-12
     * catch-all (priority 0) can never shadow it. The id gives a stable
     * QOM path (/peripheral/sc6530-intc) for debugging and qtest. */
    {
        DeviceState *intc = qdev_new(TYPE_SC6530_INTC);

        qdev_set_id(intc, g_strdup("sc6530-intc"), &error_fatal);
        s->intc = intc;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(intc), 0,
                           qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(intc), 0, B310E_INTC_BASE,
                                B310E_REGION_PRIORITY);
    }

    /* SC6530 timer (todo 14): timer2 @ 0x81000040 (block base 0x81000000)
     * + sys-timer @ 0x81003000. The 1 ms ptimer raises INTC line 23
     * (SYS_TIMER_IRQ_LINE, kernel/irq.c); the guest's sys_tick_isr clears
     * the source with a write-9 to the timer +0xc (kernel/sched.c), and
     * the INTC itself has no pending-clear. Created after the INTC so the
     * IRQ can be wired to its input line 23. */
    {
        DeviceState *timer_dev = qdev_new(TYPE_SC6530_TIMER);
        SysBusDevice *timer_sbd = SYS_BUS_DEVICE(timer_dev);

        sysbus_realize_and_unref(timer_sbd, &error_fatal);
        sysbus_connect_irq(timer_sbd, 0, qdev_get_gpio_in(s->intc, 23));
        sysbus_mmio_map_overlap(timer_sbd, 0, B310E_TIMER_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(timer_sbd, 1, B310E_SYSTIMER_BASE,
                                B310E_REGION_PRIORITY);
    }

    /* SC6530 ADI (todo 15) @ 0x82000000: the ADI mailbox (0x18 read
     * command, 0x1c read data, 0x20 status) + the ANA analog register
     * bank 0x82001000 (8 KiB - codec regs, WDG 0x82001480, EIC
     * 0x82001900; the watchdog lives HERE only, todo-12 aux does not
     * map it) + the VBC audio FIFO 0x82003000 (0x100, separate region
     * - the ANA 0x2000 would end at 0x82002fff and miss it). Every
     * access emits sc6530_ana_read/write trace events with the guest
     * PC (the audio observatory; --trace "sc6530_ana_*"). The EIC
     * 0x82001900 inside the ANA region is NOT re-mapped: todo 17's
     * keypad device holds a QOM link to this device (adi_dev below)
     * and raises the END bit through the ANA bank. */
    adi_dev = qdev_new(TYPE_SC6530_ADI);
    {
        SysBusDevice *adi_sbd = SYS_BUS_DEVICE(adi_dev);

        sysbus_mmio_map_overlap(adi_sbd, 0, B310E_ADI_MAILBOX_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(adi_sbd, 1, B310E_ADI_ANA_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(adi_sbd, 2, B310E_ADI_VBC_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(adi_sbd, &error_fatal);
    }

    /* SC6530 keypad (todo 17) @ 0x87000000: the matrix controller
     * (edge-latch model per drivers/keypad.c: int_raw bits 0-3 = DOWN
     * edges, bits 4-7 = their releases; key_status row bytes encode
     * (row<<4)|col so the guest's decode yields the s_keytrn KEY_*
     * codes). Host input arrives through the QEMU generic input layer
     * (HMP sendkey / QMP input-send-event; see the mapping table in
     * hw/misc/sc6530_keypad.c - note v11.1.0 names Enter 'ret', so
     * `sendkey ret` is CENTER). The EIC END key (0x82001900 bit 3)
     * lives inside the todo-15 ANA region and is raised through the
     * ADI's ANA bank via the keypad's 'adi' link property - there is
     * deliberately NO second mapping at 0x82001900 (the guest reads
     * the EIC through the ADI mailbox, which resolves from the ADI's
     * own register array, never from the address space). */
    {
        DeviceState *keypad_dev = qdev_new(TYPE_SC6530_KEYPAD);
        SysBusDevice *keypad_sbd = SYS_BUS_DEVICE(keypad_dev);

        object_property_set_link(OBJECT(keypad_dev), "adi",
                                 OBJECT(adi_dev), &error_fatal);
        object_property_set_bool(OBJECT(keypad_dev), "hold-end",
                                 s->hold_end, &error_fatal);
        sysbus_mmio_map_overlap(keypad_sbd, 0, B310E_KEYPAD_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(keypad_sbd, &error_fatal);
    }

    /* SC6530 LCDC + LCM (todo 16) @ 0x20d00000 / 0x20800000: two devices
     * (both live in hw/misc/sc6530_lcdc.c). The LCDC register bank
     * (refresh trigger: ctrl bit 3 -> framebuffer copy from PSRAM at
     * img.y_base_addr << 2 into the QemuConsole, then irq.raw bit 0 =
     * DMA done so lcd_show's poll terminates) + the LCM DBI store+echo
     * bank. Both mapped at B310E_REGION_PRIORITY - they sit inside the
     * todo-12 catch-all range (0x10000000..0xffffffff) and would be
     * shadowed at priority 0. */
    {
        DeviceState *lcdc_dev = qdev_new(TYPE_SC6530_LCDC);
        SysBusDevice *lcdc_sbd = SYS_BUS_DEVICE(lcdc_dev);

        sysbus_mmio_map_overlap(lcdc_sbd, 0, B310E_LCDC_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(lcdc_sbd, &error_fatal);
    }
    {
        DeviceState *lcm_dev = qdev_new(TYPE_SC6530_LCM);
        SysBusDevice *lcm_sbd = SYS_BUS_DEVICE(lcm_dev);

        sysbus_mmio_map_overlap(lcm_sbd, 0, B310E_LCM_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(lcm_sbd, 1, 0x60000000,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(lcm_sbd, &error_fatal);
    }

    /* SC6530 DSP fake (todo 18): the share-mem handshake machine +
     * APB mailbox registers, per docs/audio-dsp-protocol.md
     * "DSP-fake-spec". The 128 KiB sharemem RAM is ALREADY mapped above
     * (b310e.dsp-sharemem @ 0x10000000 + the alias @ 0x30000000): the
     * device holds a QOM link "sharemem" to that region and reads/writes
     * it via memory_region_get_ram_ptr - it does NOT re-map the RAM.
     * Its own 9 sysbus slots are: 7 four-byte APB subregisters
     * (0x8b000140..0x8b0001c4, incl. the audio-ownership PERI_CTL0
     * 0x8b0001c4 which the DSP device owns per spec 5) mapped OVER the
     * todo-12 aux APB bank, plus two 0x20-byte control windows at the
     * base of BOTH share-mem bases (they forward to the same RAM and
     * watch the handshake fields). All 9 sit at B310E_DSP_REGION_PRIORITY
     * (one level above the aux bank and the RAM regions). */
    {
        DeviceState *dsp_dev = qdev_new(TYPE_SC6530_DSP);
        SysBusDevice *dsp_sbd = SYS_BUS_DEVICE(dsp_dev);

        object_property_set_link(OBJECT(dsp_dev), "sharemem",
                                 OBJECT(&s->dsp_sharemem), &error_fatal);
        sysbus_mmio_map_overlap(dsp_sbd, 0, B310E_DSP_INT_STS0_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(dsp_sbd, 1, B310E_DSP_INT_SET_CLR0_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(dsp_sbd, 2, B310E_DSP_RST0_SET_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(dsp_sbd, 3, B310E_DSP_RST0_CLR_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(dsp_sbd, 4, B310E_DSP_MCU_CTL0_BASE,
                                B310E_DSP_REGION_PRIORITY);
        sysbus_mmio_map_overlap(dsp_sbd, 5, B310E_DSP_DSP_CTL0_BASE,
                                B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 6, B310E_DSP_PERI_CTL0_BASE,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 7, B310E_DSP_APB_EB0_SET_BASE,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 8, B310E_DSP_APB_EB0_CLR_BASE,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 9, B310E_DSP_SHARE_DL_BASE,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 10, B310E_DSP_SHARE_DL_ALIAS,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 11, B310E_DSP_SHARE_CTL_BASE,
                            B310E_DSP_REGION_PRIORITY);
    sysbus_mmio_map_overlap(dsp_sbd, 12, B310E_DSP_SHARE_CTL_ALIAS,
                            B310E_DSP_REGION_PRIORITY);
        sysbus_realize_and_unref(dsp_sbd, &error_fatal);
    }

    /* SC6530 USB (todo 19) @ 0x20300000: log+store no-op ("host never
     * connected" forever - status reads return 0, so our os.bin's
     * BOUNDED usb_debug_init connect wait burns its 10M budget and
     * returns; usb_debug_poll reads 0s and re-arms). Two regions: the
     * register bank 0x20300000 (0x1000) + the FIFO window 0x20380000
     * (0x100 - the EP2 OUT FIFO reads at 0x20380010 must not fault).
     * Mapped at B310E_REGION_PRIORITY (inside the catch-all range). */
    {
        DeviceState *usb_dev = qdev_new(TYPE_SC6530_USB);
        SysBusDevice *usb_sbd = SYS_BUS_DEVICE(usb_dev);

        sysbus_mmio_map_overlap(usb_sbd, 0, B310E_USB_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_mmio_map_overlap(usb_sbd, 1, B310E_USB_FIFO_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(usb_sbd, &error_fatal);
    }

    /* SC6530 SDIO0 (todo 19) @ 0x20700000: log+store no-op - status
     * reads return 0 = "no card", so the guest's SD init completes
     * without a card (all SDIO waits are bounded in drivers/sdio.c).
     * Mapped at B310E_REGION_PRIORITY (inside the catch-all range). */
    {
        DeviceState *sdio_dev = qdev_new(TYPE_SC6530_SDIO);
        SysBusDevice *sdio_sbd = SYS_BUS_DEVICE(sdio_dev);

        sysbus_mmio_map_overlap(sdio_sbd, 0, B310E_SDIO_BASE,
                                B310E_REGION_PRIORITY);
        sysbus_realize_and_unref(sdio_sbd, &error_fatal);
    }

    qemu_register_reset(b310e_reset, s);
}

static void b310e_machine_init(MachineClass *mc)
{
    mc->desc = "Spreadtrum SC6530C (Samsung SM-B310E)";
    mc->init = b310e_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = B310E_PSRAM_SIZE;
    mc->default_ram_id = "b310e.psram";

    object_class_property_add_str(OBJECT_CLASS(mc), "boot-mode",
                                  b310e_get_boot, b310e_set_boot);
    object_class_property_add_bool(OBJECT_CLASS(mc), "hold-end",
                                   b310e_get_hold_end, b310e_set_hold_end);
}

DEFINE_MACHINE_EXTENDED("b310e", MACHINE, B310EMachineState,
                        b310e_machine_init, false, arm_machine_interfaces)
