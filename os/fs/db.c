/* B310E OS — the data store (include/os/db.h): the file, the settings in RAM,
 * the tables' indexes.
 *
 * The file: a 16-byte header ("B3DB", version, header size, a stamp new each
 * time the file is written anew, CRC-32 of those 12 bytes), then records:
 *
 *   kind u8, table u8, length u16, id u32, body (length bytes), CRC-32 u32
 *
 * little-endian, the CRC over the 8 header bytes and the body. Kinds:
 *   KV_SET  key length u8, key, type u8, value (int: 4 bytes, bool: 1,
 *           string and bytes: length u16, then the bytes)
 *   KV_DEL  key length u8, key
 *   T_DEF   the layout of table number `table`: version u16, fields u8, index
 *           i8, next id u32, name length u8, name, then each field's type u8,
 *           name length u8, name
 *   T_DROP  table number `table` is gone
 *   R_PUT   record `id` of a table: fields u8, then each field's type u8 and
 *           value as in KV_SET (a change is a new R_PUT: the last one counts)
 *   R_DEL   record `id` of a table removed
 *
 * The index holds where each record is or will be: past the end of the file
 * it is in the RAM buffer of records not yet written (pend), which goes to the
 * end of the file as it is, so the offsets stay right.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/db.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"

#define DB_NEW       DB_PATH "-new"
#define DB_BAD       DB_PATH "-bad"
#define MAGIC        0x42443342u        /* "B3DB" */
#define VERSION      1
#define HDR          16
#define REC_HDR      8
#define REC_OVER     12                 /* header and CRC */
#define BODY_MAX     (DB_REC_MAX + DB_KEY_MAX + 16)
#define FNAME        20                 /* a field's name, the NUL included */
#define FLUSH_MS     1000               /* settings: after the last set */
#define FLUSH_MAX_MS 5000               /* and at most after the first */
#define COMPACT_MIN  (16u * 1024)       /* dead bytes before a compaction is worth it */
#define PEND_MAX     (64u * 1024)       /* records waiting for the card */
#define BULK_MAX     8192               /* an import's records, written this many at a time */
#define CHUNK        4096
#define WATCHERS     8

enum { KV_SET = 1, KV_DEL, T_DEF, T_DROP, R_PUT, R_DEL };

/* A setting. off/len: its record in the file (0: none), so a new one makes it dead. */
typedef struct kv {
    struct kv *next;
    uint32_t off, len, noff;
    uint8_t type;
    bool dirty;                         /* changed since written */
    bool gone;                          /* deleted, the delete not written yet */
    int32_t i;
    uint32_t vlen;
    uint8_t *val;                       /* a string (NUL after it) or bytes */
    char key[];
} kv_t;

/* 12 bytes a record: the whole RAM cost of a table's record */
typedef struct {
    uint32_t id, off, key;              /* key: the index field's value, or its hash */
} ent_t;

struct db_table {
    char name[DB_NAME_MAX];
    uint8_t num;                        /* its number in the file, 0: not in it */
    uint8_t nfields;
    int8_t index;
    bool opened;                        /* someone holds it: keep it in the file */
    uint16_t version;
    uint8_t types[DB_FIELDS];
    char fnames[DB_FIELDS][FNAME];
    uint32_t def_off, def_len, ndef;
    ent_t *ent;
    uint16_t *lens;                     /* while loading: each record's length */
    uint32_t count, cap, next_id, gen, bytes;
    bool rekey;                         /* the index field changed: keys to redo */
};

typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
    uint32_t base;                      /* the file offset of buf[0] */
} obuf_t;

static struct {
    mutex_t lock;
    bool ready, file, away, bulk, kv_dirty;
    uint32_t size, dead, stamp;
    obuf_t pend;
    uint32_t flush_at, first_set;
    uint32_t writes, changes;
    uint8_t *scratch;                   /* one record's body and CRC, for one call */
    uint32_t scratch_cap;
    uint32_t retry_at;                  /* a failed write or compaction: not before */
    bool waited;                        /* the log thread wakes for what is due */
    bool made;                          /* the file was made at this boot */
    bool told_crc, told_io;
} db;

static kv_t *kvs;
static db_table_t *tables[DB_TABLES];
static FIL rf;
static bool rf_open;
static bool loading;                    /* records' lengths kept (tables' lens) */
/* Loading met no memory, or a read error: what RAM has is not the file, and
 * the file is not bad. It is left as it is (no compaction, no cut tail, not
 * data-bad) and the store runs in RAM only. */
static bool load_short;

static struct {
    char name[DB_NAME_MAX];
    bool table;
    void (*fn)(void *ctx);
    void *ctx;
    volatile bool due;
} watch[WATCHERS];
static volatile bool watch_due;

static int compact(bool keep_bad);
static uint32_t due_ms(void);

/* ---- bytes ----------------------------------------------------------------- */

static uint32_t get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t crc(const void *p, uint32_t n)
{
    return ~crc32_update(0xffffffffu, p, n);
}

