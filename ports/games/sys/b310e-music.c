/* Small MUS/SMF sequencer with an integer wavetable synthesizer.
 * No external sound bank. Instruments are approximate, not an OPL emulation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "b310e-music.h"
#include <stdlib.h>
#include <string.h>
#define RATE 22050u
#define TRACKS 16
#define NOTES 24
static uint8_t *song;
static unsigned song_size, division, tempo, tick, phase, step;
static bool mus, playing, paused, looping;
static int volume = 80;
static struct track { const uint8_t *p, *end, *start; unsigned next; uint8_t status; bool done; } tracks[TRACKS];
static unsigned ntracks;
static uint8_t velocity[16], programs[16], channel_volume[16];
static struct note { unsigned phase, step, age; uint8_t channel, key, velocity; bool release, active; } notes[NOTES];
#include "b310e-notes.h"
static unsigned le16(const uint8_t *p) { return p[0]|p[1]<<8; }
static unsigned be16(const uint8_t *p) { return p[0]<<8|p[1]; }
static unsigned be32(const uint8_t *p) { return be16(p)<<16|be16(p+2); }
static unsigned vlq(struct track *t) {
    unsigned value=0;
    for (unsigned i=0; i<4 && t->p<t->end; i++) {
        unsigned c=*t->p++; value=(value<<7)|(c&127);
        if (!(c&128)) return value;
    }
    t->done=true; return 0;
}
static void timing(void) { step=(unsigned)((uint64_t)division*1000000*65536/((uint64_t)tempo*RATE)); }
static void note_off(unsigned ch, unsigned key) {
    for (unsigned i=0; i<NOTES; i++) if (notes[i].channel==ch && notes[i].key==key) notes[i].release=true;
}
static void note_on(unsigned ch, unsigned key, unsigned vel) {
    if (!vel) { note_off(ch,key); return; }
    unsigned chosen=0, oldest=0;
    for (unsigned i=0; i<NOTES; i++) {
        if (!notes[i].active) { chosen=i; break; }
        if (notes[i].age>=oldest) { oldest=notes[i].age; chosen=i; }
    }
    struct note *n=&notes[chosen];
    n->phase=n->age=0; n->step=b310e_note_step[key&127];
    n->channel=ch; n->key=key&127; n->velocity=MIN(127,vel); n->release=false; n->active=true;
}
static void control(unsigned ch, unsigned cc, unsigned value) {
    if (cc==7) channel_volume[ch]=value;
    if (cc==120 || cc==123) for (unsigned i=0; i<NOTES; i++) if (notes[i].channel==ch) notes[i].release=true;
}
static void reset(void) {
    tick=phase=0; memset(notes,0,sizeof(notes));
    memset(velocity,64,sizeof(velocity)); memset(programs,0,sizeof(programs)); memset(channel_volume,127,sizeof(channel_volume));
    tempo=mus?1000000:500000; timing();
    for (unsigned i=0; i<ntracks; i++) {
        struct track *t=&tracks[i]; t->p=t->start; t->done=false; t->status=0;
        t->next=mus?0:vlq(t);
    }
}
static void mus_event(struct track *t) {
    if (t->p>=t->end) { t->done=true; return; }
    unsigned event=*t->p++, type=(event>>4)&7, ch=event&15;
    if (type==6) { t->done=true; return; }
    if (t->p>=t->end) { t->done=true; return; }
    unsigned value=*t->p++;
    if (type==0) note_off(ch,value&127);
    else if (type==1) {
        if (value&128) { if (t->p>=t->end) {t->done=true;return;} velocity[ch]=*t->p++&127; }
        note_on(ch,value&127,velocity[ch]);
    } else if (type==3) control(ch,value==10?120:123,0);
    else if (type==4) {
        if (t->p>=t->end) {t->done=true;return;}
        unsigned val=*t->p++&127;
        if (value==0) programs[ch]=val;
        if (value==3) control(ch,7,val);
    }
    if (event&128) t->next+=vlq(t);
}
static void midi_event(struct track *t) {
    if (t->p>=t->end) {t->done=true;return;}
    unsigned status=*t->p;
    if (status&128) {t->p++; if (status<240) t->status=status;}
    else status=t->status;
    if (!status) {t->done=true;return;}
    if (status==255) {
        if (t->p>=t->end) {t->done=true;return;}
        unsigned type=*t->p++, len=vlq(t);
        if (len>(unsigned)(t->end-t->p)) {t->done=true;return;}
        if (type==47) t->done=true;
        if (type==81 && len==3) {
            unsigned value=t->p[0]<<16|t->p[1]<<8|t->p[2];
            if (value>=10000 && value<=10000000) {tempo=value;timing();}
        }
        t->p+=len;
    } else if (status==240 || status==247) {
        unsigned len=vlq(t);
        if (len>(unsigned)(t->end-t->p)) {t->done=true;return;}
        t->p+=len;
    } else if (status>=128 && status<240) {
        unsigned type=status>>4, ch=status&15, count=(type==12||type==13)?1:2;
        if ((unsigned)(t->end-t->p)<count) {t->done=true;return;}
        unsigned a=*t->p++&127, b=count==2?*t->p++&127:0;
        if (type==8) note_off(ch,a);
        if (type==9) note_on(ch,a,b);
        if (type==11) control(ch,a,b);
        if (type==12) programs[ch]=a;
    } else {t->done=true;return;}
    if (!t->done) t->next+=vlq(t);
}
bool b310e_music_start(const void *data, unsigned size, bool loop) {
    if (!data || size<16 || size>256*1024) return false;
    uint8_t *copy=malloc(size); if (!copy) return false; memcpy(copy,data,size);
    struct track parsed[TRACKS]; unsigned count=0, div=0; bool ismus=false;
    if (!memcmp(copy,"MUS\x1a",4)) {
        unsigned len=le16(copy+4), start=le16(copy+6);
        if (start<=size && len<=size-start) {
            parsed[0].start=copy+start; parsed[0].end=copy+start+len; count=1; div=140; ismus=true;
        }
    } else if (!memcmp(copy,"MThd",4) && be32(copy+4)>=6 && be32(copy+4)<=size-8) {
        div=be16(copy+12); unsigned pos=8+be32(copy+4);
        unsigned declared=be16(copy+10),format=be16(copy+8);
        if (format>1 || !declared || declared>TRACKS || (format==0 && declared!=1)) {free(copy);return false;}
        if (div && !(div&0x8000)) while (pos+8<=size && count<TRACKS) {
            unsigned len=be32(copy+pos+4);
            if (len>size-pos-8 || memcmp(copy+pos,"MTrk",4)) break;
            parsed[count].start=copy+pos+8; parsed[count].end=copy+pos+8+len; count++; pos+=8+len;
        }
        if (count!=declared) {free(copy);return false;}
    }
    if (!count || !div) {free(copy);return false;}
    int old=b310e_irq_save(); uint8_t *previous=song;
    song=copy; song_size=size; division=div; ntracks=count; mus=ismus;
    memcpy(tracks,parsed,count*sizeof(parsed[0])); looping=loop; paused=false; reset(); playing=true;
    b310e_irq_restore(old); free(previous); return true;
}
void b310e_music_stop(void) {
    int old=b310e_irq_save(); playing=false; memset(notes,0,sizeof(notes));
    uint8_t *previous=song; song=NULL; song_size=0; b310e_irq_restore(old); free(previous);
}
void b310e_music_pause(bool value) {paused=value;}
void b310e_music_volume(int value) {volume=MIN(127,MAX(0,value));}
bool b310e_music_playing(void) {return playing;}
int b310e_music_sample(void) {
    if (!playing || paused) return 0;
    unsigned done=0;
    for (unsigned i=0; i<ntracks; i++) {
        struct track *t=&tracks[i]; unsigned budget=512;
        while (!t->done && t->next<=tick && budget) {
            budget--;
            if (mus) mus_event(t); else midi_event(t);
        }
        if (!budget) t->done=true; /* malformed zero-delay event chains are bounded */
        done+=t->done;
    }
    if (done==ntracks) {
        if (looping) reset(); else {playing=false;return 0;}
    }
    phase+=step; tick+=phase>>16; phase&=65535;
    int sample=0;
    for (unsigned i=0; i<NOTES; i++) {
        struct note *n=&notes[i]; if (!n->active) continue;
        n->phase+=n->step; n->age++;
        if (n->release && (n->age&31)==0 && n->velocity) n->velocity--;
        if (!n->velocity || ((mus?n->channel==15:n->channel==9) && n->age>RATE/5)) {n->active=false;continue;}
        int wave=b310e_sine[n->phase>>24];
        if (programs[n->channel]<8) wave=(wave*3+b310e_sine[(n->phase>>23)&255])/4;
        sample+=wave*n->velocity*channel_volume[n->channel]/(127*24);
    }
    return sample*volume/127;
}
