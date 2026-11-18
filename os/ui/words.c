/* B310E OS — words for the text editor's completion.
 *
 * The user's own words come first: learned as they are typed, kept with a use
 * count and their language in the store's "words" table (os/db.h), and in RAM
 * while an editor is open (a packed pool, LEARN_POOL bytes, LEARN_MAX words;
 * past either the least used goes, the oldest of those first). Changes reach
 * the store when the editor closes, in one write; with the last editor the
 * pool goes too (words_unload), and the next one reads it again.
 *
 * Then a word list on the card, one a language: /b310e-os/words/<code>.wl,
 * made on a PC from a word-frequency list by os/tools/mkwords.py (the format is
 * there). RAM holds the first key of each 512-byte index chunk; a query reads
 * one index chunk and one bucket (512 bytes each at most, never across a
 * sector), and keeps both for the next letter of the same word.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/db.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "ui/textlang.h"

#define LEARN_MAX    500
#define LEARN_POOL   8192
#define GONE_MAX     16                 /* evicted ids waiting for the flush */
#define HEAD         8                  /* a pool entry: len, lang (bit 7: changed), uses, id */
#define WL_DIR       OS_DIR "/words"
#define WL_MAGIC     "B3WL"
#define WL_KEY       8
#define WL_ENTRY     16
#define WL_CHUNK     512
#define WL_CHUNKS    4096               /* keys kept in RAM: 32 KB at most */

enum { F_WORD, F_LANG, F_USES };
static const db_field_t fields[] = { { "word", DB_STR }, { "lang", DB_STR }, { "uses", DB_INT } };
static const db_schema_t schema = { "words", 1, 3, -1, fields, NULL };

static db_table_t *table;
static uint8_t *pool;
static int used, count;
static bool loaded;
static uint32_t gone[GONE_MAX];
static int n_gone;

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int uses_of(const uint8_t *e)
{
    return e[2] | e[3] << 8;
}

static void set_uses(uint8_t *e, int n)
{
    e[2] = (uint8_t)n;
    e[3] = (uint8_t)(n >> 8);
}

/* ---- the user's words -------------------------------------------------------- */

static uint8_t *find(int lang, const char *w, int len)
{
    for (uint8_t *e = pool; pool && e < pool + used; e += HEAD + e[0])
        if (e[0] == len && (e[1] & 0x7f) == lang && !memcmp(e + HEAD, w, (size_t)len))
            return e;
    return NULL;
}

/* The least used goes (the oldest of them: the pool is in learning order) */
static void evict(void)
{
    uint8_t *least = NULL;
    int n;

    for (uint8_t *e = pool; e < pool + used; e += HEAD + e[0])
        if (!least || uses_of(e) < uses_of(least))
            least = e;
    if (!least)
        return;
    if (rd32(least + 4)) {
        if (n_gone == GONE_MAX)
            words_flush();
        gone[n_gone++] = rd32(least + 4);
    }
    n = HEAD + least[0];
    memmove(least, least + n, (size_t)(pool + used - least - n));
    used -= n;
    count--;
}

static uint8_t *add(int lang, const char *w, int len, int uses, uint32_t id)
{
    uint8_t *e;

    while (count && (count >= LEARN_MAX || used + HEAD + len > LEARN_POOL))
        evict();
    e = pool + used;
    e[0] = (uint8_t)len;
    e[1] = (uint8_t)lang;
    set_uses(e, uses);
    wr32(e + 4, id);
    memcpy(e + HEAD, w, (size_t)len);
    used += HEAD + len;
    count++;
    return e;
}

void words_load(void)
{
    uint32_t *ids, t0 = kernel_ticks();
    char buf[WORD_MAX + 8];
    int n;

    if (loaded)
        return;
    if (!pool)
        pool = kmalloc(LEARN_POOL);
    if (!table)
        table = db_table_open(&schema);
    if (!pool || !table)
        return;
    ids = kmalloc(LEARN_MAX * sizeof(uint32_t));
    n = ids ? db_find(table, -1, NULL, ids, LEARN_MAX, 0) : 0;
    if (ids && n >= 0)
        loaded = true;                  /* without the card: again next time */
    for (int i = n - 1; i >= 0; i--) {  /* oldest first */
        db_value_t v[3];
        int lang;

        if (db_get(table, ids[i], v, 3, buf, sizeof(buf)) != DB_OK)
            continue;
        lang = input_lang_find(v[F_LANG].p);
        if (lang >= 0 && v[F_WORD].len > 0 && v[F_WORD].len < WORD_MAX &&
            !find(lang, v[F_WORD].p, (int)v[F_WORD].len))
            add(lang, v[F_WORD].p, (int)v[F_WORD].len, v[F_USES].i > 0xffff ? 0xffff : v[F_USES].i,
                ids[i]);
    }
    kfree(ids);
    klog("words: %d learned words, %d bytes, read in %lu ms", count, used,
         (unsigned long)(kernel_ticks() - t0));
}

