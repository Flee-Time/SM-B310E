/* B310E OS — the C runtime of a codec module: what the Rockbox codec and
 * metadata sources call that Rockbox's codeclib.c (built into every
 * module: memcpy, memset, memmove, memcmp, memchr, strcpy, strcat,
 * strcmp, qsort and codec_malloc) does not provide. Files go to the
 * engine through codec_api; text is ISO-8859-1 or UTF-16 to UTF-8.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include "codecs.h"
#include "rbunicode.h"
#include "errno.h"
#include <ctype.h>

int errno;
volatile long current_tick;

/* ---- files: the track, fd 0 ------------------------------------------- */
ssize_t read(int fd, void *buf, size_t n)
{
    return ci->fd_read(fd, buf, n);
}

off_t lseek(int fd, off_t offset, int whence)
{
    return ci->fd_lseek(fd, offset, whence);
}

off_t filesize(int fd)
{
    return ci->fd_size(fd);
}

int open(const char *path, int flags, ...)
{
    return -1;                          /* cue sheets and such: not here */
}

int close(int fd)
{
    return 0;
}

/* ---- log ------------------------------------------------------------------ */
void panicf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;

    va_start(ap, fmt);
    ci->vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ci->klog("codec: panic: %s", buf);
}

void debugf(const char *fmt, ...)
{
}

/* ---- strings ---------------------------------------------------------- */
size_t strlen(const char *s)
{
    return ci->strlen(s);
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);

        if (x != y || !x)
            return x - y;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);

        if (x != y || !x)
            return x - y;
    }
    return 0;
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
    const char *r = NULL;

    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;

    for (; n && *src; n--)
        *d++ = *src++;
    while (n--)
        *d++ = 0;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);

    if (size) {
        size_t k = n < size - 1 ? n : size - 1;

        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t d = 0;

    while (d < size && dst[d])
        d++;
    if (d == size)
        return d + strlen(src);
    return d + strlcpy(dst + d, src, size - d);
}

char *strmemccpy(char *dst, const char *src, size_t size)
{
    size_t n = strlcpy(dst, src, size);

    return n < size ? dst + n + 1 : NULL;
}

char *strstr(const char *h, const char *n)
{
    size_t k = strlen(n);

    for (; *h; h++)
        if (!strncmp(h, n, k))
            return (char *)h;
    return k ? NULL : (char *)h;
}

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0;

    while (isspace((unsigned char)*s))
        s++;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 0x20) == 'x') {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = *s == '0' ? 8 : 10;
    }
    for (;; s++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' :
                isalpha((unsigned char)*s) ? (*s | 0x20) - 'a' + 10 : 99;

        if (d >= base)
            break;
        v = v * base + d;
    }
    if (end)
        *end = (char *)s;
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol(s, end, base);
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

int abs(int v)
{
    return v < 0 ? -v : v;
}

long labs(long v)
{
    return v < 0 ? -v : v;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return ci->vsnprintf(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = ci->vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- text: UTF-8 out ---------------------------------------------------- */
unsigned char *utf8encode(unsigned long ucs, unsigned char *utf8)
{
    if (ucs < 0x80) {
        *utf8++ = (unsigned char)ucs;
    } else if (ucs < 0x800) {
        *utf8++ = (unsigned char)(0xc0 | ucs >> 6);
        *utf8++ = (unsigned char)(0x80 | (ucs & 0x3f));
    } else if (ucs < 0x10000) {
        *utf8++ = (unsigned char)(0xe0 | ucs >> 12);
        *utf8++ = (unsigned char)(0x80 | ((ucs >> 6) & 0x3f));
        *utf8++ = (unsigned char)(0x80 | (ucs & 0x3f));
    } else {
        *utf8++ = (unsigned char)(0xf0 | ucs >> 18);
        *utf8++ = (unsigned char)(0x80 | ((ucs >> 12) & 0x3f));
        *utf8++ = (unsigned char)(0x80 | ((ucs >> 6) & 0x3f));
        *utf8++ = (unsigned char)(0x80 | (ucs & 0x3f));
    }
    return utf8;
}

unsigned char *iso_decode_ex(const unsigned char *iso, unsigned char *utf8, int cp,
                             int count, int utf8_size)
{
    while (count-- > 0) {
        if (utf8_size >= 0 && utf8_size < 3)
            break;
        unsigned char *e = utf8encode(*iso++, utf8);

        if (utf8_size >= 0)
            utf8_size -= (int)(e - utf8);
        utf8 = e;
    }
    return utf8;
}

unsigned char *iso_decode(const unsigned char *latin1, unsigned char *utf8, int cp, int count)
{
    return iso_decode_ex(latin1, utf8, cp, count, -1);
}

unsigned char *utf16decode(const unsigned char *utf16, unsigned char *utf8, int count,
                           int utf8_size, bool le)
{
    while (count > 0) {
        unsigned long ucs = le ? (utf16[1] << 8 | utf16[0]) : (utf16[0] << 8 | utf16[1]);

        utf16 += 2;
        count--;
        if (ucs >= 0xd800 && ucs < 0xdc00 && count > 0) {
            unsigned long lo = le ? (utf16[1] << 8 | utf16[0]) : (utf16[0] << 8 | utf16[1]);

            utf16 += 2;
            count--;
            ucs = 0x10000 + ((ucs - 0xd800) << 10) + (lo - 0xdc00);
        }
        if (utf8_size >= 0 && utf8_size < 5)
            break;
        unsigned char *e = utf8encode(ucs, utf8);

        if (utf8_size >= 0)
            utf8_size -= (int)(e - utf8);
        utf8 = e;
    }
    return utf8;
}

unsigned char *utf16LEdecode(const unsigned char *utf16, unsigned char *utf8, int count)
{
    return utf16decode(utf16, utf8, count, -1, true);
}

unsigned char *utf16BEdecode(const unsigned char *utf16, unsigned char *utf8, int count)
{
    return utf16decode(utf16, utf8, count, -1, false);
}

bool utf16_has_bom(const unsigned char *utf16, bool *le)
{
    if (utf16[0] == 0xff && utf16[1] == 0xfe) {
        *le = true;
        return true;
    }
    if (utf16[0] == 0xfe && utf16[1] == 0xff) {
        *le = false;
        return true;
    }
    return false;
}

unsigned long utf8length(const unsigned char *utf8)
{
    unsigned long n = 0;

    for (; *utf8; utf8++)
        if ((*utf8 & MASK) != COMP)
            n++;
    return n;
}

const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs)
{
    unsigned c = *utf8++;
    int tail = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    unsigned long v = tail ? c & (0x3f >> tail) : c;

    while (tail-- && (*utf8 & MASK) == COMP)
        v = v << 6 | (*utf8++ & 0x3f);
    *ucs = v;
    return utf8;
}

int get_codepage(void)
{
    return ISO_8859_1;
}

/* ---- apps/misc.c and replaygain.c ------------------------------------- */
int string_option(const char *option, const char *const oplist[], bool ignore_case)
{
    for (int i = 0; oplist[i]; i++)
        if (!(ignore_case ? strcasecmp(oplist[i], option) : strcmp(oplist[i], option)))
            return i;
    return -1;
}

/* No replay gain: nothing here changes the level (no DSP stages). */
void parse_replaygain(const char *key, const char *value, struct mp3entry *entry)
{
}

void parse_replaygain_int(bool album, long gain, long peak, struct mp3entry *entry)
{
}
