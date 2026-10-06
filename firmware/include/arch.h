/*
 * B310E-OS — include/arch.h
 *
 * Architecture / SoC constants for the Spreadtrum SC6530C
 * (ARM926EJ-S @ 208 MHz, ARMv5TE). Memory map verified by fpdoom /
 * spreadtrum_flash research (see learnings.md); refined in later waves.
 */

#ifndef B310E_OS_ARCH_H
#define B310E_OS_ARCH_H

/* ---- memory map ------------------------------------------------------- */
#define SC6530_SDRAM_BASE    0x14000000u  /* PSRAM LINK base for the -pie
                                             image ONLY. NOT a valid runtime
                                             address on the SC6530 (that's
                                             the SC6531E's native window):
                                             the image self-relocates and
                                             runs at 0x04000000 (SD readbin,
                                             MEM_REMAP=0) or 0x34000000
                                             (USB ram, MEM_REMAP=1). Derive
                                             the runtime window from
                                             __image_start (fpdoom entry.c:
                                             fw_addr = image & 0xf0000000;
                                             ram = fw_addr + 0x04000000). */
#define SC6530_IRAM_BASE     0x40000000u  /* on-chip SRAM (CHIPRAM)         */
#define SC6530_VECTORS_ADDR  0x40019000u  /* exception vector table         */
#define SC6530_STACK_TOP     0x40022000u  /* boot stack (grows down)        */

/* ---- peripherals (used from Wave 3 on) -------------------------------- */
#define SC6530_SYS_TIMER     0x81003000u  /* system timer (+0xc = 1 ms)     */
#define SC6530_IRQ_ENABLE    0x80000008u  /* IRQ enable                     */
#define SC6530_IRQ_PENDING   0x80000004u  /* IRQ pending                    */

/* ---- cache maintenance (arch/cache.s — fpdoom asmcode.s:84-123,
 * Unlicense) ------------------------------------------------------------ */
/* clean_dcache(): clean the entire D-cache (CP15 test-and-clean loop) and
 * drain the write buffer. MUST be called before the LCDC DMA (0x20d00000)
 * reads the framebuffer: the CPU writes lcd_fb into cache lines while the
 * DMA reads SDRAM directly (mirrors fpdoom sys_start_refresh, syscode.c:570).
 * No-op while the D-cache is disabled (which is the documented ARM926EJ-S
 * behavior with the MMU off — see arch/cache.s). */
void clean_dcache(void);
void clean_invalidate_dcache(void);
void enable_dcache(void);
void disable_dcache(void);

/* ---- BOE LCD mount orientation (single knob) ---------------------------
 * ST7735 MADCTL (cmd 0x36) bitfield: bit7 MY (row order = vertical flip),
 * bit6 MX (col order), bit5 MV (swap axes), bit3 BGR. The BOE panel
 * (id 0x7c89f0) datasheet mac_arg is 0xd0 (MY|MX|BGR). fpdoom's proven
 * rotate math for the B310E mount: 0xd0 ^ 0xc0 (rtab[2]) = 0x10 = BGR
 * only, normal orientation. The earlier "mirrored text" report was the
 * FONT RENDERER reading glyphs upside down (drivers/lcd.c lcd_putc, now
 * fixed) — NOT the panel; 0x90 was a wrong guess chasing that bug.
 * diag-rot.bin still cycles {0x10, 0x90, 0x50, 0xd0} to confirm. */
#define B310E_MADCTL 0x10u

#endif /* B310E_OS_ARCH_H */
