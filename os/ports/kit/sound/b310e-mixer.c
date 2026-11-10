/* Bounded integer PCM mixer; no allocation or filesystem access in the IRQ.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "b310e-mixer.h"
#include "b310e-music.h"
#include <stdlib.h>
#include <string.h>
#define VOICES 8
#define RATE 22050
#define MEMORY_LIMIT (256u*1024)
static struct voice {
    uint8_t *data;
    unsigned size, position, fraction, step, rate, loop_start;
    int left, right;
    bool active, borrowed, loop;        /* borrowed: data is the caller's */
} voices[VOICES];
static unsigned allocated;
static int next_voice;
static void (*extra_render)(int16_t *, unsigned);
#define BLOCK 160
/* A voice over n samples, added to left and right; its state in registers */
static void voice_render(struct voice *v, int *left, int *right, unsigned n) {
    const uint8_t *data=v->data;
    const unsigned size=v->size, step=v->step;
    unsigned position=v->position, fraction=v->fraction;
    const int l=v->left, r=v->right;
    for (unsigned i=0; i<n; i++) {
        if (position >= size) {
            if (!v->loop) { v->active=false; break; }
            position=v->loop_start;
        }
        /* Gains are Q8: no software division in the sample loop. */
        int value = (int)data[position]-128;
        left[i] += value*l; right[i] += value*r;
        fraction += step;
        position += fraction >> 16; fraction &= 65535;
    }
    v->position=position; v->fraction=fraction;
}
static void render(int16_t *out, unsigned frames) {
    while (frames) {
        const unsigned n=frames<BLOCK?frames:BLOCK;
        int16_t extra[BLOCK*2];
        int left[BLOCK], right[BLOCK];
        memset(left,0,n*sizeof(left[0]));
        b310e_music_render(left,n);
        memcpy(right,left,n*sizeof(left[0]));
        if (extra_render) {
            extra_render(extra,n);
            for (unsigned i=0; i<n; i++) { left[i]+=extra[i*2]; right[i]+=extra[i*2+1]; }
        }
        for (unsigned ch=0; ch<VOICES; ch++)
            if (voices[ch].active) voice_render(&voices[ch],left,right,n);
        for (unsigned i=0; i<n; i++) {
            out[i*2] = MIN(32767, MAX(-32768,left[i]));
            out[i*2+1] = MIN(32767, MAX(-32768,right[i]));
        }
        out+=n*2; frames-=n;
    }
}
void b310e_mixer_init(void) {
    b310e_audio_close();
    for (unsigned ch=0; ch<VOICES; ch++) if (!voices[ch].borrowed) free(voices[ch].data);
    memset(voices,0,sizeof(voices)); allocated=0; next_voice=0; extra_render=NULL;
    b310e_audio_init(RATE); b310e_audio_render(render);
}
void b310e_mixer_hook(void (*fn)(int16_t *, unsigned)) {
    int old=b310e_irq_save(); extra_render=fn; b310e_irq_restore(old);
}
void b310e_sample_loop(int channel, bool loop) {
    if ((unsigned)channel < VOICES) voices[channel].loop=loop;
}
/* Loops from a byte of its own, not its start (the start, when past its end) */
void b310e_sample_loop_from(int channel, unsigned start) {
    if ((unsigned)channel >= VOICES) return;
    voices[channel].loop_start = start < voices[channel].size ? start : 0;
    voices[channel].loop = true;
}
void b310e_sample_stop(int channel) {
    if ((unsigned)channel >= VOICES) return;
    int old = b310e_irq_save(); voices[channel].active=false; b310e_irq_restore(old);
}
/* A playing voice's pitch, 128 its own */
void b310e_sample_pitch(int channel, int pitch) {
    if ((unsigned)channel >= VOICES) return;
    struct voice *v = &voices[channel];
    int old = b310e_irq_save();
    v->step = (unsigned)((uint64_t)v->rate*65536*MAX(1,pitch)/ (RATE*128u));
    b310e_irq_restore(old);
}
/* A new rate for it (its own pitch: Blood's Doppler) */
void b310e_sample_rate(int channel, unsigned rate) {
    if ((unsigned)channel >= VOICES || !rate || rate > 96000) return;
    struct voice *v = &voices[channel];
    int old = b310e_irq_save();
    v->rate = rate;
    v->step = (unsigned)((uint64_t)rate*65536/RATE);
    b310e_irq_restore(old);
}
bool b310e_sample_active(int channel) {
    return (unsigned)channel < VOICES && voices[channel].active;
}
void b310e_sample_volume(int channel, int volume, int separation) {
    if ((unsigned)channel >= VOICES) return;
    volume = MIN(255,MAX(0,volume)); separation = MIN(255,MAX(0,separation));
    int old = b310e_irq_save();
    voices[channel].left = (volume*MIN(255,(255-separation)*2)*256+32512)/65025;
    voices[channel].right = (volume*MIN(255,separation*2)*256+32512)/65025;
    b310e_irq_restore(old);
}
int b310e_sample_play(int channel, const uint8_t *data, unsigned bytes,
                      unsigned rate, int volume, int separation, int pitch) {
    if (!data || !bytes || bytes > 131072 || !rate || rate > 96000) return -1;
    if (channel < 0) channel = next_voice++ & (VOICES-1);
    if ((unsigned)channel >= VOICES) return -1;
    struct voice *v = &voices[channel];
    /* Retain buffers only while useful; enforce the aggregate RAM budget. */
    for (unsigned ch=0; ch<VOICES; ch++)
        if (!voices[ch].active && !voices[ch].borrowed && ch != (unsigned)channel) {
            free(voices[ch].data); allocated -= voices[ch].size;
            voices[ch].data=NULL; voices[ch].size=0;
        }
    if (v->borrowed) { v->active=false; v->borrowed=false; v->data=NULL; v->size=0; }
    if (allocated-v->size+bytes > MEMORY_LIMIT) return -1;
    uint8_t *copy = malloc(bytes);
    if (!copy) return -1;
    memcpy(copy,data,bytes);
    int old = b310e_irq_save();
    uint8_t *previous = v->data;
    allocated = allocated-v->size+bytes;
    v->active=false; v->loop=false; v->loop_start=0; v->data=copy; v->size=bytes; v->position=v->fraction=0;
    v->rate = rate; v->step = (unsigned)((uint64_t)rate*65536*MAX(1,pitch)/ (RATE*128u));
    b310e_sample_volume(channel,volume,separation); v->active=true;
    b310e_irq_restore(old); free(previous); return channel;
}
/* As b310e_sample_play, without a copy: the data must stay until the voice
 * is no longer active (b310e_sample_active). */
