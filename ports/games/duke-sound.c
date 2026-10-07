/* Embedded Duke sound adapter; GPL-2.0-or-later. */
#include "b310e-build-sound.h"
int numenvsnds;
void (*ASS_MessageOutputString)(const char *);
static int voice_sound[8],voice_sprite[8];
void SoundStartup(void) {
    for(int i=0;i<8;i++)voice_sound[i]=voice_sprite[i]=-1;
    b310e_mixer_init();
}
void SoundShutdown(void) {b310e_music_stop();b310e_audio_close();}
void MusicStartup(void) {}
void MusicShutdown(void) {b310e_music_stop();}
void MusicPause(int pause) {b310e_music_pause(pause!=0);}
void MusicSetVolume(int volume) {b310e_music_volume(volume/2);}
void stopmusic(void) {b310e_music_stop();}
void playmusic(char *name) {if(MusicToggle)b310e_group_music(name,true);}
static int play_sound(int number,int owner) {
    if(!SoundToggle || (unsigned)number>=NUM_SOUNDS)return -1;
    int ch=b310e_group_sample(sounds[number],FXVolume,128);
    if(ch>=0){voice_sound[ch]=number;voice_sprite[ch]=owner;}
    return ch;
}
int xyzsound(short number,short owner,int x,int y,int z) {
    (void)x;(void)y;(void)z;return play_sound(number,owner);
}
void sound(short number) {play_sound(number,-1);}
int spritesound(unsigned short number,short owner) {return play_sound(number,owner);}
void intomenusounds(void) {
    static unsigned next;
    const short sounds[]={LASERTRIP_EXPLODE,DUKE_GRUNT,PISTOL_FIRE,SHOTGUN_FIRE,SELECT_WEAPON};
    sound(sounds[next++%5]);
}
int issoundplaying(int owner,int number) {
    for(int ch=0;ch<8;ch++)if(b310e_sample_active(ch) && voice_sound[ch]==number &&
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
    for(int ch=0;ch<8;ch++)if(b310e_sample_active(ch))b310e_sample_volume(ch,FXVolume,128);
    b310e_audio_poll();
}
void testcallback(unsigned int value) {(void)value;}
void clearsoundlocks(void) {}
