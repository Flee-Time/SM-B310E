/* B310E-OS — arch/cache.s
 *
 * ARM926EJ-S cache maintenance ops, ported VERBATIM from fpdoom's
 * asmcode.s:84-123 (Unlicense / public domain — copy freely).
 *
 * clean_dcache / clean_invalidate_dcache are instruction-identical to
 * fpdoom: the CP15 "test and clean" loop (mrc c7,c10,#3 / c7,c14,#3 until
 * the Z flag says the cache is clean) followed by the write-buffer drain
 * (mcr c7,c10,#4). fpdoom calls clean_dcache() in sys_start_refresh
 * (syscode.c:570) right before the LCDC DMA refresh — the CPU writes the
 * framebuffer into cache lines, the LCDC DMA reads SDRAM directly, so the
 * cache must be cleaned first or the DMA streams stale pixels.
 *
 * enable_dcache: invalidate the D-cache, then set the Control Register
 * C bit (bit 2) while keeping the M bit (bit 0, MMU) CLEAR. NOTE — the
 * ARM926EJ-S TRM (DDI0198D, Table 4-3) documents that with M=0 the C bit
 * is overridden: "DCache effectively disabled. All data accesses are
 * noncachable, nonbufferable." So on this silicon the enable is a
 * documented no-op until a future wave adds MMU page tables; the
 * clean_dcache() discipline is then already in place for the LCDC DMA.
 * We must NOT set bit 0 here: the OS has no translation tables (TTBR
 * unset), so enabling the MMU would fault on the very next access.
 *
 * Entry: ARM state, called from Thumb C code — the linker inserts the
 * interworking veneer at each call site (same as `bl main` in start.s).
 */

    .syntax unified
    .arch   armv5te
    .arm

    .text

    .type   clean_dcache, %function
    .globl  clean_dcache
clean_dcache:
    /* clean entire dcache using test and clean
     * apsr_nzcv == r15 (Clang doesn't understand r15) */
1:  mrc     p15, #0, apsr_nzcv, c7, c10, #3
    bne     1b
    mov     r0, #0
    mcr     p15, #0, r0, c7, c10, #4 /* drain write buffer */
    bx      lr
    .size   clean_dcache, . - clean_dcache

    .type   clean_invalidate_dcache, %function
    .globl  clean_invalidate_dcache
clean_invalidate_dcache:
    /* clean and invalidate entire dcache */
1:  mrc     p15, #0, apsr_nzcv, c7, c14, #3
    bne     1b
    mov     r0, #0
    mcr     p15, #0, r0, c7, c10, #4 /* drain write buffer */
    bx      lr
    .size   clean_invalidate_dcache, . - clean_invalidate_dcache

    .type   enable_dcache, %function
    .globl  enable_dcache
enable_dcache:
    mov     r0, #0
    mcr     p15, #0, r0, c7, c6, #0   /* invalidate entire D-cache */
    mrc     p15, #0, r0, c1, c0, #0   /* read Control Register */
    bic     r0, #1                    /* keep MMU (bit 0) OFF — no TTBR */
    orr     r0, #2                    /* D-cache (bit 2) enable */
    mcr     p15, #0, r0, c1, c0, #0   /* write Control Register */
    bx      lr
    .size   enable_dcache, . - enable_dcache

    .type   disable_dcache, %function
    .globl  disable_dcache
disable_dcache:
    mrc     p15, #0, r0, c1, c0, #0   /* read Control Register */
    bic     r0, #2                    /* clear D-cache (bit 2) */
    mcr     p15, #0, r0, c1, c0, #0   /* write Control Register */
    bx      lr
    .size   disable_dcache, . - disable_dcache
