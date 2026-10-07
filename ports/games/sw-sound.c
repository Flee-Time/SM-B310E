/* Embedded Shadow Warrior sound adapter; GPL-2.0-or-later. */
#include "build.h"
#include "cache1d.h"
#include "game.h"
#include "ai.h"
#include "sounds.h"
#include "b310e-build-sound.h"
BOOL MusicInitialized,FxInitialized;
void (*ASS_MessageOutputString)(const char *);
VOC3D_INFOp voc3dstart,voc3dend;
#define DIGI_TABLE
VOC_INFO voc[]={
#include "digi.h"
};
#undef DIGI_TABLE
static int voice_owner[8],last_voice=-1;
void SoundStartup(void) {b310e_mixer_init();FxInitialized=TRUE;for(int i=0;i<8;i++)voice_owner[i]=-1;}
void SoundShutdown(void) {b310e_audio_close();FxInitialized=FALSE;}
void MusicStartup(void) {MusicInitialized=TRUE;}
void MusicShutdown(void) {b310e_music_stop();MusicInitialized=FALSE;}
void InitFX(void) {SoundStartup();}
void InitMusic(void) {MusicStartup();}
void StopFX(void) {for(int i=0;i<8;i++)b310e_sample_stop(i);}
void UnInitSound(void) {StopFX();SoundShutdown();MusicShutdown();}
void StopSound(void) {StopFX();}
void StopSong(void) {b310e_music_stop();}
void PauseSong(BOOL pause) {b310e_music_pause(pause!=0);}
void SetSongVolume(int volume) {b310e_music_volume(volume/2);}
BOOL SongIsPlaying(void) {return b310e_music_playing();}
BOOL PlaySong(char *name,int track,BOOL loop,BOOL restart) {
    (void)track;(void)restart;
    if(!gs.MusicOn || !b310e_group_music(name,loop!=0))return FALSE;
    SetSongVolume(gs.MusicVolume);return TRUE;
}
BOOL CacheSound(int number,int type) {(void)type;return (unsigned)number<sizeof(voc)/sizeof(*voc);}
int PlaySound(int number,int *x,int *y,int *z,Voc3D_Flags flags) {
    (void)x;(void)y;(void)z;
    if(!gs.FxOn || (flags&v3df_init) || (unsigned)number>=sizeof(voc)/sizeof(*voc))return -1;
    int ch=b310e_group_sample(voc[number].name,gs.SoundVolume,128);
    last_voice=ch;if(ch>=0)voice_owner[ch]=-1;return ch;
}
int _PlayerSound(char *file,int line,int number,int *x,int *y,int *z,Voc3D_Flags flags,PLAYERp player) {
    (void)file;(void)line;(void)player;return PlaySound(number,x,y,z,flags);
}
void Set3DSoundOwner(short owner) {if(last_voice>=0)voice_owner[last_voice]=owner;}
void DeleteNoSoundOwner(short owner) {for(int i=0;i<8;i++)if(voice_owner[i]==owner)b310e_sample_stop(i);}
void DeleteNoFollowSoundOwner(short owner) {DeleteNoSoundOwner(owner);}
void PlaySpriteSound(short owner,int index,Voc3D_Flags flags) {
    if((unsigned)owner>=MAXSPRITES || !User[owner] || !User[owner]->Attrib || (unsigned)index>=10)return;
    PlaySound(User[owner]->Attrib->Sounds[index],&sprite[owner].x,&sprite[owner].y,&sprite[owner].z,flags);
    Set3DSoundOwner(owner);
}
void DoUpdateSounds3D(void) {
    static int last_volume=-1;
    if(last_volume!=gs.SoundVolume) {
        last_volume=gs.SoundVolume;
        for(int i=0;i<8;i++)if(b310e_sample_active(i))b310e_sample_volume(i,last_volume,128);
    }
    b310e_audio_poll();
}
void Terminate3DSounds(void) {StopFX();}
void COVER_SetReverb(int amount) {(void)amount;}
void FlipStereo(void) {}
void StartAmbientSound(void) {}
void StopAmbientSound(void) {}
void PlaySoundRTS(int number) {(void)number;}