static uint32_t fnv(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static bool has_nul(const void *p, uint32_t n)
{
    const uint8_t *b = p;

    while (n--)
        if (!*b++)
            return true;
    return false;
}

/* ---- locking, the card ------------------------------------------------------- */

static void lock(void)
{
    mutex_lock(&db.lock, WAIT_FOREVER);
}

/* The file opened for reading and the record scratch last for one call into
 * the store only: the scratch was 16.5 KB held from boot for records of a
 * few dozen bytes. Work made for the log thread while it waits for none
 * (a set, a failed table write) wakes it. */
static void unlock(void)
{
    if (db.lock.depth == 1) {
        if (rf_open) {
            f_close(&rf);
            rf_open = false;
        }
        kfree(db.scratch);
        db.scratch = NULL;
        db.scratch_cap = 0;
        if (!db.waited && due_ms() != WAIT_FOREVER) {
            db.waited = true;
            fs_kick();
        }
    }
    mutex_unlock(&db.lock);
}

static void close_rf(void)
{
    if (rf_open) {
        f_close(&rf);
        rf_open = false;
    }
}

static bool card_ok(void)
{
    return db.file && !db.away && fs_info()->mounted;
}

/* At least n bytes, in 256-byte steps so a call reading many records rarely
 * grows it; values decoded from the last record point into it, and nothing
 * reads a second record while those are in use. */
static bool scratch(uint32_t n)
{
    if (db.scratch && db.scratch_cap >= n)
        return true;
    kfree(db.scratch);
    n = (n + 255) & ~255u;
    db.scratch = kmalloc(n);
    db.scratch_cap = db.scratch ? n : 0;
    return db.scratch != NULL;
}

/* n bytes at off: from the file, or from pend past its end */
static int rd(uint32_t off, void *buf, uint32_t n)
{
    UINT got;

    if (off >= db.size) {
        off -= db.size;
        if (off + n > db.pend.len)
            return DB_EIO;
        memcpy(buf, db.pend.buf + off, n);
        return DB_OK;
    }
    if (off + n > db.size)
        return DB_EIO;
    if (!card_ok())
        return DB_ENOCARD;
    if (!rf_open) {
        if (f_open(&rf, DB_PATH, FA_READ) != FR_OK)
            return DB_EIO;
        rf_open = true;
    }
    if (f_lseek(&rf, off) != FR_OK || f_read(&rf, buf, n, &got) != FR_OK || got != n)
        return DB_EIO;
    return DB_OK;
}

/* The record at off: its header into h, its body into db.scratch, the CRC checked */
static int rec_read(uint32_t off, uint8_t *h, uint32_t *len)
{
    uint32_t l;
    int r = rd(off, h, REC_HDR);

    if (r)
        return r;
    l = get16(h + 2);
    if (l > BODY_MAX)
        return DB_EIO;
    if (!scratch(l + 4))
        return DB_ENOMEM;
    if ((r = rd(off + REC_HDR, db.scratch, l + 4)))
        return r;
    if (~crc32_update(crc32_update(0xffffffffu, h, REC_HDR), db.scratch, l) != get32(db.scratch + l)) {
        if (!db.told_crc)
            klog("db: record at %lu: bad CRC", (unsigned long)off);
        db.told_crc = true;
        return DB_EIO;
    }
    *len = l;
    return DB_OK;
}

/* ---- writing records ----------------------------------------------------------- */

static bool ob_reserve(obuf_t *o, uint32_t n, uint32_t max)
{
    uint8_t *p;
    uint32_t cap;

    if (o->len + n <= o->cap)
        return true;
    if (o->len + n > max)
        return false;
    cap = (o->len + n + 1023) & ~1023u;
    if (!(p = kmalloc(cap)))
        return false;
    if (o->buf) {
        memcpy(p, o->buf, o->len);
        kfree(o->buf);
    }
    o->buf = p;
    o->cap = cap;
    return true;
}

static bool pend_reserve(uint32_t n)
{
    return ob_reserve(&db.pend, n, PEND_MAX + BODY_MAX + REC_OVER);
}

static uint8_t *wp;                     /* where the record being made goes on */
static uint32_t rec_at;

static void rec_begin(obuf_t *o, int kind, int tnum, uint32_t id)
{
    rec_at = o->len;
    wp = o->buf + o->len;
    wp[0] = (uint8_t)kind;
    wp[1] = (uint8_t)tnum;
    put32(wp + 4, id);
    wp += REC_HDR;
}

static void w8(uint32_t v)
{
    *wp++ = (uint8_t)v;
}

static void wn(const void *p, uint32_t n)
{
    if (n)
        memcpy(wp, p, n);
    wp += n;
}

/* Its file offset; the record is in o */
static uint32_t rec_end(obuf_t *o)
{
    uint8_t *r = o->buf + rec_at;
    uint32_t len = (uint32_t)(wp - r) - REC_HDR;

    put16(r + 2, len);
    put32(wp, crc(r, REC_HDR + len));
    o->len = rec_at + REC_OVER + len;
    return o->base + rec_at;
}

static uint32_t val_size(int type, uint32_t len)
{
    return type == DB_INT ? 4 : type == DB_BOOL ? 1 : 2 + len;
}

static void val_put(int type, int32_t i, const void *p, uint32_t len)
{
    if (type == DB_INT) {
        put32(wp, (uint32_t)i);
        wp += 4;
    } else if (type == DB_BOOL) {
        w8(i != 0);
    } else {
        put16(wp, len);
        wp += 2;
        wn(p, len);
    }
}

/* One value at *p (the body ends at e) */
static bool val_get(const uint8_t **p, const uint8_t *e, int type, db_value_t *v)
{
    const uint8_t *q = *p;

    v->i = 0;
    v->p = "";
    v->len = 0;
    if (type == DB_INT) {
        if (e - q < 4)
            return false;
        v->i = (int32_t)get32(q);
        q += 4;
    } else if (type == DB_BOOL) {
        if (e - q < 1)
            return false;
        v->i = *q++ != 0;
    } else if (type == DB_STR || type == DB_BLOB) {
        if (e - q < 2 || (uint32_t)(e - q - 2) < get16(q))
            return false;
        v->len = get16(q);
        v->p = q + 2;
        q += 2 + v->len;
    } else {
        return false;
    }
    *p = q;
    return true;
}

/* ---- change notification --------------------------------------------------------- */

static void wake(bool any)
{
    if (any) {
        watch_due = true;
        keypad_inject(0, false);        /* the UI loop: db_dispatch */
    }
}

static bool starts(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return false;
    return true;
}

static void changed_key(const char *key)
{
    bool any = false;

    db.changes++;
    for (int i = 0; i < WATCHERS; i++)
        if (watch[i].fn && !watch[i].table && starts(key, watch[i].name)) {
            watch[i].due = true;
            any = true;
        }
    wake(any);
}

static void changed_table(db_table_t *t)
{
    bool any = false;

    db.changes++;
    t->gen++;
    for (int i = 0; i < WATCHERS; i++)
        if (watch[i].fn && watch[i].table && !strcmp(t->name, watch[i].name)) {
            watch[i].due = true;
            any = true;
        }
    wake(any);
}

static void changed_all(void)
{
    bool any = false;

    db.changes++;
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i])
            tables[i]->gen++;
    for (int i = 0; i < WATCHERS; i++)
        if (watch[i].fn) {
            watch[i].due = true;
            any = true;
        }
    wake(any);
}

bool db_watch(const char *name, bool table, void (*fn)(void *ctx), void *ctx)
{
    for (int i = 0; i < WATCHERS; i++)
        if (!watch[i].fn && strlen(name) < DB_NAME_MAX) {
            strncpy(watch[i].name, name, DB_NAME_MAX);
            watch[i].table = table;
            watch[i].ctx = ctx;
            watch[i].fn = fn;
            return true;
        }
    return false;
}

void db_dispatch(void)
{
    if (!watch_due)
        return;
    watch_due = false;
    for (int i = 0; i < WATCHERS; i++)
        if (watch[i].due) {
            watch[i].due = false;
            watch[i].fn(watch[i].ctx);
        }
}

uint32_t db_changes(void)
{
    return db.changes;
}

/* ---- settings in RAM --------------------------------------------------------------- */

/* The list again as an array in the same (strcmp) order, for a binary
 * search: walking the list took 16.5 strcmp a lookup (QEMU settings-set,
 * 14.5k strcmp for 884 lookups), about 5 this way with 32 keys. */
static kv_t **kv_ix;
static unsigned kv_n, kv_cap;

