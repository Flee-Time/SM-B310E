/* B310E OS — volume by category: music, apps, alarm, calls, the key
 * feedback's vibration (include/os/audio.h), and a tag for each app with
 * sound. The category that last started a sound owns the codec volume, so
 * changing its level is heard at once.
 *
 * The OS's levels live in the settings. The tags are in /b310e-os/volumes.txt,
 * one a line: the level, the icon in hex or "-", the name. The name and icon
 * are copied: the app they came from may be unloaded, and the page lists them
 * all the same. Saved within the second (volume_save), never from the caller:
 * a tag can appear on a game's thread, at its first sound.
 *
 * A reset hides tags and leaves their slots: an app that is loaded still holds
 * its number. Hidden ones are not saved, so a boot frees the slots, and playing
 * lists one again at the level it has.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/fs.h"
#include "os/lib.h"
#include "os/ui.h"

#define VOL_TAGS   24
#define TAGS_FILE  OS_DIR "/volumes.txt"

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
static int active = VOL_MUSIC;

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
        tags_changed = true;
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
        tags_changed = true;
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
        tags_changed = true;
    }
    if (cat != VOL_APP && tags[cat - VOL_COUNT].hidden) {
        tags[cat - VOL_COUNT].hidden = false;
        tags_changed = true;
    }
    if (cat != VOL_APP && icon && memcmp(&tags[cat - VOL_COUNT].icon, icon, sizeof(icon_t))) {
        memcpy(&tags[cat - VOL_COUNT].icon, icon, sizeof(icon_t));
        tags[cat - VOL_COUNT].has_icon = true;
        tags_changed = true;
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

/* ---- the file ------------------------------------------------------------ */
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

void volume_load(void)
{
    char line[LINE_MAX_], c;
    int len = 0;
    FIL f;
    UINT n;

    if (f_open(&f, TAGS_FILE, FA_READ) != FR_OK)
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
}

void volume_save(void)
{
    static const char digits[] = "0123456789abcdef";
    char line[LINE_MAX_];
    FIL f;
    UINT w;

    if (!tags_changed || !fs_info()->mounted)
        return;
    if (f_open(&f, TAGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;                         /* again in a second */
    tags_changed = false;               /* before: one that comes now is saved next */
    for (int i = 0; i < n_tags; i++) {
        const tag_t *t = &tags[i];
        int len;

        if (t->hidden)
            continue;
        len = snprintf(line, sizeof(line), "%d ", t->level);
        if (t->has_icon) {
            for (unsigned b = 0; b < sizeof(t->icon); b++) {
                line[len++] = digits[t->icon.a[b] >> 4];
                line[len++] = digits[t->icon.a[b] & 15];
            }
        } else {
            line[len++] = '-';
        }
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %s\n", t->name);
        f_write(&f, line, (UINT)len, &w);
    }
    f_close(&f);
}
