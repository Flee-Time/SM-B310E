/* B310E OS — UTF-8 one character at a time: for drawing, the text editor and
 * cutting text to a buffer on a whole character (the store's check is
 * utf8_valid in fs/db.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include "os/lib.h"

int utf8_len(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    int more = p[0] < 0xc2 ? 0 : p[0] < 0xe0 ? 1 : p[0] < 0xf0 ? 2 : p[0] < 0xf5 ? 3 : 0;
    int i = 1;

    while (i <= more && (p[i] & 0xc0) == 0x80)
        i++;
    return i == more + 1 ? i : 1;       /* broken: one byte */
}

uint32_t utf8_get(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    int n = utf8_len(*s);
    uint32_t c;

    *s += n;
    if (n == 1)
        return p[0] < 0x80 ? p[0] : 0xfffd;
    c = p[0] & (0x7f >> n);
    for (int i = 1; i < n; i++)
        c = c << 6 | (p[i] & 0x3f);
    /* overlong 3- and 4-byte forms, encoded surrogates and past U+10FFFF:
     * one U+FFFD for the sequence, as utf8_valid (fs/db.c) refuses them
     * (E0 80 80 was U+0000, F4 90 80 80 U+110000). Two bytes cannot be
     * overlong: utf8_len takes no C0/C1 lead. */
    if ((n == 3 && (c < 0x800 || (c >= 0xd800 && c < 0xe000))) ||
        (n == 4 && (c < 0x10000 || c > 0x10ffff)))
        return 0xfffd;
    return c;
}

int utf8_put(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | (c >> 6 & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | (c >> 12 & 0x3f));
    out[2] = (char)(0x80 | (c >> 6 & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

size_t utf8_cut(const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t lead = n;

    while (lead > 0 && (p[lead - 1] & 0xc0) == 0x80 && n - lead < 3)
        lead--;
    if (lead == 0 || p[lead - 1] < 0xc0)
        return n;                       /* ASCII last, or no sequence to cut */
    lead--;
    return n - lead >= (size_t)(p[lead] >= 0xf0 ? 4 : p[lead] >= 0xe0 ? 3 : 2) ? n : lead;
}

/* A byte limit can end a buffer inside a character, which draws as '?': the
 * broken last one goes. snprintf does it by itself (lib/printf.c); this is
 * for text cut another way (strncpy, memcpy). */
size_t utf8_trim(char *s)
{
    size_t n = utf8_cut(s, strlen(s));

    s[n] = 0;
    return n;
}
