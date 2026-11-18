/* Host test: the cover-art decoders (os/media/art.c, built whole in
 * test_art_glue.c, which reaches its static decode_at).
 *
 *   test_art MANIFEST COUNT SEED
 *
 * MANIFEST (made by test-host.py with PIL) lists covers, one a line:
 * "path flags pos ok": flags MEDIA_ART_UNSYNC / MEDIA_ART_BASE64 as art.c
 * takes them, pos where the picture starts in the file, ok 2 when the
 * decoders support that kind (they must decode it and draw something), 1
 * when it must decode but may be blank (all transparent), 0 when it is not
 * supported (it must only fail cleanly).
 *
 *   - every cover at 96, 48 and 32 pixels: decoded (the supported ones),
 *     some pixel not the card colour, nothing written past side * side
 *     pixels (a guarded block of exactly that size), every allocation
 *     freed, no file left open;
 *   - COUNT mutations of each (random bytes, a cut, a random run, the
 *     headers damaged; the same every run for a SEED): no UBSan trap, no
 *     access violation, no heap error, no leak; a crash saves the input
 *     (HOST_CRASH_FILE) for a rerun.
 *
 * Depends on the OS: what test_art_glue.c names (art.c's decode_at, MAX_SIDE,
 * C_CARD); the flag values MEDIA_ART_UNSYNC 1 / MEDIA_ART_BASE64 2 (os/media.h),
 * which test-host.py writes into the manifest.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/kernel.h"

bool artt_decode(const char *path, uint32_t pos, uint32_t flags, int side, uint16_t *px);
int artt_max_side(void);
uint16_t artt_card(void);

#define COVERS 128

typedef struct {
    char path[400];
    uint32_t flags, pos;
    int ok;
    uint8_t *data;
    uint32_t size;
} cover_t;

static cover_t covers[COVERS];
static int ncovers;

static bool load_manifest(const char *path)
{
    FILE *m = fopen(path, "r");
    char line[600];

    if (!m)
        return false;
    while (fgets(line, sizeof(line), m) && ncovers < COVERS) {
        cover_t *c = &covers[ncovers];
        FILE *f;
        long n;

        if (sscanf(line, "%399s %u %u %d", c->path, &c->flags, &c->pos, &c->ok) != 4)
            continue;
        if (!(f = fopen(c->path, "rb")))
            return false;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        c->data = malloc((size_t)n + 1);
        c->size = (uint32_t)fread(c->data, 1, (size_t)n, f);
        fclose(f);
        ncovers++;
    }
    fclose(m);
    return ncovers > 0;
}

/* decode bytes as a cover; the pixels into a block of exactly side * side */
static bool decode(const uint8_t *d, uint32_t n, uint32_t flags, uint32_t pos, int side,
                   bool *drawn)
{
    unsigned live = ga_live();
    uint16_t *px;
    bool ok;

    memfs_put("/music/cover.bin", d, n);
    px = kmalloc((size_t)side * side * 2
#ifdef HOST_PROVE
                 - 2                    /* one pixel short: the guards must see the decoder's last */
#endif
                 );
    ok = artt_decode("/music/cover.bin", pos, flags, side, px);
    if (drawn) {
        *drawn = false;
        for (int i = 0; i < side * side - 1; i++)
            if (px[i] != artt_card())
                *drawn = true;
    }
    kfree(px);
    CHECK(ga_live() == live, "%u block(s) not freed", ga_live() - live);
    CHECK(memfs_open_count() == 0, "%d file(s) left open", memfs_open_count());
    return ok;
}

static void mutate(uint8_t *d, uint32_t *n)
{
    uint32_t len = *n;

    switch (host_rand() % 5) {
    case 0:                             /* a few random bytes */
        for (int k = 1 + (int)(host_rand() % 20); k > 0; k--)
            d[host_rand() % len] = (uint8_t)host_rand();
        break;
    case 1:                             /* cut short, a bit flipped or two */
        len = 8 + host_rand() % (len - 7);
        for (int k = (int)(host_rand() % 4); k > 0; k--)
            d[host_rand() % len] ^= (uint8_t)(1u << (host_rand() % 8));
        break;
    case 2: {                           /* a random run */
        uint32_t at = host_rand() % len, run = 1 + host_rand() % 64;

        for (uint32_t i = at; i < at + run && i < len; i++)
            d[i] = (uint8_t)host_rand();
        break;
    }
    case 3:                             /* the headers: the first 700 bytes */
        for (int k = 1 + (int)(host_rand() % 6); k > 0; k--)
            d[host_rand() % (len < 700 ? len : 700)] = (uint8_t)host_rand();
        break;
    default:                            /* a length or marker field: 0x00 / 0xff runs */
        for (int k = 1 + (int)(host_rand() % 3); k > 0; k--) {
            uint32_t at = host_rand() % len;

            d[at] = host_rand() & 1 ? 0xff : 0x00;
            if (at + 1 < len)
                d[at + 1] = (uint8_t)host_rand();
        }
        break;
    }
    *n = len;
}

int main(int argc, char **argv)
{
    unsigned count, decoded = 0, fuzzed = 0, mutant_ok = 0;
    static uint8_t buf[4 << 20];

    host_init(argc, argv);
    if (argc < 4 || !load_manifest(argv[1])) {
        fprintf(stderr, "test_art MANIFEST COUNT SEED: no covers\n");
        return 2;
    }
    count = (unsigned)atoi(argv[2]);
    memfs_reset();
    ga_reset();
    for (int i = 0; i < ncovers; i++) {
        const int sides[] = { artt_max_side(), 48, 32 };
        cover_t *c = &covers[i];

        for (int s = 0; s < 3; s++) {
            bool drawn, ok;

            host_context("%s at %d pixels", c->path, sides[s]);
            ok = decode(c->data, c->size, c->flags, c->pos, sides[s], &drawn);
            if (c->ok) {
                CHECK(ok, "a supported cover did not decode");
                CHECK(!ok || drawn || c->ok < 2, "decoded, but nothing drawn");
            }
            decoded += ok;
        }
        if (host_failures > 10)
            return host_done();
    }
    printf("art: %u of %d covers x 3 sizes decoded\n", decoded, ncovers);
    for (int i = 0; i < ncovers; i++) {
        cover_t *c = &covers[i];

        if (c->size > sizeof(buf))
            continue;
        host_srand((uint32_t)atoi(argv[3]) * 2654435761u + (uint32_t)i * 40503u + 1);
        for (unsigned k = 0; k < count; k++) {
            uint32_t n = c->size;

            memcpy(buf, c->data, n);
            mutate(buf, &n);
            host_context("%s, mutation %u (seed %s)", c->path, k, argv[3]);
            host_dump(buf, n);
            mutant_ok += decode(buf, n, c->flags, c->pos < n ? c->pos : 0, 48, NULL);
            fuzzed++;
            if (host_failures > 10)
                return host_done();
        }
        ga_check();
    }
    host_dump(NULL, 0);
    printf("art: %u mutated covers decoded without a fault (%u of them still gave a picture)\n",
           fuzzed, mutant_ok);
    return host_done();
}
