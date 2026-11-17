/* B310E OS — volume by category: music, apps, alarm, calls, the key
 * feedback's vibration (include/os/audio.h), and a tag for each app with
 * sound. The category that last started a sound owns the codec volume, so
 * changing its level is heard at once.
 *
 * The OS's levels live in the settings. The tags are records of the store's
 * "volumes" table (os/db.h): name, level, icon (none: empty). The name and icon
 * are copied: the app they came from may be unloaded, and the page lists them
 * all the same. Saved within the second (volume_save), never from the caller:
 * a tag can appear on a game's thread, at its first sound. Until 2026-11-17
 * they were lines of /b310e-os/volumes.txt (the level, the icon in hex or "-",
 * the name), read once into the store.
 *
 * A reset hides tags and leaves their slots: an app that is loaded still holds
 * its number. Hidden ones are not saved, so a boot frees the slots, and playing
 * lists one again at the level it has.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/db.h"
#include "os/fs.h"
#include "os/lib.h"
#include "os/ui.h"

#define VOL_TAGS   24
#define OLD_FILE   OS_DIR "/volumes.txt"

typedef struct {
    char name[20];                      /* an app's name fits (app_file_t) */
    uint8_t level;
    bool has_icon;
    bool hidden;                        /* reset: not listed, not saved */
    icon_t icon;
} tag_t;

/* 0 mute, then -40 dB .. 0 dB, closer together near the top */
static const int16_t level_db10[VOL_LEVELS + 1] = {
    -1000, -400, -340, -280, -230, -180, -140, -100, -60, -30, 0,
};

static const struct {
    const char *name;
    const icon_t *icon;
} fixed[VOL_COUNT] = {
    [VOL_MUSIC] = { "Music", &icon_music },
    [VOL_APP] = { "Other apps", &icon_apps },
    [VOL_ALARM] = { "Alarm", &icon_alarm },
    [VOL_CALL] = { "Calls", &icon_sim },
    [VOL_TACTILE] = { "Tactile", &icon_vibrate },
};

static tag_t tags[VOL_TAGS];
static volatile int n_tags;
static volatile bool tags_changed;
static volatile uint32_t tags_dirty;    /* a bit a tag: to write */
static uint32_t tag_id[VOL_TAGS];       /* its record, 0 none */
static int active = VOL_MUSIC;

static void changed(int i)
{
    tags_dirty |= 1u << i;
    tags_changed = true;
}

static tag_t *tag_of(int cat)
{
    cat -= VOL_COUNT;
    return cat >= 0 && cat < n_tags ? &tags[cat] : NULL;
}

int volume_db10(int level)
{
    if (level < 0)
        level = 0;
    if (level > VOL_LEVELS)
        level = VOL_LEVELS;
    return level_db10[level];
}

int volume_level(int cat)
{
    tag_t *t = tag_of(cat);

    if (t)
        return t->level;
    return cat >= 0 && cat < VOL_COUNT ? ui_settings.vol[cat] : 0;
}

void volume_use(int cat)
{
    if (cat < 0 || cat == VOL_TACTILE || (cat >= VOL_COUNT && !tag_of(cat)))
        return;
    active = cat;
    if (cat >= VOL_COUNT && tag_of(cat)->hidden) {
        tag_of(cat)->hidden = false;
        changed(cat - VOL_COUNT);
    }
    audio_set_volume(volume_db10(volume_level(cat)));
}

int volume_active(void)
{
    return active;
}

int volume_current(void)
{
    return audio_playing() || fm_is_on() ? active : VOL_MUSIC;
}

void volume_set(int cat, int level)
{
    int max = cat == VOL_TACTILE ? 3 : VOL_LEVELS;
    tag_t *t = tag_of(cat);

    if (!t && (cat < 0 || cat >= VOL_COUNT))
        return;
    if (level < 0)
        level = 0;
    if (level > max)
        level = max;
    if (cat == active)
        audio_set_volume(volume_db10(level));
    if (t) {
        t->level = (uint8_t)level;
        changed(cat - VOL_COUNT);
    } else {
        ui_settings.vol[cat] = level;
        ui_settings_save();
    }
}

