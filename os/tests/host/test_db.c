/* Host test: the data store (os/fs/db.c) under power cuts and failed
 * allocations. db.c is in its own unit (test_db_glue.c), so a "reboot" can
 * reset its statics without any hook in the OS code.
 *
 *   test_db power-cut   a scripted run of store operations (each kind of
 *                       setting, a deferred write through db_tick, table
 *                       inserts / updates / removes, a layout upgrade that
 *                       moves the index, two compactions); the card (memfs)
 *                       loses its power after every possible number of units
 *                       (bytes written, renames, unlinks, truncates,
 *                       creations), once with the torn tail clean and once
 *                       with junk in it; then a reboot and the checks: the
 *                       store loads and is usable, each setting, table
 *                       layout and record is as before the operation in
 *                       flight or as after it, nothing earlier is lost, and
 *                       data-bad appears only when the file really was
 *                       damaged. A recovery that writes is cut too, then
 *                       booted again.
 *   test_db oom         every allocation of every operation fails in turn:
 *                       the store finishes or reports the failure, what RAM
 *                       has is before-or-after per item, and after a sync and
 *                       a reboot the card has exactly what RAM had; then every
 *                       allocation of a load fails in turn (on a clean file
 *                       and on one with a torn tail): the file is never
 *                       damaged, and the next boot has everything.
 *
 * Depends on the OS: os/fs/db.c (the public API in os/db.h, and the statics
 * test_db_glue.c names, which the reboot clears), os/lib/crc32.c,
 * os/lib/utf8.c; the file format in db.c's header comment (the validator).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "os/db.h"
#include "os/kernel.h"
#include "test_db.h"

#define DB_NEW dbt_new_path()
#define DB_BAD dbt_bad_path()

/* ---- reboot ------------------------------------------------------------------------ */

static db_table_t *t_notes, *t_log;

/* RAM is gone: the allocator's blocks and every static of db.c */
static void ram_lost(void)
{
    ga_free_all();
    dbt_ram_lost();
    t_notes = t_log = NULL;
}

static void boot(void)
{
    ram_lost();
    memfs_power_on();
    host_ms += 1000;
    db_init();
}

/* ---- the reference: the file format, from db.c's header comment ----------------------- */

static uint32_t ref_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;

    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

/* the header and every record whole, with its CRC, up to the end */
static bool file_valid(const uint8_t *d, uint32_t n)
{
    uint32_t pos = 16;

    if (!d || n < 16 || le32(d) != 0x42443342u || le16(d + 4) != 1 ||
        ref_crc(d, 12) != le32(d + 12))
        return false;
    while (pos < n) {
        uint32_t len, total;

        if (n - pos < 8)
            return false;
        len = le16(d + pos + 2);
        total = 12 + len;
        if (n - pos < total || ref_crc(d + pos, 8 + len) != le32(d + pos + 8 + len))
            return false;
        pos += total;
    }
    return true;
}

/* ---- snapshots: what the store holds, through its API --------------------------------- */

#define SNAP_MAX 96
typedef struct {
    char name[128];
    char val[240];
} item_t;
typedef struct {
    int n;
    item_t it[SNAP_MAX];
} snap_t;

static void enc_bytes(char *out, size_t on, const void *p, uint32_t len)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    size_t o;

    for (uint32_t i = 0; i < len; i++)
        h = (h ^ b[i]) * 16777619u;
    o = (size_t)snprintf(out, on, "%u/%08x/", (unsigned)len, (unsigned)h);
    for (uint32_t i = 0; i < len && i < 24 && o + 3 < on; i++)
        o += (size_t)snprintf(out + o, on - o, "%02x", b[i]);
}

static void add(snap_t *s, const char *name, const char *val)
{
    if (s->n >= SNAP_MAX) {
        CHECK(0, "snapshot too big");
        return;
    }
    snprintf(s->it[s->n].name, sizeof(s->it[0].name), "%s", name);
    snprintf(s->it[s->n].val, sizeof(s->it[0].val), "%s", val);
    s->n++;
}

static const char *snap_get(const snap_t *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->it[i].name, name))
            return s->it[i].val;
    return NULL;
}

