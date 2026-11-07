/* B310E boot loader — the few C library functions it and FatFs use.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;

    if (!(((uintptr_t)dp | (uintptr_t)sp | n) & 3)) {
        uint32_t *dw = d;
        const uint32_t *sw = s;

        for (n >>= 2; n; n--)
            *dw++ = *sw++;
        return d;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;

    if (!(((uintptr_t)p | n) & 3)) {
        uint32_t v = (uint8_t)c * 0x01010101u, *w = d;

        for (n >>= 2; n; n--)
            *w++ = v;
        return d;
    }
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;

    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
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

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int strcasecmp_(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);

        if (x != y || !x)
            return x - y;
    }
}

void str_copy(char *d, const char *s, size_t n)
{
    if (!n)
        return;
    while (--n && *s)
        *d++ = *s++;
    *d = 0;
}
