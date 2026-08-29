/* B310E-OS — arch/mmu.s
 *
 * ARM926EJ-S MMU enable, ported from fpdoom's asmcode.s:84-106 (Unlicense /
 * public domain — copy freely). enable_mmu follows fpdoom's exact sequence
 * (TTBR -> DACR -> invalidate -> Control Register read/OR/write) with the
 * TLB and I-cache invalidates that fpdoom issues from C (sys_prep_vectors /
 * sys_set_handlers) folded in, so the function is self-contained:
 *
 *   r0 = translation table base (16 KB, 4096 1 MB-section entries)
 *   r1 = Domain Access Control Register value (fpdoom uses 0x57:
 *        domain 0 manager, 1..3 client, 4..15 no access)
 *
 * NOTE on the Control Register OR: fpdoom's enable_mmu does `orr r0, #5`
 * (MMU + D-cache). We OR only bit 0 (MMU): the parallel D-cache wave
 * (arch/cache.s enable_dcache) already set bit 2, and the ARM926EJ-S TRM
 * (DDI0198D Table 4-3) documents that the C bit is overridden while M=0 —
 * so D-cache comes alive here the moment M=1, with the TTB's cacheable
 * attributes (drivers/lcd.c cleans before every LCDC DMA). High vectors
 * (bit 13), I-cache (bit 12) and alignment (bit 1) were set by start.s.
 *
 * Calling convention: ARM state, called from Thumb C via the linker's
 * interworking veneer (same as `bl main` in start.s).
 */

    .syntax unified
    .arch   armv5te
    .arm

    /* ---- enable_mmu ------------------------------------------------------ */
    .section .text.enable_mmu, "ax", %progbits
    .p2align 2
    .type   enable_mmu, %function
    .globl  enable_mmu
enable_mmu:
    mcr     p15, #0, r0, c2, c0, #0   /* write TTBR (Translation Table Base) */
    mcr     p15, #0, r1, c3, c0, #0   /* write DACR (domain arg)             */
    mov     r0, #0
    mcr     p15, #0, r0, c8, c7, #0   /* invalidate TLB                      */
    mcr     p15, #0, r0, c7, c6, #0   /* invalidate DCache                   */
    mcr     p15, #0, r0, c7, c5, #0   /* invalidate ICache                   */
    mcr     p15, #0, r0, c7, c10, #4  /* drain write buffer                  */
    mrc     p15, #0, r0, c1, c0, #0   /* read Control Register               */
    orr     r0, #1                    /* enable MMU (bit 0)                  */
    mcr     p15, #0, r0, c1, c0, #0   /* write Control Register              */
    bx      lr
    .size   enable_mmu, . - enable_mmu

    /* ---- invalidate_tlb (fpdoom asmcode.s:94-97 verbatim) ---------------- */
    .section .text.invalidate_tlb, "ax", %progbits
    .p2align 2
    .type   invalidate_tlb, %function
    .globl  invalidate_tlb
invalidate_tlb:
    mov     r0, #0
    mcr     p15, #0, r0, c8, c7, #0
    bx      lr
    .size   invalidate_tlb, . - invalidate_tlb

    /* ---- invalidate_tlb_mva (fpdoom asmcode.s:99-101 verbatim) ----------- */
    .section .text.invalidate_tlb_mva, "ax", %progbits
    .p2align 2
    .type   invalidate_tlb_mva, %function
    .globl  invalidate_tlb_mva
invalidate_tlb_mva:
    mcr     p15, #0, r0, c8, c7, #1
    bx      lr
    .size   invalidate_tlb_mva, . - invalidate_tlb_mva

    /* ---- invalidate_icache (fpdoom asmcode.s:103-106 verbatim) ----------- */
    .section .text.invalidate_icache, "ax", %progbits
    .p2align 2
    .type   invalidate_icache, %function
    .globl  invalidate_icache
invalidate_icache:
    mov     r0, #0
    mcr     p15, #0, r0, c7, c5, #0
    bx      lr
    .size   invalidate_icache, . - invalidate_icache