/* The last editor closed: the pool (8 KB) goes until the next one opens and
 * loads it again (a store read a learned word). Kept while anything in it
 * has not reached the store (no card): that would lose it. */
void words_unload(void)
{
    words_flush();
    if (n_gone)
        return;
    for (uint8_t *e = pool; pool && e < pool + used; e += HEAD + e[0])
        if (e[1] & 0x80)
            return;
    kfree(pool);
    pool = NULL;
    used = count = 0;
    loaded = false;
}

void words_learn(const char *lang, const char *word)
{
    int li = input_lang_find(lang), len = (int)strlen(word);
    uint8_t *e;

    if (li < 0 || len <= 0 || len >= WORD_MAX || !pool)
        return;
    e = find(li, word, len);
    if (e) {
        if (uses_of(e) < 0xffff)
            set_uses(e, uses_of(e) + 1);
    } else {
        e = add(li, word, len, 1, 0);
    }
    e[1] |= 0x80;
}

void words_flush(void)
{
    bool any = n_gone > 0;

    for (uint8_t *e = pool; !any && pool && e < pool + used; e += HEAD + e[0])
        any = e[1] & 0x80;
    if (!any || !table)
        return;
    db_bulk(true);                      /* one write for all of it */
    for (int i = 0; i < n_gone; i++)
        db_remove(table, gone[i]);
    n_gone = 0;
    for (uint8_t *e = pool; pool && e < pool + used; e += HEAD + e[0]) {
        db_value_t v[3];

        if (!(e[1] & 0x80))
            continue;
        v[F_USES] = DB_INTV(uses_of(e));
        if (rd32(e + 4)) {
            if (db_update_field(table, rd32(e + 4), F_USES, &v[F_USES]) != DB_OK)
                continue;
        } else {
            uint32_t id;

            v[F_WORD].p = e + HEAD;
            v[F_WORD].len = e[0];
            v[F_LANG] = DB_STRV(input_langs[e[1] & 0x7f].code);
            id = db_insert(table, v, 3);
            if (!id)
                continue;               /* no card: kept for the next time */
            wr32(e + 4, id);
        }
        e[1] &= 0x7f;
    }
    db_bulk(false);
}

/* ---- the word list on the card ----------------------------------------------- */

/* Header (32 bytes, little-endian): "B3WL", version 1, key bytes 8, entries,
 * chunks, the chunk keys' offset, the index's offset (512-aligned), words, the
 * language. Index entries (16 bytes, sorted by key): the key (a prefix in small
 * letters, UTF-8, 0-padded), the bucket's offset and length, flags (bit 0: the
 * bucket holds every word with that prefix; else only the best, and longer keys
 * follow). A bucket: words by frequency, each a length byte and its bytes. */
typedef struct {
    char lang[4];
    bool tried;                         /* opened, or found missing */
    FIL f;
    uint32_t entries, chunks, index_at;
    uint8_t *keys;                      /* chunks * WL_KEY */
    int32_t chunk_no;                   /* the one in chunk[], -1 none */
    uint32_t bucket_at, bucket_len;     /* the one in bucket[] */
    uint8_t chunk[WL_CHUNK], bucket[WL_CHUNK];
} wordlist_t;

static wordlist_t *wl;

void words_close(void)
{
    if (!wl)
        return;
    if (wl->keys) {
        f_close(&wl->f);
        kfree(wl->keys);
    }
    kfree(wl);
    wl = NULL;
}

static bool read_at(uint32_t at, void *buf, uint32_t n)
{
    UINT got = 0;

    return f_lseek(&wl->f, at) == FR_OK && f_read(&wl->f, buf, n, &got) == FR_OK && got == n;
}