int volume_tag(const char *name, const void *icon)
{
    char want[sizeof(tags[0].name)] = "";
    uint32_t s;
    int cat = VOL_APP;

    if (!name || !name[0])
        return VOL_APP;
    strncpy(want, name, sizeof(want) - 1);
    s = arch_irq_save();                /* the UI's and the apps' threads */
    for (int i = 0; i < n_tags; i++)
        if (!strcmp(tags[i].name, want))
            cat = VOL_COUNT + i;
    if (cat == VOL_APP && n_tags < VOL_TAGS) {
        tag_t *t = &tags[n_tags];

        memcpy(t->name, want, sizeof(want));
        t->level = (uint8_t)ui_settings.vol[VOL_APP];
        cat = VOL_COUNT + n_tags;
        n_tags++;
        changed(cat - VOL_COUNT);
    }
    if (cat != VOL_APP && tags[cat - VOL_COUNT].hidden) {
        tags[cat - VOL_COUNT].hidden = false;
        changed(cat - VOL_COUNT);
    }
    if (cat != VOL_APP && icon && memcmp(&tags[cat - VOL_COUNT].icon, icon, sizeof(icon_t))) {
        memcpy(&tags[cat - VOL_COUNT].icon, icon, sizeof(icon_t));
        tags[cat - VOL_COUNT].has_icon = true;
        changed(cat - VOL_COUNT);
    }
    arch_irq_restore(s);
    if (cat == VOL_APP)
        klog("volume: no room for a tag for %s", name);
    return cat;
}

int volume_tags(void)
{
    int n = 0;

    for (int i = 0; i < n_tags; i++)
        if (!tags[i].hidden)
            n++;
    return n;
}

int volume_tag_at(int at)
{
    for (int i = 0; i < n_tags; i++)
        if (!tags[i].hidden && at-- == 0)
            return VOL_COUNT + i;
    return VOL_APP;
}

void volume_reset(void)
{
    static const int defaults[VOL_COUNT] = VOL_DEFAULTS;
    int playing = volume_current();
    uint32_t s = arch_irq_save();

    for (int i = 0; i < VOL_COUNT; i++)
        ui_settings.vol[i] = defaults[i];
    for (int i = 0; i < n_tags; i++) {
        tags[i].level = (uint8_t)defaults[VOL_APP];
        tags[i].hidden = VOL_COUNT + i != playing;
    }
    tags_dirty = (1u << VOL_TAGS) - 1;
    tags_changed = true;
    arch_irq_restore(s);
    audio_set_volume(volume_db10(volume_level(active)));
    ui_settings_save();
    klog("volume: reset");
}

const char *volume_name(int cat)
{
    tag_t *t = tag_of(cat);

    if (t)
        return t->name;
    return cat >= 0 && cat < VOL_COUNT ? fixed[cat].name : "";
}

const void *volume_icon(int cat)
{
    tag_t *t = tag_of(cat);

    if (t)
        return t->has_icon ? &t->icon : &icon_apps;
    return cat >= 0 && cat < VOL_COUNT ? fixed[cat].icon : &icon_apps;
}

/* ---- the old file, read once ------------------------------------------------ */
#define ICON_HEX   ((int)sizeof(icon_t) * 2)
#define LINE_MAX_  (4 + ICON_HEX + 1 + (int)sizeof(((tag_t *)0)->name) + 2)

static int hex_digit(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* "<level> <hex or -> <name>"; a line that is not one is left out */
static void parse_line(char *line)
{
    char *hex = strchr(line, ' '), *name = hex ? strchr(hex + 1, ' ') : NULL;
    int level = 0;
    tag_t *t = &tags[n_tags];

    if (!name || !name[1] || n_tags >= VOL_TAGS)
        return;
    for (const char *p = line; *p >= '0' && *p <= '9'; p++)
        level = level * 10 + (*p - '0');
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name + 1, sizeof(t->name) - 1);
    t->level = (uint8_t)(level > VOL_LEVELS ? VOL_LEVELS : level);
    hex++;
    if (name - hex == ICON_HEX) {
        t->has_icon = true;
        for (int i = 0; i < ICON_HEX; i += 2) {
            int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);

            if (hi < 0 || lo < 0) {
                t->has_icon = false;
                break;
            }
            t->icon.a[i / 2] = (uint8_t)(hi << 4 | lo);
        }
    }
    n_tags++;
}

static void import_file(void)
{
    char line[LINE_MAX_], c;
    int len = 0;
    FIL f;
    UINT n;

    if (f_open(&f, OLD_FILE, FA_READ) != FR_OK)
        return;
    while (f_read(&f, &c, 1, &n) == FR_OK && n == 1) {
        if (c == '\n') {
            line[len] = 0;
            parse_line(line);
            len = 0;
        } else if (c != '\r' && len < (int)sizeof(line) - 1) {
            line[len++] = c;
        }
    }
    f_close(&f);
    for (int i = 0; i < n_tags; i++)
        changed(i);
    klog("volume: %s read into the store, %d tags", OLD_FILE, n_tags);
}