/* the first place whose key is not below key; *hit when it is key */
static unsigned kv_pos(const char *key, bool *hit)
{
    unsigned lo = 0, hi = kv_n;

    *hit = false;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(kv_ix[mid]->key, key);

        if (!c) {
            *hit = true;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static kv_t *kv_find(const char *key)
{
    bool hit;
    unsigned at = kv_pos(key, &hit);

    return hit ? kv_ix[at] : NULL;
}

static kv_t *kv_live(const char *key)
{
    kv_t *k = kv_find(key);

    return k && !k->gone ? k : NULL;
}

/* key is not there yet (kv_find said so) */
static kv_t *kv_add(const char *key, size_t kl)
{
    kv_t *k = kzalloc(sizeof(kv_t) + kl + 1);
    unsigned at;
    bool hit;

    if (k && kv_n == kv_cap) {          /* no realloc in the kernel: a bigger one */
        kv_t **ix = kmalloc((kv_cap + 32) * sizeof(*ix));

        if (ix) {
            if (kv_n)
                memcpy(ix, kv_ix, kv_n * sizeof(*ix));
            kfree(kv_ix);
            kv_ix = ix;
            kv_cap += 32;
        } else {
            kfree(k);
            k = NULL;
        }
    }
    if (!k) {
        load_short |= loading;
        return NULL;
    }
    memcpy(k->key, key, kl + 1);
    at = kv_pos(k->key, &hit);          /* in order: listings, prefixes */
    k->next = at < kv_n ? kv_ix[at] : NULL;
    if (at)
        kv_ix[at - 1]->next = k;
    else
        kvs = k;
    memmove(kv_ix + at + 1, kv_ix + at, (kv_n - at) * sizeof(*kv_ix));
    kv_ix[at] = k;
    kv_n++;
    return k;
}

static void kv_free(kv_t *k)
{
    bool hit;
    unsigned at = kv_pos(k->key, &hit);

    if (hit && kv_ix[at] == k) {
        if (at)
            kv_ix[at - 1]->next = k->next;
        else
            kvs = k->next;
        kv_n--;
        memmove(kv_ix + at, kv_ix + at + 1, (kv_n - at) * sizeof(*kv_ix));
    }
    kfree(k->val);
    kfree(k);
}

static bool kv_value(kv_t *k, int type, int32_t i, const void *p, uint32_t n)
{
    if (type == DB_STR || type == DB_BLOB) {
        if (!k->val || k->vlen != n) {
            uint8_t *v = kmalloc(n + 1);

            if (!v) {
                load_short |= loading;
                return false;
            }
            kfree(k->val);
            k->val = v;
        }
        memcpy(k->val, p, n);
        k->val[n] = 0;
        k->vlen = n;
        i = 0;
    } else {
        kfree(k->val);
        k->val = NULL;
        k->vlen = 0;
        if (type == DB_BOOL)
            i = i != 0;
    }
    k->type = (uint8_t)type;
    k->i = i;
    return true;
}

static bool kv_same(const kv_t *k, int type, int32_t i, const void *p, uint32_t n)
{
    if (k->type != type)
        return false;
    if (type == DB_STR || type == DB_BLOB)
        return k->vlen == n && (!n || !memcmp(k->val, p, n));
    return k->i == (type == DB_BOOL ? i != 0 : i);
}

/* Each dirty setting as a record in pend */
static int kv_encode(void)
{
    for (kv_t *k = kvs, *next; k; k = next) {
        uint32_t kl = strlen(k->key), need;

        next = k->next;
        if (!k->dirty)
            continue;
        if (k->gone) {
            if (k->off) {
                need = REC_OVER + 1 + kl;
                if (!pend_reserve(need))
                    return DB_ENOMEM;
                rec_begin(&db.pend, KV_DEL, 0, 0);
                w8(kl);
                wn(k->key, kl);
                rec_end(&db.pend);
                db.dead += k->len + need;   /* both go at the next compaction */
            }
            kv_free(k);
            continue;
        }
        need = REC_OVER + 2 + kl + val_size(k->type, k->vlen);
        if (!pend_reserve(need))
            return DB_ENOMEM;
        rec_begin(&db.pend, KV_SET, 0, 0);
        w8(kl);
        wn(k->key, kl);
        w8(k->type);
        val_put(k->type, k->i, k->val, k->vlen);
        if (k->off)
            db.dead += k->len;
        k->off = rec_end(&db.pend);
        k->len = need;
        k->dirty = false;
    }
    db.kv_dirty = false;
    return DB_OK;
}

/* Write what waits in RAM to the end of the file. */
static int flush(void)
{
    FIL f;
    UINT n = 0;
    FRESULT r;
    int e;

    if (!card_ok())
        return DB_ENOCARD;
    if (db.kv_dirty && (e = kv_encode()))
        return e;
    if (!db.pend.len)
        return DB_OK;
    close_rf();
    r = f_open(&f, DB_PATH, FA_WRITE | FA_OPEN_ALWAYS);
    if (r == FR_OK) {
        if (f_size(&f) < db.size)       /* shorter than we know it: not ours */
            r = FR_INT_ERR;
        if (r == FR_OK)
            r = f_lseek(&f, db.size);
        if (r == FR_OK)
            r = f_write(&f, db.pend.buf, db.pend.len, &n);
        if (r == FR_OK && n != db.pend.len)
            r = FR_DENIED;              /* the card is full */
        if (r == FR_OK && f_size(&f) > db.size + db.pend.len)
            r = f_truncate(&f);         /* a torn write's leftover past the end */
        if (f_close(&f) != FR_OK && r == FR_OK)
            r = FR_DISK_ERR;
    }
    db.writes++;
    if (r != FR_OK) {
        if (!db.told_io)
            klog("db: write of %lu bytes failed (%d); kept in RAM", (unsigned long)db.pend.len,
                 (int)r);
        db.told_io = true;
        return DB_EIO;
    }
    db.told_io = false;
    db.size += db.pend.len;
    db.pend.len = 0;
    db.pend.base = db.size;
    if (db.pend.cap > BULK_MAX) {       /* an import's: give it back */
        kfree(db.pend.buf);
        db.pend.buf = NULL;
        db.pend.cap = 0;
    }
    return DB_OK;
}

/* Table writes go out at once, an import's in batches. */
static void write_now(void)
{
    if (!db.bulk || db.pend.len >= BULK_MAX)
        flush();
}

static void arm(void)
{
    uint32_t now = kernel_ticks();

    if (!db.flush_at)
        db.first_set = now;
    db.flush_at = now + FLUSH_MS;
    if ((int32_t)(db.flush_at - (db.first_set + FLUSH_MAX_MS)) > 0)
        db.flush_at = db.first_set + FLUSH_MAX_MS;
    if (!db.flush_at)
        db.flush_at = 1;
}

static bool key_ok(const char *key, size_t *kl)
{
    *kl = key ? strlen(key) : 0;
    return *kl && *kl < DB_KEY_MAX && utf8_valid(key, *kl);
}

static int kv_set(const char *key, int type, int32_t i, const void *p, uint32_t n)
{
    size_t kl;
    kv_t *k;

    if (!key_ok(key, &kl) || (n && !p))
        return DB_EINVAL;
    if (n > DB_REC_MAX)
        return DB_ETOOBIG;
    if (type == DB_STR && (!utf8_valid(p, n) || has_nul(p, n)))
        return DB_EINVAL;
    if (!db.ready)
        return DB_ENOCARD;
    lock();
    k = kv_find(key);
    if (k && !k->gone && kv_same(k, type, i, p, n)) {
        unlock();                       /* no change: nothing to write */
        return DB_OK;
    }
    if (!k) {
        if (!(k = kv_add(key, kl))) {
            unlock();
            return DB_ENOMEM;
        }
        /* A new entry whose value cannot be stored must not stay: it would
         * be listed (type 0) and never written. kv_free takes it out of
         * the list and the sorted index (os/tests/test-host.py db-oom). */
        if (!kv_value(k, type, i, p, n)) {
            kv_free(k);
            unlock();
            return DB_ENOMEM;
        }
    } else if (!kv_value(k, type, i, p, n)) {
        unlock();
        return DB_ENOMEM;
    }
    k->gone = false;
    k->dirty = db.kv_dirty = true;
    arm();
    changed_key(key);
    unlock();
    return DB_OK;
}

int db_set_int(const char *key, int v)
{
    return kv_set(key, DB_INT, v, NULL, 0);
}

int db_set_bool(const char *key, bool v)
{
    return kv_set(key, DB_BOOL, v, NULL, 0);
}

int db_set_str(const char *key, const char *s)
{
    return s ? kv_set(key, DB_STR, 0, s, strlen(s)) : DB_EINVAL;
}

int db_set_blob(const char *key, const void *p, size_t n)
{
    return kv_set(key, DB_BLOB, 0, p, n);
}

int db_delete(const char *key)
{
    kv_t *k;

    if (!db.ready || !key)
        return DB_ENOENT;
    lock();
    k = kv_live(key);
    if (k) {
        k->gone = k->dirty = db.kv_dirty = true;
        arm();
        changed_key(key);
    }
    unlock();
    return k ? DB_OK : DB_ENOENT;
}

int db_get_int(const char *key, int def)
{
    kv_t *k;

    if (!db.ready || !key)
        return def;
    lock();
    k = kv_live(key);
    if (k && (k->type == DB_INT || k->type == DB_BOOL))
        def = k->i;
    unlock();
    return def;
}

bool db_get_bool(const char *key, bool def)
{
    return db_get_int(key, def) != 0;
}

int db_get_str(const char *key, char *out, size_t n, const char *def)
{
    const char *s = def ? def : "";
    size_t len, c;
    kv_t *k = NULL;

    if (db.ready && key) {
        lock();
        k = kv_live(key);
        if (k && k->type != DB_STR)
            k = NULL;
    }
    if (k)
        s = (const char *)k->val;
    len = k ? k->vlen : strlen(s);
    if (out && n) {
        c = len < n ? len : utf8_cut(s, n - 1);
        memcpy(out, s, c);
        out[c] = 0;
    }
    if (db.ready && key)
        unlock();
    return (int)len;
}

int db_get_blob(const char *key, void *out, size_t n)
{
    int len = DB_ENOENT;
    kv_t *k;

    if (!db.ready || !key)
        return len;
    lock();
    k = kv_live(key);
    if (k && k->type == DB_BLOB) {
        len = (int)k->vlen;
        memcpy(out, k->val, k->vlen < n ? k->vlen : n);
    }
    unlock();
    return len;
}

int db_type(const char *key)
{
    kv_t *k;
    int t;

    if (!db.ready || !key)
        return DB_NONE;
    lock();
    k = kv_live(key);
    t = k ? k->type : DB_NONE;
    unlock();
    return t;
}

bool db_key_next(const char *after, char *out)
{
    bool found = false;
    kv_t *k0;

    if (!db.ready)
        return false;
    lock();
    {
        bool hit;
        unsigned at = kv_pos(after, &hit);

        at += hit;                      /* the keys after it are the list from there */
        k0 = at < kv_n ? kv_ix[at] : NULL;
    }
    for (kv_t *k = k0; k; k = k->next)
        if (!k->gone) {
            strncpy(out, k->key, DB_KEY_MAX);
            found = true;
            break;
        }
    unlock();
    return found;
}

/* ---- tables ----------------------------------------------------------------------- */

static db_table_t *by_num(int num)
{
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i] && tables[i]->num == num)
            return tables[i];
    return NULL;
}

