/* B310E OS — album art: the cover of the track playing, decoded small.
 *
 * Where it comes from, in this order:
 *   1. the picture in the tags (ID3v2 APIC, FLAC PICTURE, MP4 covr, the
 *      Vorbis/Opus METADATA_BLOCK_PICTURE comment), whose place Rockbox's
 *      tag parsers note in the codec modules (media_info_t art_*);
 *   2. a picture beside the file: cover, folder, front, album,
 *      AlbumArtSmall, AlbumArt (.jpg, .jpeg, .png).
 *
 * Decoders, our own and small: baseline JPEG (Huffman, 1 or 3 components,
 * any sampling, restart markers) with a reduced inverse DCT, so a 1000 px
 * cover is decoded straight at 1/8 (its DC terms) and a 300 px one at 1/4;
 * progressive JPEG from its DC scans alone (at 1/8: the AC scans are
 * stepped over, so a small progressive cover is shown small);
 * PNG (8-bit grey, RGB, palette (1-8 bit), grey+alpha, RGBA; not
 * interlaced) through our own inflate. The
 * result is scaled to the size asked for by averaging (the aspect kept,
 * the rest the card's colour).
 *
 * Bytes come through one reader that also undoes ID3 unsynchronisation
 * and, for Ogg, decodes the base64 while it steps over the Ogg page
 * headers that cut the comment packet.
 *
 * The decoding runs in a low thread ("art", below the audio decoder) so a
 * big cover never holds up the UI or the music, at no less than 104 MHz;
 * art_get() returns the pixels once they are ready. Each thumbnail is kept
 * in /b310e-os/cache/art (keyed by the picture's file and place), so a
 * track or album seen before shows at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/art.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define MAX_SIDE   96                   /* the largest size asked for */
#define WORK_MAX   (192u * 1024)        /* the decoder's planes, at most */
#define DC_MAX     (1024u * 1024)       /* a progressive JPEG's DC terms: a
                                         * 4000 px cover needs 750 KB */

/* ---- the byte reader --------------------------------------------------------- */

#define SRC_BUF    8192

typedef struct {
    FIL f;
    uint32_t left;                      /* bytes of the source still to give */
    /* big reads: FatFs reads whole sectors straight into it with one
     * multi-block command; 512-byte reads were one card command each,
     * most of the time a cover took on the phone */
    uint8_t buf[SRC_BUF];
    int n, i;
    bool unsync, last_ff;
    /* Ogg + base64 */
    bool b64;
    uint32_t page_end;                  /* file offset where this page's data ends */
    uint32_t bits;
    int nbits;
    bool eof;
    volatile uint32_t *cancel, gen;
} src_t;

static int raw_byte(src_t *s)
{
    if (s->i == s->n) {
        UINT got = 0;
        /* to the next sector boundary first, then whole sectors */
        UINT want = SRC_BUF - (UINT)(f_tell(&s->f) & 511);

        if (f_read(&s->f, s->buf, want, &got) != FR_OK || !got)
            return -1;
        s->n = (int)got;
        s->i = 0;
    }
    return s->buf[s->i++];
}

static uint32_t raw_tell(src_t *s)
{
    return (uint32_t)f_tell(&s->f) - (uint32_t)(s->n - s->i);
}

static bool raw_seek(src_t *s, uint32_t pos)
{
    s->n = s->i = 0;
    return f_lseek(&s->f, pos) == FR_OK;
}

/* the next Ogg page's header at the reader: its data's length */
static int ogg_page(src_t *s)
{
    uint8_t h[27];
    int len = 0;

    for (int k = 0; k < 27; k++) {
        int c = raw_byte(s);

        if (c < 0)
            return -1;
        h[k] = (uint8_t)c;
    }
    if (memcmp(h, "OggS", 4))
        return -1;
    for (int k = 0; k < h[26]; k++) {
        int c = raw_byte(s);

        if (c < 0)
            return -1;
        len += c;
    }
    return len;
}

