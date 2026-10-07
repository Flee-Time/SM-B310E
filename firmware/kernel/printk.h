/*
 * B310E-OS — kernel/printk.h
 *
 * Minimal freestanding formatted output. Supports %s %c %d %u %x %X %p and
 * %% (plus optional '-', '0' and width). No floats, no precision.
 *
 * The character backend is kputc(): a weak symbol whose default (this
 * translation unit) writes into a small ring buffer. Later waves (LCD/USB)
 * or host tests override it with a strong definition — the strong symbol
 * wins at link time. Host tests map kputc to stdout / a capture buffer.
 */

#ifndef B310E_OS_PRINTK_H
#define B310E_OS_PRINTK_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard-ish vsnprintf: writes at most size-1 chars + NUL, returns the
 * length the string WOULD have had (excluding the NUL). */
int vsnprintf(char *restrict str, size_t size, const char *restrict fmt,
              va_list ap);

void kprintf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Character output backend. Weak: override with a strong symbol in the
 * drivers (LCD/USB) or in host tests. */
void kputc(char c);

/* On-device console ring: every kputc backend feeds it; the demo task
 * drains it for the LCD scrolling console (visible even when USB is dead,
 * e.g. during a replug). */
void console_putc(char c);
int console_getc(void);   /* -1 when empty */

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_PRINTK_H */
