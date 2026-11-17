/* B310E OS — the data store (os/fs/db.c): the OS's settings and small data in
 * one file on the card, /b310e-os/data, in place of a text file each.
 *
 * Settings are keys with a typed value (int, bool, a UTF-8 string, bytes),
 * kept in RAM: a get never touches the card. Sets are written a second or two
 * after the last one (at most 5 s after the first), and at once by db_sync
 * (power-off, restart) and before USB takes the card (fs_suspend), so a volume
 * slider makes one write, not one a step.
 *
 * Tables hold records of a fixed list of fields (int, bool, string, bytes),
 * each with an id (1, 2, ... in the order they were added: the newest has the
 * highest). RAM keeps an index only, 12 bytes a record (id, where it is in the
 * file, the value of the table's index field or its hash); a record's fields
 * are read from the card when asked for. A record change is written at once.
 *
 * The file is a header and then records, only ever appended, each with a
 * CRC-32: a power cut loses at most the change being written, and a torn or
 * damaged tail is found at boot, logged, and left out (the file is written
 * again without it; the damaged one is kept as data-bad). Once more than half
 * of the file (and 16 KB) is old values, it is written anew as data-new and
 * renamed over the old; nothing is rewritten in place.
 *
 * Text is UTF-8 with its length, never cut inside a character (utf8_cut).
 *
 * Keys and table names: "<area>.<name>" for the OS ("ui.brightness",
 * "clock.saved"); an app's are "app/<app name>/..." and it reaches only those
 * (the app API adds the prefix, os/sdk/app.h).
 *
 * One mutex: any thread may call these, never an interrupt handler. Without
 * the card (or while USB has it) gets work, sets wait in RAM for it, and table
 * calls answer DB_ENOCARD.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_DB_H
#define OS_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DB_PATH      "/b310e-os/data"

enum { DB_NONE, DB_INT, DB_BOOL, DB_STR, DB_BLOB };

#define DB_OK         0
#define DB_ENOENT    (-1)               /* no such key, record or table */
#define DB_EINVAL    (-2)               /* bad argument, bad UTF-8, wrong type */
#define DB_ENOCARD   (-3)               /* no card, or USB has it */
#define DB_ENOMEM    (-4)
#define DB_EIO       (-5)               /* the card failed */
#define DB_ETOOBIG   (-6)               /* past DB_REC_MAX, or the buffer given */

#define DB_KEY_MAX    96                /* a key's bytes, the NUL included */
#define DB_NAME_MAX   48                /* a table's name, the NUL included */
#define DB_FIELDS     16                /* fields of a table */
#define DB_REC_MAX    16384             /* a record's or a value's encoded bytes */
#define DB_TABLES     32

/* ---- settings ------------------------------------------------------------------ */
int db_get_int(const char *key, int def);
bool db_get_bool(const char *key, bool def);
/* The string into out (n bytes, NUL included), cut at a whole character; its full
 * length in bytes, or the default's when the key is missing or not a string. */
int db_get_str(const char *key, char *out, size_t n, const char *def);
/* Bytes into out; their full length, or DB_ENOENT. */
int db_get_blob(const char *key, void *out, size_t n);
int db_set_int(const char *key, int v);
int db_set_bool(const char *key, bool v);
int db_set_str(const char *key, const char *s);             /* UTF-8, checked */
int db_set_blob(const char *key, const void *p, size_t n);
int db_delete(const char *key);
int db_type(const char *key);                               /* DB_*, DB_NONE: missing */
/* The key after `after` ("": the first) in byte order, into out (DB_KEY_MAX);
 * false past the last. A listing that holds no lock between keys. */
bool db_key_next(const char *after, char *out);

/* ---- tables -------------------------------------------------------------------- */
typedef struct db_table db_table_t;

typedef struct {
    const char *name;
    uint8_t type;                       /* DB_INT, DB_BOOL, DB_STR, DB_BLOB */
} db_field_t;

/* A table's layout. Fields are only ever added at the end, with a higher version:
 * records written before read the new fields as 0 / empty. upgrade (may be NULL)
 * runs once, at the open that raised the version, with the version found. */
typedef struct {
    const char *name;
    uint16_t version;
    uint8_t nfields;
    int8_t index;                       /* the field db_find filters on fast, -1 none */
    const db_field_t *fields;
    void (*upgrade)(db_table_t *t, unsigned from_version);
} db_schema_t;

