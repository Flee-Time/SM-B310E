/* PCM effects and MUS/MIDI output for the embedded Doom-family ports.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"
#include "i_sound.h"
#include "b310e-mixer.h"
#include "b310e-music.h"
#include <stdlib.h>
#include <stdint.h>
#if B310E_CLASSIC
#include "sounds.h"
#else
int snd_pitchshift=-1;
int snd_musicdevice=SNDDEVICE_SB;
static bool sfx_prefix;
#endif
static unsigned le16(const uint8_t *p) {return p[0]|p[1]<<8;}
static unsigned le32(const uint8_t *p) {return le16(p)|le16(p+2)<<16;}
static int play_lump(sfxinfo_t *sfx, int channel, int volume, int separation, int pitch) {
    if (sfx->link) sfx=sfx->link;
    int lump=I_GetSfxLumpNum(sfx);
    if (lump<0) return -1;
    int len=W_LumpLength(lump);
    if (len<8) return -1;
    uint8_t *data=W_CacheLumpNum(lump,PU_STATIC);
    unsigned bytes=le32(data+4);
    int handle=-1;
    if (le16(data)==3 && bytes<=(unsigned)len-8)
        handle=b310e_sample_play(channel,data+8,bytes,le16(data+2),volume*2,separation,pitch);
    Z_ChangeTag(data,PU_CACHE);
    return handle;
}
int I_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[16];
#if B310E_CLASSIC
    snprintf(name,sizeof(name),"ds%s",sfx->name);
#else
    snprintf(name,sizeof(name),"%s%s",sfx_prefix?"ds":"",sfx->name);
#endif
    return W_CheckNumForName(name);
}
#if B310E_CLASSIC
void I_InitSound(void) {b310e_mixer_init();}
int I_StartSound(int id,int vol,int sep,int pitch,int priority) {
    (void)priority;
    if (id<=0 || id>=NUMSFX) return -1;
    return play_lump(&S_sfx[id],-1,vol,sep,pitch);
}
int I_SoundIsPlaying(int channel) {return b310e_sample_active(channel);}
void I_UpdateSoundParams(int channel,int vol,int sep,int pitch) {
    (void)pitch;b310e_sample_volume(channel,vol*2,sep);
}
void I_SetChannels(void) {}
void I_SetSfxVolume(int volume) {(void)volume;}
void I_SubmitSound(void) {}
void I_HandleSoundTimer(int value) {(void)value;}
int I_SoundSetTimer(int value) {(void)value;return 0;}
void I_SoundDelTimer(void) {}
#else
void I_InitSound(GameMission_t mission) {
    sfx_prefix=mission==doom || mission==doom2 || mission==pack_tnt || mission==pack_plut;
    b310e_mixer_init();
}
int I_StartSound(sfxinfo_t *sfx,int channel,int vol,int sep,int pitch) {
    return play_lump(sfx,channel,vol,sep,pitch);
}
boolean I_SoundIsPlaying(int channel) {return b310e_sample_active(channel);}
void I_UpdateSoundParams(int channel,int vol,int sep) {b310e_sample_volume(channel,vol*2,sep);}
void I_SetOPLDriverVer(opl_driver_ver_t version) {(void)version;}
void I_OPL_DevMessages(char *result,size_t bytes) {if(bytes)result[0]=0;}
void I_PrecacheSounds(sfxinfo_t *sounds,int count) {(void)sounds;(void)count;}
void I_BindSoundVariables(void) {}
#endif
void I_StopSound(int channel) {b310e_sample_stop(channel);}
void I_UpdateSound(void) {b310e_audio_poll();}
void I_ShutdownSound(void) {b310e_music_stop();b310e_audio_close();}
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {b310e_music_stop();}
void I_SetMusicVolume(int volume) {
#if B310E_CLASSIC
    b310e_music_volume(volume*8);
#else
    b310e_music_volume(volume);
#endif
}
struct b310e_song {void *data;unsigned size;};
#if B310E_CLASSIC
int I_RegisterSong(void *data) {
    uint8_t *p=data;
    if (!data) return 0;
    struct b310e_song *s=malloc(sizeof(*s));if (!s) return 0;
    s->data=data;s->size=le16(p+4)+le16(p+6);return (int)(uintptr_t)s;
}
void I_PlaySong(int handle,int looping) {
    struct b310e_song *s=(void*)(uintptr_t)handle;if(s)b310e_music_start(s->data,s->size,looping);
}
void I_PauseSong(int handle) {(void)handle;b310e_music_pause(true);}
void I_ResumeSong(int handle) {(void)handle;b310e_music_pause(false);}
void I_StopSong(int handle) {(void)handle;b310e_music_stop();}
void I_UnRegisterSong(int handle) {b310e_music_stop();free((void*)(uintptr_t)handle);}
int I_QrySongPlaying(int handle) {(void)handle;return b310e_music_playing();}
#else
void *I_RegisterSong(void *data,int len) {
    struct b310e_song *s=malloc(sizeof(*s));if (!s)return NULL;
    s->data=data;s->size=len;return s;
}
void I_PlaySong(void *handle,boolean looping) {
    struct b310e_song *s=handle;if(s)b310e_music_start(s->data,s->size,looping);
}
void I_PauseSong(void) {b310e_music_pause(true);}
void I_ResumeSong(void) {b310e_music_pause(false);}
void I_StopSong(void) {b310e_music_stop();}
void I_UnRegisterSong(void *handle) {b310e_music_stop();free(handle);}
boolean I_MusicIsPlaying(void) {return b310e_music_playing();}
#endif
