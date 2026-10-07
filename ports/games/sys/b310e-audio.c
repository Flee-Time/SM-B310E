/* Stereo DMA output shared by the B310E game ports.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "b310e-audio.h"
#include "syscode.h"
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define DMA(ch, off) REG(0x20101000u + (ch)*0x40u + (off))
#define CHANNELS 12u
#define DMA_IRQ (1u << 20)
#define QUEUE 8192u
static int16_t queue[QUEUE][2];
static int16_t plane[2][160] __attribute__((aligned(32)));
static int16_t mixed[320];
static volatile unsigned head, tail;
static unsigned completed, banks, peak[2], gain[2] = {32768,32768};
static bool running;
static bool opened;
static int volume = -120;
static unsigned last_poll;
static void (*renderer)(int16_t *, unsigned);
const unsigned hw_freq_sampr[HW_NUM_FREQ] = {
    96000,48000,44100,32000,24000,22050,16000,12000,11025,9600,8000
};
#include "b310e-volume.h"

void pcm_set_master_volume(int left, int right) {
    int values[2] = {left, right};
    for (unsigned ch=0; ch<2; ch++)
        gain[ch] = values[ch] <= -1000 ? 0 :
            b310e_volume_gain[MIN(100, MAX(0, (-values[ch]+9)/10))];
}
void sc6530_pcm_debug(uint32_t *count, unsigned p[2]) {
    *count = banks; p[0] = peak[0]; p[1] = peak[1];
}
static void arm(void) {
    completed = 0;
    for (unsigned ch=2; ch<=3; ch++) {
        unsigned side = ch == 3 ? 0 : 1;
        DMA(ch,8) = 0;
        DMA(ch,12) = 0x1f000012;
        DMA(ch,16) = (uintptr_t)plane[side];
        DMA(ch,20) = SC_VBC_BASE + side*4;
        DMA(ch,24) = 0x50300140;
        DMA(ch,28) = 320;
        DMA(ch,8) = 0x3001;
    }
}
static void fill(void) {
    if (renderer) renderer(mixed, 160);
    for (unsigned i=0; i<160; i++) {
        for (unsigned ch=0; ch<2; ch++) {
            int value = renderer ? mixed[i*2+ch] : head != tail ? queue[tail & (QUEUE-1)][ch] : 0;
            value = value * (int)gain[ch] / 32768;
            plane[ch][i] = value;
            unsigned magnitude = value < 0 ? -value : value;
            if (magnitude > peak[ch]) peak[ch] = magnitude;
        }
        if (!renderer && head != tail) tail++;
    }
    clean_invalidate_dcache_range(plane, (char*)plane + sizeof(plane));
}
void b310e_audio_irq(void) {
    unsigned pending = REG(0x20100010) & CHANNELS;
    bool error = false;
    for (unsigned ch=2; ch<=3; ch++) if (pending & (1u << ch)) {
        unsigned status = DMA(ch,12);
        DMA(ch,12) = 0x1f000012;
        if (status & (1u << 9)) completed |= 1u << ch;
        if (status & (1u << 12)) error = true;
    }
    if (!running) return;
    if (error) {
        REG(0x8000000c) = DMA_IRQ;
        SC_VBC_CTRL &= ~SC_VBC_PLAY;
        DMA(2,8) = DMA(3,8) = 0;
        running = false;
        return;
    }
    if (completed != CHANNELS) return;
    banks++; fill(); arm();
}
void b310e_audio_close(void) {
    int old = b310e_irq_save();
    REG(0x8000000c) = DMA_IRQ;
    SC_VBC_CTRL &= ~SC_VBC_PLAY;
    DMA(2,8) = DMA(3,8) = 0;
    DMA(2,12) = DMA(3,12) = 0x1f000000;
    if (opened) audiohw_close();
    opened = false;
    running = false; renderer = NULL; head = tail = 0;
    b310e_irq_restore(old);
}
void b310e_audio_init(unsigned rate) {
    b310e_audio_close();
    audiohw_set_volume(volume, volume);
    audiohw_init();
    extern bool b310e_codec_ready(void);
    if (!b310e_codec_ready()) return;
    opened = true;
    unsigned fsel = HW_FREQ_DEFAULT;
    for (unsigned i=0; i<HW_NUM_FREQ; i++) if (hw_freq_sampr[i] == rate) fsel = i;
    audiohw_set_frequency(fsel);
    audiohw_set_volume(volume, volume);
    banks = peak[0] = peak[1] = 0;
    fill();
    REG(0x20102038) = 4;
    REG(0x2010203c) = 3;
    arm(); running = true;
    audiohw_mute(false);
    SC_VBC_CTRL = (SC_VBC_CTRL & ~SC_VBC_CPU_ACCESS) | SC_VBC_PLAY;
    REG(0x80000008) |= DMA_IRQ;
    /* SYS mode, IRQ enabled; the loader installed the high-vector handler. */
    set_cpsr_c(0x5f);
}
void b310e_audio_poll(void) {
    if (!running || sys_timer_ms() - last_poll < 100) return;
    last_poll = sys_timer_ms();
    audiohw_enable_speaker(!headphones_inserted());
}
void b310e_audio_volume(int direction) {
    volume = MIN(240, MAX(-600, volume + direction*30));
    audiohw_set_volume(volume, volume);
}
void b310e_audio_render(void (*fn)(int16_t *, unsigned)) {
    int old = b310e_irq_save(); renderer = fn; b310e_irq_restore(old);
}
unsigned b310e_audio_queued(void) { return head-tail; }
void b310e_audio_submit(const int16_t *stereo, unsigned frames) {
    while (frames && running) {
        int old = b310e_irq_save();
        unsigned space = QUEUE - (head-tail);
        unsigned n = MIN(frames, MIN(space, 160));
        for (unsigned i=0; i<n; i++) {
            queue[head & (QUEUE-1)][0] = *stereo++;
            queue[head & (QUEUE-1)][1] = *stereo++;
            head++;
        }
        b310e_irq_restore(old);
        frames -= n;
        if (!n) break; /* bounded: drop excess, never hang gameplay */
    }
}
void b310e_audio_u8(const uint8_t *data, unsigned bytes, bool stereo) {
    int16_t buf[320];
    while (bytes) {
        unsigned n = MIN(160, bytes / (stereo ? 2 : 1));
        if (!n) break;
        for (unsigned i=0; i<n; i++) {
            buf[i*2] = ((int)*data++ - 128)*256;
            buf[i*2+1] = stereo ? ((int)*data++ - 128)*256 : buf[i*2];
        }
        bytes -= n*(stereo ? 2 : 1);
        b310e_audio_submit(buf, n);
    }
}
