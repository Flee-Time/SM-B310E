/* B310E OS — freestanding C library subset.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_LIB_H
#define OS_LIB_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* UTF-8 (lib/utf8.c). A broken sequence counts as one byte, read as U+FFFD. */
int utf8_len(const char *s);            /* bytes of the character at s */
uint32_t utf8_get(const char **s);      /* its code point; *s moves past it */
int utf8_put(uint32_t c, char *out);    /* 1-4 bytes written, no NUL */
size_t utf8_cut(const char *s, size_t n);   /* the most of s's n bytes ending on a character */
size_t utf8_trim(char *s);              /* drop a cut last character; the new length */

/* CRC-32, IEEE (zlib's crc32) (lib/crc32.c). crc32_update is the bare
 * register: start at 0xffffffff, ~ the result. crc32_part: the CRC of the n
 * bytes between two of its running values. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t n);
uint32_t crc32_part(uint32_t before, uint32_t after, uint32_t n);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
