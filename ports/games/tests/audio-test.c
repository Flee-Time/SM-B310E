#include "b310e-mixer.h"
#include "b310e-music.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void (*renderer)(int16_t *,unsigned);
int b310e_irq_save(void){return 0;}
void b310e_irq_restore(int mode){(void)mode;}
void b310e_audio_init(unsigned rate){assert(rate==22050);}
void b310e_audio_close(void){renderer=NULL;}
void b310e_audio_render(void (*fn)(int16_t *,unsigned)){renderer=fn;}
static int peak(const int16_t *data,unsigned n){int p=0;for(unsigned i=0;i<n;i++){int a=data[i]<0?-data[i]:data[i];if(a>p)p=a;}return p;}
int main(void){
    int16_t out[320];uint8_t pcm[4000];memset(pcm,255,sizeof(pcm));
    b310e_mixer_init();assert(renderer);
    int ch=b310e_sample_play(0,pcm,sizeof(pcm),22050,255,0,128);assert(ch==0);
    renderer(out,160);assert(out[0]==32512 && out[1]==0);
    b310e_sample_volume(ch,128,255);renderer(out,160);assert(out[0]==0 && out[1]>16000 && out[1]<16400);
    b310e_sample_stop(ch);renderer(out,160);assert(peak(out,320)==0);
    assert(b310e_sample_file(0,NULL,100,255,128)==-1);
    uint8_t voc[38]={0};memcpy(voc,"Creative Voice File",19);voc[20]=26;voc[26]=1;voc[27]=8;voc[30]=211;
    memset(voc+32,255,6);assert(b310e_sample_file(0,voc,sizeof(voc),255,128)==0);renderer(out,160);assert(peak(out,320)>0);
    assert(!b310e_sample_active(0));
    const uint8_t midi[]={ 'M','T','h','d',0,0,0,6,0,0,0,1,0,96,
        'M','T','r','k',0,0,0,12,0,0x90,69,100,96,0x80,69,0,0,0xff,0x2f,0};
    assert(b310e_music_start(midi,sizeof(midi),false));
    int energy=0;for(int i=0;i<2000;i++){int v=b310e_music_sample();energy+=v<0?-v:v;}assert(energy>10000);
    b310e_music_pause(true);for(int i=0;i<200;i++)assert(!b310e_music_sample());
    b310e_music_pause(false);assert(b310e_music_playing());
    for(int i=0;i<22050;i++)b310e_music_sample();
    assert(!b310e_music_playing());
    assert(b310e_music_start(midi,sizeof(midi),true));for(int i=0;i<44100;i++)b310e_music_sample();assert(b310e_music_playing());
    b310e_music_volume(0);assert(!b310e_music_sample());b310e_music_stop();assert(!b310e_music_playing());
    const uint8_t mus[]={ 'M','U','S',26,7,0,16,0,1,0,0,0,0,0,0,0,0x90,0xc5,100,140,0x60,0,0 };
    assert(b310e_music_start(mus,sizeof(mus),false));b310e_music_volume(127);
    energy=0;for(int i=0;i<2000;i++){int v=b310e_music_sample();energy+=v<0?-v:v;}assert(energy>10000);
    b310e_music_stop();
    uint8_t invalid[sizeof(midi)];memcpy(invalid,midi,sizeof(midi));invalid[10]=0x7f;assert(!b310e_music_start(invalid,sizeof(invalid),false));
    for(unsigned size=0;size<sizeof(midi);size++)assert(!b310e_music_start(midi,size,false));
    b310e_mixer_init();renderer(out,160);assert(!peak(out,320));
    puts("PASS: PCM resampling, stereo volume, VOC, MUS/MIDI timing, pause, looping and malformed input");return 0;
}
