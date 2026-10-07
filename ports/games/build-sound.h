/* Shared foreground-only loading from Build's GRP filesystem. */
#include "b310e-mixer.h"
#include "b310e-music.h"
#include <stdlib.h>
#include <string.h>
/* Frequent effects should not stall gameplay on an SD read each time.
 * Raw immutable files are kept in a small LRU; the mixer owns active PCM.
 * Loading and eviction happen only in foreground game code. */
#define B310E_SOUND_CACHE_BYTES (96u*1024)
static struct b310e_sound_cache {
    char name[64]; uint8_t *data; unsigned size, stamp;
} b310e_sound_cache[16];
static unsigned b310e_sound_cache_used, b310e_sound_cache_clock;
static uint8_t *b310e_group_load(const char *name,unsigned *size) {
    if(!name || !*name)return NULL;
    int file=kopen4load(name,0);if(file<0)return NULL;
    int bytes=kfilelength(file);
    if(bytes<=0 || bytes>262144){kclose(file);return NULL;}
    uint8_t *data=(uint8_t*)malloc(bytes);
    if(!data){kclose(file);return NULL;}
    if(kread(file,data,bytes)!=bytes){free(data);data=NULL;}
    kclose(file);*size=bytes;return data;
}
static int b310e_group_sample(const char *name,int volume,int separation) {
    if(!name || !*name)return -1;
    for(unsigned i=0;i<16;i++) {
        struct b310e_sound_cache *s=&b310e_sound_cache[i];
        if(s->data && !strcmp(s->name,name)) {
            s->stamp=++b310e_sound_cache_clock;
            return b310e_sample_file(-1,s->data,s->size,volume,separation);
        }
    }
    unsigned size;uint8_t *data=b310e_group_load(name,&size);
    if(!data)return -1;
    int ch=b310e_sample_file(-1,data,size,volume,separation);
    if(ch<0 || size>B310E_SOUND_CACHE_BYTES || strlen(name)>=64) {free(data);return ch;}
    unsigned slot=0;
    do {
        unsigned oldest=~0u;
        for(unsigned i=0;i<16;i++) {
            if(!b310e_sound_cache[i].data) {
                if(b310e_sound_cache_used+size<=B310E_SOUND_CACHE_BYTES) {slot=i;break;}
                continue;
            }
            if(b310e_sound_cache[i].stamp<oldest) {oldest=b310e_sound_cache[i].stamp;slot=i;}
        }
        struct b310e_sound_cache *s=&b310e_sound_cache[slot];
        b310e_sound_cache_used-=s->size;free(s->data);s->data=NULL;s->size=0;
    } while(b310e_sound_cache_used+size>B310E_SOUND_CACHE_BYTES);
    struct b310e_sound_cache *s=&b310e_sound_cache[slot];
    strcpy(s->name,name);s->data=data;s->size=size;s->stamp=++b310e_sound_cache_clock;
    b310e_sound_cache_used+=size;return ch;
}
static bool b310e_group_music(const char *name,bool loop) {
    unsigned size;uint8_t *data=b310e_group_load(name,&size);
    if(!data)return false;
    bool ok=b310e_music_start(data,size,loop);free(data);return ok;
}
