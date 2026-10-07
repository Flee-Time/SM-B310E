/* B310E-OS linker script — Spreadtrum SC6530C (ARM926EJ-S @ 208 MHz, ARMv5TE)
 *
 * Memory map (verified by fpdoom/spreadtrum_flash research, see
 * .omo/notepads/b310e-custom-os/learnings.md):
 *   SDRAM/PSRAM  0x14000000  (4 MB) — the fpdoom app window (IMAGE_START for
 *                                     CHIP != 2). The sdboot readbin loads the
 *                                     image here (diff = 0, runs in place);
 *                                     spd_dump `fdl os.bin ram` loads at the
 *                                     SC6530 ram_addr 0x34000000, where
 *                                     arch/start.s self-relocates (the .rel
 *                                     table appended to the .bin).
 *   IRAM/CHIPRAM 0x40000000  (on-chip SRAM) — exception vectors @ +0x19000,
 *                                     stacks @ +0x22000. Not part of our
 *                                     load image; reserved via NOLOAD.
 *
 * The image is built -pie and ships with a pack_reloc .rel table appended;
 * arch/start.s applies it (diff = runtime - __image_start) so the same
 * binary runs at 0x14000000 (SD boot) or 0x34000000 (USB ram load).
 */

ENTRY(_start)

MEMORY
{
    /* External PSRAM embedded in the SC6530C package (auto-detected 2M/4M) */
    SDRAM (rwx) : ORIGIN = 0x14000000, LENGTH = 4M
    /* On-chip SRAM ("CHIPRAM") — vectors + stacks live here */
    IRAM  (rw)  : ORIGIN = 0x40000000, LENGTH = 256K
}

/* ---- absolute symbols for the fixed on-chip IRAM layout -----------------
 * Plain symbol definitions (kept in the ELF symbol table) so later C/asm
 * code can reference the vector table and stack without magic numbers.
 * They are NOT loadable sections — nothing from the image lands in IRAM.
 */
__image_start   = 0x14000000;
__vectors_start = 0x40019000;
__stack_bottom  = 0x40009000;   /* EARLY boot stack — the only IRAM window
                                   guaranteed valid at entry (fpdoom
                                   sdboot/start.s:43). iram(1..3) above this
                                   is enabled later by sc6530_init_iram. */
__stack_top     = 0x40022000;   /* high IRAM stack — only AFTER iram(1..3)
                                   enable (sc6530_chip_init); later phases may
                                   switch to it. */

SECTIONS
{
    /* ---- load image, all in SDRAM ------------------------------------ */
    . = ORIGIN(SDRAM);

    .text : {
        KEEP(*(.text.boot))     /* boot stub first (menu.bin only - the
                                   merged loader: copy-to-RAM + key check;
                                   empty for os.bin) */
        KEEP(*(.text.start))    /* _start after the boot stub */
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
    } > SDRAM

    .rodata : {
        *(.rodata*)
    } > SDRAM

    .data : {
        *(.data*)
    } > SDRAM

    /* Loadable image size + _start offset (for the menu's boot stub) -
     * BEFORE the NOLOAD .bss so the size covers only .text+.rodata+.data
     * (exactly what objcopy puts in the .bin). */
    __image_size   = . - __image_start;
    __start_offset = _start - __image_start;

    /* Offset of the SMC init routine within the image (for the menu's
     * boot stub: it must re-run the SMC init when NOR-booting, where the
     * PSRAM is at the stock 0x04000000 window, to move it to 0x34000000
     * before copying the image there). Computed at LINK time — the stub
     * assembler cannot resolve a cross-object subtraction. */
    __smc_offset = sc6530_init_smc_asm - __image_start;

    /* Zero-initialized BSS — cleared by arch/start.s before main() */
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > SDRAM

    /* BSS size — arch/start.s clears bss RELATIVE to the runtime base
     * (the absolute __bss_start is wrong when the image runs relocated). */
    __bss_size = __bss_end - __bss_start;

    /* ---- on-chip IRAM reservations (NOLOAD, address-only) ------------- */
    /* The exception stubs (arch/vectors.s int_vectors, ~0x98 bytes) are
     * copied here at runtime by kernel/irq.c irq_init(); 0x100 reserves
     * headroom. The 4 KB coarse page table for the 0xffff0000 high-vector
     * page sits right after at 0x40019400 (VECTOR_L2, still within IRAM). */
    .vectors 0x40019000 (NOLOAD) : {
        . = . + 0x100;          /* 8 exception vectors + stub bodies */
    } > IRAM

    .stack 0x40022000 (NOLOAD) : {
        . = . + 0x1000;         /* 4 KB stack, grows down from __stack_top */
    } > IRAM
}
