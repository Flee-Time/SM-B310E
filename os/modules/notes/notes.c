/* B310E OS — Notes: short texts, records of the app's "notes" table in the
 * store (os/db.h; the text in UTF-8), written with the multi-tap editor. The
 * list shows each note's first line, newest first; Center opens one, LSoft
 * deletes.
 *
 * Until 2026-11-17 each note was a file, /b310e-os/notes/NOTE0001.TXT ...:
 * they are read into the table once, oldest first, and left where they are.
 * A note longer than the editor takes (one from a PC) opens to read only:
 * saving what the editor loaded would cut the rest.
 *
 * A loadable app (os/modules); it was built into the OS until 2026-11-07.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "icons.h"

#define NOTES_DIR  "/b310e-os/notes"    /* the old files */
#define NOTE_MAX   2000                 /* bytes the editor takes */
#define MAX_NOTES  256                  /* listed */
#define MAX_FILES  1024                 /* old files read in */
#define FIRST_LINE 28
#define BUF_SIZE   (DB_REC_MAX + 16)
#define FILE_MAX   ((DB_REC_MAX - 64) / 2)  /* an old file's bytes: as UTF-8 they may double */

enum { F_TEXT, F_FILE };
static const db_field_t fields[] = { { "text", DB_STR }, { "file", DB_INT } };
static const db_schema_t schema = { "notes", 1, 2, -1, fields, NULL };

typedef struct {
    uint32_t id;
    char first[FIRST_LINE];
} note_t;

static db_table_t *table;
static note_t *notes;
static char *buf;                       /* one note */
static int count;
static list_t list;
static bool stale = true;
static uint32_t editing;                /* the note being edited; 0: new */

static int number_of(const char *name)
{
    int v = 0;

    if (strlen(name) != 12 || memcmp(name, "NOTE", 4) || strcmp(name + 8, ".TXT"))
        return 0;
    for (int i = 4; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9')
            return 0;
        v = v * 10 + name[i] - '0';
    }
    return v;
}

/* NOTEnnnn.TXT into the table, by number; one too long for a record stays a file */
static void import_one(int num, char *tmp)
{
    char path[48];
    FIL f;
    UINT n = 0;
    size_t len;
    db_value_t v[2];

    snprintf(path, sizeof(path), NOTES_DIR "/NOTE%04d.TXT", num);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return;
    if (f_size(&f) > FILE_MAX) {
        f_close(&f);
        klog("notes: %s too long for the store, left as a file", path);
        return;
    }
    f_read(&f, buf, FILE_MAX, &n);
    f_close(&f);
    len = 0;
    for (UINT k = 0; k < n; k++)        /* CR LF from a PC: LF */
        if (buf[k] != '\r' && buf[k])
            buf[len++] = buf[k];
    len = utf8_from_latin1(buf, len, tmp, BUF_SIZE);
    v[F_TEXT].p = tmp;
    v[F_TEXT].len = len;
    v[F_FILE].i = num;
    if (db_insert(table, v, 2))
        klog("notes: %s read into the store", path);
}

static void import_files(void)
{
    uint16_t *nums = malloc(MAX_FILES * sizeof(uint16_t));
    char *tmp = malloc(BUF_SIZE);
    DIR dir;
    FILINFO fi;
    int n = 0;

    if (nums && tmp && f_opendir(&dir, NOTES_DIR) == FR_OK) {
        while (n < MAX_FILES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            int num = number_of(fi.fname), at;

            if (!num || (fi.fattrib & AM_DIR))
                continue;
            for (at = n; at > 0 && nums[at - 1] > num; at--)    /* oldest first */
                nums[at] = nums[at - 1];
            nums[at] = (uint16_t)num;
            n++;
        }
        f_closedir(&dir);
        for (int i = 0; i < n; i++)
            import_one(nums[i], tmp);
    }
    if (nums && tmp)
        db_set_bool("imported", true);
    free(nums);
    free(tmp);
}

