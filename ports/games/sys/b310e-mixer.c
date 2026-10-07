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
    unsigned size, position, fraction, step;
    int left, right;
    bool active;
} voices[VOICES];
static unsigned allocated;
static int next_voice;
static void (*extra_render)(int16_t *, unsigned);
static void render(int16_t *out, unsigned frames) {
    int16_t extra[320];
    if (extra_render) extra_render(extra,frames);
    for (unsigned i=0; i<frames; i++) {
        int left=b310e_music_sample(), right=left;
        if (extra_render) { left+=extra[i*2]; right+=extra[i*2+1]; }
        for (unsigned ch=0; ch<VOICES; ch++) {
            struct voice *v = &voices[ch];
            if (!v->active) continue;
            if (v->position >= v->size) { v->active=false; continue; }
            /* Gains are Q8: no software division in the sample loop. */
            int value = (int)v->data[v->position]-128;
            left += value*v->left; right += value*v->right;
            v->fraction += v->step;
            v->position += v->fraction >> 16; v->fraction &= 65535;
        }
        out[i*2] = MIN(32767, MAX(-32768,left));
        out[i*2+1] = MIN(32767, MAX(-32768,right));
    }
}
void b310e_mixer_init(void) {
    b310e_audio_close();
    for (unsigned ch=0; ch<VOICES; ch++) free(voices[ch].data);
    memset(voices,0,sizeof(voices)); allocated=0; next_voice=0; extra_render=NULL;
    b310e_audio_init(RATE); b310e_audio_render(render);
}
void b310e_mixer_hook(void (*fn)(int16_t *, unsigned)) {
    int old=b310e_irq_save(); extra_render=fn; b310e_irq_restore(old);
}
void b310e_sample_stop(int channel) {
    if ((unsigned)channel >= VOICES) return;
    int old = b310e_irq_save(); voices[channel].active=false; b310e_irq_restore(old);
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
    for (unsigned ch=0; ch<VOICES; ch++) if (!voices[ch].active && ch != (unsigned)channel) {
        free(voices[ch].data); allocated -= voices[ch].size;
        voices[ch].data=NULL; voices[ch].size=0;
    }
    if (allocated-v->size+bytes > MEMORY_LIMIT) return -1;
    uint8_t *copy = malloc(bytes);
    if (!copy) return -1;
    memcpy(copy,data,bytes);
    int old = b310e_irq_save();
    uint8_t *previous = v->data;
    allocated = allocated-v->size+bytes;
    v->active=false; v->data=copy; v->size=bytes; v->position=v->fraction=0;
    v->step = (unsigned)((uint64_t)rate*65536*MAX(1,pitch)/ (RATE*128u));
    b310e_sample_volume(channel,volume,separation); v->active=true;
    b310e_irq_restore(old); free(previous); return channel;
}
static unsigned le16(const uint8_t *p) { return p[0]|p[1]<<8; }
static unsigned le32(const uint8_t *p) { return le16(p)|le16(p+2)<<16; }
int b310e_sample_file(int channel, const uint8_t *data, unsigned bytes, int volume, int separation) {
    if (!data) return -1;
    if (bytes >= 26 && !memcmp(data,"Creative Voice File",19)) {
        unsigned pos = le16(data+20), rate = 0;
        while (pos+4 <= bytes) {
            unsigned type=data[pos], n=data[pos+1]|data[pos+2]<<8|data[pos+3]<<16;
            pos += 4;
            if (n > bytes-pos) return -1;
            if (type == 1 && n >= 2 && data[pos+1] == 0) {
                rate = 1000000u/(256-data[pos]);
                return b310e_sample_play(channel,data+pos+2,n-2,rate,volume,separation,128);
            }
            if (type == 9 && n >= 12 && data[pos+4] == 8 && data[pos+5] == 1 && le16(data+pos+6) == 0)
                return b310e_sample_play(channel,data+pos+12,n-12,le32(data+pos),volume,separation,128);
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
                return b310e_sample_play(channel,p,n,rate,volume,separation,128);
            pos += 8+n+(n&1);
        }
    }
    return -1;
}
