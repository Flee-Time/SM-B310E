/* Host test glue: os/fs/db.c in this translation unit, and the few of its
 * statics the test reaches (test_db.c). Its own unit, without <stdio.h>:
 * mingw's stdio defines snprintf inline, which os/lib.h declares.
 *
 * A reboot puts every static of db.c back as it was when the program
 * started, whatever db.c's statics are (they change: other work goes on in
 * that file). test-host.py renames this unit's .data to .data$dbt1 and its
 * .bss to .data$dbt1z (objcopy, after compiling); the two markers below are
 * .data$dbt0 and .data$dbt2, and the linker sorts .data$* by name, so
 * [dbt_begin, dbt_end) holds them all. dbt_ram_lost checks that db is inside.
 *
 * Depends on db.c: db (.ready .file .kv_dirty .pend.len), card_ok(), DB_NEW,
 * DB_BAD; with HOST_PROVE also kvs and kv_t (.type .dirty .i .next).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
__attribute__((section(".data$dbt0"), used)) static volatile char dbt_begin = 1;

#include "../../fs/db.c"

#include "test_db.h"

__attribute__((section(".data$dbt2"), used)) static volatile char dbt_end = 1;

static uint8_t *dbt_initial;            /* db.c's statics at the start (host malloc) */
static size_t dbt_size;

void *malloc(size_t n);
void abort(void);

/* RAM is gone: every static of db.c as at the start (the allocator's blocks
 * go separately) */
void dbt_ram_lost(void)
{
    uint8_t *b = (uint8_t *)&dbt_begin + 1, *e = (uint8_t *)&dbt_end;

    if (!dbt_initial) {
        if (e <= b || (uint8_t *)&db < b || (uint8_t *)&db + sizeof(db) > e)
            abort();                    /* the sections are not laid out as they must be */
        dbt_size = (size_t)(e - b);
        dbt_initial = malloc(dbt_size);
        memcpy(dbt_initial, b, dbt_size);
        return;
    }
    memcpy(b, dbt_initial, dbt_size);
}

bool dbt_has_file(void)
{
    return db.ready && db.file && card_ok();
}

bool dbt_file(void)
{
    return db.file;
}

bool dbt_unwritten(void)
{
    return db.kv_dirty || db.pend.len;
}

bool dbt_kv_dirty(void)
{
    return db.kv_dirty;
}

/* a store that changes RAM without marking it for the card */
void dbt_break_ram(void)
{
#ifdef HOST_PROVE
    for (kv_t *k = kvs; k; k = k->next)
        if (k->type == DB_INT && !k->dirty) {
            k->i ^= 0x100;
            return;
        }
#endif
}

const char *dbt_new_path(void)
{
    return DB_NEW;
}

const char *dbt_bad_path(void)
{
    return DB_BAD;
}