/* A field's value. Strings and bytes point at their data (len bytes, no NUL
 * counted); from db_get they point into the caller's buffer, strings NUL-ended. */
typedef struct {
    int32_t i;                          /* DB_INT, DB_BOOL */
    const void *p;                      /* DB_STR, DB_BLOB */
    uint32_t len;
} db_value_t;
#define DB_INTV(v)  ((db_value_t){ .i = (v) })
#define DB_STRV(s)  ((db_value_t){ .p = (s), .len = strlen(s) })

/* The table, made if new; NULL on a bad schema or no memory. Works without the
 * card for a table already in the store. */
db_table_t *db_table_open(const db_schema_t *schema);
/* A new record of nfields values (fewer: the rest 0 / empty): its id, 0 on failure. */
uint32_t db_insert(db_table_t *t, const db_value_t *v, int n);
/* Record id's first n fields into v; its strings and bytes are copied into buf
 * (size bytes). DB_OK, DB_ENOENT, DB_ETOOBIG (buf too small), DB_ENOCARD. */
int db_get(db_table_t *t, uint32_t id, db_value_t *v, int n, void *buf, size_t size);
/* Ids of records, newest first, older than before (0: from the newest), whose
 * field equals *value (field -1: every record), at most max. How many. */
int db_find(db_table_t *t, int field, const db_value_t *value, uint32_t *ids, int max,
            uint32_t before);
int db_update(db_table_t *t, uint32_t id, const db_value_t *v, int n);
int db_update_field(db_table_t *t, uint32_t id, int field, const db_value_t *v);
int db_remove(db_table_t *t, uint32_t id);
int db_count(db_table_t *t);
uint32_t db_table_gen(db_table_t *t);   /* changes with every change of the table */

/* ---- change notification ---------------------------------------------------------- */
/* fn(ctx) on the UI thread (db_dispatch) after a key starting with prefix, or
 * (table) the table of that name, changed, by anyone. A few at once (8). */
bool db_watch(const char *name, bool table, void (*fn)(void *ctx), void *ctx);
uint32_t db_changes(void);              /* changes with every change of the store */

/* ---- the OS's side ---------------------------------------------------------------- */
void db_init(void);                     /* load the file (main.c, after fs_init) */
void db_tick(void);                     /* the log thread: due writes, compaction */
void db_sync(void);                     /* write everything now */
void db_suspend(void);                  /* sync, then hands off the card (fs_suspend) */
void db_resume(void);                   /* the card is back (fs_resume) */
void db_dispatch(void);                 /* the UI thread: watchers' calls */
int db_compact(void);                   /* write the file anew now */
int db_table_list(db_table_t **out, int max);   /* the tables in the store */
db_table_t *db_table_find(const char *name);    /* one in the store, or NULL */
const char *db_table_name(db_table_t *t);
uint32_t db_table_bytes(db_table_t *t);         /* its records' bytes in the file */
/* A table's stored layout: its fields (names stay valid) and version; nfields. */
int db_table_layout(db_table_t *t, db_field_t *f, int max, int *version, int *index);
/* The file's size, its bytes of old values, the keys */
void db_stats(uint32_t *size, uint32_t *dead, int *keys);
uint32_t db_writes(void);               /* writes to the card since boot (the tests) */
/* Text export and import (fs/db_text.c): every key and table, one line each.
 * Import replaces the whole store with the file's contents; it reads the file
 * through once first and changes nothing if a line is wrong (*line: which). */
int db_export(const char *path);
int db_import(const char *path, int *line);
/* For the import: drop everything, then put records with their own ids. */
void db_clear_all(void);
uint32_t db_insert_id(db_table_t *t, uint32_t id, const db_value_t *v, int n);
void db_bulk(bool on);                  /* table writes gathered until it ends */

/* ---- UTF-8 ------------------------------------------------------------------------ */
bool utf8_valid(const char *s, size_t n);
size_t utf8_cut(const char *s, size_t n);   /* the most of s's n bytes ending on a character */
/* in (n bytes) as UTF-8 into out (size, NUL-ended): bytes that are not UTF-8 are
 * taken as Latin-1 (old files from a PC). Its length. */
size_t utf8_from_latin1(const char *in, size_t n, char *out, size_t size);

#endif
