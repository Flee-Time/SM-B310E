/* Exercise actual InfoNES register events and waveform timing. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "K6502.h"
/* DMC memory reads are outside this pulse/envelope test. */
#define K6502_RW_H_INCLUDED
static inline BYTE K6502_Read(WORD address) {(void)address;return 0;}
#include "InfoNES_pAPU.c"
WORD g_wPassedClocks;
void InfoNES_SoundInit(void) {}
int InfoNES_SoundOpen(int n,int rate) {(void)n;(void)rate;return 1;}
void InfoNES_SoundClose(void) {}
void InfoNES_SoundOutput(int n,BYTE *a,BYTE *b,BYTE *c,BYTE *d,BYTE *e) {
    (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;
}
void *InfoNES_MemorySet(void *p,int value,int n) {return memset(p,value,n);}
static void quality(unsigned q) {
    InfoNES_pAPUInit();
    ApuPulseMagic=ApuQual[q].pulse_magic;
    ApuSamplesPerSync=ApuQual[q].samples_per_sync;
    ApuCyclesPerSample=ApuQual[q].cycles_per_sample;
    ApuCtrl=3;
    ApuC1a=0x80; ApuC1Freq=253; ApuC1Atl=100;
    ApuC1Skip=(ApuPulseMagic<<1)/(ApuC1Freq+1);
    ApuC1EnvPhase=ApuSamplesPerSync;
}
int main(void) {
    for(unsigned q=0;q<3;q++) {
        quality(q);
        ApuRenderingWave1();
        assert(ApuC1EnvVol>=3 && ApuC1EnvVol<=4); /* 240 Hz, not sample rate */
        for(unsigned i=0;i<4;i++) ApuRenderingWave1();
        assert(ApuC1EnvVol==15);
        for(unsigned i=0;i<ApuSamplesPerSync;i++) assert(wave_buffers[0][i]==0);
        /* The constant-volume flag must bypass envelope decay. */
        quality(q); ApuC1a=0x9f;
        for(unsigned i=0;i<5;i++) ApuRenderingWave1();
        unsigned maximum=0;
        for(unsigned i=0;i<ApuSamplesPerSync;i++) if(wave_buffers[0][i]>maximum) maximum=wave_buffers[0][i];
        assert(maximum==255);
        /* Pulse two's negative sweep subtracts, without pulse one's extra -1. */
        quality(q); ApuC2a=0x9f; ApuC2b=0x89; ApuC2Freq=512; ApuC2Atl=100;
        ApuRenderingWave2(); assert(ApuC2Freq==128); /* two 120-Hz ticks per frame */
        /* A silent first sample cannot leave last frame's PCM in the tail. */
        quality(q); ApuC1Freq=0; memset(wave_buffers[0],0xa5,sizeof(wave_buffers[0]));
        ApuRenderingWave1();
        for(unsigned i=0;i<ApuSamplesPerSync;i++) assert(wave_buffers[0][i]==0);
        /* Later timer writes must still activate a channel that started muted. */
        quality(q); ApuC1Freq=0;ApuC1a=0x9f;
        ApuEventQueue[0]=(struct ApuEvent_t){ApuSamplesPerSync*ApuCyclesPerSample/2,APUET_W_C1C,253};
        cur_event=1; ApuRenderingWave1();
        maximum=0;for(unsigned i=ApuSamplesPerSync/2;i<ApuSamplesPerSync;i++) maximum|=wave_buffers[0][i];
        assert(maximum);cur_event=0;
    }
    puts("PASS: InfoNES envelopes, volume, negative sweep, silence and timed writes at all three sample rates");
}