static db_table_t *by_name(const char *name)
{
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i] && !strcmp(tables[i]->name, name))
            return tables[i];
    return NULL;
}

static bool valid(const db_table_t *t)
{
    for (int i = 0; t && i < DB_TABLES; i++)
        if (tables[i] == t)
            return true;
    return false;
}

static db_table_t *table_new(const char *name)
{
    for (int i = 0; i < DB_TABLES; i++)
        if (!tables[i]) {
            db_table_t *t = kzalloc(sizeof(*t));

            load_short |= loading && !t;
            if (t) {
                strncpy(t->name, name, DB_NAME_MAX - 1);
                t->next_id = 1;
                t->index = -1;
                tables[i] = t;
            }
            return t;
        }
    return NULL;
}

static void table_clear(db_table_t *t)
{
    t->num = 0;
    t->count = 0;
    t->def_off = t->def_len = 0;
    t->bytes = 0;
    t->next_id = 1;
}

/* Where id is, or -(where it would go) - 1 */
static int ent_pos(const db_table_t *t, uint32_t id)
{
    int lo = 0, hi = (int)t->count - 1;

    while (lo <= hi) {
        int m = (lo + hi) / 2;

        if (t->ent[m].id == id)
            return m;
        if (t->ent[m].id < id)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -lo - 1;
}

static uint32_t ent_len(const db_table_t *t, int i)
{
    uint8_t h[REC_HDR];

    if (t->lens)
        return t->lens[i];
    return rd(t->ent[i].off, h, REC_HDR) ? 0 : REC_OVER + get16(h + 2);
}

static void gone_bytes(db_table_t *t, uint32_t n)
{
    db.dead += n;
    t->bytes = t->bytes > n ? t->bytes - n : 0;
}

static bool ent_grow(db_table_t *t)
{
    uint32_t cap = t->cap ? t->cap * 2 : 16;
    ent_t *e = kmalloc(cap * sizeof(ent_t));
    uint16_t *l = NULL;
    bool lens = t->lens || (loading && !t->count);

    if (!e || (lens && !(l = kmalloc(cap * sizeof(uint16_t))))) {
        kfree(e);
        load_short |= loading;
        return false;
    }
    if (t->count)
        memcpy(e, t->ent, t->count * sizeof(ent_t));
    kfree(t->ent);
    t->ent = e;
    if (l) {
        if (t->count && t->lens)
            memcpy(l, t->lens, t->count * sizeof(uint16_t));
        kfree(t->lens);
        t->lens = l;
    }
    t->cap = cap;
    return true;
}

static bool ent_put(db_table_t *t, uint32_t id, uint32_t off, uint32_t key, uint32_t len)
{
    int i = ent_pos(t, id);

    if (i >= 0) {
        gone_bytes(t, ent_len(t, i));
    } else {
        i = -i - 1;
        if (t->count == t->cap && !ent_grow(t))
            return false;
        memmove(&t->ent[i + 1], &t->ent[i], (t->count - (uint32_t)i) * sizeof(ent_t));
        if (t->lens)
            memmove(&t->lens[i + 1], &t->lens[i], (t->count - (uint32_t)i) * sizeof(uint16_t));
        t->count++;
    }
    t->ent[i].id = id;
    t->ent[i].off = off;
    t->ent[i].key = key;
    if (t->lens)
        t->lens[i] = (uint16_t)len;
    t->bytes += len;
    if (id >= t->next_id)
        t->next_id = id + 1;
    return true;
}

static void ent_del(db_table_t *t, int i)
{
    gone_bytes(t, ent_len(t, i));
    memmove(&t->ent[i], &t->ent[i + 1], (t->count - (uint32_t)i - 1) * sizeof(ent_t));
    if (t->lens)
        memmove(&t->lens[i], &t->lens[i + 1], (t->count - (uint32_t)i - 1) * sizeof(uint16_t));
    t->count--;
}

/* A record's fields into v[0..n): missing ones, and ones of another type, 0 or empty */
static bool rec_decode(const db_table_t *t, const uint8_t *b, uint32_t len, db_value_t *v, int n)
{
    const uint8_t *p = b, *e = b + len;
    int nf;

    for (int i = 0; i < n; i++) {
        v[i].i = 0;
        v[i].p = "";
        v[i].len = 0;
    }
    if (!len)
        return false;
    nf = *p++;
    for (int i = 0; i < nf; i++) {
        db_value_t x;
        int type;

        if (p >= e)
            return false;
        type = *p++;
        if (!val_get(&p, e, type, &x))
            return false;
        if (i < n && i < t->nfields && type == t->types[i])
            v[i] = x;
    }
    return true;
}

static uint32_t key_of(int type, const db_value_t *v)
{
    if (type == DB_STR || type == DB_BLOB)
        return fnv(v->p, v->len);
    return type == DB_BOOL ? v->i != 0 : (uint32_t)v->i;
}

static uint32_t rec_key(const db_table_t *t, const uint8_t *b, uint32_t len)
{
    db_value_t v[DB_FIELDS];

    if (t->index < 0 || !rec_decode(t, b, len, v, t->nfields))
        return 0;
    return key_of(t->types[t->index], &v[t->index]);
}

static void rekey(db_table_t *t)
{
    uint8_t h[REC_HDR];
    uint32_t len;

    for (uint32_t i = 0; i < t->count; i++)
        if (rec_read(t->ent[i].off, h, &len) == DB_OK)
            t->ent[i].key = rec_key(t, db.scratch, len);
    t->rekey = false;
}

static void def_put(obuf_t *o, db_table_t *t)
{
    uint32_t nl = strlen(t->name);

    rec_begin(o, T_DEF, t->num, 0);
    w8(t->version);
    w8(t->version >> 8);
    w8(t->nfields);
    w8((uint8_t)t->index);
    put32(wp, t->next_id);
    wp += 4;
    w8(nl);
    wn(t->name, nl);
    for (int i = 0; i < t->nfields; i++) {
        uint32_t fl = strlen(t->fnames[i]);

        w8(t->types[i]);
        w8(fl);
        wn(t->fnames[i], fl);
    }
}

static uint32_t def_size(const db_table_t *t)
{
    uint32_t n = REC_OVER + 9 + strlen(t->name);

    for (int i = 0; i < t->nfields; i++)
        n += 2 + strlen(t->fnames[i]);
    return n;
}

static int def_write(db_table_t *t)
{
    uint32_t need = def_size(t);

    if (!pend_reserve(need))
        return DB_ENOMEM;
    def_put(&db.pend, t);
    if (t->def_off)
        db.dead += t->def_len;
    t->def_off = rec_end(&db.pend);
    t->def_len = need;
    return DB_OK;
}

static int free_num(void)
{
    for (int n = 1; n < 256; n++)
        if (!by_num(n))
            return n;
    return 0;
}

static void set_schema(db_table_t *t, const db_schema_t *s)
{
    t->version = s->version;
    t->nfields = s->nfields;
    t->index = s->index;
    memset(t->fnames, 0, sizeof(t->fnames));
    for (int i = 0; i < s->nfields; i++) {
        t->types[i] = s->fields[i].type;
        strncpy(t->fnames[i], s->fields[i].name, FNAME - 1);
    }
}

static bool schema_ok(const db_schema_t *s)
{
    size_t nl = s && s->name ? strlen(s->name) : 0;

    if (!nl || nl >= DB_NAME_MAX || !utf8_valid(s->name, nl) || !s->nfields ||
        s->nfields > DB_FIELDS || s->index >= s->nfields || !s->fields)
        return false;
    for (int i = 0; i < s->nfields; i++) {
        size_t fl = s->fields[i].name ? strlen(s->fields[i].name) : 0;

        if (!fl || fl >= FNAME || s->fields[i].type < DB_INT || s->fields[i].type > DB_BLOB)
            return false;
    }
    return true;
}

db_table_t *db_table_open(const db_schema_t *s)
{
    void (*up)(db_table_t *, unsigned) = NULL;
    unsigned from = 0;
    db_table_t *t;

    if (!schema_ok(s) || !db.ready)
        return NULL;
    lock();
    t = by_name(s->name);
    if (t && t->num) {
        if (s->version > t->version && card_ok()) {     /* fields added: a new layout */
            bool rekeyed = t->index != s->index;

            from = t->version;
            set_schema(t, s);
            if (def_write(t) == DB_OK) {
                flush();
                if (rekeyed)
                    rekey(t);
                up = s->upgrade;
                klog("db: %s from version %u to %u", t->name, from, t->version);
            }
        }
        t->opened = true;
        unlock();
        if (up)
            up(t, from);
        return t;
    }
    if (!card_ok() || (!t && !(t = table_new(s->name)))) {
        unlock();
        return NULL;
    }
    table_clear(t);
    set_schema(t, s);
    t->num = (uint8_t)free_num();
    if (!t->num || def_write(t) != DB_OK) {
        t->num = 0;
        unlock();
        return NULL;
    }
    t->opened = true;
    flush();
    changed_table(t);
    unlock();
    return t;
}

static int table_ok(db_table_t *t)
{
    if (!db.ready || !valid(t) || !t->num)
        return DB_ENOENT;
    return card_ok() ? DB_OK : DB_ENOCARD;
}

/* A record of t's fields from v (n of them; the rest 0 / empty) as id */
static int put_rec(db_table_t *t, uint32_t id, const db_value_t *v, int n)
{
    static const db_value_t none = { 0, "", 0 };
    uint32_t need = 1, off;

    for (int i = 0; i < t->nfields; i++) {
        const db_value_t *x = i < n ? &v[i] : &none;
        int type = t->types[i];

        if (type == DB_STR || type == DB_BLOB) {
            if (x->len > DB_REC_MAX)
                return DB_ETOOBIG;
            if (x->len && !x->p)
                return DB_EINVAL;
            if (type == DB_STR && (!utf8_valid(x->p, x->len) || has_nul(x->p, x->len)))
                return DB_EINVAL;
        }
        need += 1 + val_size(type, x->len);
    }
    if (need > DB_REC_MAX)
        return DB_ETOOBIG;
    if (!pend_reserve(need + REC_OVER))
        return DB_ENOMEM;
    rec_begin(&db.pend, R_PUT, t->num, id);
    w8(t->nfields);
    for (int i = 0; i < t->nfields; i++) {
        const db_value_t *x = i < n ? &v[i] : &none;

        w8(t->types[i]);
        val_put(t->types[i], x->i, x->p, x->len);
    }
    off = rec_end(&db.pend);
    if (!ent_put(t, id, off, t->index < 0 ? 0 :
                 key_of(t->types[t->index], t->index < n ? &v[t->index] : &none),
                 need + REC_OVER)) {
        db.pend.len -= need + REC_OVER;     /* no room in the index: not written */
        return DB_ENOMEM;
    }
    changed_table(t);
    write_now();
    return DB_OK;
}

uint32_t db_insert_id(db_table_t *t, uint32_t id, const db_value_t *v, int n)
{
    int r;

    if (!id || !v)
        return 0;
    lock();
    r = table_ok(t);
    if (!r)
        r = put_rec(t, id, v, n);
    unlock();
    return r ? 0 : id;
}

uint32_t db_insert(db_table_t *t, const db_value_t *v, int n)
{
    uint32_t id = 0;

    lock();
    if (!table_ok(t))
        id = db_insert_id(t, t->next_id, v, n);
    unlock();
    return id;
}

int db_get(db_table_t *t, uint32_t id, db_value_t *v, int n, void *buf, size_t size)
{
    uint8_t h[REC_HDR], *out = buf;
    uint32_t len, used = 0;
    int r, i;

    lock();
    r = !db.ready || !valid(t) || !t->num ? DB_ENOENT : DB_OK;
    if (!r)
        r = (i = ent_pos(t, id)) < 0 ? DB_ENOENT : rec_read(t->ent[i].off, h, &len);
    if (!r && !rec_decode(t, db.scratch, len, v, n))
        r = DB_EIO;
    for (i = 0; !r && i < n; i++) {
        int type = i < t->nfields ? t->types[i] : DB_NONE;
        uint32_t need = v[i].len + (type == DB_STR);

        if (type != DB_STR && type != DB_BLOB)
            continue;
        if (used + need > size) {
            r = DB_ETOOBIG;
            break;
        }
        memcpy(out + used, v[i].p, v[i].len);
        if (type == DB_STR)
            out[used + v[i].len] = 0;
        v[i].p = out + used;
        used += need;
    }
    unlock();
    return r;
}

static bool same(int type, const db_value_t *a, const db_value_t *b)
{
    if (type == DB_STR || type == DB_BLOB)
        return a->len == b->len && (!a->len || !memcmp(a->p, b->p, a->len));
    return type == DB_BOOL ? (a->i != 0) == (b->i != 0) : a->i == b->i;
}

int db_find(db_table_t *t, int field, const db_value_t *value, uint32_t *ids, int max,
            uint32_t before)
{
    db_value_t v[DB_FIELDS];
    uint8_t h[REC_HDR];
    uint32_t key = 0, len;
    int got = 0, i, type = 0;

    lock();
    if (!db.ready || !valid(t) || !t->num || field >= t->nfields || (field >= 0 && !value)) {
        unlock();
        return 0;
    }
    if (field >= 0) {
        type = t->types[field];
        key = key_of(type, value);
    }
    i = before ? ent_pos(t, before) : (int)t->count;
    if (i < 0)
        i = -i - 1;
    while (--i >= 0 && got < max) {
        const ent_t *e = &t->ent[i];

        if (field >= 0) {
            bool indexed = field == t->index;

            if (indexed && e->key != key)
                continue;
            /* a string's key is a hash, and other fields are not in RAM: read it */
            if ((!indexed || type == DB_STR || type == DB_BLOB) &&
                (rec_read(e->off, h, &len) || !rec_decode(t, db.scratch, len, v, t->nfields) ||
                 !same(type, &v[field], value)))
                continue;
        }
        ids[got++] = e->id;
    }
    unlock();
    return got;
}

int db_update(db_table_t *t, uint32_t id, const db_value_t *v, int n)
{
    int r;

    lock();
    r = table_ok(t);
    if (!r && ent_pos(t, id) < 0)
        r = DB_ENOENT;
    if (!r)
        r = put_rec(t, id, v, n);
    unlock();
    return r;
}

int db_update_field(db_table_t *t, uint32_t id, int field, const db_value_t *v)
{
    db_value_t vals[DB_FIELDS];
    uint8_t h[REC_HDR];
    uint32_t len;
    int r, i;

    lock();
    r = table_ok(t);
    if (!r && (field < 0 || field >= t->nfields || !v))
        r = DB_EINVAL;
    if (!r)
        r = (i = ent_pos(t, id)) < 0 ? DB_ENOENT : rec_read(t->ent[i].off, h, &len);
    if (!r && !rec_decode(t, db.scratch, len, vals, t->nfields))
        r = DB_EIO;
    if (!r) {
        vals[field] = *v;               /* the others point into scratch: put_rec copies */
        r = put_rec(t, id, vals, t->nfields);
    }
    unlock();
    return r;
}

int db_remove(db_table_t *t, uint32_t id)
{
    int r, i = -1;

    lock();
    r = table_ok(t);
    if (!r && (i = ent_pos(t, id)) < 0)
        r = DB_ENOENT;
    if (!r && !pend_reserve(REC_OVER))
        r = DB_ENOMEM;
    if (!r) {
        rec_begin(&db.pend, R_DEL, t->num, id);
        rec_end(&db.pend);
        db.dead += REC_OVER;
        ent_del(t, i);
        changed_table(t);
        write_now();
    }
    unlock();
    return r;
}

int db_count(db_table_t *t)
{
    return db.ready && valid(t) ? (int)t->count : 0;
}

uint32_t db_table_gen(db_table_t *t)
{
    return valid(t) ? t->gen : 0;
}

const char *db_table_name(db_table_t *t)
{
    return valid(t) ? t->name : "";
}

uint32_t db_table_bytes(db_table_t *t)
{
    return valid(t) ? t->bytes + t->def_len : 0;
}

int db_table_list(db_table_t **out, int max)
{
    int n = 0;

    for (int i = 0; i < DB_TABLES && n < max; i++)
        if (tables[i] && tables[i]->num)
            out[n++] = tables[i];
    return n;
}

db_table_t *db_table_find(const char *name)
{
    db_table_t *t = name ? by_name(name) : NULL;

    return t && t->num ? t : NULL;
}

int db_table_layout(db_table_t *t, db_field_t *f, int max, int *version, int *index)
{
    if (!valid(t))
        return 0;
    for (int i = 0; i < t->nfields && i < max; i++) {
        f[i].name = t->fnames[i];
        f[i].type = t->types[i];
    }
    if (version)
        *version = t->version;
    if (index)
        *index = t->index;
    return t->nfields;
}

void db_stats(uint32_t *size, uint32_t *dead, int *keys)
{
    int n = 0;

    lock();
    for (kv_t *k = kvs; k; k = k->next)
        n += !k->gone;
    *size = db.size + db.pend.len;
    *dead = db.dead;
    *keys = n;
    unlock();
}

uint32_t db_writes(void)
{
    return db.writes;
}

/* ---- loading ----------------------------------------------------------------------- */

static void apply_kv(int kind, const uint8_t *b, uint32_t len, uint32_t off, uint32_t total)
{
    const uint8_t *p = b + 2, *e = b + len;
    char key[DB_KEY_MAX];
    db_value_t v;
    uint32_t kl = len ? b[0] : 0;
    int type = 0;
    kv_t *k;
    bool ok = kl && kl < DB_KEY_MAX && kl + 1 <= len;

    if (ok && kind == KV_SET) {
        p += kl;
        ok = kl + 2 <= len && val_get(&p, e, type = b[1 + kl], &v);
    }
    if (!ok) {
        db.dead += total;
        return;
    }
    memcpy(key, b + 1, kl);
    key[kl] = 0;
    k = kv_find(key);
    if (kind == KV_DEL) {
        db.dead += total;
        if (k) {
            db.dead += k->len;
            k->off = k->len = 0;
            if (!k->dirty)
                kv_free(k);
        }
        return;
    }
    if (!k && !(k = kv_add(key, kl))) {
        db.dead += total;
        return;
    }
    if (k->off)
        db.dead += k->len;
    k->off = off;
    k->len = total;
    if (!k->dirty && !kv_value(k, type, v.i, v.p, v.len))
        kv_free(k);
}

static void apply_def(int num, const uint8_t *b, uint32_t len, uint32_t off, uint32_t total)
{
    const uint8_t *p = b + 9, *e = b + len;
    db_schema_t s;
    db_field_t f[DB_FIELDS];
    char name[DB_NAME_MAX], fn[DB_FIELDS][FNAME];
    db_table_t *t, *old;
    uint32_t nl;

    if (len < 9 || !num)
        goto bad;
    s.version = (uint16_t)get16(b);
    s.nfields = b[2];
    s.index = (int8_t)b[3];
    nl = b[8];
    if (!nl || nl >= DB_NAME_MAX || (uint32_t)(e - p) < nl || s.nfields > DB_FIELDS)
        goto bad;
    memcpy(name, p, nl);
    name[nl] = 0;
    p += nl;
    for (int i = 0; i < s.nfields; i++) {
        uint32_t fl;

        if (e - p < 2 || (fl = p[1]) >= FNAME || (uint32_t)(e - p - 2) < fl)
            goto bad;
        f[i].type = p[0];
        memcpy(fn[i], p + 2, fl);
        fn[i][fl] = 0;
        f[i].name = fn[i];
        p += 2 + fl;
    }
    s.name = name;
    s.fields = f;
    if (!schema_ok(&s))
        goto bad;
    t = by_name(name);
    old = by_num(num);
    if (old && old != t) {              /* its number had another table: that one is gone */
        db.dead += old->def_len + old->bytes;
        table_clear(old);
    }
    if (!t && !(t = table_new(name)))
        goto bad;
    if (t->num && t->num != num) {      /* moved: what it had stays dead */
        db.dead += t->def_len + t->bytes;
        table_clear(t);
    }
    if (t->num == num) {
        db.dead += t->def_len;
        if (t->count && t->index != s.index)
            t->rekey = true;
    }
    t->num = (uint8_t)num;
    set_schema(t, &s);
    t->def_off = off;
    t->def_len = total;
    if (get32(b + 4) > t->next_id)
        t->next_id = get32(b + 4);
    return;
bad:
    db.dead += total;
}

static void apply(const uint8_t *h, const uint8_t *b, uint32_t len, uint32_t off)
{
    uint32_t id = get32(h + 4), total = REC_OVER + len;
    db_table_t *t = h[1] ? by_num(h[1]) : NULL;
    int i;

    switch (h[0]) {
    case KV_SET:
    case KV_DEL:
        apply_kv(h[0], b, len, off, total);
        break;
    case T_DEF:
        apply_def(h[1], b, len, off, total);
        break;
    case T_DROP:
        db.dead += total;
        if (t) {
            db.dead += t->def_len + t->bytes;
            table_clear(t);
        }
        break;
    case R_PUT:
        if (!t || !ent_put(t, id, off, rec_key(t, b, len), total))
            db.dead += total;
        break;
    case R_DEL:
        db.dead += total;
        if (t && (i = ent_pos(t, id)) >= 0)
            ent_del(t, i);
        break;
    default:
        db.dead += total;
    }
}

static uint32_t new_stamp(void)
{
    return kernel_cycles() ^ kernel_ticks() << 16 ^ db.stamp * 2654435761u;
}

static void header(uint8_t *h, uint32_t stamp)
{
    put32(h, MAGIC);
    put16(h + 4, VERSION);
    put16(h + 6, HDR);
    put32(h + 8, stamp);
    put32(h + 12, crc(h, 12));
}

static int create(void)
{
    uint8_t h[HDR];
    FIL f;
    UINT n = 0;
    FRESULT r;

    db.stamp = new_stamp();
    header(h, db.stamp);
    r = f_open(&f, DB_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    if (r == FR_OK) {
        r = f_write(&f, h, HDR, &n);
        if (f_close(&f) != FR_OK || n != HDR)
            r = FR_DISK_ERR;
    }
    db.size = db.pend.base = HDR;
    return r == FR_OK ? DB_OK : DB_EIO;
}

/* Scan the file; the offset where the good records end, *why when it is not the end */
static uint32_t scan(FIL *f, const char **why)
{
    uint32_t cap = BODY_MAX + REC_OVER + CHUNK, base = HDR, have = 0, pos = 0, total;
    uint8_t *buf = kmalloc(cap);
    bool eof = false;
    UINT got;

    *why = NULL;
    if (!buf) {
        *why = "no memory to read it";
        load_short = true;
        return HDR;
    }
    for (;;) {
        uint32_t left = have - pos;

        if (!eof && (left < REC_HDR || left < REC_OVER + get16(buf + pos + 2))) {
            memmove(buf, buf + pos, have - pos);
            base += pos;
            have -= pos;
            pos = 0;
            if (f_read(f, buf + have, cap - have, &got) != FR_OK) {
                *why = "a read error";
                load_short = true;
                break;
            }
            eof = got < cap - have;
            have += got;
        }
        if (have == pos)
            break;                      /* the end, clean */
        if (have - pos < REC_HDR) {
            *why = "a torn record";
            break;
        }
        total = REC_OVER + get16(buf + pos + 2);
        if (total - REC_OVER > BODY_MAX) {
            *why = "a bad record";
            break;
        }
        if (have - pos < total) {
            *why = "a torn record";
            break;
        }
        if (crc(buf + pos, total - 4) != get32(buf + pos + total - 4)) {
            *why = "a record with a bad CRC";
            break;
        }
        apply(buf + pos, buf + pos + REC_HDR, total - REC_OVER, base + pos);
        pos += total;
    }
    kfree(buf);
    return base + pos;
}

/* Tables someone holds that the file lacks (after an import, or a file from the
 * PC) get their layout written again. */
static void rebind(void)
{
    for (int i = 0; i < DB_TABLES; i++) {
        db_table_t *t = tables[i];

        if (t && t->opened && !t->num && (t->num = (uint8_t)free_num()) && def_write(t))
            t->num = 0;
    }
}

/* Read the file into RAM, from nothing. Settings changed in RAM and not yet
 * written keep their value; the tables' handles stay. */
static void load(void)
{
    uint8_t h[HDR];
    FIL f;
    UINT n = 0;
    FILINFO fi;
    FRESULT r;
    const char *why = NULL;
    uint32_t t0 = kernel_ticks(), end = HDR, fsize = 0, records = 0;
    int keys = 0;

    for (kv_t *k = kvs, *next; k; k = next) {
        next = k->next;
        k->off = k->len = 0;
        if (!k->dirty)
            kv_free(k);
        else
            db.kv_dirty = true;
    }
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i])
            table_clear(tables[i]);
    close_rf();
    db.dead = 0;
    db.pend.len = 0;
    load_short = false;
    db.file = fs_info()->mounted;
    if (!db.file) {
        klog("db: no card: settings in RAM only");
        return;
    }
    /* a compaction cut short: the new file is whole once the old one is gone */
    if (f_stat(DB_NEW, &fi) == FR_OK) {
        if (f_stat(DB_PATH, &fi) != FR_OK)
            f_rename(DB_NEW, DB_PATH);
        else
            f_unlink(DB_NEW);
    }
    r = f_open(&f, DB_PATH, FA_READ);
    if (r == FR_OK) {
        fsize = (uint32_t)f_size(&f);
        if (f_read(&f, h, HDR, &n) != FR_OK) {
            f_close(&f);                /* the card, not the file: it stays */
            klog("db: %s cannot be read: settings in RAM only", DB_PATH);
            db.file = false;
            return;
        }
        if (n != HDR || get32(h) != MAGIC ||
            get16(h + 4) != VERSION || crc(h, 12) != get32(h + 12)) {
            f_close(&f);
            klog("db: %s has no valid header: kept as %s, a new one made", DB_PATH, DB_BAD);
            f_unlink(DB_BAD);
            f_rename(DB_PATH, DB_BAD);
            r = FR_NO_FILE;
        }
    }
    if (r == FR_NO_FILE || r == FR_NO_PATH) {
        if (create())
            db.file = false;
        db.made = db.file;
        klog("db: %s %s", DB_PATH, db.file ? "made" : "cannot be made: settings in RAM only");
        rebind();
        if (db.kv_dirty)
            arm();
        return;
    }
    if (r != FR_OK) {
        klog("db: %s cannot be opened (%d): settings in RAM only", DB_PATH, (int)r);
        db.file = false;
        return;
    }
    db.stamp = get32(h + 8);
    loading = true;
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i] && tables[i]->cap && !tables[i]->lens)
            tables[i]->lens = kmalloc(tables[i]->cap * sizeof(uint16_t));
    end = scan(&f, &why);
    loading = false;
    f_close(&f);
    db.size = db.pend.base = end;
    for (int i = 0; i < DB_TABLES; i++) {
        db_table_t *t = tables[i];

        if (t && t->rekey)
            rekey(t);
        if (t) {
            records += t->count;
            kfree(t->lens);
            t->lens = NULL;
        }
    }
    if (load_short) {                   /* not the file's fault: never rewritten from this */
        klog("db: %s at byte %lu of %lu: kept as it is, settings in RAM only",
             why ? why : "no memory", (unsigned long)end, (unsigned long)fsize);
        db.file = false;
        return;
    }
    for (kv_t *k = kvs; k; k = k->next)
        keys += !k->gone;
    klog("db: %lu bytes, %d keys, %lu records, %lu bytes old, read in %lu ms",
         (unsigned long)end, keys, (unsigned long)records, (unsigned long)db.dead,
         (unsigned long)(kernel_ticks() - t0));
    if (why || end < fsize) {
        klog("db: %s at byte %lu of %lu: the rest left out", why ? why : "garbage",
             (unsigned long)end, (unsigned long)fsize);
        if (compact(true) != DB_OK) {
            /* no room for a new file: cut the bad tail, so new records can follow */
            if (f_open(&f, DB_PATH, FA_WRITE) == FR_OK) {
                if (f_lseek(&f, end) == FR_OK)
                    f_truncate(&f);
                f_close(&f);
            }
            klog("db: could not write it anew: the tail cut instead");
        }
    }
    rebind();
    if (db.kv_dirty)
        arm();
}

