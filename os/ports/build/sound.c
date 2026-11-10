/* A Build engine game on B310E OS: the part of JonoF's audiolib the games use (fx_man.h,
 * music.h), on the game kit's mixer and MIDI player. The game's own sounds.c, upstream's
 * and unchanged, does the rest: distance, priorities, looping, who owns what; Duke keeps
 * its sounds in the tile cache, where they play from, no copy.
 *
 * The audiolib's callback comes from its mixer, when a voice ends. The mixer here
 * only marks the voice; fx_poll (the game's thread, every frame) makes the call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>

#include "build.h"
#include "fx_man.h"
#include "music.h"
#include "cd.h"
#include "b310e-mixer.h"
#include "b310e-music.h"
#include "build_game.h"

void kit_log(const char *s);            /* (kit_os.c) */

#define VOICES 8

static void (*callback)(unsigned int);
static int master = 255, reversed;

static struct {
    bool live;
    int priority;
    unsigned int value;
    int volume, pan;                    /* 0..255 */
} voice[VOICES];

int FX_ErrorCode = FX_Ok;
int MUSIC_ErrorCode = MUSIC_Ok;
void (*ASS_MessageOutputString)(const char *);

const char *FX_ErrorString(int number) { (void)number; return ""; }
const char *MUSIC_ErrorString(int number) { (void)number; return ""; }

static void finish(int ch)
{
    voice[ch].live = false;
    if (callback)
        callback(voice[ch].value);
}

/* A voice that has ended: the game hears of it */
void fx_poll(void)
{
    for (int ch = 0; ch < VOICES; ch++)
        if (voice[ch].live && !b310e_sample_active(ch))
            finish(ch);
    b310e_audio_poll();
}

int FX_Init(int card, int numvoices, int *numchannels, int *samplebits, int *mixrate, void *initdata)
{
    (void)card; (void)numvoices; (void)numchannels; (void)samplebits; (void)mixrate; (void)initdata;
    b310e_mixer_init();
    return FX_Ok;
}

int FX_Shutdown(void)
{
    FX_StopAllSounds();
    b310e_audio_close();
    return FX_Ok;
}

int FX_SetCallBack(void (*function)(unsigned int))
{
    callback = function;
    return FX_Ok;
}

static void apply(int ch)
{
    b310e_sample_volume(ch, voice[ch].volume * master / 255, reversed ? 255 - voice[ch].pan : voice[ch].pan);
}

void FX_SetVolume(int volume)
{
    master = volume < 0 ? 0 : volume > 255 ? 255 : volume;
    for (int ch = 0; ch < VOICES; ch++)
        if (voice[ch].live)
            apply(ch);
}

void FX_SetReverseStereo(int setting)
{
    reversed = setting != 0;
    FX_SetVolume(master);
}

int FX_GetVolume(void) { return master; }
int FX_GetReverseStereo(void) { return reversed; }
const char *FX_GetCurrentDriverName(void) { return "B310E OS"; }

void FX_SetReverb(int reverb) { (void)reverb; }
void FX_SetReverbDelay(int delay) { (void)delay; }

