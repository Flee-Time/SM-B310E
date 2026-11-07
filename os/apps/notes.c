/* B310E OS — Notes: short texts, one file each in /b310e-os/notes
 * (NOTE0001.TXT ...), written with the multi-tap editor. The list shows
 * each note's first line, newest first; Center opens one, LSoft deletes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

#define NOTES_DIR  OS_DIR "/notes"
#define NOTE_MAX   2000                 /* characters in one note */
#define MAX_NOTES  64
#define FIRST_LINE 28

typedef struct {
    uint16_t number;                    /* NOTEnnnn.TXT */
    char first[FIRST_LINE];
} note_t;

static note_t *notes;
static int count;
static list_t list;
static bool stale = true;
static int editing = -1;                /* the note number being edited; 0: new */

static void path_of(int number, char *out, int n)
{
    snprintf(out, (size_t)n, NOTES_DIR "/NOTE%04d.TXT", number);
}

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

static void load(void)
{
    DIR dir;
    FILINFO fi;

    count = 0;
    stale = false;
    if (!notes)
        notes = kmalloc(sizeof(*notes) * MAX_NOTES);
    if (!notes || f_opendir(&dir, NOTES_DIR) != FR_OK)
        goto done;
    while (count < MAX_NOTES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        int num = number_of(fi.fname);
        char path[48];
        FIL f;
        UINT n = 0;
        note_t *t;
        int at;

        if (!num || (fi.fattrib & AM_DIR))
            continue;
        /* newest (highest number) first */
        for (at = 0; at < count && notes[at].number > num; at++)
            ;
        memmove(&notes[at + 1], &notes[at], sizeof(*notes) * (size_t)(count - at));
        t = &notes[at];
        t->number = (uint16_t)num;
        t->first[0] = 0;
        path_of(num, path, sizeof(path));
        if (f_open(&f, path, FA_READ) == FR_OK) {
            f_read(&f, t->first, FIRST_LINE - 1, &n);
            f_close(&f);
        }
        t->first[n] = 0;
        for (UINT i = 0; i < n; i++)
            if (t->first[i] == '\n' || t->first[i] == '\r') {
                t->first[i] = 0;
                break;
            }
        if (!t->first[0])
            snprintf(t->first, FIRST_LINE, "(empty)");
        count++;
    }
    f_closedir(&dir);
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
    return i == 0 ? &icon_notes : &icon_file;
}

static void saved(const char *text, void *ctx)
{
    char path[48];
    FIL f;
    UINT n;
    int num = editing;
    size_t len = strlen(text);

    if (num == 0) {                     /* new: the next free number */
        if (!len)
            return;                     /* nothing typed: no file */
        f_mkdir(OS_DIR);
        f_mkdir(NOTES_DIR);
        num = 1;
        for (int i = 0; i < count; i++)
            if (notes[i].number >= num)
                num = notes[i].number + 1;
    }
    path_of(num, path, sizeof(path));
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        ui_message("Notes", "Could not save the note.");
        return;
    }
    f_write(&f, text, (UINT)len, &n);
    f_close(&f);
    klog("notes: saved NOTE%04d.TXT, %u bytes", num, (unsigned)len);
    stale = true;
}

static void open_note(int i)
{
    char path[48];
    char *buf;
    FIL f;
    UINT n = 0;

    if (i == 0) {
        editing = 0;
        ui_text_edit("New note", "", NOTE_MAX, true, saved, NULL);
        return;
    }
    buf = kmalloc(NOTE_MAX + 1);
    if (!buf)
        return;
    editing = notes[i - 1].number;
    path_of(editing, path, sizeof(path));
    if (f_open(&f, path, FA_READ) == FR_OK) {
        f_read(&f, buf, NOTE_MAX, &n);
        f_close(&f);
    }
    buf[n] = 0;
    for (UINT k = 0; k < n; k++)        /* CR LF from a PC: LF */
        if (buf[k] == '\r')
            buf[k] = ' ';
    ui_text_edit("Note", buf, NOTE_MAX, true, saved, NULL);
    kfree(buf);
}

static int to_delete;

static void delete_yes(void)
{
    char path[48];

    path_of(to_delete, path, sizeof(path));
    if (f_unlink(path) == FR_OK)
        klog("notes: deleted NOTE%04d.TXT", to_delete);
    stale = true;
}

static void draw(screen_t *s)
{
    if (stale)
        load();
    list_draw(&list);
    if (!fs_info()->mounted)
        gfx_text_centered(UI_BOTTOM - 12, "no card", C_BAD, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&list, ev)) {
        s->left = list.sel ? "Delete" : "New";
        return;
    }
    if (ev->code == KEY_CENTER || (ev->code == KEY_LSOFT && list.sel == 0)) {
        if (fs_info()->mounted)
            open_note(list.sel);
    } else if (ev->code == KEY_LSOFT && list.sel > 0) {
        to_delete = notes[list.sel - 1].number;
        ui_confirm("Delete note", notes[list.sel - 1].first, delete_yes);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static void leave(screen_t *s)
{
    kfree(notes);
    notes = NULL;
    stale = true;
}

static screen_t screen = {
    .title = "Notes", .left = "New", .center = "Open", .right = "Back",
    .draw = draw, .key = key, .leave = leave,
};

screen_t *app_notes(void)
{
    list.count = 1;
    list.sel = 0;
    list.item = item;
    list.icon = item_icon;
    stale = true;
    screen.left = "New";
    return &screen;
}
