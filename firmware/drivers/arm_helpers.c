/*
 * B310E-OS — drivers/arm_helpers.c
 *
 * Minimal libgcc substitutes for ARM EABI helpers the kernel needs.
 *
 * WHY: os.elf links with -nostdlib -nodefaultlibs, so libgcc is not
 * available at link time. The moment something calls kprintf(), printk.c
 * becomes live and requires:
 *   - __aeabi_uidiv / __aeabi_uidivmod   (unsigned division in %d/%u/%x)
 *   - __aeabi_idiv / __aeabi_idivmod     (signed division)
 *   - __gnu_thumb1_case_shi              (Thumb-1 switch table dispatch)
 *
 * All implementations are extracted verbatim (semantically and, for the
 * asm ones, instruction-for-instruction) from the pinned toolchain's own
 * libgcc (Arm GNU Toolchain 14.2.Rel1, thumb/nofp multilib) — disassembled
 * and transcribed, so behavior matches the real helpers exactly, including
 * the divide-by-zero convention (0 if the dividend is 0, else 0xffffffff)
 * and the EABI divmod register contract (r0 = quotient, r1 = remainder).
 *
 * ARM EABI divmod result-in-registers cannot be expressed in plain C, so
 * the mod wrappers are naked asm that mirror libgcc's own sequence:
 *   push {r0, r1, lr}; bl <div>; pop {r1, r2, lr}
 *   mul r3, r2, r0; sub r1, r1, r3; bx lr
 *
 * All Thumb-1 (armv5te), all -Os friendly. No 64-bit intermediates
 * (64-bit ops would recursively pull in more helpers).
 */

#include <stddef.h>
#include <stdint.h>

/* ---- unsigned division: restoring shift-subtract ----------------------- */

/* quotient = n/d, *rem = n%d. d != 0 guaranteed by the div0 handling in
 * __aeabi_uidiv (checked before the call). */
static uint32_t udiv_rem(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    int i;

    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    if (rem != NULL)
        *rem = r;
    return q;
}

/* libgcc __aeabi_idiv0 convention: div-by-zero yields 0 when the dividend
 * was 0, otherwise 0xffffffff. */
static uint32_t div0_result(uint32_t n)
{
    return (n == 0) ? 0u : 0xffffffffu;
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d)
{
    if (d == 0)
        return div0_result(n);
    return udiv_rem(n, d, NULL);
}

int32_t __aeabi_idiv(int32_t n, int32_t d)
{
    uint32_t un, ud, q;
    int neg;

    if (d == 0)
        return (int32_t)div0_result((uint32_t)n);
    un = (n < 0) ? (uint32_t)0u - (uint32_t)n : (uint32_t)n;
    ud = (d < 0) ? (uint32_t)0u - (uint32_t)d : (uint32_t)d;
    neg = (n < 0) != (d < 0);
    q = udiv_rem(un, ud, NULL);
    /* negating q wraps to INT32_MIN for INT32_MIN / -1 — matches ARM */
    return neg ? -(int32_t)q : (int32_t)q;
}

/* ---- EABI divmod wrappers (r0 = quotient, r1 = remainder) -------------- */
/* Thumb-1 notes: POP cannot take lr (only PC), and MUL requires the dest to
 * overlap a source — hence the pop-retaddr-into-r3 + mul-r2-r0 sequence. */

__attribute__((naked)) void __aeabi_uidivmod(uint32_t n, uint32_t d)
{
    (void)n; (void)d;
    __asm__ __volatile__(
        ".syntax unified\n\t"
        "push  {r0, r1, lr}\n\t"
        "bl    __aeabi_uidiv\n\t"
        "pop   {r1, r2}\n\t"
        "pop   {r3}\n\t"
        "muls  r2, r0\n\t"
        "subs  r1, r1, r2\n\t"
        "bx    r3\n\t");
}

__attribute__((naked)) void __aeabi_idivmod(int32_t n, int32_t d)
{
    (void)n; (void)d;
    __asm__ __volatile__(
        ".syntax unified\n\t"
        "push  {r0, r1, lr}\n\t"
        "bl    __aeabi_idiv\n\t"
        "pop   {r1, r2}\n\t"
        "pop   {r3}\n\t"
        "muls  r2, r0\n\t"
        "subs  r1, r1, r2\n\t"
        "bx    r3\n\t");
}

/* ---- Thumb-1 switch table dispatch (libgcc _thumb1_case_*i, verbatim) --
 *
 * Called as `bl __gnu_thumb1_case_shi` from a Thumb-1 switch: lr points at
 * the inline table (halfword offsets relative to the table), r0 = index.
 * Computes lr = table + 2 * (signed halfword table[index]) and returns
 * there via bx — i.e. jumps to the case label with the Thumb bit set.
 * Instruction sequence transcribed from the disassembly of the pinned
 * toolchain's libgcc (thumb/nofp multilib, _thumb1_case_shi.o). */
__attribute__((naked)) void __gnu_thumb1_case_shi(void)
{
    __asm__ __volatile__(
        ".syntax unified\n\t"
        "push  {r0, r1}\n\t"
        "mov   r1, lr\n\t"
        "lsrs  r1, r1, #1\n\t"
        "lsls  r0, r0, #1\n\t"
        "lsls  r1, r1, #1\n\t"
        "ldrsh r1, [r1, r0]\n\t"
        "lsls  r1, r1, #1\n\t"
        "add   lr, r1\n\t"
        "pop   {r0, r1}\n\t"
        "bx    lr\n\t");
}

/* Signed byte variant (small switches, e.g. keypad key_name()'s case list).
 * Disassembled from the pinned libgcc (_thumb1_case_sqi.o): note there is
 * NO sign-extension fixup — the compiler emits the byte table pre-shifted
 * so a plain lsls #1 gives the byte-relative jump offset. */
__attribute__((naked)) void __gnu_thumb1_case_sqi(void)
{
    __asm__ __volatile__(
        ".syntax unified\n\t"
        "push  {r1}\n\t"
        "mov   r1, lr\n\t"
        "lsrs  r1, r1, #1\n\t"
        "lsls  r1, r1, #1\n\t"
        "ldrsb r1, [r1, r0]\n\t"
        "lsls  r1, r1, #1\n\t"
        "add   lr, r1\n\t"
        "pop   {r1}\n\t"
        "bx    lr\n\t");
}

/* Unsigned byte variant (_thumb1_case_uqi.o) — identical shape to sqi,
 * byte load instead of signed. */
__attribute__((naked)) void __gnu_thumb1_case_uqi(void)
{
    __asm__ __volatile__(
        ".syntax unified\n\t"
        "push  {r1}\n\t"
        "mov   r1, lr\n\t"
        "lsrs  r1, r1, #1\n\t"
        "lsls  r1, r1, #1\n\t"
        "ldrb  r1, [r1, r0]\n\t"
        "lsls  r1, r1, #1\n\t"
        "add   lr, r1\n\t"
        "pop   {r1}\n\t"
        "bx    lr\n\t");
}

/* ---- minimal libc string helpers (for drivers/sdio FAT layer) -----------
 * The FAT32 driver (microfat.c) needs memcpy/memset/strlen/strcmp/memcmp
 * and the kernel is -nostdlib. Implementations are the classic byte-loop
 * forms (compiled -Os they are fine on ARMv5TE Thumb-1). memcpy/memset
 * also serve the SDIO DMA path and future Rockbox code. */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) *d++ = (uint8_t)c;
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++, y++;
    }
    return 0;
}
