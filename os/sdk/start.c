/* B310E OS app SDK: the API pointer and the memory functions the compiler
 * calls on its own (struct copies, zeroing), linked into every app.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <stdint.h>

struct os_api;
const struct os_api *os;

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
        for (; n >= 4; n -= 4, d += 4, s += 4)
            *(uint32_t *)d = *(const uint32_t *)s;
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
    while (n--)
        d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}
