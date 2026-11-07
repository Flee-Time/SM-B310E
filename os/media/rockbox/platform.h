/* B310E OS — Rockbox's rbcodec platform.h for the codec modules: the C
 * library and file calls the codec and metadata sources use, provided by
 * the module runtime (os/media/codecs/rt.c, mostly through codec_api).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLATFORM_H_INCLUDED
#define PLATFORM_H_INCLUDED

#include "config.h"
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* files: only the track (fd 0), read-only */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
ssize_t read(int fd, void *buf, size_t n);
off_t lseek(int fd, off_t offset, int whence);
off_t filesize(int fd);
#define ffilesize(fd) filesize(fd)
int open(const char *path, int flags, ...);
int close(int fd);
#ifndef O_RDONLY
#define O_RDONLY 0
#endif

size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char *strmemccpy(char *dst, const char *src, size_t size);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#include "logf.h"
void panicf(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
void debugf(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);
#ifndef DEBUGF
#define DEBUGF(...) do { } while (0)
#endif

/* time in Rockbox ticks (10 ms) */
extern volatile long current_tick;

#endif
