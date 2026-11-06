/* B310E OS — freestanding C library subset.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_LIB_H
#define OS_LIB_H

#include <stdarg.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strncpy(char *dst, const char *src, size_t n);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