/* Nearer than the game's loudest (its LOUDESTVOLUME, 150 of 255) is full volume */
static int loudness(int distance)
{
    int v = (255 - distance) * 255 / 150;

    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* angle: 32 to the turn, 8 to the right */
static int pan_of(int angle)
{
    return 128 + (sintable[(angle << 6) & 2047] >> 7);
}

int FX_VoiceAvailable(int priority)
{
    fx_poll();
    for (int ch = 0; ch < VOICES; ch++)
        if (!voice[ch].live || voice[ch].priority <= priority)
            return 1;
    return 0;
}

static int a_voice(int priority)
{
    int best = -1;

    fx_poll();
    for (int ch = 0; ch < VOICES; ch++) {
        if (!voice[ch].live)
            return ch;
        if (voice[ch].priority <= priority && (best < 0 || voice[ch].priority < voice[best].priority))
            best = ch;
    }
    if (best >= 0) {
        b310e_sample_stop(best);
        finish(best);
    }
    return best;
}

/* Hundredths of a semitone to the mixer's 128ths, near enough for the range used */
static int pitch_of(int offset)
{
    int p = 128 + offset * 71 / 1000;

    return p < 16 ? 16 : p;
}

static int play(char *ptr, unsigned int length, int pitchoffset, int volume, int pan, int priority,
                unsigned int value, bool loop)
{
    int ch = a_voice(priority);

    if (ch < 0)
        return FX_Warning;
    voice[ch].priority = priority;
    voice[ch].value = value;
    voice[ch].volume = volume;
    voice[ch].pan = pan;
    if (b310e_sample_file_borrow(ch, (const uint8_t *)ptr, length, volume * master / 255,
                                 reversed ? 255 - pan : pan, pitch_of(pitchoffset)) < 0)
        return FX_Warning;
    b310e_sample_loop(ch, loop);
    voice[ch].live = true;
    return ch + 1;                      /* a handle is above FX_Ok */
}

/* Raw 8-bit unsigned samples at their own rate (Blood's): played where they lie */
static int play_raw(char *ptr, unsigned int length, unsigned rate, int pitchoffset, int volume,
                    int pan, int priority, unsigned int value, int loop_from)
{
    int ch = a_voice(priority);

    if (ch < 0)
        return FX_Warning;
    voice[ch].priority = priority;
    voice[ch].value = value;
    voice[ch].volume = volume;
    voice[ch].pan = pan;
    if (b310e_sample_borrow(ch, (const uint8_t *)ptr, length, rate, volume * master / 255,
                            reversed ? 255 - pan : pan, pitch_of(pitchoffset)) < 0)
        return FX_Warning;
    if (loop_from >= 0)
        b310e_sample_loop_from(ch, (unsigned)loop_from);
    voice[ch].live = true;
    return ch + 1;
}

int FX_PlayAuto3D(char *ptr, unsigned int length, int pitchoffset, int angle, int distance,
                  int priority, unsigned int callbackval)
{
    return play(ptr, length, pitchoffset, loudness(distance), pan_of(angle), priority, callbackval, false);
}

/* A volume of 0..255 and its two sides: where it is */
static int pan_lr(int left, int right)
{
    return left + right ? 128 + (right - left) * 127 / (left + right) : 128;
}

static int volume_of(int vol)
{
    return vol < 0 ? 0 : vol > 255 ? 255 : vol;
}

/* (Duke's sounds.c gives the distance as the volume: build_looped_vol_is_distance) */
int FX_PlayLoopedAuto(char *ptr, unsigned int length, int loopstart, int loopend, int pitchoffset,
                      int vol, int left, int right, int priority, unsigned int callbackval)
{
    (void)loopstart; (void)loopend;
    if (build_looped_vol_is_distance)
        return play(ptr, length, pitchoffset, loudness(vol), 128, priority, callbackval, true);
    return play(ptr, length, pitchoffset, volume_of(vol), pan_lr(left, right), priority,
                callbackval, true);
}

int FX_PlayAuto(char *ptr, unsigned int length, int pitchoffset, int vol, int left, int right,
                int priority, unsigned int callbackval)
{
    return play(ptr, length, pitchoffset, volume_of(vol), pan_lr(left, right), priority,
                callbackval, false);
}

int FX_PlayRaw(char *ptr, unsigned int length, unsigned rate, int pitchoffset, int vol, int left,
               int right, int priority, unsigned int callbackval)
{
    return play_raw(ptr, length, rate, pitchoffset, volume_of(vol), pan_lr(left, right), priority,
                    callbackval, -1);
}

int FX_PlayLoopedRaw(char *ptr, unsigned int length, char *loopstart, char *loopend, unsigned rate,
                     int pitchoffset, int vol, int left, int right, int priority,
                     unsigned int callbackval)
{
    (void)loopend;                      /* (its loops run to the end) */
    return play_raw(ptr, length, rate, pitchoffset, volume_of(vol), pan_lr(left, right), priority,
                    callbackval, loopstart > ptr ? (int)(loopstart - ptr) : 0);
}

int FX_PlayVOC(char *ptr, unsigned int length, int pitchoffset, int vol, int left, int right,
               int priority, unsigned int callbackval)
{
    return FX_PlayAuto(ptr, length, pitchoffset, vol, left, right, priority, callbackval);
}

static int live(int handle)
{
    int ch = handle - 1;

    return (unsigned)ch < VOICES && voice[ch].live ? ch : -1;
}

int FX_Pan3D(int handle, int angle, int distance)
{
    int ch = live(handle);

    if (ch >= 0) {
        voice[ch].volume = loudness(distance);
        voice[ch].pan = pan_of(angle);
        apply(ch);
    }
    return FX_Ok;
}

int FX_SetVoiceVolume(int handle, int volume)
{
    int ch = live(handle);

    if (ch >= 0) {
        voice[ch].volume = volume;
        apply(ch);
    }
    return FX_Ok;
}

int FX_EndLooping(int handle)
{
    int ch = live(handle);

    if (ch >= 0)
        b310e_sample_loop(ch, false);
    return FX_Ok;
}

int FX_SetPan(int handle, int vol, int left, int right)
{
    int ch = live(handle);

    if (ch >= 0) {
        voice[ch].volume = volume_of(vol);
        voice[ch].pan = pan_lr(left, right);
        apply(ch);
    }
    return FX_Ok;
}

int FX_PauseSound(int handle, int pauseon) { (void)handle; (void)pauseon; return FX_Ok; }

int FX_SetPitch(int handle, int pitchoffset)
{
    int ch = live(handle);

    if (ch >= 0)
        b310e_sample_pitch(ch, pitch_of(pitchoffset));
    return FX_Ok;
}

int FX_SetFrequency(int handle, int frequency)
{
    int ch = live(handle);

    if (ch >= 0)
        b310e_sample_rate(ch, (unsigned)frequency);
    return FX_Ok;
}

int FX_SoundActive(int handle)
{
    int ch = live(handle);

    return ch >= 0 && b310e_sample_active(ch);
}

int FX_SoundsPlaying(void)
{
    int n = 0;

    for (int ch = 0; ch < VOICES; ch++)
        n += voice[ch].live && b310e_sample_active(ch);
    return n;
}

int FX_StopSound(int handle)
{
    int ch = live(handle);

    if (ch >= 0) {
        b310e_sample_stop(ch);
        finish(ch);
    }
    return FX_Ok;
}

int FX_StopAllSounds(void)
{
    for (int ch = 0; ch < VOICES; ch++)
        if (voice[ch].live) {
            b310e_sample_stop(ch);
            finish(ch);
        }
    return FX_Ok;
}

/* ---- music: the song stays with the game (sounds.c's MusicPtr) ------------------------ */
int MUSIC_Init(int card, const char *params) { (void)card; (void)params; return MUSIC_Ok; }
int MUSIC_Shutdown(void) { b310e_music_stop(); return MUSIC_Ok; }
void MUSIC_SetVolume(int volume) { b310e_music_volume(volume / 2); }
void MUSIC_Continue(void) { b310e_music_pause(false); }
void MUSIC_Pause(void) { b310e_music_pause(true); }
int MUSIC_StopSong(void) { b310e_music_stop(); return MUSIC_Ok; }
void MUSIC_RegisterTimbreBank(unsigned char *timbres) { (void)timbres; }
int MUSIC_SongPlaying(void) { return b310e_music_playing(); }
const char *MUSIC_GetCurrentDriverName(void) { return "B310E OS"; }

int MUSIC_PlaySong(void *song, unsigned int length, int loopflag)
{
    if (b310e_music_start_borrowed(song, length, loopflag != 0))
        return MUSIC_Ok;
    kit_log("a song the player did not take");
    return MUSIC_Error;
}

/* ---- a CD's music (Shadow Warrior's): there is no CD; the game plays its MIDI instead -- */
int CD_GetError(void) { return CD_Error; }
const char *CD_ErrorString(int code) { (void)code; return "no CD"; }
int CD_Init(int SoundCard) { (void)SoundCard; return CD_Error; }
int CD_GetCurrentDriver(void) { return -1; }
const char *CD_GetCurrentDriverName(void) { return "none"; }
void CD_Shutdown(void) {}
int CD_Play(int track, int loop) { (void)track; (void)loop; return CD_Error; }
void CD_Stop(void) {}
void CD_Pause(int pauseon) { (void)pauseon; }
int CD_IsPlaying(void) { return 0; }
void CD_SetVolume(int volume) { (void)volume; }
