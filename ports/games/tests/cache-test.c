#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned reads, current_size=12000;
static int kopen4load(const char *name,int only) {(void)only;return !strcmp(name,"missing")?-1:1;}
static int kfilelength(int file) {(void)file;return current_size;}
static int kread(int file,void *data,int bytes) {
    (void)file;reads++;memset(data,128,bytes);
    unsigned char *p=data;memset(p,0,32);memcpy(p,"Creative Voice File",19);p[20]=26;
    p[26]=1;unsigned n=bytes-30;p[27]=n;p[28]=n>>8;p[29]=n>>16;p[30]=211;p[31]=0;
    return bytes;
}
static void kclose(int file) {(void)file;}
#include "b310e-build-sound.h"
static void (*renderer)(int16_t *,unsigned);
int b310e_irq_save(void){return 0;}
void b310e_irq_restore(int mode){(void)mode;}
void b310e_audio_init(unsigned rate){assert(rate==22050);}
void b310e_audio_close(void){renderer=NULL;}
void b310e_audio_render(void (*fn)(int16_t *,unsigned)){renderer=fn;}
static void finish(void) {
    int16_t out[320];for(unsigned i=0;i<200;i++) renderer(out,160);
}
int main(void) {
    b310e_mixer_init();
    assert(b310e_group_sample("shot.voc",255,128)>=0);finish();
    for(unsigned i=0;i<20;i++) {assert(b310e_group_sample("shot.voc",255,128)>=0);finish();}
    assert(reads==1); /* A repeated effect performs no second SD read. */
    for(unsigned i=0;i<40;i++) {
        char name[64];sprintf(name,"effect%u.voc",i);
        assert(b310e_group_sample(name,255,128)>=0);finish();
        assert(b310e_sound_cache_used<=B310E_SOUND_CACHE_BYTES);
    }
    unsigned before=reads;assert(b310e_group_sample("effect39.voc",255,128)>=0);finish();assert(reads==before);
    assert(b310e_group_sample("shot.voc",255,128)>=0);finish();assert(reads==before+1);
    assert(b310e_group_sample("missing",255,128)<0);
    current_size=110000;
    assert(b310e_group_sample("large.voc",255,128)>=0);finish();
    assert(b310e_sound_cache_used<=B310E_SOUND_CACHE_BYTES);
    puts("PASS: Build effect cache hits, eviction, memory bound, missing and oversized files");
}
