/* Embedded NBlood RFF sound adapter; GPL-2.0-only. */
#include "resource.h"
#include "sound.h"
#include "sfx.h"
#include "b310e-build-sound.h"
int32_t SoundToggle,MusicToggle,CDAudioToggle,FXVolume,MusicVolume,CDVolume;
int32_t NumVoices,NumChannels,NumBits,MixRate,ReverseStereo,MusicDevice;
Resource gSoundRes;
static spritetype *voice_sprite[8];
static int voice_id[8];
void ambProcess(void) {}
void ambKillAll(void) {}
void ambInit(void) {}
void sndInit(void) {b310e_mixer_init();b310e_music_volume(MusicVolume/2);}
void sndTerm(void) {b310e_music_stop();b310e_audio_close();}
int sndGetRate(int format) {return format>=9?44100:format>=5?22050:11025;}
DICTNODE *sndLookupRawCached(int id,const char *name) {(void)id;return gSoundRes.Lookup(name,"RAW");}
static int raw_sample(DICTNODE *node,int ch,int vol,int rate) {
    if(!node || node->size>131072)return -1;
    uint8_t *data=(uint8_t*)gSoundRes.Lock(node);
    if(!data)return -1;
    int result=b310e_sample_play(ch<0?-1:ch%8,data,node->size,rate,vol*FXVolume/255,128,128);
    gSoundRes.Unlock(node);return result;
}
static int effect_sample(unsigned id,int vol,int ch) {
    if(!SoundToggle)return -1;
    DICTNODE *node=gSoundRes.Lookup(id,"SFX");
    if(!node || node->size<sizeof(SFX))return -1;
    SFX *effect=(SFX*)gSoundRes.Lock(node);if(!effect)return -1;
    char name[10];memcpy(name,effect->rawName,9);name[9]=0;
    int rate=sndGetRate(effect->format);
    if(vol<0)vol=effect->relVol;
    gSoundRes.Unlock(node);
    int result=raw_sample(gSoundRes.Lookup(name,"RAW"),ch,MIN(255,MAX(0,vol)*80),rate);
    if(result>=0){voice_id[result]=id;voice_sprite[result]=NULL;}
    return result;
}
void sndStartSample(const char *name,int vol,int ch) {if(SoundToggle)raw_sample(gSoundRes.Lookup(name,"RAW"),ch,vol,11025);}
void sndStartSample(unsigned id,int vol,int ch,bool loop) {(void)loop;effect_sample(id,vol,ch);}
void sndFadeSong(int time) {(void)time;b310e_music_stop();}
void sndStopSong(void) {b310e_music_stop();}
bool sndIsSongPlaying(void) {return b310e_music_playing();}
void sndLoadGMTimbre(void) {}
int sndPlaySong(const char *name,bool loop) {
    if(!MusicToggle)return 0;
    if(b310e_group_music(name,loop))return 0;
    if(!name)return 1;
    DICTNODE *node=gSoundRes.Lookup(name,"MID");if(!node || node->size>262144)return 2;
    uint8_t *data=(uint8_t*)gSoundRes.Lock(node);if(!data)return 3;
    bool ok=b310e_music_start(data,node->size,loop);gSoundRes.Unlock(node);return ok?0:5;
}
void sndKillAllSounds(void) {for(int ch=0;ch<8;ch++)b310e_sample_stop(ch);}
void sndProcess(void) {b310e_audio_poll();}
void sndSetMusicVolume(int vol) {MusicVolume=vol;b310e_music_volume(vol/2);}
void sndSetFXVolume(int vol) {FXVolume=vol;}
void sndStartWavID(unsigned id,int vol,int ch) {
    if(!SoundToggle)return;
    DICTNODE *node=gSoundRes.Lookup(id,"WAV");if(!node || node->size>262144)return;
    uint8_t *data=(uint8_t*)gSoundRes.Lock(node);if(!data)return;
    b310e_sample_file(ch<0?-1:ch%8,data,node->size,vol*FXVolume/255,128);gSoundRes.Unlock(node);
}
void sndStartWavDisk(const char *name,int vol,int ch) {(void)ch;if(SoundToggle)b310e_group_sample(name,vol*FXVolume/255,128);}
void sfxInit(void) {}
void sfxTerm(void) {sndKillAllSounds();}
void sfxPlay3DSound(int x,int y,int z,int id,int sector) {(void)x;(void)y;(void)z;(void)sector;effect_sample(id,-1,-1);}
void sfxPlay3DSound(spritetype *sprite,int id,int channel,int flags) {
    (void)flags;int ch=effect_sample(id,-1,channel);if(ch>=0)voice_sprite[ch]=sprite;
}
void sfxPlay3DSoundCP(spritetype *sprite,int id,int channel,int flags,int pitch,int volume) {
    (void)pitch;(void)flags;int ch=effect_sample(id,volume?volume:-1,channel);if(ch>=0)voice_sprite[ch]=sprite;
}
void sfxKill3DSound(spritetype *sprite,int channel,int id) {
    for(int ch=0;ch<8;ch++)if(voice_sprite[ch]==sprite && (id<0 || voice_id[ch]==id) && (channel<0 || ch==channel%8))b310e_sample_stop(ch);
}
void sfxKillAllSounds(void) {sndKillAllSounds();}
void sfxKillSpriteSounds(spritetype *sprite) {sfxKill3DSound(sprite,-1,-1);}
void sfxUpdate3DSounds(void) {b310e_audio_poll();}
void sfxSetReverb(bool enable) {(void)enable;}
void sfxSetReverb2(bool enable) {(void)enable;}
