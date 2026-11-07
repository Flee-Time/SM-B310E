/* B310E OS — codec module loader (os/codec.h): read a .cdc into the heap,
 * relocate it, and list what the card's codecs play.
 *
 * The file has the .app layout (os/app.h, os/loader/loader.c) with
 * CODEC_MAGIC; the checks are the app loader's: header, sizes, every
 * relocation inside the image, the CRC over image and relocations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/arch.h"
#include "os/codec.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "media/media_internal.h"

#define RELOC_CHUNK 64

static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    static const uint32_t t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *p = data;

    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return crc;
}

static const char *check_header(const app_file_t *h, uint32_t file_size)
{
    if (h->magic != CODEC_MAGIC || h->header_size != sizeof(*h))
        return "not a codec";
    if (h->api_major != CODEC_ABI_MAJOR || h->api_minor > CODEC_ABI_MINOR)
        return "built for another codec ABI";
    if (h->image_size < sizeof(codec_module_t) || h->image_size > CODEC_MAX_SIZE ||
        h->bss_size > CODEC_MAX_SIZE - h->image_size)
        return "bad size";
    if (file_size != sizeof(*h) + h->image_size + h->reloc_count * 4u)
        return "file truncated";
    return NULL;
}

void codec_unload(codec_t *c)
{
    if (!c)
        return;
    kfree(c->image);
    kfree(c);
}

const char *codec_load(const char *name, codec_t **out)
{
    char path[48];
    app_file_t h;
    FIL f;
    UINT n = 0;
    codec_t *c;
    const char *err = NULL;
    uint32_t reloc[RELOC_CHUNK], crc = 0xffffffffu, left, t = kernel_ticks();

    *out = NULL;
    snprintf(path, sizeof(path), CODEC_DIR "/%s.cdc", name);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return "no codec";
    if (f_read(&f, &h, sizeof(h), &n) != FR_OK || n != sizeof(h)) {
        f_close(&f);
        return "not a codec";
    }
    if ((err = check_header(&h, (uint32_t)f_size(&f)))) {
        f_close(&f);
        return err;
    }
    c = kzalloc(sizeof(*c));
    if (c) {
        c->size = h.image_size + h.bss_size;
        c->image = kmalloc(c->size);
    }
    if (!c || !c->image) {
        f_close(&f);
        codec_unload(c);
        return "not enough memory";
    }
    if (f_read(&f, c->image, h.image_size, &n) != FR_OK || n != h.image_size)
        err = "read error";
    if (!err)
        crc = crc32_update(crc, c->image, h.image_size);
    for (left = h.reloc_count; !err && left; ) {
        uint32_t k = left > RELOC_CHUNK ? RELOC_CHUNK : left;

        if (f_read(&f, reloc, k * 4, &n) != FR_OK || n != k * 4) {
            err = "read error";
            break;
        }
        crc = crc32_update(crc, reloc, k * 4);
        for (uint32_t i = 0; i < k && !err; i++) {
            uint32_t off = reloc[i], *w;

            if (off & 3 || off > h.image_size - 4) {
                err = "bad relocation";
                break;
            }
            w = (uint32_t *)(c->image + off);
            if (*w > c->size)
                err = "bad relocation";
            else
                *w += (uint32_t)c->image;
        }
        left -= k;
    }
    f_close(&f);
    if (!err && ~crc != h.crc32)
        err = "damaged (CRC)";
    if (!err) {
        memset(c->image + h.image_size, 0, h.bss_size);
        c->mod = (const codec_module_t *)c->image;
        if (c->mod->abi_major != CODEC_ABI_MAJOR || !c->mod->probe || !c->mod->run ||
            !c->mod->entry)
            err = "bad codec info";
    }
    if (err) {
        codec_unload(c);
        klog("media: codec %s: %s", path, err);
        return err;
    }
    dcache_clean_range(c->image, c->size);
    icache_invalidate_all();
    snprintf(c->name, sizeof(c->name), "%s", name);
    klog("media: codec %s loaded, %lu + %lu bytes, %lu relocations, %lu ms", name,
         (unsigned long)h.image_size, (unsigned long)h.bss_size,
         (unsigned long)h.reloc_count, (unsigned long)(kernel_ticks() - t));
    *out = c;
    return NULL;
}

/* ---- what the card's codecs play ------------------------------------------ */

/* The extension list of a module, read from its file without loading it:
 * the codec_module_t at the image's start holds image offsets (before
 * relocation) of its strings. */
static bool read_exts(const char *path, char *exts, size_t n)
{
    app_file_t h;
    codec_module_t m;
    FIL f;
    UINT got = 0;
    bool ok = false;

    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    if (f_read(&f, &h, sizeof(h), &got) == FR_OK && got == sizeof(h) &&
        !check_header(&h, (uint32_t)f_size(&f)) &&
        f_read(&f, &m, sizeof(m), &got) == FR_OK && got == sizeof(m)) {
        uint32_t off = (uint32_t)m.extensions;

        if (off < h.image_size && f_lseek(&f, sizeof(h) + off) == FR_OK &&
            f_read(&f, exts, n - 1, &got) == FR_OK) {
            exts[got] = 0;
            ok = true;
        }
    }
    f_close(&f);
    return ok;
}

int codec_scan(codec_entry_t *list, int max)
{
    DIR d;
    FILINFO fi;
    int count = 0;

    if (f_opendir(&d, CODEC_DIR) != FR_OK)
        return 0;
    while (count < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t len = strlen(fi.fname);
        char path[64], low[20];
        codec_entry_t *e = &list[count];

        if (fi.fattrib & AM_DIR || len < 5 || len >= sizeof(low))
            continue;
        for (size_t i = 0; i <= len; i++)
            low[i] = fi.fname[i] >= 'A' && fi.fname[i] <= 'Z' ?
                     (char)(fi.fname[i] + 'a' - 'A') : fi.fname[i];
        if (strcmp(low + len - 4, ".cdc"))
            continue;
        memcpy(e->name, low, len - 4);
        e->name[len - 4] = 0;
        snprintf(path, sizeof(path), CODEC_DIR "/%s", fi.fname);
        if (read_exts(path, e->exts, sizeof(e->exts)))
            count++;
    }
    f_closedir(&d);
    return count;
}
