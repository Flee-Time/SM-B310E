/* B310E OS — volume by category: music, apps, alarm, calls, and the key
 * feedback's vibration (include/os/audio.h). The levels live in the
 * settings; the category that last started a sound owns the codec volume,
 * so changing its level in Settings is heard at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/ui.h"

/* 0 mute, then -40 dB .. 0 dB, closer together near the top */
static const int16_t level_db10[VOL_LEVELS + 1] = {
    -1000, -400, -340, -280, -230, -180, -140, -100, -60, -30, 0,
};

static int active = VOL_MUSIC;

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
    return cat >= 0 && cat < VOL_COUNT ? ui_settings.vol[cat] : 0;
}

void volume_use(int cat)
{
    if (cat < 0 || cat >= VOL_TACTILE)
        return;
    active = cat;
    audio_set_volume(volume_db10(ui_settings.vol[cat]));
}

int volume_active(void)
{
    return active;
}

void volume_set(int cat, int level)
{
    int max = cat == VOL_TACTILE ? 3 : VOL_LEVELS;

    if (cat < 0 || cat >= VOL_COUNT)
        return;
    if (level < 0)
        level = 0;
    if (level > max)
        level = max;
    ui_settings.vol[cat] = level;
    if (cat == active)
        audio_set_volume(volume_db10(level));
    ui_settings_save();
}
