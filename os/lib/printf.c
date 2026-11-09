/* B310E OS — vsnprintf: %d %i %u %x %X %o %p %s %c %%, flags '-' '0' '+',
 * width, precision for strings, 'l' and 'z' length modifiers.
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

static void number(out_t *o, unsigned long v, unsigned base, bool upper,
                   char sign, int width, bool left, bool zero)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, len;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
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
        bool left = false, zero = false, lng = false, plus = false;
        int width = 0, prec = -1;

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
            lng = true;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            number(&o, v < 0 ? -(unsigned long)v : (unsigned long)v, 10,
                   false, v < 0 ? '-' : plus ? '+' : 0, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long v = lng ? va_arg(ap, unsigned long)
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
            len = (int)strlen(s);
            if (prec >= 0 && len > prec)
                len = prec;
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
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
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
