/* The data store's tables through the app API, for os/tests/test-db.py.
 *
 * "fill": 300 records in the app's "items" table (n, group = n % 10 as the
 * index field, a name in Turkish, German or Russian, a flag), then queries:
 * newest first, by the index field, by a string and a bool that are not in
 * the index, an update of one field, a whole record, removals; and that the
 * app's keys are its own. "verify": the same results read back (after a
 * compaction, a restart, an import). "upgrade": the table opened with a field
 * more, version 2: the hook runs once, old records read the new field empty.
 * The outcome goes to the app's key "result" ("ok ...", or "fail: step").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"

enum { F_N, F_GROUP, F_NAME, F_FLAG, F_NOTE };
static const db_field_t fields[] = {
    { "n", DB_INT }, { "group", DB_INT }, { "name", DB_STR }, { "flag", DB_BOOL },
    { "note", DB_STR },
};
static const char *const scripts[3] = { "Çağrı ğüşıöç İ", "Grüße äöüß", "Звонок" };

static int upgraded_from = -1;
static void upgrade(db_table_t *t, unsigned from)
{
    upgraded_from = (int)from;
}

static const db_schema_t v1 = { "items", 1, 4, F_GROUP, fields, NULL };
static const db_schema_t v2 = { "items", 2, 5, F_GROUP, fields, upgrade };

static char result[160];
static uint32_t ids[400];
static char buf[512];

static void name_of(int n, char *out, int size)
{
    snprintf(out, (size_t)size, "%s %d", scripts[n % 3], n);
}

static int count_where(db_table_t *t, int field, db_value_t v)
{
    return db_find(t, field, &v, ids, 400, 0);
}

static const char *check(db_table_t *t, bool fresh)
{
    db_value_t v[5];
    char want[48];

    if (db_count(t) != 270)
        return "count";
    if (count_where(t, F_GROUP, DB_INTV(3)) != 30)
        return "group 3";
    for (int i = 0; i < 30; i++)
        if ((ids[i] - 1) % 10 != 3 || (i && ids[i] >= ids[i - 1]))
            return "group 3 ids, newest first";
    if (count_where(t, F_FLAG, DB_INTV(1)) != 30)
        return "flag";
    if (count_where(t, F_NAME, DB_STRV("Звонок 8")) != 1 || ids[0] != 9)     /* id = n + 1 */
        return "name";
    if (db_get(t, 9, v, 4, buf, sizeof(buf)) != DB_OK || v[F_N].i != 8 ||
        strcmp(v[F_NAME].p, "Звонок 8"))
        return "get 9";
    if (db_get(t, 1, v, 4, buf, sizeof(buf)) != DB_OK || strcmp(v[F_NAME].p, "Güncellendi"))
        return "updated record";
    if (db_get(t, 10, v, 4, buf, sizeof(buf)) != DB_ENOENT)
        return "removed record";
    name_of(298, want, sizeof(want));   /* id 300 was n 299, group 9: removed */
    if (db_find(t, -1, NULL, ids, 3, 0) != 3 || ids[0] != 299 || ids[1] != 298 ||
        db_get(t, 299, v, 4, buf, sizeof(buf)) || strcmp(v[F_NAME].p, want))
        return "newest";
    if (db_find(t, -1, NULL, ids, 2, 299) != 2 || ids[0] != 298 || ids[1] != 297)
        return "before";
    if (db_get_str("hello", buf, sizeof(buf), "") <= 0 || strcmp(buf, "Merhaba dünya"))
        return "own key";
    if (db_get_int("ui.brightness", -7) != -7)
        return "namespace";
    return NULL;
}

static const char *fill(void)
{
    db_table_t *t = db_table_open(&v1);
    db_value_t v[4];
    char name[48];
    const char *bad;
    int n;

    if (!t)
        return "open";
    if (db_count(t))
        return "not empty";
    for (int i = 0; i < 300; i++) {
        name_of(i, name, sizeof(name));
        v[F_N] = DB_INTV(i);
        v[F_GROUP] = DB_INTV(i % 10);
        v[F_NAME] = DB_STRV(name);
        v[F_FLAG] = DB_INTV(0);
        if (db_insert(t, v, 4) != (uint32_t)i + 1)
            return "insert";
    }
    if (db_count(t) != 300)
        return "count 300";
    n = count_where(t, F_GROUP, DB_INTV(5));
    for (int i = 0; i < n; i++)
        if (db_update_field(t, ids[i], F_FLAG, &DB_INTV(1)) != DB_OK)
            return "update field";
    n = count_where(t, F_GROUP, DB_INTV(9));
    for (int i = 0; i < n; i++)
        if (db_remove(t, ids[i]) != DB_OK)
            return "remove";
    v[F_N] = DB_INTV(0);
    v[F_GROUP] = DB_INTV(0);
    v[F_NAME] = DB_STRV("Güncellendi");
    v[F_FLAG] = DB_INTV(0);
    if (db_update(t, 1, v, 4) != DB_OK)
        return "update";
    if (db_set_str("hello", "Merhaba dünya") != DB_OK)
        return "set";
    if (db_set_str("broken", "\xc3") != DB_EINVAL)
        return "bad UTF-8 taken";
    if ((bad = check(t, true)))
        return bad;
    return NULL;
}

static const char *upgrade_check(void)
{
    db_table_t *t = db_table_open(&v2);
    db_value_t v[5];
    uint32_t id;

    if (!t || upgraded_from != 1)
        return "upgrade hook";
    if (db_get(t, 9, v, 5, buf, sizeof(buf)) != DB_OK || v[F_NOTE].len || strcmp(v[F_NAME].p,
                                                                              "Звонок 8"))
        return "old record, new field";
    v[F_NOTE] = DB_STRV("yeni alan");
    if (!(id = db_insert(t, v, 5)) || db_get(t, id, v, 5, buf, sizeof(buf)) ||
        strcmp(v[F_NOTE].p, "yeni alan"))
        return "new field";
    return db_remove(t, id) ? "remove new" : NULL;
}

static void work(void *arg)
{
    const char *what = app_arg(), *bad;

    if (!what)
        what = "verify";
    if (!strcmp(what, "fill"))
        bad = fill();
    else if (!strcmp(what, "upgrade"))
        bad = upgrade_check();
    else
        bad = db_table_open(&v1) ? check(db_table_open(&v1), false) : "open";
    if (bad)
        snprintf(result, sizeof(result), "fail: %s %s", what, bad);
    else
        snprintf(result, sizeof(result), "ok %s", what);
    db_set_str("result", result);
    klog("dbtest: %s", result);
}

static void draw(screen_t *s)
{
    gfx_text(UI_PAD, UI_TOP + 10, result[0] ? result : "working...", C_TEXT, FONT_BODY, 1,
             LCD_WIDTH - 2 * UI_PAD);
}

static screen_t screen = { .title = "DB test", .right = "Close", .draw = draw, .tick_ms = 200 };

static screen_t *start(void)
{
    result[0] = 0;
    db_delete("result");
    thread_create("dbtest", PRIO_LOW, 4096, work, NULL);
    return &screen;
}

APP("DB test", start, NULL, NULL);