/* ---- compaction --------------------------------------------------------------------- */

static int out_write(FIL *f, obuf_t *o)
{
    UINT n = 0;

    if (o->len && (f_write(f, o->buf, o->len, &n) != FR_OK || n != o->len))
        return DB_EIO;
    o->base += o->len;
    o->len = 0;
    return DB_OK;
}

/* The live records into DB_NEW, then renamed over DB_PATH (the old one kept as
 * DB_BAD when keep_bad). */
static int compact(bool keep_bad)
{
    uint32_t *noff[DB_TABLES] = { 0 }, t0 = kernel_ticks(), before = db.size, stamp;
    obuf_t o = { 0 };
    FIL f;
    int r;
    bool open = false;

    if ((r = flush()))
        return r;
    if (!(o.buf = kmalloc(CHUNK + BODY_MAX + 2 * REC_OVER + 64)))
        return DB_ENOMEM;
    o.cap = CHUNK + BODY_MAX + 2 * REC_OVER + 64;
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i] && tables[i]->num && tables[i]->count &&
            !(noff[i] = kmalloc(tables[i]->count * sizeof(uint32_t)))) {
            r = DB_ENOMEM;
            goto done;
        }
    close_rf();
    if (f_open(&f, DB_NEW, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        r = DB_EIO;
        goto done;
    }
    open = true;
    stamp = new_stamp();
    header(o.buf, stamp);
    o.len = HDR;
    for (kv_t *k = kvs; k; k = k->next) {
        uint32_t kl = strlen(k->key);

        rec_begin(&o, KV_SET, 0, 0);
        w8(kl);
        wn(k->key, kl);
        w8(k->type);
        val_put(k->type, k->i, k->val, k->vlen);
        k->noff = rec_end(&o);
        if (o.len >= CHUNK && (r = out_write(&f, &o)))
            goto done;
    }
    for (int i = 0; i < DB_TABLES; i++) {
        db_table_t *t = tables[i];

        if (!t || !t->num)
            continue;
        def_put(&o, t);
        t->ndef = rec_end(&o);
        for (uint32_t j = 0; j < t->count; j++) {
            uint8_t h[REC_HDR];
            uint32_t len;

            if ((r = rd(t->ent[j].off, h, REC_HDR)))
                goto done;
            len = REC_OVER + get16(h + 2);
            if (len - REC_OVER > BODY_MAX || (r = rd(t->ent[j].off, o.buf + o.len, len))) {
                r = r ? r : DB_EIO;
                goto done;
            }
            noff[i][j] = o.base + o.len;
            o.len += len;
            if (o.len >= CHUNK && (r = out_write(&f, &o)))
                goto done;
        }
    }
    r = out_write(&f, &o);
    open = false;
    if (f_close(&f) != FR_OK && !r)
        r = DB_EIO;
    if (r)
        goto done;
    close_rf();
    if (keep_bad) {
        f_unlink(DB_BAD);
        f_rename(DB_PATH, DB_BAD);
    } else {
        f_unlink(DB_PATH);
    }
    if (f_rename(DB_NEW, DB_PATH) != FR_OK && f_rename(DB_NEW, DB_PATH) != FR_OK) {
        klog("db: %s could not be renamed: settings in RAM only until the next boot", DB_NEW);
        db.file = false;
        r = DB_EIO;
        goto done;
    }
    /* the new file is the file: its offsets */
    for (kv_t *k = kvs; k; k = k->next) {
        k->off = k->noff;
        k->len = REC_OVER + 2 + strlen(k->key) + val_size(k->type, k->vlen);
    }
    for (int i = 0; i < DB_TABLES; i++) {
        db_table_t *t = tables[i];

        if (!t || !t->num)
            continue;
        t->def_off = t->ndef;
        t->def_len = def_size(t);
        for (uint32_t j = 0; j < t->count; j++)
            t->ent[j].off = noff[i][j];
    }
    db.size = db.pend.base = o.base;
    db.dead = 0;
    db.stamp = stamp;
    klog("db: written anew: %lu bytes from %lu, in %lu ms", (unsigned long)db.size,
         (unsigned long)before, (unsigned long)(kernel_ticks() - t0));
done:
    if (open) {
        f_close(&f);
        f_unlink(DB_NEW);
    }
    if (r && db.file && !keep_bad)
        klog("db: compaction failed (%d)", r);
    for (int i = 0; i < DB_TABLES; i++)
        kfree(noff[i]);
    kfree(o.buf);
    return r;
}

