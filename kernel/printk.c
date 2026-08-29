/*
 * B310E-OS — kernel/printk.c
 *
 * Minimal freestanding printf for the kernel. Original implementation,
 * no libc dependencies beyond the freestanding headers <stdarg.h> and
 * <stdint.h> (both supplied by the compiler in -ffreestanding mode).
 *
 * Supported conversions: %c %s %d %u %x %X %p %%
 * Optional flags: '-', '0'; optional decimal width. Length modifiers are
 * accepted and ignored (integer args are 32-bit on both the ARM926 and
 * the x86-64 host for the types we use). No floats, no precision, no '#',
 * no '+'/space flags.
 *
 * The formatter emits through a putc callback, so the same core drives
 * both vsnprintf() (buffer sink) and kprintf() (kputc sink).
 */

#include "printk.h"

#include <stdint.h>

/* ---- sink abstraction -------------------------------------------------- */

typedef void (*putc_fn)(void *opaque, char c);

/* ---- small helpers ----------------------------------------------------- */

static unsigned kstrlen(const char *s)
{
    unsigned n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static unsigned num_digits(uint32_t v, unsigned base)
{
    unsigned n = 1;
    while (v >= base) {
        v /= base;
        n++;
    }
    return n;
}

/* Emit v in [base] (2..16) without padding; returns the digit count. */
static unsigned emit_num(putc_fn put, void *op, uint32_t v, unsigned base,
                         int upper)
{
    char tmp[33];
    unsigned n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0 && n < sizeof(tmp));

    {
        unsigned cnt = n;
        while (n > 0)
            put(op, tmp[--n]);
        return cnt;
    }
}

/* ---- core formatter ---------------------------------------------------- */

static unsigned vformat(putc_fn put, void *op, const char *fmt, va_list ap)
{
    unsigned n = 0;

    for (;;) {
        char c = *fmt++;
        if (c == '\0')
            return n;
        if (c != '%') {
            put(op, c);
            n++;
            continue;
        }

        /* parse flags: '-', '0' */
        int zero_pad = 0;
        int left = 0;
        for (;;) {
            c = *fmt++;
            if (c == '0') {
                zero_pad = 1;
                continue;
            }
            if (c == '-') {
                left = 1;
                continue;
            }
            break;
        }

        /* parse decimal width */
        unsigned width = 0;
        while (c >= '0' && c <= '9') {
            width = width * 10u + (unsigned)(c - '0');
            c = *fmt++;
        }

        /* length modifiers — accepted, ignored (32-bit ints everywhere) */
        while (c == 'l' || c == 'h' || c == 'j' || c == 'z' || c == 't' ||
               c == 'L')
            c = *fmt++;

        if (c == '\0') {          /* format ended with a lone '%' */
            put(op, '%');
            n++;
            return n;
        }

        int32_t iv;
        uint32_t uv;
        const char *s;
        unsigned core;
        unsigned pad;

        switch (c) {
        case '%':
            put(op, '%');
            n++;
            break;

        case 'c':
            put(op, (char)va_arg(ap, int));
            n++;
            break;

        case 's':
            s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            core = kstrlen(s);
            pad = width > core ? width - core : 0;
            if (!left)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            while (*s != '\0') {
                put(op, *s++);
                n++;
            }
            if (left)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            break;

        case 'd':
        case 'i': {
            int neg;
            iv = va_arg(ap, int);
            neg = (iv < 0);
            uv = neg ? (uint32_t)(-(int64_t)iv) : (uint32_t)iv;
            core = num_digits(uv, 10);
            pad = width > core ? width - core : 0;
            if (!left && !zero_pad)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            if (neg) {
                put(op, '-');
                n++;
            }
            if (zero_pad)
                while (pad--) {
                    put(op, '0');
                    n++;
                }
            n += emit_num(put, op, uv, 10, 0);
            if (left)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            break;
        }

        case 'u':
            uv = va_arg(ap, unsigned int);
            core = num_digits(uv, 10);
            pad = width > core ? width - core : 0;
            if (!left && !zero_pad)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            if (zero_pad)
                while (pad--) {
                    put(op, '0');
                    n++;
                }
            n += emit_num(put, op, uv, 10, 0);
            if (left)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            break;

        case 'x':
        case 'X':
            uv = va_arg(ap, unsigned int);
            core = num_digits(uv, 16);
            pad = width > core ? width - core : 0;
            if (!left && !zero_pad)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            if (zero_pad)
                while (pad--) {
                    put(op, '0');
                    n++;
                }
            n += emit_num(put, op, uv, 16, (c == 'X'));
            if (left)
                while (pad--) {
                    put(op, ' ');
                    n++;
                }
            break;

        case 'p':
            uv = (uint32_t)(uintptr_t)va_arg(ap, void *);
            put(op, '0');
            put(op, 'x');
            n += 2;
            n += emit_num(put, op, uv, 16, 0);
            break;

        default:
            put(op, '%');
            put(op, c);
            n += 2;
            break;
        }
    }
}

