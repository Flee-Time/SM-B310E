/* B310E OS — the playback engine's parts (os/media/).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef MEDIA_INTERNAL_H
#define MEDIA_INTERNAL_H

#include "os/codec.h"

typedef struct {
    const codec_module_t *mod;
    uint8_t *image;
    uint32_t size;                      /* image + bss */
    char name[16];
} codec_t;

typedef struct {
    char name[16];                      /* the file's name without .cdc */
    char exts[48];                      /* its codec_module_t.extensions */
} codec_entry_t;

/* media/module.c */
const char *codec_load(const char *name, codec_t **out);   /* NULL: loaded */
void codec_unload(codec_t *c);
int codec_scan(codec_entry_t *list, int max);

#endif
