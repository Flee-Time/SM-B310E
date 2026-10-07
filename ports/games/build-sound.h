/* Shared foreground-only loading from Build's GRP filesystem. */
#include "b310e-mixer.h"
#include "b310e-music.h"
#include <stdlib.h>
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
    unsigned size;uint8_t *data=b310e_group_load(name,&size);
    if(!data)return -1;
    int ch=b310e_sample_file(-1,data,size,volume,separation);free(data);return ch;
}
static bool b310e_group_music(const char *name,bool loop) {
    unsigned size;uint8_t *data=b310e_group_load(name,&size);
    if(!data)return false;
    bool ok=b310e_music_start(data,size,loop);free(data);return ok;
}