/* ---- the OS's side ------------------------------------------------------------------- */

void db_init(void)
{
    mutex_init(&db.lock);
    lock();
    load();
    db.ready = true;
    unlock();
}

static bool compact_due(void)
{
    return card_ok() && !db.kv_dirty && !db.pend.len && db.dead >= COMPACT_MIN &&
           db.dead > db.size / 2;
}

/* What db_tick will do and when (the lock held). Without the card nothing:
 * db_resume (fs_resume) sees to it when it is back. */
static uint32_t due_ms(void)
{
    uint32_t now = kernel_ticks();
    int32_t left;

    if (!db.ready || !card_ok())
        return WAIT_FOREVER;
    if (db.pend.len)
        left = 0;
    else if (db.kv_dirty)
        left = db.flush_at ? (int32_t)(db.flush_at - now) : 0;
    else if (compact_due())
        left = 0;
    else
        return WAIT_FOREVER;
    if (db.retry_at && (int32_t)(db.retry_at - now) > left)
        left = (int32_t)(db.retry_at - now);
    return left > 0 ? (uint32_t)left : 0;
}

uint32_t db_next_ms(void)
{
    uint32_t ms;

    if (!db.ready)
        return WAIT_FOREVER;
    lock();
    ms = due_ms();
    db.waited = ms != WAIT_FOREVER;     /* so unlock does not kick for it */
    unlock();
    return ms;
}