static void snap_take(snap_t *s)
{
    static uint8_t buf[2 * DB_REC_MAX];
    char key[DB_KEY_MAX] = "", next[DB_KEY_MAX], name[160], val[240];
    db_table_t *ts[DB_TABLES];
    int nt;

    s->n = 0;
    while (db_key_next(key, next)) {
        int type = db_type(next);
        int len;

        snprintf(name, sizeof(name), "k/%s", next);
        switch (type) {
        case DB_INT:
            snprintf(val, sizeof(val), "int %d", db_get_int(next, -12345));
            break;
        case DB_BOOL:
            snprintf(val, sizeof(val), "bool %d", db_get_bool(next, false));
            break;
        case DB_STR:
            len = db_get_str(next, (char *)buf, sizeof(buf), "?");
            memcpy(val, "str ", 4);
            enc_bytes(val + 4, sizeof(val) - 4, buf, (uint32_t)len);
            break;
        case DB_BLOB:
            len = db_get_blob(next, buf, sizeof(buf));
            memcpy(val, "blob ", 5);
            enc_bytes(val + 5, sizeof(val) - 5, buf, (uint32_t)(len < 0 ? 0 : len));
            break;
        default:
            snprintf(val, sizeof(val), "type %d", type);
        }
        add(s, name, val);
        strcpy(key, next);
    }
    nt = db_table_list(ts, DB_TABLES);
    for (int k = 0; k < nt; k++) {
        db_field_t f[DB_FIELDS];
        int ver = 0, idx = 0, nf = db_table_layout(ts[k], f, DB_FIELDS, &ver, &idx);
        uint32_t ids[64];
        int n, o;

        o = snprintf(val, sizeof(val), "v%d i%d", ver, idx);
        for (int i = 0; i < nf && o < (int)sizeof(val) - 40; i++)
            o += snprintf(val + o, sizeof(val) - (size_t)o, " %d:%s", f[i].type, f[i].name);
        snprintf(name, sizeof(name), "t/%s", db_table_name(ts[k]));
        add(s, name, val);
        n = db_find(ts[k], -1, NULL, ids, 64, 0);
        for (int j = 0; j < n; j++) {
            db_value_t v[DB_FIELDS];
            int r = db_get(ts[k], ids[j], v, nf, buf, sizeof(buf));

            o = snprintf(val, sizeof(val), "r%d", r);
            for (int i = 0; !r && i < nf && o < (int)sizeof(val) - 60; i++) {
                if (f[i].type == DB_INT || f[i].type == DB_BOOL)
                    o += snprintf(val + o, sizeof(val) - (size_t)o, " %d", (int)v[i].i);
                else {
                    val[o++] = ' ';
                    enc_bytes(val + o, sizeof(val) - (size_t)o, v[i].p, v[i].len);
                    o += (int)strlen(val + o);
                }
            }
            snprintf(name, sizeof(name), "r/%s/%u", db_table_name(ts[k]), (unsigned)ids[j]);
            add(s, name, val);
        }
    }
}