/* ---- vsnprintf: buffer sink -------------------------------------------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} buf_sink_t;

static void buf_putc(void *op, char c)
{
    buf_sink_t *b = (buf_sink_t *)op;
    if (b->size > 0 && b->len + 1 < b->size)
        b->buf[b->len] = c;
    b->len++;
}

int vsnprintf(char *restrict str, size_t size, const char *restrict fmt,
              va_list ap)
{
    buf_sink_t b;
    b.buf = str;
    b.size = (str == NULL) ? 0 : size;
    b.len = 0;

    (void)vformat(buf_putc, &b, fmt, ap);

    if (b.size > 0) {
        size_t end = (b.len < b.size - 1) ? b.len : b.size - 1;
        b.buf[end] = '\0';
    }
    return (int)b.len;
}

/* ---- kprintf: kputc sink ----------------------------------------------- */

#define KPUTC_RING_SIZE 256u

static volatile char s_kputc_ring[KPUTC_RING_SIZE];
static volatile uint32_t s_kputc_wr;

/* ---- on-device console ring (LCD scrolling console, demo task reads it) --
 * Every kputc backend (the weak default below AND usb_debug.c's strong
 * override) feeds this ring, so the demo console task can render all
 * kprintf output on the LCD — including during a USB replug, when the
 * libc_server channel is dead. Ring 2 KiB, producer overwrites the oldest
 * unread byte when full (never blocks); the display drains it regularly. */
#define CONSOLE_RING_SIZE 2048u
static char s_console_ring[CONSOLE_RING_SIZE];
static uint32_t s_console_wr;
static uint32_t s_console_rd;

void console_putc(char c)
{
    s_console_ring[s_console_wr] = c;
    s_console_wr = (s_console_wr + 1u) % CONSOLE_RING_SIZE;
    if (s_console_wr == s_console_rd)   /* full: drop the oldest unread */
        s_console_rd = (s_console_rd + 1u) % CONSOLE_RING_SIZE;
}

/* Consume one char from the console ring; -1 when empty. */
int console_getc(void)
{
    int c;

    if (s_console_rd == s_console_wr)
        return -1;
    c = s_console_ring[s_console_rd];
    s_console_rd = (s_console_rd + 1u) % CONSOLE_RING_SIZE;
    return c;
}

/* Default backend: 256-byte write-only ring (read by a later wave's debug
 * channel). Override with a strong `void kputc(char c)` somewhere else. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void kputc(char c)
{
    s_kputc_ring[s_kputc_wr++ & (KPUTC_RING_SIZE - 1u)] = c;
    console_putc(c);
}

static void kputc_sink(void *op, char c)
{
    (void)op;
    kputc(c);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vformat(kputc_sink, NULL, fmt, ap);
    va_end(ap);
}