/* a byte of the comment packet: page headers stepped over */
static int ogg_byte(src_t *s)
{
    while (raw_tell(s) >= s->page_end) {
        int len = ogg_page(s);

        if (len < 0)
            return -1;
        s->page_end = raw_tell(s) + (uint32_t)len;
    }
    return raw_byte(s);
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int src_byte(src_t *s)
{
    int c;

    if (s->eof || !s->left)
        return -1;
    if (s->b64) {
        while (s->nbits < 8) {
            int v;

            c = ogg_byte(s);
            if (c < 0 || c == '=' || (v = b64_value(c)) < 0) {
                s->eof = true;          /* the end of the comment's value */
                return -1;
            }
            s->bits = s->bits << 6 | (uint32_t)v;
            s->nbits += 6;
        }
        s->nbits -= 8;
        s->left--;
        return (int)(s->bits >> s->nbits) & 0xff;
    }
    c = raw_byte(s);
    if (c < 0) {
        s->eof = true;
        return -1;
    }
    s->left--;
    if (s->unsync && s->last_ff && c == 0 && s->left) {
        c = raw_byte(s);                /* FF 00 -> FF */
        s->left--;
        if (c < 0)
            return -1;
    }
    s->last_ff = c == 0xff;
    return c;
}

static bool cancelled(src_t *s)
{
    return s->cancel && *s->cancel != s->gen;
}

/* Open at pos for size bytes; base64 sources start from the Ogg page that
 * holds pos (the pages before are walked from the start of the file). */
static bool src_open(src_t *s, const char *path, uint32_t pos, uint32_t size, uint32_t flags)
{
    memset(s, 0, sizeof(*s));
    if (f_open(&s->f, path, FA_READ) != FR_OK)
        return false;
    s->left = size ? size : (uint32_t)f_size(&s->f) - pos;
    s->unsync = flags & MEDIA_ART_UNSYNC;
    s->b64 = flags & MEDIA_ART_BASE64;
    if (s->b64) {
        uint32_t at = 0;

        raw_seek(s, 0);
        for (;;) {                      /* the page whose data holds pos */
            int len = ogg_page(s);

            if (len < 0)
                goto fail;
            at = raw_tell(s) + (uint32_t)len;
            if (pos < at)
                break;
            raw_seek(s, at);
        }
        s->page_end = at;
        raw_seek(s, pos);
        s->left = 0xffffffffu;          /* until the base64 ends */
        return true;
    }
    if (!raw_seek(s, pos))
        goto fail;
    return true;
fail:
    f_close(&s->f);
    return false;
}

static void src_close(src_t *s)
{
    f_close(&s->f);
}

/* ---- the output: scaled into a size x size box ---------------------------------- */

typedef struct {
    int src_w, src_h;                   /* the decoded picture */
    int w, h, x0, y0;                   /* the picture inside the box */
    int side;
    uint32_t *acc;                      /* r, g, b, n per output pixel of a row */
    int out_row;                        /* the output row being summed */
    uint16_t *px;                       /* side * side */
} sink_t;

static bool sink_init(sink_t *k, int src_w, int src_h, int side, uint16_t *px)
{
    k->src_w = src_w;
    k->src_h = src_h;
    k->side = side;
    k->px = px;
    if (src_w >= src_h) {
        k->w = side;
        k->h = (int)((int64_t)side * src_h / src_w);
    } else {
        k->h = side;
        k->w = (int)((int64_t)side * src_w / src_h);
    }
    if (k->w > src_w || k->h > src_h) {     /* small: as it is, not blown up */
        k->w = src_w;
        k->h = src_h;
    }
    if (k->w < 1) k->w = 1;
    if (k->h < 1) k->h = 1;
    k->x0 = (side - k->w) / 2;
    k->y0 = (side - k->h) / 2;
    for (int i = 0; i < side * side; i++)
        px[i] = C_CARD;
    k->acc = kzalloc(sizeof(uint32_t) * 4 * (size_t)k->w);
    k->out_row = -1;
    return k->acc != NULL;
}

static void sink_flush(sink_t *k)
{
    if (k->out_row < 0)
        return;
    for (int x = 0; x < k->w; x++) {
        uint32_t *a = &k->acc[x * 4];

        if (a[3]) {
            uint32_t r = a[0] / a[3], g = a[1] / a[3], b = a[2] / a[3];

            k->px[(k->y0 + k->out_row) * k->side + k->x0 + x] =
                (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        }
        a[0] = a[1] = a[2] = a[3] = 0;
    }
}

/* one source row of RGB888 */
static void sink_row(sink_t *k, int y, const uint8_t *rgb)
{
    int oy = (int)((int64_t)y * k->h / k->src_h);

    if (oy != k->out_row) {
        sink_flush(k);
        k->out_row = oy;
    }
    for (int x = 0; x < k->src_w; x++) {
        uint32_t *a = &k->acc[(int)((int64_t)x * k->w / k->src_w) * 4];

        a[0] += rgb[x * 3];
        a[1] += rgb[x * 3 + 1];
        a[2] += rgb[x * 3 + 2];
        a[3]++;
    }
}

static void sink_done(sink_t *k)
{
    sink_flush(k);
    kfree(k->acc);
    k->acc = NULL;
}

/* ---- JPEG (baseline) ----------------------------------------------------------- */

typedef struct {
    uint8_t bits[17];
    uint8_t vals[256];
    int32_t maxcode[18];                /* 16-bit codes reach 0xfffe: not int16 */
    int16_t valptr[17];
    int32_t mincode[17];
    uint16_t lut[512];                  /* the next 9 bits: length << 8 | value, 0 none */
} huff_t;

typedef struct {
    int id, h, v, tq, td, ta;
    int bw, bh;                         /* blocks across and down (whole MCUs) */
    int pw;                             /* plane width in pixels */
    uint8_t *plane;
    int dc;
} comp_t;

typedef struct {
    src_t *s;
    uint16_t q[4][64];
    huff_t dc[4], ac[4];
    comp_t c[3];
    int nc, w, h, hmax, vmax, restart;
    uint32_t bitbuf;
    int nbits;
    int marker;                         /* a marker met inside the data */
} jpeg_t;

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* cos(k pi / 16) * 2^14, k = 0..31 */
static const int16_t cos16[32] = {
    16384, 16069, 15137, 13623, 11585, 9102, 6270, 3196,
    0, -3196, -6270, -9102, -11585, -13623, -15137, -16069,
    -16384, -16069, -15137, -13623, -11585, -9102, -6270, -3196,
    0, 3196, 6270, 9102, 11585, 13623, 15137, 16069,
};

static int get16(src_t *s)
{
    int a = src_byte(s), b = src_byte(s);

    return a < 0 || b < 0 ? -1 : a << 8 | b;
}

static void skip_n(src_t *s, int n)
{
    while (n-- > 0 && src_byte(s) >= 0)
        ;
}

/* false: more codes of a length than there are (the table below would overrun) */
static bool huff_build(huff_t *t)
{
    int code = 0, k = 0;

    for (int l = 1; l <= 16; l++) {
        if (code + t->bits[l] > 1 << l)
            return false;
        t->valptr[l] = (int16_t)k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fff;
    memset(t->lut, 0, sizeof(t->lut));
    for (int l = 1, n = 0; l <= 9; l++)     /* codes up to 9 bits: one look */
        for (int i = 0; i < t->bits[l]; i++, n++) {
            int c = t->mincode[l] + i, first = c << (9 - l);

            for (int f = 0; f < 1 << (9 - l); f++)
                t->lut[first + f] = (uint16_t)(l << 8 | t->vals[n]);
        }
    return true;
}

/* The entropy-coded bits, MSB first, up to 32 at a time in bitbuf (its
 * top bits are the next ones). Byte stuffing (FF 00) undone; at a marker
 * (RSTn, EOI) zeros follow and the marker is kept for the caller. */
static void fill(jpeg_t *j)
{
    while (j->nbits <= 24) {
        int c;

        if (j->marker)
            c = 0;                      /* past a marker: zeros */
        else {
            c = src_byte(j->s);
            if (c == 0xff) {
                int d = src_byte(j->s);

                if (d != 0) {           /* a marker (RSTn, EOI) */
                    j->marker = d < 0 ? 0xd9 : d;
                    c = 0;
                }
            } else if (c < 0) {
                j->marker = 0xd9;
                c = 0;
            }
        }
        j->bitbuf |= (uint32_t)c << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int bits_n(jpeg_t *j, int n)
{
    int v;

    if (!n)
        return 0;
    if (j->nbits < n)
        fill(j);
    v = (int)(j->bitbuf >> (32 - n));
    j->bitbuf <<= n;
    j->nbits -= n;
    return v;
}

static int bit(jpeg_t *j)
{
    return bits_n(j, 1);
}

static int decode_huff(jpeg_t *j, const huff_t *t)
{
    unsigned e;

    if (j->nbits < 16)
        fill(j);
    e = t->lut[j->bitbuf >> 23];        /* 9 bits */
    if (e) {
        j->bitbuf <<= e >> 8;
        j->nbits -= (int)(e >> 8);
        return (int)(e & 0xff);
    }
    for (int l = 10; l <= 16; l++) {
        int32_t code = (int32_t)(j->bitbuf >> (32 - l));

        if (code <= t->maxcode[l]) {
            j->bitbuf <<= l;
            j->nbits -= l;
            return t->vals[t->valptr[l] + code - t->mincode[l]];
        }
    }
    return -1;
}

static int extend(int v, int n)
{
    return n && v < 1 << (n - 1) ? v - (1 << n) + 1 : v;
}

/* one block into the plane at (bx, by), scale N pixels a side */
static bool block(jpeg_t *j, comp_t *c, int bx, int by, int N)
{
    int coef[64] = { 0 };
    int s = decode_huff(j, &j->dc[c->td]), k;
    const uint16_t *q = j->q[c->tq];

    if (s < 0)
        return false;
    c->dc += extend(bits_n(j, s), s);
    coef[0] = c->dc * q[0];
    for (k = 1; k < 64; ) {
        int rs = decode_huff(j, &j->ac[c->ta]), r, sz;

        if (rs < 0)
            return false;
        r = rs >> 4;
        sz = rs & 15;
        if (!sz) {
            if (r != 15)
                break;                  /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return false;
        if (N == 1) {
            bits_n(j, sz);              /* DC only: the value is not needed */
        } else {
            int pos = zigzag[k];

            if ((pos >> 3) < N && (pos & 7) < N)    /* only the N x N corner */
                coef[pos] = extend(bits_n(j, sz), sz) * q[k];
            else
                bits_n(j, sz);
        }
        k++;
    }
    if (bx >= c->bw || by >= c->bh)
        return true;                    /* a block past the plane: decoded, unused */
    {
        uint8_t *out = c->plane + (by * N) * c->pw + bx * N;
        int step = 16 / N;              /* the cos16 index step: (2x+1)u pi / (2N) */

        if (N == 1) {
            int v = (coef[0] >> 3) + 128;

            out[0] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            return true;
        }
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int32_t sum = 0;

                for (int v = 0; v < N; v++) {
                    int32_t row = 0, cv = v ? cos16[((2 * y + 1) * v * step / 2) & 31] : 11585;

                    for (int u = 0; u < N; u++) {
                        int cu = u ? cos16[((2 * x + 1) * u * step / 2) & 31] : 11585;

                        row += coef[v * 8 + u] * cu;
                    }
                    sum += (row >> 14) * cv >> 14;
                }
                sum = (sum >> 2) + 128;  /* the 1/4 of the 2D IDCT */
                out[y * c->pw + x] = (uint8_t)(sum < 0 ? 0 : sum > 255 ? 255 : sum);
            }
    }
    return true;
}

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* After a scan: the next marker (an RSTn in the data is not one). */
static int next_marker(jpeg_t *j)
{
    int m = j->marker;

    j->marker = 0;
    j->nbits = 0;
    j->bitbuf = 0;
    if (m && (m < 0xd0 || m > 0xd7))
        return m;
    for (;;) {
        int c = src_byte(j->s);

        if (c < 0)
            return -1;
        if (c != 0xff)
            continue;
        do {
            c = src_byte(j->s);
        } while (c == 0xff);
        if (c < 0)
            return -1;
        if (c && (c < 0xd0 || c > 0xd7))
            return c;
    }
}

/* at a restart interval: realign, the DC predictions from 0 */
static void restart(jpeg_t *j)
{
    j->nbits = 0;
    j->bitbuf = 0;
    if (!j->marker) {
        int c;

        do {
            c = src_byte(j->s);
        } while (c >= 0 && c != 0xff);
        src_byte(j->s);
    }
    j->marker = 0;
    for (int i = 0; i < j->nc; i++)
        j->c[i].dc = 0;
}

/* a progressive DC scan: first (ah 0) or refining (one more bit) */
static bool dc_unit(jpeg_t *j, comp_t *c, int16_t *dcv, int bx, int by, int ah, int al)
{
    int16_t *p;

    if (bx >= c->bw || by >= c->bh)
        p = NULL;
    else
        p = &dcv[by * c->bw + bx];
    if (!ah) {
        int s = decode_huff(j, &j->dc[c->td]);

        if (s < 0)
            return false;
        c->dc += extend(bits_n(j, s), s);
        if (p)
            *p = (int16_t)(c->dc * (1 << al));
    } else if (bit(j) && p) {
        *p = (int16_t)(*p | 1 << al);
    }
    return true;
}

static bool dc_scan(jpeg_t *j, int ns, const int *sc, int ah, int al, int16_t **dcv)
{
    int units = 0;

    j->nbits = 0;

    j->bitbuf = 0;
    j->marker = 0;
    for (int i = 0; i < j->nc; i++)
        j->c[i].dc = 0;
    if (ns == 1) {                      /* one component: its own block order */
        comp_t *c = &j->c[sc[0]];
        int cw = (j->w * c->h + j->hmax - 1) / j->hmax, ch = (j->h * c->v + j->vmax - 1) / j->vmax;
        int bw = (cw + 7) / 8, bh = (ch + 7) / 8;

        for (int by = 0; by < bh; by++) {
            if (cancelled(j->s))
                return false;
            for (int bx = 0; bx < bw; bx++, units++) {
                if (j->restart && units && units % j->restart == 0)
                    restart(j);
                if (!dc_unit(j, c, dcv[sc[0]], bx, by, ah, al))
                    return false;
            }
        }
        return true;
    }
    {
        int mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
        int mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);

        for (int my = 0; my < mcuy; my++) {
            if (cancelled(j->s))
                return false;
            for (int mx = 0; mx < mcux; mx++, units++) {
                if (j->restart && units && units % j->restart == 0)
                    restart(j);
                for (int i = 0; i < ns; i++) {
                    comp_t *c = &j->c[sc[i]];

                    for (int by = 0; by < c->v; by++)
                        for (int bx = 0; bx < c->h; bx++)
                            if (!dc_unit(j, c, dcv[sc[i]], mx * c->h + bx, my * c->v + by,
                                         ah, al))
                                return false;
                }
            }
        }
    }
    return true;
}

static bool jpeg_decode(src_t *s, int side, uint16_t *px)
{
    jpeg_t *j = kzalloc(sizeof(*j));
    bool ok = false, sof = false, progressive = false, planes = false;
    int N = 8, m, dc_scans = 0;
    sink_t k = { 0 };
    uint8_t *rgb = NULL;
    int16_t *dcv[3] = { NULL, NULL, NULL };
    int mcux = 0, mcuy = 0;

    if (!j)
        return false;
    j->s = s;
    /* SOI, allowing a few stray bytes first. A Vorbis comment's base64
     * position is rounded to whole groups and can be past the SOI: then
     * the first marker after it (APPn, DQT, DHT, SOF, DRI, COM) starts. */
    m = -1;
    for (int tries = 0; ; tries++) {
        int c = src_byte(s);

        if (c < 0 || tries > 64)
            goto out;
        if (c == 0xff) {
            int d = src_byte(s);

            if (d == 0xd8)
                break;
            if ((d >= 0xe0 && d <= 0xef) || d == 0xdb || d == 0xc4 || d == 0xc0 ||
                d == 0xc1 || d == 0xc2 || d == 0xdd || d == 0xfe) {
                m = d;
                break;
            }
        }
    }
    for (;;) {                          /* marker by marker, scan by scan */
        int len;

        if (m < 0) {
            do {
                m = src_byte(s);
            } while (m >= 0 && m != 0xff);
            while (m == 0xff)
                m = src_byte(s);
        }
        if (m < 0 || m == 0xd9) {       /* the end */
            if (progressive && dc_scans)
                break;                  /* what the DC scans gave */
            goto out;
        }
        len = get16(s) - 2;
        if (len < 0)
            goto out;
        if (m == 0xdb) {                /* DQT */
            while (len > 0) {
                int pq = src_byte(s), t = pq & 3;

                for (int i = 0; i < 64; i++)
                    j->q[t][i] = (uint16_t)(pq >> 4 ? get16(s) : src_byte(s));
                len -= 1 + (pq >> 4 ? 128 : 64);
            }
        } else if (m == 0xc4) {         /* DHT */
            while (len > 0) {
                int tc = src_byte(s), n = 0;
                huff_t *t = (tc >> 4 ? j->ac : j->dc) + (tc & 3);

                for (int l = 1; l <= 16; l++)
                    n += t->bits[l] = (uint8_t)src_byte(s);
                if (n > 256)
                    goto out;
                for (int i = 0; i < n; i++)
                    t->vals[i] = (uint8_t)src_byte(s);
                if (!huff_build(t))
                    goto out;
                len -= 17 + n;
            }
        } else if (m == 0xc0 || m == 0xc1 || m == 0xc2) {   /* SOF0/1/2 */
            progressive = m == 0xc2;
            if (src_byte(s) != 8)
                goto out;
            j->h = get16(s);
            j->w = get16(s);
            j->nc = src_byte(s);
            if (j->nc != 1 && j->nc != 3)
                goto out;
            for (int i = 0; i < j->nc; i++) {
                int hv;

                j->c[i].id = src_byte(s);
                hv = src_byte(s);
                j->c[i].h = hv >> 4;
                j->c[i].v = hv & 15;
                j->c[i].tq = src_byte(s) & 3;
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4)
                    goto out;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            if (j->nc == 1)             /* one component: blocks in plain order */
                j->c[0].h = j->c[0].v = j->hmax = j->vmax = 1;
            if (!j->w || !j->h)
                goto out;
            mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                j->c[i].bw = mcux * j->c[i].h;
                j->c[i].bh = mcuy * j->c[i].v;
            }
            if (progressive) {          /* the DC terms only: 1/8 */
                size_t blocks = 0;

                for (int i = 0; i < j->nc; i++)
                    blocks += (size_t)j->c[i].bw * (size_t)j->c[i].bh;
                if (blocks * 2 > DC_MAX)
                    goto out;
                for (int i = 0; i < j->nc; i++) {
                    dcv[i] = kzalloc((size_t)j->c[i].bw * (size_t)j->c[i].bh * 2);
                    if (!dcv[i])
                        goto out;
                }
            }
            sof = true;
        } else if (m >= 0xc3 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            goto out;                   /* lossless, arithmetic */
        } else if (m == 0xdd) {         /* DRI */
            j->restart = get16(s);
        } else if (m == 0xda) {         /* SOS */
            int ns = src_byte(s), sc[4], ss, se, a;

            if (!sof || ns < 1 || ns > j->nc)
                goto out;
            for (int i = 0; i < ns; i++) {
                int id = src_byte(s), t = src_byte(s);

                sc[i] = -1;
                for (int c = 0; c < j->nc; c++)
                    if (j->c[c].id == id) {
                        sc[i] = c;
                        j->c[c].td = t >> 4 & 3;
                        j->c[c].ta = t & 3;
                    }
                if (sc[i] < 0)
                    goto out;
            }
            ss = src_byte(s);
            se = src_byte(s);
            a = src_byte(s);
            if (!progressive) {
                if (ns != j->nc)
                    goto out;           /* baseline: one scan with every component */
                break;
            }
            if (ss == 0) {              /* a DC scan */
                if (!dc_scan(j, ns, sc, a >> 4, a & 15, dcv))
                    goto out;
                dc_scans++;
            }
            (void)se;
            m = next_marker(j);         /* AC scans: stepped over */
            continue;
        } else {
            skip_n(s, len);
        }
        m = -1;
    }

    if (progressive) {
        N = 1;
        for (int i = 0; i < j->nc; i++) {
            comp_t *c = &j->c[i];
            int q0 = j->q[c->tq][0];

            c->pw = c->bw;
            c->plane = kmalloc((size_t)c->bw * (size_t)c->bh);
            if (!c->plane)
                goto out;
            for (int b = 0; b < c->bw * c->bh; b++)
                c->plane[b] = clamp8((dcv[i][b] * q0 >> 3) + 128);
        }
        planes = true;
    }

    if (!planes) {
        /* the scale: the smallest N whose picture still covers the box */
        size_t total = 0;

        for (N = 1; N < 8; N *= 2) {
            int sw = (j->w * N + 7) / 8, sh = (j->h * N + 7) / 8;

            if (sw >= side && sh >= side)
                break;
        }
        for (;;) {
            total = 0;
            for (int i = 0; i < j->nc; i++)
                total += (size_t)(j->c[i].bw * N) * (size_t)(j->c[i].bh * N);
            if (total <= WORK_MAX || N == 1)
                break;
            N /= 2;
        }
        if (total > WORK_MAX * 4)
            goto out;
        for (int i = 0; i < j->nc; i++) {
            comp_t *c = &j->c[i];

            c->pw = c->bw * N;
            c->plane = kmalloc((size_t)c->pw * (size_t)(c->bh * N));
            if (!c->plane)
                goto out;
        }
        /* the scan */
        for (int my = 0, mcus = 0; my < mcuy; my++) {
            if (cancelled(s))
                goto out;
            for (int mx = 0; mx < mcux; mx++, mcus++) {
                if (j->restart && mcus && mcus % j->restart == 0)
                    restart(j);
                for (int i = 0; i < j->nc; i++) {
                    comp_t *c = &j->c[i];

                    for (int by = 0; by < c->v; by++)
                        for (int bx = 0; bx < c->h; bx++)
                            if (!block(j, c, mx * c->h + bx, my * c->v + by, N))
                                goto out;
                }
            }
        }
    }

    /* colour, row by row, into the sink */
    {
        int sw = (j->w * N + 7) / 8, sh = (j->h * N + 7) / 8;

        rgb = kmalloc((size_t)sw * 3);
        if (!rgb || !sink_init(&k, sw, sh, side, px))
            goto out;
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {
                int yy, cb = 128, cr = 128;
                comp_t *c0 = &j->c[0];

                yy = c0->plane[(y * c0->v / j->vmax) * c0->pw + x * c0->h / j->hmax];
                if (j->nc == 3) {
                    comp_t *c1 = &j->c[1], *c2 = &j->c[2];

                    cb = c1->plane[(y * c1->v / j->vmax) * c1->pw + x * c1->h / j->hmax];
                    cr = c2->plane[(y * c2->v / j->vmax) * c2->pw + x * c2->h / j->hmax];
                }
                cb -= 128;
                cr -= 128;
                rgb[x * 3] = clamp8(yy + ((91881 * cr) >> 16));
                rgb[x * 3 + 1] = clamp8(yy - ((22554 * cb + 46802 * cr) >> 16));
                rgb[x * 3 + 2] = clamp8(yy + ((116130 * cb) >> 16));
            }
            sink_row(&k, y, rgb);
        }
        sink_done(&k);
        ok = true;
    }
out:
    if (k.acc)
        sink_done(&k);
    kfree(rgb);
    for (int i = 0; i < 3; i++) {
        kfree(j->c[i].plane);
        kfree(dcv[i]);
    }
    kfree(j);
    return ok;
}

/* ---- inflate (RFC 1951) ---------------------------------------------------------- */

typedef struct {
    uint16_t count[16], sym[288];
} itable_t;

typedef struct {
    src_t *s;
    uint32_t bitbuf;
    int nbits;
    uint8_t *win;                       /* 32 KB */
    uint32_t wpos;
    /* the zlib stream comes in IDAT pieces: the reader takes chunk data */
    uint32_t chunk_left;
    bool err;
    void (*out)(void *ctx, uint8_t b);
    void *ctx;
} inflate_t;

static int idat_byte(inflate_t *z)
{
    src_t *s = z->s;

    while (!z->chunk_left) {            /* the next IDAT chunk */
        uint8_t h[8];

        skip_n(s, 4);                   /* the last chunk's CRC */
        for (int i = 0; i < 8; i++) {
            int c = src_byte(s);

            if (c < 0)
                return -1;
            h[i] = (uint8_t)c;
        }
        if (memcmp(h + 4, "IDAT", 4))
            return -1;
        z->chunk_left = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
    }
    z->chunk_left--;
    return src_byte(s);
}

static int ibits(inflate_t *z, int n)
{
    uint32_t v;

    while (z->nbits < n) {
        int c = idat_byte(z);

        if (c < 0) {
            z->err = true;
            return 0;
        }
        z->bitbuf |= (uint32_t)c << z->nbits;
        z->nbits += 8;
    }
    v = z->bitbuf & ((1u << n) - 1);
    z->bitbuf >>= n;
    z->nbits -= n;
    return (int)v;
}

static void itable(itable_t *t, const uint8_t *len, int n)
{
    uint16_t offs[16];

    memset(t->count, 0, sizeof(t->count));
    for (int i = 0; i < n; i++)
        t->count[len[i]]++;
    t->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + t->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i])
            t->sym[offs[len[i]]++] = (uint16_t)i;
}

