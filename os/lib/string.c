/* B310E OS — string and memory functions (the compiler also emits calls
 * to memcpy and memset on its own).
 *
 * Built ARM -O2 (os/Makefile): memcpy was 7% of a list-scroll redraw
 * (lcd_update's band copy) and memset 2.4% of the MP3 decoder in Thumb
 * -Os (prof, 2026-11-17). ARM mode gives the word loops ldm/stm, constant
 * shifts folded into orr and no Thumb low-register squeeze. The loops are
 * plain C: the global -fno-tree-loop-distribute-patterns keeps GCC from
 * turning them back into calls to memcpy/memset.
 *
 * Copies load a whole block before storing it, in ascending order, so
 * memmove can use memcpy whenever dst is below src.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include "os/lib.h"

/* a word that may hold any type's bytes (these copy every type) */
typedef uint32_t __attribute__((may_alias)) word_t;

/* d word-aligned, s k bytes past a word boundary (k 1..3, little endian):
 * each output word is the top of one aligned source word and the bottom of
 * the next. The aligned reads stay inside words that hold a byte of the
 * source, so they never touch another page. 4 words a loop, then 1. */
#define SHIFTED(k)                                                        \
    do {                                                                  \
        const word_t *sw = (const word_t *)(s - (k));                     \
        word_t *dw = (word_t *)d;                                         \
        uint32_t w = *sw++;                                               \
                                                                          \
        for (; n >= 16; n -= 16, sw += 4, dw += 4) {                      \
            uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];          \
                                                                          \
            dw[0] = w >> (8 * (k)) | a << (32 - 8 * (k));                 \
            dw[1] = a >> (8 * (k)) | b << (32 - 8 * (k));                 \
            dw[2] = b >> (8 * (k)) | c << (32 - 8 * (k));                 \
            dw[3] = c >> (8 * (k)) | e << (32 - 8 * (k));                 \
            w = e;                                                        \
        }                                                                 \
        for (; n >= 4; n -= 4) {                                          \
            uint32_t a = *sw++;                                           \
                                                                          \
            *dw++ = w >> (8 * (k)) | a << (32 - 8 * (k));                 \
            w = a;                                                        \
        }                                                                 \
        d = (uint8_t *)dw;                                                \
        s = (const uint8_t *)sw - 4 + (k);                                \
    } while (0)

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (n >= 8) {
        /* dst to a word boundary (at most 3 bytes: n stays >= 5) */
        while ((uintptr_t)d & 3) {
            *d++ = *s++;
            n--;
        }
        switch ((uintptr_t)s & 3) {
        case 0: {                       /* both aligned: 8 words a loop */
            word_t *dw = (word_t *)d;
            const word_t *sw = (const word_t *)s;

            for (; n >= 32; n -= 32, sw += 8, dw += 8) {
                uint32_t a0 = sw[0], a1 = sw[1], a2 = sw[2], a3 = sw[3];
                uint32_t a4 = sw[4], a5 = sw[5], a6 = sw[6], a7 = sw[7];

                dw[0] = a0; dw[1] = a1; dw[2] = a2; dw[3] = a3;
                dw[4] = a4; dw[5] = a5; dw[6] = a6; dw[7] = a7;
            }
            for (; n >= 4; n -= 4)
                *dw++ = *sw++;
            d = (uint8_t *)dw;
            s = (const uint8_t *)sw;
            break;
        }
        case 1: SHIFTED(1); break;
        case 2: SHIFTED(2); break;
        default: SHIFTED(3); break;
        }
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    /* backwards, dst above src: a block is loaded before it is stored, and
     * the store only reaches source bytes already read */
    d += n;
    s += n;
    if (n >= 8 && (((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
        word_t *dw;
        const word_t *sw;

        while ((uintptr_t)d & 3) {
            *--d = *--s;
            n--;
        }
        dw = (word_t *)d;
        sw = (const word_t *)s;
        for (; n >= 32; n -= 32) {
            uint32_t a0, a1, a2, a3, a4, a5, a6, a7;

            sw -= 8;
            dw -= 8;
            a0 = sw[0]; a1 = sw[1]; a2 = sw[2]; a3 = sw[3];
            a4 = sw[4]; a5 = sw[5]; a6 = sw[6]; a7 = sw[7];
            dw[0] = a0; dw[1] = a1; dw[2] = a2; dw[3] = a3;
            dw[4] = a4; dw[5] = a5; dw[6] = a6; dw[7] = a7;
        }
        for (; n >= 4; n -= 4)
            *--dw = *--sw;
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--)
        *--d = *--s;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint32_t w = (uint8_t)c * 0x01010101u;

    if (n >= 8) {
        word_t *dw;

        while ((uintptr_t)d & 3) {
            *d++ = (uint8_t)c;
            n--;
        }
        dw = (word_t *)d;
        for (; n >= 32; n -= 32, dw += 8) {     /* 8 words a loop */
            dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w;
            dw[4] = w; dw[5] = w; dw[6] = w; dw[7] = w;
        }
        for (; n >= 4; n -= 4)
            *dw++ = w;
        d = (uint8_t *)dw;
    }
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

/* Words while they match (lcd_update compares 256-byte rows with it), then
 * bytes from the first word that differs: the same result as a byte loop. */
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;

    if (n >= 8 && (((uintptr_t)x ^ (uintptr_t)y) & 3) == 0) {
        const word_t *p, *q;

        while ((uintptr_t)x & 3) {
            if (*x != *y)
                return *x - *y;
            x++, y++, n--;
        }
        p = (const word_t *)x;
        q = (const word_t *)y;
        /* 16 bytes a step to an end pointer. GCC loads the eight words
         * with one ldm and six ldr and counts n as well: 20 instructions a
         * step (lcd_update's 256-byte rows: 320). In ARM the loop by hand:
         * two ldm, four eor, three orr, the test and the end, 12. */
        {
            const word_t *e = p + (n >> 4) * 4;

#if defined(__arm__) && !defined(__thumb__)
            /* the ldm lists in ascending registers: r4-r7 and r8-r10, ip */
            register uint32_t a0 __asm__("r4"), a1 __asm__("r5"), a2 __asm__("r6"),
                a3 __asm__("r7"), b0 __asm__("r8"), b1 __asm__("r9"), b2 __asm__("r10"),
                b3 __asm__("ip");

            __asm__ volatile("1:\n\t"
                             "cmp   %[p], %[e]\n\t"
                             "beq   2f\n\t"
                             "ldmia %[p]!, {%[a0], %[a1], %[a2], %[a3]}\n\t"
                             "ldmia %[q]!, {%[b0], %[b1], %[b2], %[b3]}\n\t"
                             "eor   %[a0], %[a0], %[b0]\n\t"
                             "eor   %[a1], %[a1], %[b1]\n\t"
                             "orr   %[a0], %[a0], %[a1]\n\t"
                             "eor   %[a2], %[a2], %[b2]\n\t"
                             "orr   %[a0], %[a0], %[a2]\n\t"
                             "eor   %[a3], %[a3], %[b3]\n\t"
                             "orrs  %[a0], %[a0], %[a3]\n\t"
                             "beq   1b\n\t"
                             "sub   %[p], %[p], #16\n\t"    /* back to the step that differs */
                             "sub   %[q], %[q], #16\n"
                             "2:"
                             : [p] "+r"(p), [q] "+r"(q), [a0] "=&r"(a0), [a1] "=&r"(a1),
                               [a2] "=&r"(a2), [a3] "=&r"(a3), [b0] "=&r"(b0),
                               [b1] "=&r"(b1), [b2] "=&r"(b2), [b3] "=&r"(b3)
                             : [e] "r"(e)
                             : "cc", "memory");
#endif
            while (p != e) {
                uint32_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint32_t b0 = q[0], b1 = q[1], b2 = q[2], b3 = q[3];

                if ((a0 ^ b0) | (a1 ^ b1) | (a2 ^ b2) | (a3 ^ b3))
                    break;
                p += 4;
                q += 4;
            }
            n -= (size_t)((const uint8_t *)p - x);
        }
        for (; n >= 4; n -= 4, p++, q++)
            if (*p != *q)
                break;
        x = (const uint8_t *)p;
        y = (const uint8_t *)q;
    }
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}