/* ---- the store ------------------------------------------------------------- */
enum { F_NAME, F_LEVEL, F_ICON };
static const db_field_t fields[] = {
    { "name", DB_STR }, { "level", DB_INT }, { "icon", DB_BLOB },
};
static const db_schema_t schema = { "volumes", 1, 3, F_NAME, fields, NULL };
static db_table_t *table;
static uint32_t own_gen;                /* the table as we last left it */

/* The records into the tags, by name: a loaded app keeps its slot (its category).
 * Tags with no record are hidden. */
static void from_table(void)
{
    uint32_t ids[VOL_TAGS], s;
    int n = db_find(table, -1, NULL, ids, VOL_TAGS, 0);

    s = arch_irq_save();
    for (int i = 0; i < n_tags; i++) {
        tags[i].hidden = true;
        tag_id[i] = 0;
    }
    arch_irq_restore(s);
    while (n-- > 0) {                   /* oldest first: the order they came in */
        uint8_t buf[sizeof(tag_t) + 8];
        db_value_t v[3];
        char name[sizeof(tags[0].name)];
        int at = -1;

        if (db_get(table, ids[n], v, 3, buf, sizeof(buf)) != DB_OK || !v[F_NAME].len)
            continue;
        snprintf(name, sizeof(name), "%s", (const char *)v[F_NAME].p);
        s = arch_irq_save();
        for (int i = 0; i < n_tags && at < 0; i++)
            if (!strcmp(tags[i].name, name))
                at = i;
        if (at < 0 && n_tags < VOL_TAGS) {
            at = n_tags++;
            memset(&tags[at], 0, sizeof(tags[at]));
            memcpy(tags[at].name, name, sizeof(name));
        }
        if (at >= 0) {
            tags[at].level = (uint8_t)(v[F_LEVEL].i > VOL_LEVELS ? VOL_LEVELS : v[F_LEVEL].i);
            tags[at].has_icon = v[F_ICON].len == sizeof(icon_t);
            if (tags[at].has_icon)
                memcpy(&tags[at].icon, v[F_ICON].p, sizeof(icon_t));
            tags[at].hidden = false;
            tag_id[at] = ids[n];
        }
        arch_irq_restore(s);
    }
    own_gen = db_table_gen(table);
}

/* Changed by someone else (an import): taken up */
static void table_changed(void *ctx)
{
    if (table && db_table_gen(table) != own_gen)
        from_table();
}

void volume_load(void)
{
    table = db_table_open(&schema);
    if (!db_get_bool("import.volumes", false)) {
        import_file();
        db_set_bool("import.volumes", true);
        volume_save();
    } else if (table) {
        from_table();
    }
    db_watch("volumes", true, table_changed, NULL);
}

/* The tags that changed, each its record; a hidden one's goes. What fails
 * (no card) is tried again the next second. */
void volume_save(void)
{
    uint32_t dirty, s;

    if (!tags_changed || (!table && !(table = db_table_open(&schema))))
        return;
    s = arch_irq_save();
    dirty = tags_dirty;
    tags_dirty = 0;
    tags_changed = false;               /* before: one that comes now is saved next */
    arch_irq_restore(s);
    for (int i = 0; i < n_tags; i++) {
        tag_t t;
        int r = DB_OK;

        if (!(dirty & 1u << i))
            continue;
        s = arch_irq_save();
        t = tags[i];
        arch_irq_restore(s);
        if (t.hidden) {
            if (tag_id[i] && (r = db_remove(table, tag_id[i])) == DB_ENOENT)
                r = DB_OK;
            if (!r)
                tag_id[i] = 0;
        } else {
            db_value_t v[3] = {
                DB_STRV(t.name), DB_INTV(t.level),
                { .p = &t.icon, .len = t.has_icon ? sizeof(icon_t) : 0 },
            };

            if (tag_id[i])
                r = db_update(table, tag_id[i], v, 3);
            else if (!(tag_id[i] = db_insert(table, v, 3)))
                r = DB_EIO;
        }
        if (r) {                        /* this one and those after it: next time */
            s = arch_irq_save();
            tags_dirty |= dirty & ~((1u << i) - 1);
            tags_changed = true;
            arch_irq_restore(s);
            break;
        }
    }
    own_gen = db_table_gen(table);
}