bool db_new(void)
{
    return db.made;
}

/* A failure is tried again later, not at every wake: each try is a card hold
 * and a PLL start (until 2026-11-17 every second, as the log thread woke). */
#define RETRY_MS         5000
#define COMPACT_RETRY_MS 60000

void db_tick(void)
{
    uint32_t now;

    if (!db.ready)
        return;
    lock();
    now = kernel_ticks();
    if (db.retry_at && (int32_t)(now - db.retry_at) < 0) {
        unlock();
        return;
    }
    db.retry_at = 0;
    if (db.pend.len || (db.kv_dirty && (int32_t)(now - db.flush_at) >= 0)) {
        if (flush() == DB_OK)
            db.flush_at = 0;
        else if (card_ok())
            db.retry_at = (now + RETRY_MS) | 1;
    }
    if (compact_due() && compact(false) != DB_OK)
        db.retry_at = (now + COMPACT_RETRY_MS) | 1;
    unlock();
}

void db_sync(void)
{
    if (!db.ready)
        return;
    lock();
    if (flush() == DB_OK)
        db.flush_at = 0;
    unlock();
}

void db_suspend(void)
{
    if (!db.ready)
        return;
    lock();
    if (flush() == DB_OK)
        db.flush_at = 0;
    close_rf();
    db.away = true;
    unlock();
}