static void load(void)
{
    uint32_t *ids;
    int n;

    count = 0;
    stale = false;
    if (!notes)
        notes = malloc(sizeof(*notes) * MAX_NOTES);
    if (!buf)
        buf = malloc(BUF_SIZE);
    if (!table)
        table = db_table_open(&schema);
    if (!notes || !buf || !table)
        goto done;
    if (!db_get_bool("imported", false))
        import_files();
    ids = malloc(MAX_NOTES * sizeof(uint32_t));
    n = ids ? db_find(table, -1, NULL, ids, MAX_NOTES, 0) : 0;
    for (int i = 0; i < n; i++) {
        db_value_t v[2];
        note_t *t = &notes[count];
        uint32_t len = 0;

        if (db_get(table, ids[i], v, 2, buf, BUF_SIZE) != DB_OK)
            continue;
        while (len < v[F_TEXT].len && buf[len] != '\n')
            len++;
        if (len > FIRST_LINE - 1)
            len = utf8_cut(v[F_TEXT].p, FIRST_LINE - 1);
        memcpy(t->first, v[F_TEXT].p, len);
        t->first[len] = 0;
        if (!t->first[0])
            snprintf(t->first, FIRST_LINE, "(empty)");
        t->id = ids[i];
        count++;
    }
    free(ids);
done:
    list.count = count + 1;
    if (list.sel >= list.count)
        list.sel = list.count - 1;
}

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    if (i == 0)
        snprintf(text, (size_t)tn, "New note");
    else
        snprintf(text, (size_t)tn, "%s", notes[i - 1].first);
}

static const icon_t *item_icon(void *ctx, int i)
{
    return i == 0 ? ui_icon("notes") : ui_icon("file");
}

static void saved(const char *text, void *ctx)
{
    size_t len = strlen(text);
    db_value_t v = { .p = text, .len = len };
    char *fixed = NULL;
    int r;

    if (!table)
        return;
    if (!utf8_valid(text, len) && (fixed = malloc(2 * len + 1))) {
        v.len = utf8_from_latin1(text, len, fixed, 2 * len + 1);
        v.p = fixed;
    }
    if (editing) {
        r = db_update_field(table, editing, F_TEXT, &v);
    } else {
        if (!len) {
            free(fixed);
            return;                     /* nothing typed: no note */
        }
        r = (editing = db_insert(table, &v, 1)) ? DB_OK : DB_EIO;
    }
    free(fixed);
    if (r != DB_OK) {
        ui_message("Notes", "Could not save the note.");
        return;
    }
    klog("notes: saved note %lu, %u bytes", (unsigned long)editing, (unsigned)v.len);
    stale = true;
}

static void open_note(int i)
{
    db_value_t v[2];

    if (i == 0) {
        editing = 0;
        ui_text_edit("New note", "", NOTE_MAX, true, saved, NULL);
        return;
    }
    editing = notes[i - 1].id;
    if (!buf || db_get(table, editing, v, 2, buf, BUF_SIZE) != DB_OK) {
        ui_message("Notes", "Could not open the note.");   /* an empty editor would save over it */
        return;
    }
    if (v[F_TEXT].len > NOTE_MAX) {     /* saving the part loaded would cut the rest */
        ui_text_view("Note (too long to edit)", buf);
        return;
    }
    ui_text_edit("Note", buf, NOTE_MAX, true, saved, NULL);
}

static uint32_t to_delete;

static void delete_yes(void)
{
    if (table && db_remove(table, to_delete) == DB_OK)
        klog("notes: deleted note %lu", (unsigned long)to_delete);
    stale = true;
}

static void draw(screen_t *s)
{
    if (stale)
        load();
    list_draw(&list);
    if (!card_mounted())
        gfx_text_centered(UI_BOTTOM - 12, "no card", C_BAD, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&list, ev)) {
        s->left = list.sel ? "Delete" : "New";
        return;
    }
    if (ev->code == KEY_CENTER || (ev->code == KEY_LSOFT && list.sel == 0)) {
        if (card_mounted() && table)
            open_note(list.sel);
    } else if (ev->code == KEY_LSOFT && list.sel > 0) {
        to_delete = notes[list.sel - 1].id;
        ui_confirm("Delete note", notes[list.sel - 1].first, delete_yes);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static void leave(screen_t *s)
{
    free(notes);
    free(buf);
    notes = NULL;
    buf = NULL;
    stale = true;
}

static screen_t screen = {
    .title = "Notes", .left = "New", .center = "Open", .right = "Back",
    .draw = draw, .key = key, .leave = leave,
};

static screen_t *start(void)
{
    list.count = 1;
    list.sel = 0;
    list.item = item;
    list.icon = item_icon;
    stale = true;
    screen.left = "New";
    return &screen;
}

static const icon_t app_icon = APP_ICON_NOTES;
APP("Notes", start, NULL, &app_icon);
