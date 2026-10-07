/* Embedded Duke sound adapter; GPL-2.0-or-later. */
#include "b310e-build-sound.h"
int numenvsnds;
void (*ASS_MessageOutputString)(const char *);
static int voice_sound[8],voice_sprite[8],voice_pan[8];
void SoundStartup(void) {
    for(int i=0;i<8;i++){voice_sound[i]=voice_sprite[i]=-1;voice_pan[i]=128;}
    b310e_mixer_init();
    /* PC device IDs from an old config do not select phone hardware. */
    FXDevice=MusicDevice=0;
}
void SoundShutdown(void) {b310e_music_stop();b310e_audio_close();}
void MusicStartup(void) {}
void MusicShutdown(void) {b310e_music_stop();}
void MusicPause(int pause) {b310e_music_pause(pause!=0);}
void MusicSetVolume(int volume) {b310e_music_volume(volume/2);}
void stopmusic(void) {b310e_music_stop();}
void playmusic(char *name) {
    if(MusicToggle && b310e_group_music(name,true)) MusicSetVolume(MusicVolume);
}
void FX_SetVolume(int volume) {
    FXVolume=MIN(255,MAX(0,volume));
    for(int ch=0;ch<8;ch++) if(b310e_sample_active(ch))
        b310e_sample_volume(ch,FXVolume,ReverseStereo?255-voice_pan[ch]:voice_pan[ch]);
}
void FX_SetReverseStereo(int reverse) {ReverseStereo=reverse!=0;FX_SetVolume(FXVolume);}
int FX_StopAllSounds(void) {
    for(int ch=0;ch<8;ch++) b310e_sample_stop(ch);
    return 0;
}
static int play_sound(int number,int owner) {
    if(!SoundToggle || (unsigned)number>=NUM_SOUNDS)return -1;
    if((soundm[number]&4) && !VoiceToggle)return -1;
    if((soundm[number]&8) && ud.lockout)return -1;
    int ch=b310e_group_sample(sounds[number],FXVolume,128);
    if(ch>=0){voice_sound[ch]=number;voice_sprite[ch]=owner;voice_pan[ch]=128;}
    return ch;
}
int xyzsound(short number,short owner,int x,int y,int z) {
    (void)z;int ch=play_sound(number,owner);
    if(ch>=0) {
        int angle=(getangle(x-ps[screenpeek].posx,y-ps[screenpeek].posy)-ps[screenpeek].ang)&2047;
        voice_pan[ch]=MIN(255,MAX(0,128+(sintable[angle]>>7)));
        b310e_sample_volume(ch,FXVolume,ReverseStereo?255-voice_pan[ch]:voice_pan[ch]);
    }
    return ch;
}
void sound(short number) {play_sound(number,-1);}
int spritesound(unsigned short number,short owner) {
    if((unsigned)owner>=MAXSPRITES)return -1;
    return xyzsound(number,owner,sprite[owner].x,sprite[owner].y,sprite[owner].z);
}
void intomenusounds(void) {
    static unsigned next;
    const short sounds[]={LASERTRIP_EXPLODE,DUKE_GRUNT,PISTOL_FIRE,SHOTGUN_FIRE,SELECT_WEAPON};
    sound(sounds[next++%5]);
}
int issoundplaying(int owner,int number) {
    for(int ch=0;ch<8;ch++)if(b310e_sample_active(ch) && (number<0 || voice_sound[ch]==number) &&
                             (owner<0 || voice_sprite[ch]==owner))return 1;
    return 0;
}
int isspritemakingsound(short owner,int number) {return issoundplaying(owner,number);}
void stopspritesound(short number,short owner) {
    for(int ch=0;ch<8;ch++)if(voice_sound[ch]==number && (owner<0 || voice_sprite[ch]==owner))b310e_sample_stop(ch);
}
void stopsound(short number) {stopspritesound(number,-1);}
void stopenvsound(short number,short owner) {stopspritesound(number,owner);}
void pan3dsound(void) {
    b310e_audio_poll();
}
void testcallback(unsigned int value) {(void)value;}
void clearsoundlocks(void) {}