static bool same_val(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

/* now: each item as in before or as in after; what = which run */
static bool snap_between(const snap_t *now, const snap_t *before, const snap_t *after,
                         const char *what)
{
    const snap_t *all[3] = { now, before, after };
    bool ok = true;

    for (int w = 0; w < 3; w++)
        for (int i = 0; i < all[w]->n; i++) {
            const char *name = all[w]->it[i].name;
            const char *n = snap_get(now, name), *b = snap_get(before, name),
                       *a = snap_get(after, name);

            if (!same_val(n, b) && !same_val(n, a)) {
                CHECK(0, "%s: %s is \"%s\", before the operation \"%s\", after it \"%s\"",
                      what, name, n ? n : "(none)", b ? b : "(none)", a ? a : "(none)");
                ok = false;
            }
        }
    return ok;
}

static bool snap_equal(const snap_t *now, const snap_t *want, const char *what)
{
    return snap_between(now, want, want, what);
}

/* ---- the script ------------------------------------------------------------------------- */

static const db_field_t notes_f[] = {
    { "title", DB_STR }, { "n", DB_INT }, { "flag", DB_BOOL }, { "data", DB_BLOB },
    { "extra", DB_STR },
};
static const db_schema_t notes_v1 = { "test.notes", 1, 4, 1, notes_f, NULL };
static const db_schema_t notes_v2 = { "test.notes", 2, 5, 0, notes_f, NULL };
static const db_field_t log_f[] = { { "when", DB_INT }, { "what", DB_STR } };
static const db_schema_t log_s = { "test.log", 1, 2, 1, log_f, NULL };

static uint8_t blob40[40], blob100[100];
static char long_str[301], name120[121];
static uint32_t id1, id2, id3, id4;
static bool dry;                        /* the first run: every result checked */

#define OPS 23
static const char *const op_names[OPS] = {
    "boot (a new file)", "set int", "set bool", "set string", "set blob",
    "three settings in one write", "open a table", "insert", "insert", "insert (300 bytes)",
    "update", "update a field", "remove", "set a longer string", "compaction",
    "deferred set (db_tick)", "insert", "delete a setting", "a second table and an insert",
    "insert in the second table", "layout upgrade (index moved)", "compaction",
    "remove and set",
};

#define WANT(expr)                                                                     \
    do {                                                                               \
        int r_ = (int)(expr);                                                          \
        if (dry)                                                                       \
            CHECK(r_ >= 0, "op %d (%s): %s returned %d", i, op_names[i], #expr, r_);   \
    } while (0)

static db_value_t sv(const char *s) { return (db_value_t){ .p = s, .len = (uint32_t)strlen(s) }; }
static db_value_t iv(int v) { return (db_value_t){ .i = v }; }
static db_value_t bv(const void *p, uint32_t n) { return (db_value_t){ .p = p, .len = n }; }

static void run_op(int i)
{
    switch (i) {
    case 0:
        db_init();
        break;
    case 1: WANT(db_set_int("ui.a", 5)); break;
    case 2: WANT(db_set_bool("ui.b", true)); break;
    case 3: WANT(db_set_str("ui.name", "Merhaba dünya İı ğüşöç")); break;
    case 4: WANT(db_set_blob("ui.blob", blob40, sizeof(blob40))); break;
    case 5:
        WANT(db_set_int("ui.a", 6));
        WANT(db_set_str("ui.c", "x"));
        WANT(db_delete("ui.b"));
        break;
    case 6:
        t_notes = db_table_open(&notes_v1);
        if (dry)
            CHECK(t_notes, "op 6: the table did not open");
        break;
    case 7: {
        db_value_t v[4] = { sv("first"), iv(10), iv(1), bv(blob40, 5) };
        id1 = db_insert(t_notes, v, 4);
        if (dry)
            CHECK(id1 == 1, "op 7: id %u", (unsigned)id1);
        break;
    }
    case 8: {
        db_value_t v[3] = { sv("second"), iv(20), iv(0) };     /* the blob left out: empty */
        id2 = db_insert(t_notes, v, 3);
        if (dry)
            CHECK(id2 == 2, "op 8: id %u", (unsigned)id2);
        break;
    }
    case 9: {
        db_value_t v[4] = { sv(long_str), iv(30), iv(1), bv(blob100, 100) };
        id3 = db_insert(t_notes, v, 4);
        if (dry)
            CHECK(id3 == 3, "op 9: id %u", (unsigned)id3);
        break;
    }
    case 10: {
        db_value_t v[4] = { sv("second v2"), iv(21), iv(1), bv("abc", 3) };
        WANT(db_update(t_notes, id2, v, 4));
        break;
    }
    case 11: {
        db_value_t v = sv("first*");
        WANT(db_update_field(t_notes, id1, 0, &v));
        break;
    }
    case 12: WANT(db_remove(t_notes, id3)); break;
    case 13: WANT(db_set_str("ui.name", name120)); break;
    case 14: WANT(db_compact()); break;
    case 15:
        WANT(db_set_int("ui.a", 7));
        host_ms += 6000;                /* past FLUSH_MAX_MS: the log thread writes it */
        db_tick();
        if (dry)
            CHECK(!dbt_unwritten(), "op 15: db_tick left the set unwritten");
        break;
    case 16: {
        db_value_t v[4] = { sv("fourth"), iv(40), iv(0), bv(blob100, 7) };
        id4 = db_insert(t_notes, v, 4);
        if (dry)
            CHECK(id4 == 4, "op 16: id %u", (unsigned)id4);
        break;
    }
    case 17: WANT(db_delete("ui.blob")); break;
    case 18: {
        /* one insert an op: a failed first insert would give the second its id */
        db_value_t a[2] = { iv(100), sv("wrote") };
        uint32_t x;

        t_log = db_table_open(&log_s);
        x = db_insert(t_log, a, 2);
        if (dry)
            CHECK(t_log && x == 1, "op 18: %p %u", (void *)t_log, (unsigned)x);
        break;
    }
    case 19: {
        db_value_t b[2] = { iv(101), sv("read") };
        uint32_t y = db_insert(t_log, b, 2);

        if (dry)
            CHECK(y == 2, "op 19: %u", (unsigned)y);
        break;
    }
    case 20:
        t_notes = db_table_open(&notes_v2);
        if (dry)
            CHECK(t_notes, "op 20: the upgrade did not open");
        break;
    case 21: WANT(db_compact()); break;
    case 22:
        WANT(db_remove(t_notes, id1));
        WANT(db_set_bool("ui.d", false));
        break;
    }
    db_sync();
}

static void fresh(void)
{
    memfs_reset();
    ga_reset();
    ram_lost();
    host_log_clear();
    host_ms = 1000;
}

/* ops 0 .. n-1 from a new card */
static void replay(int n)
{
    fresh();
    for (int i = 0; i < n; i++)
        run_op(i);
}

static snap_t S[OPS + 1];               /* S[i]: after ops 0 .. i-1 (S[0] empty) */
static uint32_t units[OPS + 1];         /* memfs units spent after ops 0 .. i-1 */

static void setup_data(void)
{
    for (int i = 0; i < 40; i++)
        blob40[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 100; i++)
        blob100[i] = (uint8_t)(255 - i);
    for (int i = 0; i < 300; i++)
        long_str[i] = (char)('a' + i % 26);
    long_str[300] = 0;
    for (int i = 0; i < 120; i++)
        name120[i] = (char)('A' + i % 26);
    name120[120] = 0;
}

/* the first run: every result checked, each state kept, and each one must
 * survive a reboot as it is */
static void dry_run(void)
{
    snap_t now;

    dry = true;
    fresh();
    S[0].n = 0;
    units[0] = 0;
    for (int i = 0; i < OPS; i++) {
        host_context("dry run, op %d (%s)", i, op_names[i]);
        run_op(i);
        CHECK(memfs_open_count() == 0, "op %d left %d file(s) open", i, memfs_open_count());
        CHECK(dbt_has_file(), "op %d: the store has no file", i);
        snap_take(&S[i + 1]);
        units[i + 1] = memfs_used();
    }
    /* the state each op left must be what the store reads at boot */
    for (int i = 0; i < OPS; i++) {
        uint32_t size;
        const uint8_t *d;

        host_context("dry run, reboot after op %d (%s)", i, op_names[i]);
        replay(i + 1);
        d = memfs_data(DB_PATH, &size);
        CHECK(file_valid(d, size), "after op %d the file is not valid", i);
        boot();
        snap_take(&now);
        snap_equal(&now, &S[i + 1], "reboot after a whole operation");
        CHECK(!memfs_exists(DB_BAD), "data-bad after a clean reboot (op %d)", i);
    }
    /* expected values, independent of the snapshots */
    replay(OPS);
    {
        char s[200];
        db_value_t v[5];
        uint8_t buf[512];

        host_context("dry run, the values");
        CHECK(db_get_int("ui.a", 0) == 7, "ui.a %d", db_get_int("ui.a", 0));
        CHECK(db_type("ui.b") == DB_NONE, "ui.b still there");
        CHECK(db_type("ui.blob") == DB_NONE, "ui.blob still there");
        CHECK(db_get_str("ui.name", s, sizeof(s), "") == 120 && !strcmp(s, name120), "ui.name");
        CHECK(db_get_str("ui.c", s, sizeof(s), "") == 1 && !strcmp(s, "x"), "ui.c");
        CHECK(db_type("ui.d") == DB_BOOL && !db_get_bool("ui.d", true), "ui.d");
        CHECK(db_count(t_notes) == 2, "notes count %d", db_count(t_notes));
        CHECK(db_get(t_notes, id2, v, 5, buf, sizeof(buf)) == DB_OK && v[1].i == 21 &&
              v[0].len == 9 && !memcmp(v[0].p, "second v2", 9) && v[4].len == 0, "record 2");
        CHECK(db_get(t_notes, id1, v, 5, buf, sizeof(buf)) == DB_ENOENT, "record 1 removed");
        CHECK(db_get(t_notes, id3, v, 5, buf, sizeof(buf)) == DB_ENOENT, "record 3 removed");
        {
            uint32_t ids[4];
            db_value_t k = sv("fourth");

            CHECK(db_find(t_notes, 0, &k, ids, 4, 0) == 1 && ids[0] == id4,
                  "find by the moved index");
        }
    }
    /* a steady state: the same work over and over does not grow the heap */
    {
        unsigned live0 = 0;
        size_t bytes0 = 0;

        host_context("dry run, repeated work (a leak)");
        for (int k = 0; k < 40; k++) {
            char s[32];
            db_value_t v[4] = { sv("tmp"), iv(k), iv(0), bv(blob40, 4) };
            uint32_t id;

            snprintf(s, sizeof(s), "value %d", k % 3 ? k : 7);
            db_set_str("leak.k", s);
            db_sync();
            db_get_str("leak.k", s, sizeof(s), "");
            id = db_insert(t_notes, v, 4);
            db_remove(t_notes, id);
            if (k == 4) {
                live0 = ga_live();
                bytes0 = ga_live_bytes();
            }
        }
        CHECK(ga_live() <= live0 + 1 && ga_live_bytes() <= bytes0 + 2048,
              "the heap grew: %u blocks %u bytes, from %u blocks %u bytes", ga_live(),
              (unsigned)ga_live_bytes(), live0, (unsigned)bytes0);
    }
    dry = false;
}

/* ---- power cuts ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *path, *bad;                /* the card before the reboot */
    uint32_t path_n, bad_n;
    bool path_valid;
    bool had_path, had_bad;
} card_t;

static void card_take(card_t *c)
{
    const uint8_t *d;
    const char *src = memfs_exists(DB_PATH) ? DB_PATH : DB_NEW;   /* load's own rule */

    memset(c, 0, sizeof(*c));
    c->had_path = memfs_exists(src);
    if ((d = memfs_data(src, &c->path_n))) {
        c->path = malloc(c->path_n + 1);
        memcpy(c->path, d, c->path_n);
    }
    c->path_valid = file_valid(c->path, c->path_n);
    c->had_bad = memfs_exists(DB_BAD);
    if ((d = memfs_data(DB_BAD, &c->bad_n))) {
        c->bad = malloc(c->bad_n + 1);
        memcpy(c->bad, d, c->bad_n);
    }
}

static void card_free(card_t *c)
{
    free(c->path);
    free(c->bad);
}

/* After a reboot: the store loads and works, data-bad only for a damaged
 * file (and then it is that file), the file is whole again, and each item is
 * before or after the operation in flight. */
static void after_boot(const card_t *c, int op, const char *what)
{
    snap_t now;
    uint32_t n, bn;
    const uint8_t *d = memfs_data(DB_PATH, &n), *b = memfs_data(DB_BAD, &bn);
    bool new_bad = b && (!c->had_bad || bn != c->bad_n || memcmp(b, c->bad, bn));

    CHECK(dbt_has_file(), "%s: the store did not load its file", what);
    CHECK(file_valid(d, n), "%s: the file after the boot is not valid (%u bytes)", what,
          (unsigned)n);
    CHECK(memfs_open_count() == 0, "%s: %d file(s) left open", what, memfs_open_count());
    if (new_bad) {
        CHECK(c->had_path && !c->path_valid, "%s: the file was readable, yet kept as data-bad",
              what);
        CHECK(bn == c->path_n && !memcmp(b, c->path, bn),
              "%s: data-bad is not the damaged file", what);
    }
    snap_take(&now);
    snap_between(&now, &S[op], &S[op + 1], what);
}

/* A write after the reboot sticks, and a second boot reads the same. */
static void still_works(int op, uint32_t tag, const char *what)
{
    snap_t a, b;

    CHECK(db_set_int("test.probe", (int)tag) == DB_OK, "%s: a set after the boot failed", what);
    db_sync();
    snap_take(&a);
    boot();
    snap_take(&b);
    snap_equal(&b, &a, what);
    CHECK(db_get_int("test.probe", -1) == (int)tag, "%s: the set after the boot was lost", what);
    (void)op;
}

static void power_cuts(void)
{
    uint32_t total = units[OPS], tried = 0, recoveries = 0, recovery_cuts = 0;
    uint32_t step = host_long ? 1 : (total > 9000 ? total / 4500 : 1);

    printf("power-cut: %u units over %d operations, cuts every %u unit(s), clean and junk tails\n",
           (unsigned)total, OPS, (unsigned)step);
    for (uint32_t c = 0; c < total; c += step) {
        for (int junk = 0; junk < 2; junk++) {
            int op = -1;
            card_t card;
            char what[160];
            uint32_t before_boot;

            fresh();
            memfs_budget(c);
            memfs_set_junk(junk, c * 2654435761u + 1);
            for (int i = 0; i < OPS && op < 0; i++) {
                run_op(i);
                if (!memfs_powered())
                    op = i;
            }
            snprintf(what, sizeof(what), "cut after %u units (%s tail), in op %d (%s)",
                     (unsigned)c, junk ? "junk" : "clean", op, op >= 0 ? op_names[op] : "-");
            host_context("%s", what);
            CHECK(op >= 0, "%s: the power never went", what);
            if (op < 0)
                continue;
            CHECK(c >= units[op] && c < units[op + 1], "%s: the units do not match the dry run "
                  "(op %d spans %u..%u)", what, op, (unsigned)units[op], (unsigned)units[op + 1]);
#ifdef HOST_PROVE
            /* break the card: an earlier record's byte, as a bad stand-in would */
            {
                uint32_t n;
                const uint8_t *d = memfs_data(DB_PATH, &n);

                if (d && n > 40) {
                    uint8_t *w = malloc(n);

                    memcpy(w, d, n);
                    w[16 + 9] ^= 0x40;
                    memfs_put(DB_PATH, w, n);
                    free(w);
                }
            }
#endif
            card_take(&card);
            before_boot = memfs_used();
            boot();
            tried++;
            if (memfs_used() != before_boot && (host_long || tried % 4 == 0)) {
                /* the recovery wrote: cut it too, at a few places, and boot again */
                uint32_t spent = memfs_used() - before_boot;

                recoveries++;
                for (int k = 0; k < 6; k++) {
                    uint32_t at = (uint32_t)((uint64_t)spent * k / 6);
                    card_t c2;
                    char what2[200];

                    fresh();
                    memfs_budget(c);
                    memfs_set_junk(junk, c * 2654435761u + 1);
                    for (int i = 0; i <= op; i++)
                        run_op(i);
                    card_take(&c2);
                    ram_lost();
                    memfs_power_on();
                    memfs_budget(at);
                    db_init();
                    snprintf(what2, sizeof(what2), "%s; the recovery cut after %u of its %u units",
                             what, (unsigned)at, (unsigned)spent);
                    host_context("%s", what2);
                    card_free(&c2);
                    card_take(&c2);
                    boot();
                    after_boot(&c2, op, what2);
                    card_free(&c2);
                    recovery_cuts++;
                }
                /* and the first boot once more, to go on from it */
                fresh();
                memfs_budget(c);
                memfs_set_junk(junk, c * 2654435761u + 1);
                for (int i = 0; i <= op; i++)
                    run_op(i);
                boot();
                host_context("%s", what);
            }
            after_boot(&card, op, what);
            still_works(op, c * 2 + (uint32_t)junk, what);
            card_free(&card);
            if (host_failures > 12)
                return;
        }
    }
    printf("power-cut: %u boots after a cut, %u recoveries cut %u times\n", (unsigned)tried,
           (unsigned)recoveries, (unsigned)recovery_cuts);
}

/* ---- failed allocations ------------------------------------------------------------------ */

static void oom_ops(void)
{
    unsigned runs = 0;

    for (int i = 1; i < OPS; i++) {
        unsigned allocs;

        replay(i);
        allocs = ga_count();
        run_op(i);
        allocs = ga_count() - allocs;
        for (unsigned n = 1; n <= allocs; n++) {
            snap_t ram, card;
            char what[160];
            int dirty_before;

            replay(i);
            snprintf(what, sizeof(what), "op %d (%s) with allocation %u of %u failing", i,
                     op_names[i], n, allocs);
            host_context("%s", what);
            ga_fail_at(n);
            run_op(i);
            ga_fail_at(0);
            CHECK(ga_failed(), "%s: the allocation was not made", what);
            CHECK(memfs_open_count() == 0, "%s: %d file(s) left open", what, memfs_open_count());
            snap_take(&ram);
            snap_between(&ram, &S[i], &S[i + 1], what);
            dirty_before = dbt_kv_dirty();
#ifdef HOST_PROVE
            dbt_break_ram();            /* a store that changes RAM behind the card's back */
            snap_take(&ram);
#endif
            db_sync();                  /* the allocations work again */
            CHECK(!dbt_unwritten(), "%s: changes still unwritten after a sync (dirty before %d)",
                  what, dirty_before);
            boot();
            snap_take(&card);
            snap_equal(&card, &ram, what);
            CHECK(!memfs_exists(DB_BAD), "%s: data-bad made", what);
            runs++;
            if (host_failures > 12)
                return;
        }
    }
    printf("oom: %u operations run with one allocation failing\n", runs);
}

static void oom_load_on(const memfs_image_t *card, const char *kind)
{
    snap_t want, now;
    unsigned allocs, runs = 0;
    uint32_t orig_n;
    uint8_t *orig;
    bool orig_valid;
    const uint8_t *d;

    memfs_restore(card);
    d = memfs_data(DB_PATH, &orig_n);
    orig = malloc(orig_n + 1);
    memcpy(orig, d, orig_n);
    orig_valid = file_valid(orig, orig_n);
    /* what a boot with memory makes of it */
    ram_lost();
    ga_reset();
    host_ms += 1000;
    db_init();
    allocs = ga_count();
    snap_take(&want);
    for (unsigned n = 1; n <= allocs; n++) {
        uint32_t size, bn;
        const uint8_t *b;
        bool file;
        char what[160];

        snprintf(what, sizeof(what), "load of %s with allocation %u of %u failing", kind, n,
                 allocs);
        host_context("%s", what);
        memfs_restore(card);
        ram_lost();
        ga_reset();
        ga_fail_at(n);
        db_init();
        ga_fail_at(0);
        CHECK(ga_failed(), "%s: the allocation was not made", what);
        CHECK(memfs_open_count() == 0, "%s: %d file(s) left open", what, memfs_open_count());
        file = dbt_file();
        d = memfs_data(DB_PATH, &size);
        if (!file) {
            /* kept as it is, and nothing written into it from RAM */
            CHECK(memfs_same(card), "%s: the card changed, the store in RAM only", what);
            db_set_int("test.oom", 1);
            db_sync();
            CHECK(memfs_same(card), "%s: a set in RAM-only mode reached the card", what);
        } else {
            /* the load worked (the failure was somewhere it can do without) */
            CHECK(file_valid(d, size), "%s: the file is not valid after the load", what);
            if (orig_valid)
                CHECK(memfs_same(card), "%s: a good file was changed by its load", what);
            snap_take(&now);
            snap_equal(&now, &want, what);
        }
        if ((b = memfs_data(DB_BAD, &bn))) {
            CHECK(!orig_valid, "%s: data-bad made from a good file", what);
            CHECK(bn == orig_n && !memcmp(b, orig, bn), "%s: data-bad is not the file", what);
        }
        boot();
        snap_take(&now);
        if (!file)
            CHECK(!snap_get(&now, "k/test.oom"), "%s: a set made in RAM only reached the card",
                  what);
        snap_equal(&now, &want, what);
        runs++;
        if (host_failures > 12)
            break;
    }
    free(orig);
    printf("oom: the load of %s run %u times, each allocation failing once\n", kind, runs);
}

static void oom_load(void)
{
    memfs_image_t *clean, *torn;
    uint32_t n;
    const uint8_t *d;
    uint8_t *w;

    replay(OPS);
    clean = memfs_save();
    d = memfs_data(DB_PATH, &n);
    w = malloc(n + 7);
    memcpy(w, d, n);
    memcpy(w + n, "\x05\x01\x30\x00junk", 7);      /* a torn record */
    memfs_put(DB_PATH, w, n + 7);
    free(w);
    torn = memfs_save();
    oom_load_on(clean, "a clean file");
    oom_load_on(torn, "a file with a torn tail");
    memfs_image_free(clean);
    memfs_image_free(torn);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "power-cut";

    host_init(argc, argv);
    setup_data();
    dry_run();
    if (host_failures)
        return host_done();
    if (!strcmp(which, "power-cut"))
        power_cuts();
    else if (!strcmp(which, "oom")) {
        oom_ops();
        if (!host_failures)
            oom_load();
    } else {
        fprintf(stderr, "test_db: power-cut or oom\n");
        return 2;
    }
    ram_lost();
    return host_done();
}
