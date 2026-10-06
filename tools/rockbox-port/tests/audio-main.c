/* Test entry point around the real Rockbox PCM core, software volume,
 * target startup, IRQ dispatcher, codec and DMA sink. No driver stubs.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "system.h"
#include "pcm-internal.h"
#include "pcm_sw_volume.h"
#include "audiohw.h"
#include "audio.h"
#include "audio-target.h"

#ifndef TEST_RATE
#define TEST_RATE 44100
#endif
#ifndef TEST_VOLUME
#define TEST_VOLUME 0
#endif
#ifndef TEST_CASE
#define TEST_CASE 0
#endif
#ifndef TEST_FRAMES
#define TEST_FRAMES 4093
#endif
#ifndef TEST_HEADSET
#define TEST_HEADSET 0
#endif
#define FRAMES TEST_FRAMES
static int16_t data[FRAMES * 2] __attribute__((aligned(32)));
static unsigned cursor, packet;
static volatile bool dma_error;
volatile long current_tick;

static void host_call(unsigned op, const void *args)
{
    register unsigned r0 asm("r0") = op;
    register const void *r1 asm("r1") = args;
    asm volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory");
}

static void finish(bool success) __attribute__((noreturn));
static void finish(bool success)
{
    host_call(4, success ? "PASS Rockbox ARM audio\n" : "FAIL Rockbox ARM audio\n");
    const unsigned args[] = {0x20026, !success};
    host_call(0x20, args);
    for (;;) {}
}

static void diagnostic(unsigned value)
{
    char message[] = "state=0x00000000\n";
    for (unsigned i = 0; i < 8; i++)
        message[8+i] = "0123456789abcdef"[(value >> (28-i*4)) & 15];
    host_call(4, message);
}

/* Outer application/scheduler hooks only; the playback core is unchanged. */
void mixer_set_frequency(unsigned int rate) { (void)rate; }
void TIMER23(void)
{
    current_tick++;
    SC_AUDIO_REG(0x8100004c) = 9;
}
void TIMER0(void) {}
static void wait_ms(unsigned ms)
{
    uint32_t start = SC_AUDIO_REG(0x8100300c);
    while ((uint32_t)(SC_AUDIO_REG(0x8100300c) - start) < ms)
        asm volatile("mcr p15, 0, %0, c7, c0, 4" : : "r"(0) : "memory");
}
void sleep(int ticks) { wait_ms(ticks > 0 ? ticks * 10 : 1); }
void undef_instr_handler(void) { finish(false); }
void software_int_handler(void) { finish(false); }
void prefetch_abort_handler(void) { finish(false); }
void data_abort_handler(void) { finish(false); }
void reserved_handler(void) { finish(false); }

static void more(const void **addr, size_t *size)
{
    static const unsigned packets[] = {13, 333, 1024, 1, 159, 321, 640, 160};
    unsigned n = packets[packet++ % 8];
    if (n > FRAMES - cursor)
        n = FRAMES - cursor;
    *addr = data + cursor * 2;
    *size = n * 4;
    cursor += n;
}

static enum pcm_dma_status status(enum pcm_dma_status s)
{
    if (s == PCM_DMAST_ERR_DMA)
        dma_error = true;
    return s;
}

static bool output_matches(bool speaker)
{
    return (SC_AUDIO_REG(0x82001a7c) & 0xff) == (speaker ? 0x33u : 0x84u) &&
           (SC_AUDIO_REG(0x82001a84) & 0xfc) == (speaker ? 0x10u : 0xc4u) &&
           (SC_AUDIO_REG(0x82001a44) & 0xa8) == (speaker ? 0x88u : 0u) &&
           (SC_AUDIO_REG(0x82001a88) & 0xf0) == (speaker ? 0x30u : 0u) &&
           (SC_AUDIO_REG(0x82001a8c) & 0x80) == (speaker ? 0x80u : 0u) &&
           (SC_AUDIO_REG(0x82001a94) & 0xff) == (speaker ? 0u : 0x44u) &&
           (SC_AUDIO_REG(0x82001a9c) & 0xff) == (speaker ? 0x70u : 0u) &&
           (SC_AUDIO_REG(0x8a000000) & 1) == !speaker &&
           (SC_AUDIO_REG(0x82001164) & 0x31f0) == 0x3000 &&
           (SC_AUDIO_REG(0x82001a40) & 0xfa) == 0xfa;
}

static bool shared_state_preserved(void)
{
    return (SC_AUDIO_REG(0x8a000000) & 0x40) &&
           (SC_AUDIO_REG(0x8a000004) & 0x40) &&
           (SC_AUDIO_REG(0x8a000008) & 0x40) &&
           (SC_AUDIO_REG(0x8a000018) & 0x40) &&
           SC_AUDIO_REG(0x82001904) == 8 &&
           SC_AUDIO_REG(0x820010e4) == 0x20 &&
           SC_AUDIO_REG(0x820010e0) == 0x80;
}

