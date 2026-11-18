/* B310E OS — vsnprintf: %d %i %u %x %X %o %p %s %c %%, flags '-' '0' '+',
 * width, precision for strings, 'l', 'll' and 'z' length modifiers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include "os/lib.h"

typedef struct {
    char *buf;
    size_t size, len;
} out_t;

static void emit(out_t *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void pad(out_t *o, char c, int n)
{
    while (n-- > 0)
        emit(o, c);
}

static void number(out_t *o, unsigned long long v, unsigned base, bool upper,
                   char sign, int width, bool left, bool zero)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, len;

    do {                                /* 32-bit steps once it fits: no libgcc call */
        unsigned d;

        if (v >> 32) {
            d = (unsigned)(v % base);
            v /= base;
        } else {
            uint32_t w = (uint32_t)v;

            d = w % base;
            v = w / base;
        }
        tmp[n++] = digits[d];
    } while (v);
    len = n + (sign ? 1 : 0);
    if (!left && !zero)
        pad(o, ' ', width - len);
    if (sign)
        emit(o, sign);
    if (!left && zero)
        pad(o, '0', width - len);
    while (n)
        emit(o, tmp[--n]);
    if (left)
        pad(o, ' ', width - len);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        bool left = false, zero = false, plus = false;
        int width = 0, prec = -1, lng = 0;  /* lng: 1 'l' or 'z', 2 'll' */

        if (*fmt != '%') {
            emit(&o, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            prec = 0;
            fmt++;
            if (*fmt == '*') {          /* "%.*s": the precision from the args */
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') {
            if (*fmt++ == 'l')
                lng++;
            else if (!lng)
                lng = 1;                /* 'z': size_t is 32 bits */
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = lng > 1 ? va_arg(ap, long long) :
                          lng ? va_arg(ap, long) : va_arg(ap, int);
            number(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10,
                   false, v < 0 ? '-' : plus ? '+' : 0, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v = lng > 1 ? va_arg(ap, unsigned long long) :
                                   lng ? va_arg(ap, unsigned long)
                                       : va_arg(ap, unsigned int);
            unsigned base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
            number(&o, v, base, *fmt == 'X', 0, width, left, zero);
            break;
        }
        case 'p':
            emit(&o, '0');
            emit(&o, 'x');
            number(&o, (uintptr_t)va_arg(ap, void *), 16, false, 0,
                   8, false, true);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len;

            if (!s)
                s = "(null)";
            /* no further than the precision: the buffer may not end in a 0 */
            for (len = 0; (prec < 0 || len < prec) && s[len]; len++)
                ;
            if (prec >= 0 && len == prec)  /* and not inside a UTF-8 character */
                len = (int)utf8_cut(s, (size_t)len);
            if (!left)
                pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++)
                emit(&o, s[i]);
            if (left)
                pad(&o, ' ', width - len);
            break;
        }
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case '%':
            emit(&o, '%');
            break;
        case 0:
            fmt--;
            break;
        default:
            emit(&o, '%');
            emit(&o, *fmt);
            break;
        }
    }
    if (size && o.len < size)
        buf[o.len] = 0;
    else if (size)                      /* cut: on a character */
        buf[utf8_cut(buf, size - 1)] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
