/* Wolf4SDL IMF/AdLib and digitized output. No I/O in the DMA IRQ.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "b310e-mixer.h"
extern void b310e_opl_init(void);
extern void b310e_opl_write(unsigned,unsigned);
extern void b310e_opl_block(int32_t *,unsigned);
boolean AdLibPresent=true,SoundBlasterPresent=true,SoundPositioned;
byte SoundMode=sdm_Off,MusicMode=smm_Off,DigiMode=sds_Off;
int DigiMap[LASTSOUND],DigiChannel[STARTMUSIC-STARTDIGISOUNDS];
globalsoundpos channelSoundPos[8];
word NumDigi;
digiinfo *DigiList;
static const byte *music,*effect;
static unsigned music_bytes,music_pos,music_delay,music_clock,effect_clock;
static volatile unsigned effect_left;
static byte effect_block;
static bool music_on;
static unsigned sound_number,sound_priority;
static int left_position,right_position;
static void opl(unsigned reg,unsigned val) {b310e_opl_write(reg,val);}
static unsigned le16(const byte *p) {return p[0]|p[1]<<8;}
static void render(int16_t *out,unsigned frames) {
    int32_t samples[160];
    unsigned i=0;
    while(i<frames) {
        music_clock+=700;
        if(music_clock>=22050) {
            music_clock-=22050;
            if(music_on && music_bytes) {
                if(music_delay) music_delay--;
                unsigned budget=1024;
                while(!music_delay && budget--) {
                    if(music_pos+4>music_bytes) music_pos=0;
                    opl(music[music_pos],music[music_pos+1]);
                    music_delay=le16(music+music_pos+2); music_pos+=4;
                }
            }
        }
        effect_clock+=140;
        if(effect_clock>=22050) {
            effect_clock-=22050;
            if(effect_left) {
                byte value=*effect++; effect_left--;
                opl(0xa0,value); opl(0xb0,value?effect_block:0);
                if(!effect_left) {opl(0xb0,0);sound_priority=0;}
            }
        }
        /* Batch only up to the next register write. IMF stays at 700 Hz,
         * effects at 140 Hz, including their fractional sample intervals. */
        unsigned n=MIN(frames-i,160);
        n=MIN(n,(22049-music_clock)/700+1);
        n=MIN(n,(22049-effect_clock)/140+1);
        b310e_opl_block(samples,n);
        for(unsigned j=0;j<n;j++) {
            int value=MIN(32767,MAX(-32768,samples[j]));
            out[(i+j)*2]=out[(i+j)*2+1]=value;
        }
        music_clock+=(n-1)*700;effect_clock+=(n-1)*140;i+=n;
    }
}
void Delay(int32_t ticks) {if(ticks>0)SDL_Delay(ticks*100/7);}
void SD_StopDigitized(void) {for(int i=0;i<8;i++)b310e_sample_stop(i);}
void SD_PrepareSound(int which) {(void)which;}
int SD_GetChannelForDigi(int which) {(void)which;return -1;}
void SD_SetPosition(int channel,int left,int right) {
    int l=15-MIN(15,MAX(0,left)),r=15-MIN(15,MAX(0,right));
    b310e_sample_volume(channel,MAX(l,r)*17,(l+r)?r*255/(l+r):128);
}
void SD_PositionSound(int left,int right) {left_position=left;right_position=right;SoundPositioned=true;}
int SD_PlayDigitized(word which,int left,int right) {
    if(which>=NumDigi || DigiMode==sds_Off)return -1;
    unsigned page=PMSoundStart+DigiList[which].startpage;
    byte *data=PM_GetPage(page);
    unsigned length=DigiList[which].length;
    if(data+length>PM_GetPageEnd())return -1;
    int ch=b310e_sample_play(-1,data,length,7042,255,128,128);
    SD_SetPosition(ch,left,right);return ch;
}
void SD_SetDigiDevice(byte mode) {SD_StopDigitized();DigiMode=mode;}
void SD_StopSound(void) {
    int old=b310e_irq_save();effect_left=0;sound_number=sound_priority=0;opl(0xb0,0);b310e_irq_restore(old);
    SD_StopDigitized();
}
boolean SD_SetSoundMode(byte mode) {SD_StopSound();SoundMode=mode==sdm_PC?sdm_AdLib:mode;return true;}
int SD_MusicOff(void) {
    int old=b310e_irq_save();music_on=false;
    for(unsigned i=1;i<9;i++)opl(0xb0+i,0);
    unsigned pos=music_pos/2;b310e_irq_restore(old);return pos;
}
boolean SD_SetMusicMode(byte mode) {SD_MusicOff();MusicMode=mode;return true;}
void SD_Startup(void) {
    b310e_opl_init();b310e_mixer_init();b310e_mixer_hook(render);
    for(int i=0;i<LASTSOUND;i++)DigiMap[i]=-1;
    for(unsigned i=0;i<sizeof(DigiChannel)/sizeof(*DigiChannel);i++)DigiChannel[i]=-1;
    byte *info=PM_GetPage(ChunksInFile-1);
    NumDigi=PM_GetPageSize(ChunksInFile-1)/4;
    DigiList=calloc(NumDigi,sizeof(*DigiList));
    if(!DigiList){NumDigi=0;return;}
    for(unsigned i=0;i<NumDigi;i++) {
        unsigned start=le16(info+i*4),next=i+1<NumDigi?le16(info+i*4+4):0;
        if(start+PMSoundStart>=ChunksInFile-1){NumDigi=i;break;}
        unsigned end=next>start && next+PMSoundStart<ChunksInFile?next+PMSoundStart:ChunksInFile-1;
        unsigned size=0;
        for(unsigned p=start+PMSoundStart;p<end;p++)size+=PM_GetPageSize(p);
        if(end==ChunksInFile-1 && PMSoundInfoPagePadded && size)size--;
        unsigned lower=le16(info+i*4+2);
        if((size&65535)<lower && size>=65536)size-=65536;
        DigiList[i].startpage=start;DigiList[i].length=(size&~65535u)|lower;
    }
}
void SD_Shutdown(void) {SD_MusicOff();SD_StopSound();b310e_audio_close();free(DigiList);DigiList=NULL;NumDigi=0;}
boolean SD_PlaySound(int sound) {
    if((unsigned)sound>=LASTSOUND)return false;
    int left=left_position,right=right_position;left_position=right_position=0;
    if(DigiMode!=sds_Off && DigiMap[sound]>=0)return SD_PlayDigitized(DigiMap[sound],left,right)+1;
    if(SoundMode==sdm_Off)return false;
    AdLibSound *s=(void*)audiosegs[STARTADLIBSOUNDS+sound];
    if(!s || !s->common.length || s->common.priority<sound_priority)return false;
    int old=b310e_irq_save();opl(0xb0,0);
    const byte *inst=(const byte*)&s->inst;
    const unsigned regs[]={0x20,0x40,0x60,0x80,0xe0};
    for(unsigned i=0;i<5;i++){opl(regs[i],inst[i*2]);opl(regs[i]+3,inst[i*2+1]);}
    opl(0xc0,s->inst.nConn);
    effect=s->data;effect_left=s->common.length;effect_block=((s->block&7)<<2)|0x20;
    sound_number=sound;sound_priority=s->common.priority;b310e_irq_restore(old);return false;
}
word SD_SoundPlaying(void) {return effect_left?sound_number:0;}
void SD_WaitSoundDone(void) {
    unsigned start=sys_timer_ms();
    while(SD_SoundPlaying()) {
        if(sys_timer_ms()-start>30000) {SD_StopSound();break;}
        SDL_Delay(5);
    }
}
void SD_MusicOn(void) {music_on=MusicMode==smm_AdLib;}
void SD_StartMusic(int chunk) {
    SD_MusicOff();
    if(MusicMode!=smm_AdLib)return;
    unsigned bytes=CA_CacheAudioChunk(chunk);const byte *data=audiosegs[chunk];
    if(bytes<4)return;
    unsigned len=le16(data);
    if(len){if(len>bytes-2)return;data+=2;bytes=len;}
    bytes&=~3u;if(!bytes)return;
    int old=b310e_irq_save();music=data;music_bytes=bytes;music_pos=music_delay=music_clock=0;
    music_on=true;b310e_irq_restore(old);
}
void SD_ContinueMusic(int chunk,int offset) {
    SD_StartMusic(chunk);int old=b310e_irq_save();
    unsigned end=offset>0?(unsigned)offset*2:0;if(end>=music_bytes)end=0;
    for(unsigned i=0;i+4<=end;i+=4) {
        unsigned reg=music[i],val=music[i+1];
        if(reg>=0xb1 && reg<=0xb8)val&=~0x20;
        if(reg==0xbd)val&=0xe0;
        opl(reg,val);
    }
    music_pos=end;b310e_irq_restore(old);
}
void SD_FadeOutMusic(void) {SD_MusicOff();}
boolean SD_MusicPlaying(void) {return music_on;}
