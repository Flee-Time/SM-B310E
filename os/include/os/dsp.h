/* B310E OS — the SC6530C's DSP and a receive-only layer 1 on it.
 *
 * The DSP program is the phone's own: dsp_boot() reads the LZMA image
 * from this phone's NOR (0xcc874), checks it is the image the driver was
 * written against, decodes its first segment into the heap, downloads it
 * through the boot ROM's share-memory protocol and starts it. No stock
 * bytes are carried in the OS. docs/gsm-plan.md has the share-memory
 * layout and the frame-instruction format.
 *
 * Receive only: the driver posts idle (type 0) and power-measurement
 * (type 11) frame instructions and never touches the TX buffers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_DSP_H
#define OS_DSP_H

#include <stdbool.h>
#include <stdint.h>

enum {
    DSP_OK = 0,
    DSP_EIMAGE = -1,        /* no DSP image in NOR, or not the known one */
    DSP_ENOMEM = -2,        /* no room to decode the program */
    DSP_EDECODE = -3,       /* the image did not decode to the known program */
    DSP_ENOREADY = -4,      /* the boot ROM never answered DSP_READY */
    DSP_EBLOCK = -5,        /* a download block's handshake timed out */
    DSP_ENOTRUN = -6,       /* not running */
    DSP_ECMD = -7,          /* the DSP did not take a command */
    DSP_EFULL = -8,         /* frame-instruction ring full */
};

enum { DSP_OFF, DSP_BOOTING, DSP_RUNNING, DSP_FAILED };

/* Everything is word-sized so QEMU tests read it by the ELF symbol
 * dsp_info. Append only. */
typedef struct {
    uint32_t state;             /* DSP_OFF .. */
    int32_t error;              /* last dsp_boot() / l1_start() result */
    uint32_t stage;             /* boot progress: 1 image, 2 decoded, 3 reset,
                                   4 ready, 5 blocks, 6 started, 7 l1 up */
    uint32_t blocks;            /* download blocks sent */
    uint32_t program_words;     /* halfwords downloaded */
    uint32_t boot_ms;           /* dsp_boot() duration */
    uint32_t irqs;              /* DSP interrupts (line 13) */
    uint32_t irq_storms;        /* times line 13 was shut off */
    uint32_t records;           /* parameter records the DSP took */
    uint32_t cmds;              /* commands posted */
    uint32_t frames_posted;     /* frame instructions written */
    uint32_t fi_out;            /* last frame-instruction "out" index seen */
    uint32_t fi_moves;          /* times the DSP's "out" index moved */
    uint32_t pm_results;        /* power-monitor results read */
    uint32_t ready_word;        /* dsp_ctl seen at DSP_READY */
} dsp_info_t;

extern volatile dsp_info_t dsp_info;

int dsp_boot(void);             /* DSP_OK or an error; holds 104 MHz+ */
void dsp_stop(void);            /* reset held, interrupt off, clock released */
void dsp_hold_reset(void);      /* at boot: the OS does not use the DSP. The chip
                                 * lets it go at power-on, and nobody else stops
                                 * its boot ROM. dsp_boot() releases it again. */
const char *dsp_strerror(int err);

/* ---- layer 1, receive only ---------------------------------------------- */

/* After dsp_boot(): share memory cleared, the 0x8181 start marker, the
 * DSP image's parameter records sent (the stock layer-1 start order). */
int l1_start(void);

/* Stock "arfcn index" (RF_TOTAL_CHANS = 971 over five bands) for a band
 * and ARFCN; the band order is a guess (docs/gsm-plan.md). 0x3ff: none. */
enum { L1_BAND_GSM900, L1_BAND_DCS1800, L1_BAND_PCS1900, L1_BAND_GSM850 };
#define L1_NO_INDEX 0x3ffu
uint16_t l1_arfcn_index(int band, int arfcn);

/* One power-measurement frame instruction: up to 6 arfcn indexes, each
 * with a 6-bit gain hint (the stock passes its per-channel AGC byte). */
#define L1_PM_PER_FRAME 6
int l1_pm_post(const uint16_t *index, const uint8_t *gain, int n);
int l1_idle_post(int frames);   /* idle frame instructions */
int l1_fi_free(void);           /* free frame-instruction slots */

typedef struct {
    uint16_t index;             /* arfcn index */
    uint16_t level;             /* bits 11:4 rxlev (0..110), 1/16 steps */
    uint16_t aux;               /* meaning unknown (docs/gsm-plan.md) */
    uint16_t status;            /* 1 measured, 2 / 3 saturated (hi / lo?) */
} l1_pm_t;

int l1_pm_read(l1_pm_t *out, int max);  /* results waiting, oldest first */

#endif
