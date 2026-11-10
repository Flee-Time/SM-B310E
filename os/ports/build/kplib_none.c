/* Duke Nukem 3D on B310E OS: the engine's kplib is its PNG, JPEG and ZIP decoding, for
 * high-resolution art and mods in ZIPs, which a phone at 160 x 128 does not need: over
 * 100 KB of code and tables. These are what the rest of the engine calls of it.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include "kplib.h"

int kzaddstack(const char *name) { (void)name; return -1; }
void kzuninit(void) {}
intptr_t kzopen(const char *name) { (void)name; return 0; }
int kzread(void *buf, int len) { (void)buf; (void)len; return 0; }
int kzfilelength(void) { return 0; }
int kzseek(int offset, int whence) { (void)offset; (void)whence; return -1; }
int kztell(void) { return -1; }
int kzgetc(void) { return -1; }
int kzeof(void) { return 1; }
void kzclose(void) {}
void kzfindfilestart(const char *name) { (void)name; }
int kzfindfile(char *name) { (void)name; return 0; }