int b310e_sample_borrow(int channel, const uint8_t *data, unsigned bytes,
                        unsigned rate, int volume, int separation, int pitch) {
    if (!data || !bytes || !rate || rate > 96000 || (unsigned)channel >= VOICES) return -1;
    struct voice *v = &voices[channel];
    int old = b310e_irq_save();
    uint8_t *previous = v->borrowed ? NULL : v->data;
    if (previous) allocated -= v->size;
    v->active=false; v->borrowed=true; v->loop=false; v->loop_start=0; v->data=(uint8_t *)data; v->size=bytes;
    v->position=v->fraction=0;
    v->rate = rate; v->step = (unsigned)((uint64_t)rate*65536*MAX(1,pitch)/ (RATE*128u));
    b310e_sample_volume(channel,volume,separation); v->active=true;
    b310e_irq_restore(old); free(previous); return channel;
}
static unsigned le16(const uint8_t *p) { return p[0]|p[1]<<8; }
static unsigned le32(const uint8_t *p) { return le16(p)|le16(p+2)<<16; }
/* A VOC or WAV: copied (channel may be -1), or, from b310e_sample_file_borrow,
 * played where it lies */
static int (*start)(int, const uint8_t *, unsigned, unsigned, int, int, int) = b310e_sample_play;
static int file_pitch = 128;

static int sample_file(int channel, const uint8_t *data, unsigned bytes, int volume, int separation);

int b310e_sample_file(int channel, const uint8_t *data, unsigned bytes, int volume, int separation) {
    start = b310e_sample_play;
    file_pitch = 128;
    return sample_file(channel,data,bytes,volume,separation);
}
int b310e_sample_file_borrow(int channel, const uint8_t *data, unsigned bytes, int volume, int separation, int pitch) {
    start = b310e_sample_borrow;
    file_pitch = pitch;
    return sample_file(channel,data,bytes,volume,separation);
}
static int sample_file(int channel, const uint8_t *data, unsigned bytes, int volume, int separation) {
    if (!data) return -1;
    if (bytes >= 26 && !memcmp(data,"Creative Voice File",19)) {
        unsigned pos = le16(data+20), rate = 0;
        while (pos+4 <= bytes) {
            unsigned type=data[pos], n=data[pos+1]|data[pos+2]<<8|data[pos+3]<<16;
            pos += 4;
            if (n > bytes-pos) return -1;
            if (type == 1 && n >= 2 && data[pos+1] == 0) {
                rate = 1000000u/(256-data[pos]);
                return start(channel,data+pos+2,n-2,rate,volume,separation,file_pitch);
            }
            if (type == 9 && n >= 12 && data[pos+4] == 8 && data[pos+5] == 1 && le16(data+pos+6) == 0)
                return start(channel,data+pos+12,n-12,le32(data+pos),volume,separation,file_pitch);
            if (!type) break;
            pos += n;
        }
    } else if (bytes >= 44 && !memcmp(data,"RIFF",4) && !memcmp(data+8,"WAVE",4)) {
        unsigned pos=12, rate=0, bits=0, channels=0;
        while (pos+8 <= bytes) {
            unsigned n=le32(data+pos+4);
            if (n > bytes-pos-8) return -1;
            const uint8_t *p=data+pos+8;
            if (!memcmp(data+pos,"fmt ",4) && n >= 16 && le16(p) == 1) {
                channels=le16(p+2); rate=le32(p+4); bits=le16(p+14);
            } else if (!memcmp(data+pos,"data",4) && channels == 1 && bits == 8)
                return start(channel,p,n,rate,volume,separation,file_pitch);
            pos += 8+n+(n&1);
        }
    }
    return -1;
}
