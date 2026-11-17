/* B310E OS — the data store as text (include/os/db.h): the backup, and the way to
 * change it on a PC. One line each, UTF-8:
 *
 *   key "ui.brightness" int 60
 *   key "app/Notes/imported" bool 1
 *   key "gsm.imei" str "35..."
 *   key "app/x/icon" blob x00ff10
 *   table "app/Notes/notes" 1 -1 "text" str "time" int     name, version, index field,
 *                                                           then each field and its type
 *   rec "app/Notes/notes" 3 "Hi\nthere" 1762560000         table, id, the values
 *
 * Strings are quoted, with \" \\ \n \r \t and \xHH for other control bytes;
 * bytes are x and hex; bools 0 or 1. A line starting with # is a comment.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/db.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"

#define OUT_BUF   4096
#define LINE_MAX_ (4 * DB_REC_MAX + 512)
#define TOKENS    (4 + 2 * DB_FIELDS)
#define VALS_BUF  (DB_REC_MAX + DB_FIELDS + 16)

static const char *const type_names[] = { "", "int", "bool", "str", "blob" };

/* ---- export ---------------------------------------------------------------------- */

typedef struct {
    FIL f;
    char buf[OUT_BUF];
    int len;
    bool bad;
} out_t;

static void out_flush(out_t *o)
{
    UINT n = 0;

    if (o->len && (f_write(&o->f, o->buf, (UINT)o->len, &n) != FR_OK || n != (UINT)o->len))
        o->bad = true;
    o->len = 0;
}

static void out_c(out_t *o, char c)
{
    if (o->len == OUT_BUF)
        out_flush(o);
    o->buf[o->len++] = c;
}

static void out_s(out_t *o, const char *s)
{
    while (*s)
        out_c(o, *s++);
}

static void out_num(out_t *o, long v)
{
    char b[16];

    snprintf(b, sizeof(b), "%ld", v);
    out_s(o, b);
}

static void out_str(out_t *o, const char *s, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";

    out_c(o, '"');
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];

        if (c == '"' || c == '\\') {
            out_c(o, '\\');
            out_c(o, (char)c);
        } else if (c == '\n') {
            out_s(o, "\\n");
        } else if (c == '\r') {
            out_s(o, "\\r");
        } else if (c == '\t') {
            out_s(o, "\\t");
        } else if (c < 0x20 || c == 0x7f) {
            out_s(o, "\\x");
            out_c(o, hex[c >> 4]);
            out_c(o, hex[c & 15]);
        } else {
            out_c(o, (char)c);
        }
    }
    out_c(o, '"');
}

static void out_blob(out_t *o, const uint8_t *p, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";

    out_c(o, 'x');
    for (uint32_t i = 0; i < n; i++) {
        out_c(o, hex[p[i] >> 4]);
        out_c(o, hex[p[i] & 15]);
    }
}

static void out_value(out_t *o, int type, const db_value_t *v)
{
    if (type == DB_STR)
        out_str(o, v->p, v->len);
    else if (type == DB_BLOB)
        out_blob(o, v->p, v->len);
    else
        out_num(o, type == DB_BOOL ? v->i != 0 : v->i);
}

