/* Chocolate Doom, Heretic and Hexen on B310E OS: the game's sound (its
 * i_sound.h) on the game kit's mixer and MUS/MIDI synth: DMX effects from
 * the WAD, music lumps (MUS or MIDI) by the synth. No OPL, no PC speaker:
 * the menus' sound and music devices do not change it.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_config.h"
#include "w_wad.h"
#include "z_zone.h"

#include "b310e-mixer.h"
#include "b310e-music.h"

/* what upstream's i_sound.c defines, and the menus and m_config.c bind */
int snd_sfxdevice = SNDDEVICE_SB;
int snd_musicdevice = SNDDEVICE_SB;
int snd_samplerate = 22050;
int snd_cachesize = 64 * 1024 * 1024;
int snd_maxslicetime_ms = 28;
char *snd_musiccmd = "";
int snd_pitchshift = -1;
char *snd_dmxoption = "";
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *music_pack_path = "";
char *timidity_cfg_path = "";
int opl_io_port = 0x388;

static boolean sfx_prefix;              /* Doom's lumps are DS<name> */

static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[16];

    if (sfx->link)
        sfx = sfx->link;
    snprintf(name, sizeof(name), "%s%s", sfx_prefix ? "ds" : "", sfx->name);
    return W_CheckNumForName(name);
}

void I_InitSound(GameMission_t mission)
{
    sfx_prefix = mission == doom || mission == doom2 || mission == pack_tnt ||
                 mission == pack_plut || mission == pack_chex || mission == pack_hacx;
    b310e_mixer_init();
}

void I_ShutdownSound(void)
{
    b310e_music_stop();
    b310e_audio_close();
}

/* A sound plays from its lump in the zone, no copy: held there (PU_STATIC)
 * while a voice plays it, purgeable again when none does. (The copies these
 * were cost a malloc each time, and Hexen's longest sounds never found room.) */
#define VOICES 8
static uint8_t *playing[VOICES];

static void release(int channel)
{
    uint8_t *data = playing[channel];

    if (!data)
        return;
    playing[channel] = NULL;
    for (int i = 0; i < VOICES; i++)
        if (playing[i] == data)
            return;                     /* another voice has it too */
    Z_ChangeTag(data, PU_CACHE);
}

int I_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep, int pitch)
{
    int lump = I_GetSfxLumpNum(sfx), len, handle = -1;
    uint8_t *data;
    unsigned bytes;

    if (lump < 0 || (len = W_LumpLength(lump)) < 8 || (unsigned)channel >= VOICES)
        return -1;
    b310e_sample_stop(channel);
    release(channel);
    data = W_CacheLumpNum(lump, PU_STATIC);
    bytes = le32(data + 4);
    if (le16(data) == 3 && bytes <= (unsigned)len - 8)
        handle = b310e_sample_borrow(channel, data + 8, bytes, le16(data + 2), vol * 2, sep, pitch);
    if (handle >= 0) {
        playing[channel] = data;
    } else {
        playing[channel] = data;        /* (released as any: another voice may have it) */
        release(channel);
    }
    return handle;
}

void I_StopSound(int channel)
{
    b310e_sample_stop(channel);
    if ((unsigned)channel < VOICES)
        release(channel);
}
boolean I_SoundIsPlaying(int channel) { return b310e_sample_active(channel); }
void I_UpdateSoundParams(int channel, int vol, int sep) { b310e_sample_volume(channel, vol * 2, sep); }

void I_UpdateSound(void)
{
    for (int i = 0; i < VOICES; i++)
        if (playing[i] && !b310e_sample_active(i))
            release(i);
    b310e_audio_poll();
}

void I_PrecacheSounds(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }
void I_SetOPLDriverVer(opl_driver_ver_t ver) { (void)ver; }
void I_OPL_DevMessages(char *result, size_t size) { if (size) result[0] = 0; }

void I_BindSoundVariables(void)
{
    M_BindIntVariable("snd_sfxdevice", &snd_sfxdevice);
    M_BindIntVariable("snd_musicdevice", &snd_musicdevice);
    M_BindIntVariable("snd_samplerate", &snd_samplerate);
    M_BindIntVariable("snd_pitchshift", &snd_pitchshift);
}

/* ---- music ----------------------------------------------------------------------- */
struct song {
    void *data;
    unsigned size;
};

void I_InitMusic(void) {}
void I_ShutdownMusic(void) { b310e_music_stop(); }
void I_SetMusicVolume(int volume) { b310e_music_volume(volume); }
void I_PauseSong(void) { b310e_music_pause(true); }
void I_ResumeSong(void) { b310e_music_pause(false); }
void I_StopSong(void) { b310e_music_stop(); }
boolean I_MusicIsPlaying(void) { return b310e_music_playing(); }

void *I_RegisterSong(void *data, int len)
{
    struct song *s = malloc(sizeof(*s));

    if (s) {
        s->data = data;
        s->size = (unsigned)len;
    }
    return s;
}

void I_UnRegisterSong(void *handle)
{
    b310e_music_stop();
    free(handle);
}

void I_PlaySong(void *handle, boolean looping)
{
    struct song *s = handle;

    if (s)
        b310e_music_start(s->data, s->size, looping);
}