static bool list_open(const char *lang)
{
    char path[48];
    uint8_t h[32];

    if (wl && !strcmp(wl->lang, lang))
        return wl->keys != NULL;
    words_close();
    wl = kzalloc(sizeof(*wl));
    if (!wl)
        return false;
    snprintf(wl->lang, sizeof(wl->lang), "%s", lang);
    wl->tried = true;
    wl->chunk_no = -1;
    snprintf(path, sizeof(path), WL_DIR "/%s.wl", lang);
    if (f_open(&wl->f, path, FA_READ) != FR_OK)
        return false;
    if (!read_at(0, h, sizeof(h)) || memcmp(h, WL_MAGIC, 4) || (h[4] | h[5] << 8) != 1 ||
        (h[6] | h[7] << 8) != WL_KEY || !rd32(h + 12) || rd32(h + 12) > WL_CHUNKS) {
        f_close(&wl->f);
        klog("words: %s is not a word list", path);
        return false;
    }
    wl->entries = rd32(h + 8);
    wl->chunks = rd32(h + 12);
    wl->index_at = rd32(h + 20);
    wl->keys = kmalloc(wl->chunks * WL_KEY);
    if (!wl->keys || !read_at(rd32(h + 16), wl->keys, wl->chunks * WL_KEY)) {
        kfree(wl->keys);
        wl->keys = NULL;
        f_close(&wl->f);
        return false;
    }
    return true;
}

/* The last of n keys (WL_KEY apart, sorted) not after key; -1 if all are */
static int floor_key(const uint8_t *keys, int n, int stride, const uint8_t *key)
{
    int lo = 0, hi = n - 1, at = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (memcmp(keys + mid * stride, key, WL_KEY) <= 0) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return at;
}

static int list_find(const char *lang, const char *prefix, char (*out)[WORD_MAX], int have,
                     int max)
{
    uint8_t key[WL_KEY] = { 0 };
    size_t plen = strlen(prefix), klen = utf8_cut(prefix, plen < WL_KEY ? plen : WL_KEY);
    const uint8_t *e;
    int c, i, n;
    uint32_t at, len, ek;

    if (!list_open(lang) || !klen)
        return have;
    memcpy(key, prefix, klen);
    c = floor_key(wl->keys, (int)wl->chunks, WL_KEY, key);
    if (c < 0)
        return have;
    if (wl->chunk_no != c) {
        if (!read_at(wl->index_at + (uint32_t)c * WL_CHUNK, wl->chunk, WL_CHUNK))
            return have;
        wl->chunk_no = c;
    }
    n = (int)(wl->entries - (uint32_t)c * (WL_CHUNK / WL_ENTRY));
    if (n > WL_CHUNK / WL_ENTRY)
        n = WL_CHUNK / WL_ENTRY;
    i = floor_key(wl->chunk, n, WL_ENTRY, key);
    if (i < 0)
        return have;
    e = wl->chunk + i * WL_ENTRY;
    for (ek = 0; ek < WL_KEY && e[ek]; ek++)
        ;
    /* its key must start ours; a shorter one must hold every word under it */
    if (ek > klen || memcmp(e, key, ek) || (ek < klen && !(e[14] & 1)))
        return have;
    at = rd32(e + 8);
    len = e[12] | (uint32_t)e[13] << 8;
    if (len > WL_CHUNK)
        len = WL_CHUNK;
    if (wl->bucket_at != at || wl->bucket_len != len) {
        if (!read_at(at, wl->bucket, len))
            return have;
        wl->bucket_at = at;
        wl->bucket_len = len;
    }
    for (uint32_t p = 0; p < len && have < max; p += 1u + wl->bucket[p]) {
        uint32_t wn = wl->bucket[p];
        const char *w = (const char *)wl->bucket + p + 1;
        bool dup = false;

        if (p + 1 + wn > len || wn <= plen || wn >= WORD_MAX || memcmp(w, prefix, plen))
            continue;
        for (int k = 0; k < have && !dup; k++)
            dup = strlen(out[k]) == wn && !memcmp(out[k], w, wn);
        if (dup)
            continue;
        memcpy(out[have], w, wn);
        out[have++][wn] = 0;
    }
    return have;
}

int words_find(const char *lang, const char *prefix, char (*out)[WORD_MAX], int max)
{
    const uint8_t *best[4];
    int li = input_lang_find(lang), plen = (int)strlen(prefix), n = 0;

    if (max > 4)
        max = 4;
    for (const uint8_t *e = pool; li >= 0 && pool && e < pool + used; e += HEAD + e[0]) {
        int at;

        if ((e[1] & 0x7f) != li || e[0] <= plen || memcmp(e + HEAD, prefix, (size_t)plen))
            continue;
        for (at = n; at > 0 && uses_of(best[at - 1]) < uses_of(e); at--)   /* most used first */
            if (at < max)
                best[at] = best[at - 1];
        if (at < max) {
            best[at] = e;
            if (n < max)
                n++;
        }
    }
    for (int i = 0; i < n; i++) {
        memcpy(out[i], best[i] + HEAD, best[i][0]);
        out[i][best[i][0]] = 0;
    }
    return n < max ? list_find(lang, prefix, out, n, max) : n;
}