static void export_table(out_t *o, db_table_t *t, uint8_t *buf)
{
    db_field_t f[DB_FIELDS];
    db_value_t v[DB_FIELDS];
    int version, index, nf = db_table_layout(t, f, DB_FIELDS, &version, &index);
    int count = db_count(t), got = 0;
    uint32_t *ids = count ? kmalloc((size_t)count * 4) : NULL, before = 0;
    const char *name = db_table_name(t);

    out_s(o, "table ");
    out_str(o, name, strlen(name));
    out_c(o, ' ');
    out_num(o, version);
    out_c(o, ' ');
    out_num(o, index);
    for (int i = 0; i < nf; i++) {
        out_c(o, ' ');
        out_str(o, f[i].name, strlen(f[i].name));
        out_c(o, ' ');
        out_s(o, type_names[f[i].type]);
    }
    out_c(o, '\n');
    if (count && !ids) {
        o->bad = true;
        return;
    }
    /* newest first in batches, written oldest first: an import adds them in order */
    while (got < count) {
        int n = db_find(t, -1, NULL, ids + got, count - got, before);

        if (n <= 0)
            break;
        got += n;
        before = ids[got - 1];
    }
    for (int i = got - 1; i >= 0 && !o->bad; i--) {
        if (db_get(t, ids[i], v, nf, buf, VALS_BUF) != DB_OK) {
            o->bad = true;
            break;
        }
        out_s(o, "rec ");
        out_str(o, name, strlen(name));
        out_c(o, ' ');
        out_num(o, (long)ids[i]);
        for (int j = 0; j < nf; j++) {
            out_c(o, ' ');
            out_value(o, f[j].type, &v[j]);
        }
        out_c(o, '\n');
    }
    kfree(ids);
}

int db_export(const char *path)
{
    out_t *o = kzalloc(sizeof(out_t));
    uint8_t *buf = kmalloc(VALS_BUF);
    db_table_t *list[DB_TABLES];
    char key[DB_KEY_MAX] = "";
    int r = DB_OK, n;

    if (!o || !buf) {
        kfree(o);
        kfree(buf);
        return DB_ENOMEM;
    }
    if (f_open(&o->f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        kfree(o);
        kfree(buf);
        return DB_EIO;
    }
    out_s(o, "# B310E OS data store (os/fs/db_text.c)\n");
    while (db_key_next(key, key)) {
        int type = db_type(key);
        db_value_t v = { 0 };

        if (type == DB_STR) {
            v.len = (uint32_t)db_get_str(key, (char *)buf, VALS_BUF, NULL);
            v.p = buf;
        } else if (type == DB_BLOB) {
            int len = db_get_blob(key, buf, VALS_BUF);

            v.len = len > 0 ? (uint32_t)len : 0;
            v.p = buf;
        } else if (type) {
            v.i = db_get_int(key, 0);
        } else {
            continue;
        }
        out_s(o, "key ");
        out_str(o, key, strlen(key));
        out_c(o, ' ');
        out_s(o, type_names[type]);
        out_c(o, ' ');
        out_value(o, type, &v);
        out_c(o, '\n');
    }
    n = db_table_list(list, DB_TABLES);
    for (int i = 0; i < n; i++)
        export_table(o, list[i], buf);
    out_flush(o);
    if (f_close(&o->f) != FR_OK || o->bad)
        r = DB_EIO;
    kfree(o);
    kfree(buf);
    return r;
}

/* ---- import ---------------------------------------------------------------------- */

typedef struct {
    char *p;
    uint32_t len;
    bool quoted;
} tok_t;

typedef struct {
    char name[DB_NAME_MAX];
    uint8_t nfields;
    uint8_t types[DB_FIELDS];
    db_table_t *t;
} decl_t;

typedef struct {
    FIL f;
    char in[OUT_BUF];
    UINT have, pos;
    bool eof;
} in_t;

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* One line into line (NUL-ended, CR LF taken), its length; -1 at the end, -2 too long */
static int read_line(in_t *r, char *line)
{
    int n = 0;
    bool any = false, over = false;

    for (;;) {
        char c;

        if (r->pos == r->have) {
            if (r->eof || f_read(&r->f, r->in, sizeof(r->in), &r->have) != FR_OK || !r->have) {
                r->eof = true;
                break;
            }
            r->pos = 0;
        }
        c = r->in[r->pos++];
        any = true;
        if (c == '\n')
            break;
        if (n < LINE_MAX_ - 1)
            line[n++] = c;
        else
            over = true;
    }
    if (n && line[n - 1] == '\r')
        n--;
    line[n] = 0;
    return over ? -2 : any ? n : -1;
}

/* Words and quoted strings (unescaped in place); how many, -1 on a bad one */
static int split(char *p, tok_t *t, int max)
{
    int n = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return n;
        if (n == max)
            return -1;
        if (*p == '"') {
            char *o = ++p;

            t[n].p = o;
            t[n].quoted = true;
            while (*p != '"') {
                int c = (uint8_t)*p++;

                if (!c)
                    return -1;          /* no closing quote */
                if (c == '\\') {
                    c = *p++;
                    if (c == 'n')
                        c = '\n';
                    else if (c == 'r')
                        c = '\r';
                    else if (c == 't')
                        c = '\t';
                    else if (c == 'x') {
                        int hi = hexval(p[0]), lo = hi < 0 ? -1 : hexval(p[1]);

                        if (lo < 0)
                            return -1;
                        c = hi << 4 | lo;
                        p += 2;
                    } else if (c != '"' && c != '\\') {
                        return -1;
                    }
                }
                *o++ = (char)c;
            }
            t[n].len = (uint32_t)(o - t[n].p);
            p++;
            *o = 0;
        } else {
            t[n].p = p;
            t[n].quoted = false;
            while (*p && *p != ' ' && *p != '\t')
                p++;
            t[n].len = (uint32_t)(p - t[n].p);
            if (*p)
                *p++ = 0;
        }
        n++;
    }
}