static int isym(inflate_t *z, const itable_t *t)
{
    int code = 0, first = 0, index = 0;

    for (int l = 1; l < 16; l++) {
        int count;

        code |= ibits(z, 1);
        count = t->count[l];
        if (code - count < first)
            return t->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (z->err)
            return -1;
    }
    z->err = true;
    return -1;
}

static void iput(inflate_t *z, uint8_t b)
{
    z->win[z->wpos++ & 0x7fff] = b;
    z->out(z->ctx, b);
}

static const uint16_t lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool inflate_run(inflate_t *z)
{
    itable_t *lt = kmalloc(sizeof(itable_t) * 2), *dt;
    bool last = false;

    if (!lt)
        return false;
    dt = lt + 1;
    ibits(z, 16);                       /* the zlib header */
    while (!last && !z->err) {
        int type;

        if (cancelled(z->s))
            break;
        last = ibits(z, 1);
        type = ibits(z, 2);
        if (type == 0) {                /* stored */
            int len;

            z->bitbuf = 0;
            z->nbits = 0;
            len = ibits(z, 16);
            ibits(z, 16);
            while (len-- > 0 && !z->err)
                iput(z, (uint8_t)ibits(z, 8));
            continue;
        }
        if (type == 1) {                /* fixed */
            uint8_t l[320];

            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            itable(lt, l, 288);
            for (int i = 0; i < 30; i++) l[i] = 5;
            itable(dt, l, 30);
        } else if (type == 2) {         /* dynamic */
            static const uint8_t order[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t l[320] = { 0 };
            int hlit = ibits(z, 5) + 257, hdist = ibits(z, 5) + 1, hclen = ibits(z, 4) + 4;
            itable_t *ct = dt;          /* the code-length table, briefly */

            for (int i = 0; i < hclen; i++)
                l[order[i]] = (uint8_t)ibits(z, 3);
            itable(ct, l, 19);
            memset(l, 0, sizeof(l));
            for (int i = 0; i < hlit + hdist && !z->err; ) {
                int sym = isym(z, ct), rep = 0, val = 0;

                if (sym < 16) {
                    l[i++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!i) { z->err = true; break; }
                    val = l[i - 1];
                    rep = 3 + ibits(z, 2);
                } else if (sym == 17) {
                    rep = 3 + ibits(z, 3);
                } else {
                    rep = 11 + ibits(z, 7);
                }
                while (rep-- && i < hlit + hdist)
                    l[i++] = (uint8_t)val;
            }
            itable(lt, l, hlit);
            itable(dt, l + hlit, hdist);
        } else {
            z->err = true;
            break;
        }
        for (;;) {                      /* the block's symbols */
            int sym = isym(z, lt);

            if (sym < 0 || z->err)
                break;
            if (sym < 256) {
                iput(z, (uint8_t)sym);
            } else if (sym == 256) {
                break;
            } else {
                int len, d, dist;

                sym -= 257;
                if (sym >= 29) { z->err = true; break; }
                len = lbase[sym] + ibits(z, lext[sym]);
                d = isym(z, dt);
                if (d < 0 || d >= 30) { z->err = true; break; }
                dist = dbase[d] + ibits(z, dext[d]);
                while (len--)
                    iput(z, z->win[(z->wpos - (uint32_t)dist) & 0x7fff]);
            }
        }
    }
    kfree(lt);
    return !z->err;
}

/* ---- PNG ------------------------------------------------------------------------- */

typedef struct {
    int w, h, ctype, depth, bpp, channels;  /* bpp: bytes a pixel, at least 1 */
    uint32_t stride;                    /* bytes a row, without the filter byte */
    uint8_t *cur, *prev, *rgb;
    uint32_t col;                       /* bytes of the row so far */
    int filter, row;
    uint8_t pal[256][4];
    sink_t *k;
} png_t;

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static void png_row_done(png_t *p)
{
    for (uint32_t i = 0; i < p->stride; i++) {
        int a = i >= (uint32_t)p->bpp ? p->cur[i - p->bpp] : 0, b = p->prev[i];
        int c = i >= (uint32_t)p->bpp ? p->prev[i - p->bpp] : 0;

        switch (p->filter) {
        case 1: p->cur[i] = (uint8_t)(p->cur[i] + a); break;
        case 2: p->cur[i] = (uint8_t)(p->cur[i] + b); break;
        case 3: p->cur[i] = (uint8_t)(p->cur[i] + ((a + b) >> 1)); break;
        case 4: p->cur[i] = (uint8_t)(p->cur[i] + paeth(a, b, c)); break;
        }
    }
    for (int x = 0; x < p->w; x++) {     /* to RGB, alpha over the card colour */
        int r, g, b, a = 255;
        uint8_t *px = p->rgb + x * 3;

        if (p->ctype == 3 || (p->ctype == 0 && p->depth < 8)) {
            int bitpos = x * p->depth, v = p->cur[bitpos >> 3] >> (8 - p->depth - (bitpos & 7));

            v &= (1 << p->depth) - 1;
            if (p->ctype == 3) {
                r = p->pal[v][0]; g = p->pal[v][1]; b = p->pal[v][2]; a = p->pal[v][3];
            } else {
                r = g = b = v * 255 / ((1 << p->depth) - 1);
            }
        } else {
            const uint8_t *q = p->cur + x * p->bpp;

            switch (p->ctype) {
            case 0: r = g = b = q[0]; break;
            case 4: r = g = b = q[0]; a = q[1]; break;
            case 2: r = q[0]; g = q[1]; b = q[2]; break;
            default: r = q[0]; g = q[1]; b = q[2]; a = q[3]; break;
            }
        }
        if (a != 255) {                 /* C_CARD is RGB(46, 43, 56) */
            r = (r * a + 46 * (255 - a)) / 255;
            g = (g * a + 43 * (255 - a)) / 255;
            b = (b * a + 56 * (255 - a)) / 255;
        }
        px[0] = (uint8_t)r; px[1] = (uint8_t)g; px[2] = (uint8_t)b;
    }
    sink_row(p->k, p->row++, p->rgb);
    {
        uint8_t *t = p->prev;

        p->prev = p->cur;
        p->cur = t;
    }
}

static void png_byte(void *ctx, uint8_t b)
{
    png_t *p = ctx;

    if (p->row >= p->h)
        return;
    if (p->col == 0 && p->filter < 0) {
        p->filter = b;                  /* each row starts with its filter */
        return;
    }
    p->cur[p->col++] = b;
    if (p->col == p->stride) {
        png_row_done(p);
        p->col = 0;
        p->filter = -1;
    }
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static bool png_decode(src_t *s, int side, uint16_t *px)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    png_t *p = kzalloc(sizeof(*p));
    inflate_t z = { 0 };
    sink_t k = { 0 };
    bool ok = false;
    uint8_t h[8];

    if (!p)
        return false;
    for (int i = 0; i < 8; i++)
        if (src_byte(s) != sig[i])
            goto out;
    for (int i = 0; i < 256; i++)
        p->pal[i][3] = 255;
    for (;;) {                          /* chunks up to the first IDAT */
        uint32_t len;

        for (int i = 0; i < 8; i++) {
            int c = src_byte(s);

            if (c < 0)
                goto out;
            h[i] = (uint8_t)c;
        }
        len = be32(h);
        if (!memcmp(h + 4, "IHDR", 4)) {
            uint8_t d[13];

            for (int i = 0; i < 13; i++)
                d[i] = (uint8_t)src_byte(s);
            p->w = (int)be32(d);
            p->h = (int)be32(d + 4);
            p->depth = d[8];
            p->ctype = d[9];
            if (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8)
                goto out;               /* 0 would never end a row; 16 is not done */
            if (d[12] || p->w <= 0 || p->h <= 0 || p->w > 8192 || p->h > 8192)
                goto out;               /* interlaced, or nonsense */
            p->channels = p->ctype == 2 ? 3 : p->ctype == 4 ? 2 : p->ctype == 6 ? 4 : 1;
            if (p->depth != 8 && !((p->ctype == 3 || p->ctype == 0) && p->depth < 8))
                goto out;               /* 16-bit not done */
            p->bpp = p->depth < 8 ? 1 : p->channels;
            p->stride = ((uint32_t)p->w * (uint32_t)p->channels * (uint32_t)p->depth + 7) / 8;
            skip_n(s, 4);
        } else if (!memcmp(h + 4, "PLTE", 4)) {
            for (uint32_t i = 0; i < len / 3 && i < 256; i++)
                for (int c = 0; c < 3; c++)
                    p->pal[i][c] = (uint8_t)src_byte(s);
            skip_n(s, (int)(len - (len / 3 < 256 ? len / 3 : 256) * 3) + 4);
        } else if (!memcmp(h + 4, "tRNS", 4) && p->ctype == 3) {
            for (uint32_t i = 0; i < len; i++) {
                int a = src_byte(s);

                if (i < 256)
                    p->pal[i][3] = (uint8_t)a;
            }
            skip_n(s, 4);
        } else if (!memcmp(h + 4, "IDAT", 4)) {
            z.chunk_left = len;
            break;
        } else {
            skip_n(s, (int)len + 4);
        }
        if (s->eof)
            goto out;
    }
    if (!p->w)
        goto out;
    p->cur = kzalloc(p->stride);
    p->prev = kzalloc(p->stride);
    p->rgb = kmalloc((size_t)p->w * 3);
    z.win = kmalloc(0x8000);
    if (!p->cur || !p->prev || !p->rgb || !z.win || !sink_init(&k, p->w, p->h, side, px))
        goto out;
    p->k = &k;
    p->filter = -1;
    z.s = s;
    z.out = png_byte;
    z.ctx = p;
    inflate_run(&z);
    ok = p->row >= p->h;
    sink_done(&k);
out:
    if (k.acc)
        sink_done(&k);
    if (p) {
        kfree(p->cur);
        kfree(p->prev);
        kfree(p->rgb);
    }
    kfree(z.win);
    kfree(p);
    return ok;
}

/* ---- the sources, the worker ------------------------------------------------------- */

/* decode one source: by its magic bytes, whatever the tags said */
static bool decode_at(const char *path, uint32_t pos, uint32_t size, uint32_t flags,
                      int side, uint16_t *px, volatile uint32_t *cancel, uint32_t gen)
{
    src_t *s = kmalloc(sizeof(src_t));
    int c0, c1;
    bool ok = false;

    if (!s)
        return false;
    if (!src_open(s, path, pos, size, flags)) {
        kfree(s);
        return false;
    }
    s->cancel = cancel;
    s->gen = gen;
    c0 = src_byte(s);
    c1 = src_byte(s);
    src_close(s);
    if (src_open(s, path, pos, size, flags)) {
        s->cancel = cancel;
        s->gen = gen;
        if (c0 == 0x89 && c1 == 'P')
            ok = png_decode(s, side, px);
        else
            ok = jpeg_decode(s, side, px);  /* it looks for the start itself */
        src_close(s);
    }
    kfree(s);
    return ok;
}

static const char *const folder_names[] = {
    "cover", "folder", "front", "album", "AlbumArtSmall", "AlbumArt",
};
static const char *const folder_exts[] = { ".jpg", ".jpeg", ".png" };

static struct {
    mutex_t lock;
    sem_t wake;
    thread_t *thread;
    volatile uint32_t gen;              /* a new request cancels the decode */
    /* the request */
    char path[160];
    media_info_t info;
    int side;
    bool pending;
    /* the result */
    char done_path[160];
    uint16_t px[MAX_SIDE * MAX_SIDE];
    int done_side;
    int state;                          /* 0 none yet, 1 ready, 2 no cover */
} art;

/* ---- the thumbnail cache: /b310e-os/cache/art/<hash>.bin -------------------- */

#define CACHE_DIR   "/b310e-os/cache/art"
#define CACHE_MAGIC 0x31545241u         /* "ART1" */

/* FNV-1a of the picture's source: the file and where in it */
static uint32_t cache_key(const char *path, uint32_t pos, uint32_t size)
{
    uint32_t h = 2166136261u;

    for (const char *p = path; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < 4; i++)
        h = (h ^ ((pos >> (8 * i)) & 0xff)) * 16777619u;
    for (int i = 0; i < 4; i++)
        h = (h ^ ((size >> (8 * i)) & 0xff)) * 16777619u;
    return h;
}

static bool cache_load(uint32_t key, int side, uint16_t *px)
{
    char path[48];
    uint32_t hdr[2];
    FIL f;
    UINT n = 0;
    bool ok;

    snprintf(path, sizeof(path), CACHE_DIR "/%08lx.bin", (unsigned long)key);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    ok = f_read(&f, hdr, sizeof(hdr), &n) == FR_OK && n == sizeof(hdr) &&
         hdr[0] == CACHE_MAGIC && hdr[1] == (uint32_t)side &&
         f_read(&f, px, (UINT)(side * side * 2), &n) == FR_OK && n == (UINT)(side * side * 2);
    f_close(&f);
    return ok;
}

static void cache_save(uint32_t key, int side, const uint16_t *px)
{
    char path[48];
    uint32_t hdr[2] = { CACHE_MAGIC, (uint32_t)side };
    FIL f;
    UINT n;

    f_mkdir("/b310e-os/cache");
    f_mkdir(CACHE_DIR);
    snprintf(path, sizeof(path), CACHE_DIR "/%08lx.bin", (unsigned long)key);
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    f_write(&f, hdr, sizeof(hdr), &n);
    f_write(&f, px, (UINT)(side * side * 2), &n);
    f_close(&f);
}

/* the first picture beside the track: its path and size, or false */
static bool folder_cover(const char *track, char *out, size_t on, uint32_t *size)
{
    const char *slash = strrchr(track, '/');
    size_t dl = slash ? (size_t)(slash - track) : 0;

    if (dl + 24 >= on)
        return false;
    for (unsigned i = 0; i < ARRAY_SIZE(folder_names); i++)
        for (unsigned e = 0; e < ARRAY_SIZE(folder_exts); e++) {
            FILINFO fi;

            snprintf(out, on, "%.*s/%s%s", (int)dl, track, folder_names[i], folder_exts[e]);
            if (f_stat(out, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) {
                *size = (uint32_t)fi.fsize;
                return true;
            }
        }
    return false;
}

/* One source: from the cache, else decoded (at full clock) and cached.
 * *from tells which. */
static bool cover_from(const char *path, uint32_t pos, uint32_t size, uint32_t flags,
                       int side, uint16_t *px, uint32_t gen, const char **from)
{
    uint32_t key = cache_key(path, pos, size);
    bool ok;

    if (cache_load(key, side, px)) {
        *from = "the cache";
        return true;
    }
    cpufreq_hold();                     /* no 26 MHz while decoding */
    ok = decode_at(path, pos, size, flags, side, px, &art.gen, gen);
    cpufreq_release();
    if (ok && art.gen == gen)
        cache_save(key, side, px);
    *from = ok ? "decoded" : "";
    return ok;
}


static void art_main(void *arg)
{
    static uint16_t px[MAX_SIDE * MAX_SIDE];

    for (;;) {
        char path[160], cover[200], from[240];
        const char *how = "";
        media_info_t info;
        int side;
        uint32_t gen, t0, csize;
        bool ok = false;

        sem_wait(&art.wake, WAIT_FOREVER);
        mutex_lock(&art.lock, WAIT_FOREVER);
        if (!art.pending) {
            mutex_unlock(&art.lock);
            continue;
        }
        art.pending = false;
        memcpy(path, art.path, sizeof(path));
        info = art.info;
        side = art.side;
        gen = art.gen;
        mutex_unlock(&art.lock);

        t0 = kernel_ticks();
        from[0] = 0;
        if (info.art_type != MEDIA_ART_NONE && info.art_size) {
            ok = cover_from(path, info.art_pos, info.art_size, info.art_flags, side, px, gen,
                            &how);
            if (ok)
                snprintf(from, sizeof(from), "the tags (%s)", how);
        }
        if (!ok && art.gen == gen && folder_cover(path, cover, sizeof(cover), &csize)) {
            ok = cover_from(cover, 0, csize, 0, side, px, gen, &how);
            if (ok)
                snprintf(from, sizeof(from), "%s (%s)", cover, how);
        }
        mutex_lock(&art.lock, WAIT_FOREVER);
        if (art.gen == gen) {
            memcpy(art.done_path, path, sizeof(path));
            art.done_side = side;
            art.state = ok ? 1 : 2;
            if (ok)
                memcpy(art.px, px, sizeof(uint16_t) * (size_t)side * (size_t)side);
            klog("art: %s: %s%s, %lu ms", path, ok ? "cover from " : "no cover",
                 ok ? from : "", (unsigned long)(kernel_ticks() - t0));
        }
        mutex_unlock(&art.lock);
        ui_refresh();                   /* the cover (or its placeholder) now */
    }
}

void art_request(const char *path, const media_info_t *info, int side)
{
    if (side > MAX_SIDE)
        side = MAX_SIDE;
    if (!art.thread) {
        mutex_init(&art.lock);
        sem_init(&art.wake, 0);
        art.thread = thread_create("art", PRIO_NORMAL - 3, 6144, art_main, NULL);
        if (!art.thread)
            return;
    }
    mutex_lock(&art.lock, WAIT_FOREVER);
    /* the last one asked for is waiting, being decoded or done: asking
     * again (Now playing draws ten times a second) must not restart it */
    if (art.side == side && !strcmp(art.path, path)) {
        mutex_unlock(&art.lock);
        return;
    }
    snprintf(art.path, sizeof(art.path), "%s", path);
    art.info = *info;
    art.side = side;
    art.pending = true;
    art.state = 0;
    art.gen++;                          /* the decode under way gives up */
    mutex_unlock(&art.lock);
    sem_post(&art.wake);
}

const uint16_t *art_get(const char *path, int side, int *state)
{
    const uint16_t *r = NULL;
    int st = 0;

    if (!art.thread)
        return NULL;
    mutex_lock(&art.lock, WAIT_FOREVER);
    if (!strcmp(art.done_path, path) && art.done_side == side && !art.pending) {
        st = art.state;
        if (st == 1)
            r = art.px;
    }
    mutex_unlock(&art.lock);
    if (state)
        *state = st;
    return r;
}