int main(void)
{
    system_init();
    /* Sentinel states catch whole-register writes to unrelated pins,
     * regulators and the analog power-button EIC/clock bank. */
    SC_AUDIO_REG(0x82001164) = 0x31f0;
    SC_AUDIO_REG(0x8a000004) = 0x40;
    SC_AUDIO_REG(0x8a000008) = 0x40;
    SC_AUDIO_REG(0x8a000000) = 0x40;
    SC_AUDIO_REG(0x8a000018) = 0x40;
    SC_AUDIO_REG(0x82001904) = 8;
    SC_AUDIO_REG(0x820010e4) = 0x20;
    SC_AUDIO_REG(0x820010e0) = 0x80;
    pcm_init();
    pcm_postinit();
    audio_enable_speaker(2); /* real Rockbox Auto/jack policy */
    if (headphones_inserted() != TEST_HEADSET ||
        !output_matches(!TEST_HEADSET) || !shared_state_preserved())
        finish(false);
    pcm_set_frequency(TEST_RATE);
    /* Exercise sound.c's codec hook; calling the scaler directly hid a
     * silent full-player integration bug in the original driver. */
    audiohw_set_volume(TEST_VOLUME);
    pcm_sync_pcm_factors();
    for (unsigned i = 0; i < FRAMES; i++)
    {
        data[i * 2] = 1200 + i % 200;
        data[i * 2 + 1] = -2500 - i % 200;
    }
    enable_irq();
    /* A 1 ms scheduler tick also wakes WFI after the playback IRQ stops. */
    SC_AUDIO_REG(0x81000040) = 26000;
    SC_AUDIO_REG(0x8100004c) = 1;
    SC_AUDIO_REG(0x81000048) = 0xc0;
    SC_AUDIO_REG(0x80000008) = 1u << 23;
    pcm_play_data(more, status, NULL, 0);
    if (TEST_CASE == 1 || TEST_CASE == 5)
    {
        wait_ms(15);
        pcm_play_stop();
        if ((SC_VBC_CTRL & SC_VBC_PLAY) || SC_AUDIO_REG(0x20100018))
            finish(false);
        if (TEST_CASE == 5)
        {
            audiohw_close();
            audiohw_init();
            if (!output_matches(!TEST_HEADSET) || !shared_state_preserved())
                finish(false);
        }
        wait_ms(30);
        cursor = packet = 0;
        for (unsigned i = 0; i < FRAMES; i++)
        {
            data[i * 2] = 20000 + i % 200;
            data[i * 2 + 1] = -21000 - i % 200;
        }
        pcm_play_data(more, status, NULL, 0);
    }
    else if (TEST_CASE == 2)
        SC_AUDIO_REG(0x201010c8) |= 0x10; /* provoke a DMA config error */
    else if (TEST_CASE == 3)
    {
        pcm_play_lock();
        pcm_play_lock();
        wait_ms(1);
        pcm_play_unlock();
        if (SC_AUDIO_REG(0x80000008) & (1u << 20))
            finish(false);
        pcm_play_unlock();
    }
    else if (TEST_CASE == 4)
    {
        wait_ms(10);
        audio_enable_speaker(TEST_HEADSET);
        if (!output_matches(TEST_HEADSET))
            finish(false);
        wait_ms(10);
        audio_enable_speaker(2);
        if (!output_matches(!TEST_HEADSET))
            finish(false);
    }
    /* Includes the final partial bank and the FIFO drain after core stop. */
    wait_ms(FRAMES * 1000 / TEST_RATE + 200);
    bool success = !pcm_is_playing() && !(SC_VBC_CTRL & SC_VBC_PLAY) &&
                   !SC_AUDIO_REG(0x20100010) && !SC_AUDIO_REG(0x20100018) &&
                   (SC_AUDIO_REG(0x80000008) & (1u << 23)) && current_tick > 0;
    if (TEST_CASE == 2)
        success &= dma_error;
    else
        success &= !dma_error && cursor == FRAMES;
    if (!success)
    {
        diagnostic(pcm_is_playing());
        diagnostic(SC_VBC_CTRL);
        diagnostic(SC_AUDIO_REG(0x20100010));
        diagnostic(SC_AUDIO_REG(0x20100018));
        diagnostic(cursor);
        diagnostic(dma_error);
    }
    audiohw_close();
    success &= !(SC_AUDIO_REG(0x8a002000) & 5) &&
               !(SC_AUDIO_REG(0x82001a44) & 0xa8) &&
               !(SC_AUDIO_REG(0x82001a84) & 0xfc) &&
               !(SC_AUDIO_REG(0x8a000000) & 1) &&
               (SC_AUDIO_REG(0x82001164) & 0x31f0) == 0x31f0 &&
               shared_state_preserved();
    /* Keep the host backend alive long enough to write its last samples. */
    wait_ms(100);
    finish(success);
}