static bool to_int(const tok_t *t, int32_t *v)
{
    const char *p = t->p;
    bool neg = *p == '-';
    int64_t x = 0;

    if (t->quoted)
        return false;
    if (neg)
        p++;
    if (!*p)
        return false;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || x > 0x80000000LL)
            return false;
        x = x * 10 + (*p - '0');
    }
    if (neg)
        x = -x;
    if (x < -0x80000000LL || x > 0x7fffffffLL)
        return false;
    *v = (int32_t)x;
    return true;
}

static int type_of(const tok_t *t)
{
    for (int i = DB_INT; i <= DB_BLOB; i++)
        if (!t->quoted && !strcmp(t->p, type_names[i]))
            return i;
    return DB_NONE;
}

/* A value of type from t; blobs are decoded in place */
static bool to_value(tok_t *t, int type, db_value_t *v)
{
    v->i = 0;
    v->p = "";
    v->len = 0;
    if (type == DB_INT)
        return to_int(t, &v->i);
    if (type == DB_BOOL)
        return to_int(t, &v->i) && (v->i == 0 || v->i == 1);
    if (type == DB_STR) {
        v->p = t->p;
        v->len = t->len;
        return t->quoted && t->len <= DB_REC_MAX && utf8_valid(t->p, t->len) &&
               strlen(t->p) == t->len;
    }
    if (type == DB_BLOB && !t->quoted && t->p[0] == 'x' && t->len % 2 == 1) {
        uint8_t *o = (uint8_t *)t->p;

        for (uint32_t i = 1; i < t->len; i += 2) {
            int hi = hexval(t->p[i]), lo = hexval(t->p[i + 1]);

            if (hi < 0 || lo < 0)
                return false;
            *o++ = (uint8_t)(hi << 4 | lo);
        }
        v->p = t->p;
        v->len = (t->len - 1) / 2;
        return v->len <= DB_REC_MAX;
    }
    return false;
}

static decl_t *decl_find(decl_t *d, int n, const tok_t *t)
{
    for (int i = 0; i < n; i++)
        if (t->quoted && !strcmp(d[i].name, t->p))
            return &d[i];
    return NULL;
}