/* The card is back from USB: the PC may have changed the file (a backup put
 * back, the file deleted). Then it is read again; settings set meanwhile win. */
void db_resume(void)
{
    uint8_t h[HDR];
    FIL f;
    UINT n = 0;
    bool same = false;

    if (!db.ready)
        return;
    lock();
    db.away = false;
    if (db.file && f_open(&f, DB_PATH, FA_READ) == FR_OK) {
        same = f_size(&f) == db.size && f_read(&f, h, HDR, &n) == FR_OK && n == HDR &&
               get32(h + 8) == db.stamp;
        f_close(&f);
    }
    if (!same && fs_info()->mounted) {
        klog("db: the file changed while the card was away: read again");
        load();
        changed_all();
    }
    if (db.kv_dirty)
        arm();
    unlock();
}

int db_compact(void)
{
    int r;

    if (!db.ready)
        return DB_ENOCARD;
    lock();
    r = compact(false);
    unlock();
    return r;
}

void db_clear_all(void)
{
    lock();
    while (kvs)
        kv_free(kvs);
    db.kv_dirty = false;
    db.flush_at = 0;
    for (int i = 0; i < DB_TABLES; i++)
        if (tables[i])
            table_clear(tables[i]);
    db.pend.len = 0;
    if (card_ok())
        compact(false);                 /* a file with nothing in it */
    unlock();
}

void db_bulk(bool on)
{
    lock();
    db.bulk = on;
    if (!on) {
        rebind();
        flush();
        changed_all();
    }
    unlock();
}

/* ---- UTF-8 ----------------------------------------------------------------------------- */

/* The length of the sequence at p (n bytes left) if it is one, else 0 */
static int utf8_seq(const uint8_t *p, size_t n)
{
    uint32_t c = p[0], min;
    int more;

    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        more = 1, c &= 0x1f, min = 0x80;
    else if ((c & 0xf0) == 0xe0)
        more = 2, c &= 0x0f, min = 0x800;
    else if (c >= 0xf0 && c <= 0xf4)
        more = 3, c &= 0x07, min = 0x10000;
    else
        return 0;
    if (n < (size_t)more + 1)
        return 0;
    for (int i = 1; i <= more; i++) {
        if ((p[i] & 0xc0) != 0x80)
            return 0;
        c = c << 6 | (p[i] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return 0;
    return more + 1;
}

bool utf8_valid(const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;

    while (n) {
        int k = utf8_seq(p, n);

        if (!k)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

size_t utf8_from_latin1(const char *in, size_t n, char *out, size_t size)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t o = 0;

    while (n && size) {
        int k = utf8_seq(p, n);

        if (!k) {                       /* a Latin-1 byte: two in UTF-8 */
            if (o + 2 >= size)
                break;
            out[o++] = (char)(0xc0 | *p >> 6);
            out[o++] = (char)(0x80 | (*p & 0x3f));
            p++;
            n--;
            continue;
        }
        if (o + (size_t)k >= size)
            break;
        memcpy(out + o, p, (size_t)k);
        o += (size_t)k;
        p += k;
        n -= (size_t)k;
    }
    if (size)
        out[o < size ? o : size - 1] = 0;
    return o;
}