/* One line: checked only (apply false), or put into the store */
static bool do_line(tok_t *tok, int n, decl_t *decl, int *ndecl, bool apply)
{
    db_value_t v[DB_FIELDS];
    const char *what = n ? tok[0].p : "";

    if (!n || what[0] == '#')
        return true;
    if (!strcmp(what, "key")) {
        int type = n == 4 ? type_of(&tok[2]) : DB_NONE;

        if (!type || !tok[1].quoted || !tok[1].len || tok[1].len >= DB_KEY_MAX ||
            !to_value(&tok[3], type, &v[0]))
            return false;
        if (!apply)
            return true;
        if (type == DB_STR)
            return db_set_str(tok[1].p, v[0].p) == DB_OK;
        if (type == DB_BLOB)
            return db_set_blob(tok[1].p, v[0].p, v[0].len) == DB_OK;
        return (type == DB_BOOL ? db_set_bool(tok[1].p, v[0].i) : db_set_int(tok[1].p, v[0].i)) ==
               DB_OK;
    }
    if (!strcmp(what, "table")) {
        db_field_t f[DB_FIELDS];
        db_schema_t s;
        int32_t version, index;
        int nf = (n - 4) / 2;
        decl_t *d;

        if (n < 6 || (n - 4) % 2 || nf > DB_FIELDS || !tok[1].quoted ||
            tok[1].len >= DB_NAME_MAX || !to_int(&tok[2], &version) || version < 0 ||
            version > 0xffff || !to_int(&tok[3], &index) || index < -1 || index >= nf ||
            decl_find(decl, *ndecl, &tok[1]) || *ndecl == DB_TABLES)
            return false;
        for (int i = 0; i < nf; i++) {
            f[i].name = tok[4 + 2 * i].p;
            f[i].type = (uint8_t)type_of(&tok[5 + 2 * i]);
            if (!tok[4 + 2 * i].quoted || !f[i].type)
                return false;
        }
        d = &decl[(*ndecl)++];
        strncpy(d->name, tok[1].p, DB_NAME_MAX - 1);
        d->nfields = (uint8_t)nf;
        for (int i = 0; i < nf; i++)
            d->types[i] = f[i].type;
        if (!apply)
            return true;
        s.name = tok[1].p;
        s.version = (uint16_t)version;
        s.nfields = (uint8_t)nf;
        s.index = (int8_t)index;
        s.fields = f;
        s.upgrade = NULL;
        d->t = db_table_open(&s);
        return d->t != NULL;
    }
    if (!strcmp(what, "rec")) {
        decl_t *d = n >= 3 ? decl_find(decl, *ndecl, &tok[1]) : NULL;
        int32_t id;

        if (!d || n != 3 + d->nfields || !to_int(&tok[2], &id) || id <= 0)
            return false;
        for (int i = 0; i < d->nfields; i++)
            if (!to_value(&tok[3 + i], d->types[i], &v[i]))
                return false;
        return !apply || (d->t && db_insert_id(d->t, (uint32_t)id, v, d->nfields));
    }
    return false;
}

static int import_pass(const char *path, char *line, tok_t *tok, decl_t *decl, bool apply,
                       int *at)
{
    in_t *r = kzalloc(sizeof(in_t));
    int ndecl = 0, n, len, err = DB_OK;

    if (!r)
        return DB_ENOMEM;
    if (f_open(&r->f, path, FA_READ) != FR_OK) {
        kfree(r);
        return DB_ENOENT;
    }
    *at = 0;
    while ((len = read_line(r, line)) != -1) {
        (*at)++;
        if (len == -2 || (n = split(line, tok, TOKENS)) < 0 ||
            !do_line(tok, n, decl, &ndecl, apply)) {
            err = apply ? DB_EIO : DB_EINVAL;
            if (!apply)
                break;
        }
    }
    f_close(&r->f);
    kfree(r);
    return err;
}

int db_import(const char *path, int *line_no)
{
    char *line = kmalloc(LINE_MAX_);
    tok_t *tok = kmalloc(sizeof(tok_t) * TOKENS);
    decl_t *decl = kmalloc(sizeof(decl_t) * DB_TABLES);
    int r = DB_ENOMEM, at = 0;

    if (line && tok && decl) {
        r = import_pass(path, line, tok, decl, false, &at);     /* the whole file is right */
        if (r == DB_OK) {
            klog("db: import of %s: %d lines", path, at);
            db_clear_all();
            db_bulk(true);
            r = import_pass(path, line, tok, decl, true, &at);
            db_bulk(false);
        }
    }
    if (line_no)
        *line_no = at;
    kfree(line);
    kfree(tok);
    kfree(decl);
    return r;
}
