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
#include "os/l1cell.h"

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
    uint32_t started;           /* the DSP took the start command */
    uint32_t param_blocks;      /* NV parameter blocks it took */
} dsp_info_t;

extern volatile dsp_info_t dsp_info;

/* dsp_probe(): is the program really running? Each field is something only
 * a running DSP can produce (see docs/gsm-plan.md). Words only, for QEMU
 * tests through the ELF symbol dsp_probe_info. Append only. */
enum { DSP_P_START = 1, DSP_P_RECORDS, DSP_P_FRAMES, DSP_P_PING, DSP_P_DUMP, DSP_P_DONE };
#define DSP_PROBE_REPLIES 16
typedef struct {
    uint32_t phase;             /* DSP_P_*: the step begun last */
    uint32_t early_words;       /* non-zero share words soon after the start */
    uint32_t early_first;       /* the first: offset << 16 | value */
    uint32_t records_total;
    uint32_t records_taken;     /* parameter records the DSP took */
    uint32_t cmd_in, cmd_out;
    uint32_t int_sts;           /* status bits seen at 0x8b000140 */
    uint32_t replies;           /* response-ring entries */
    uint32_t msgs;              /* messages on the control block */
    uint32_t msg_first, msg_last;       /* code << 16 | parameter */
    uint32_t idle_posted, idle_consumed;
    uint32_t frame_us;          /* measured time per idle frame */
    uint32_t ping_before, ping_after, ping_ms;
    uint32_t dump_blocks;       /* of 64, saved to dsp-dump.bin */
    uint32_t dump_nonzero;
    uint32_t dump_head[4];      /* the first four words */
    uint32_t dump_is_program;   /* block 0 is the program's first block */
    uint32_t rec_words, rec_match;      /* record words found in the dump */
    uint32_t reply[DSP_PROBE_REPLIES][5];
    /* Start-up trace: bit n, the program reached point n (trace_points[] in
     * drivers/dsp.c; bit 0 the reset vector, 7 waiting, 9 command handler). */
    uint32_t trace_start, trace_records, trace_end;
    uint32_t reply_kept;        /* in reply[]: all but the records' acks */
    uint32_t wake_taken;        /* the run command (l1_run) was taken */
    uint32_t after_wake;        /* unused */
    uint32_t after_each_wake[3];        /* unused */
    uint32_t last_ack[5];       /* the last ack: word 2.. carry the DSP's ids */
    uint32_t msg_phase;         /* DSP_P_* when the first message came */
    uint32_t msg_log[8];        /* the last 8 messages, msg_log[msgs % 8] newest */
    uint32_t beps;              /* BEP reports drained (+0xfa0 ring) */
    uint32_t bep_last[2];       /* the last one's two words */
} dsp_probe_t;

extern volatile dsp_probe_t dsp_probe_info;

/* dsp_selftest(): our own few words of DSP code instead of the stock program:
 * does the DSP execute what the boot ROM takes, and at what rate? Run with
 * 0x8b0001a0 bits 21:19 set and clear. Words only (symbol dsp_selftest_info). */
typedef struct {
    int32_t result;             /* download(), DSP_OK */
    uint32_t mark0, mark1;      /* 0x5a01 it ran; 0x5a02 after the first set-up */
    uint32_t echo;              /* 0x1234 it read share memory */
    uint32_t busy, idle;        /* its count per 20 ms: ARM busy, ARM idle */
    uint32_t mcu_ctl0, dsp_ctl0;
} dsp_selftest_case_t;

typedef struct {
    dsp_selftest_case_t banks_set, banks_clear;
    uint32_t trace_start, trace_records, trace_end;     /* the stock program */
} dsp_selftest_t;

extern volatile dsp_selftest_t dsp_selftest_info;

int dsp_boot(void);             /* DSP_OK or an error; holds 104 MHz+ */
void dsp_stop(void);            /* reset held, interrupt off, clock released */
void dsp_hold_reset(void);      /* at boot: the OS does not use the DSP. The chip
                                 * lets it go at power-on, and nobody else stops
                                 * its boot ROM. dsp_boot() releases it again. */
const char *dsp_strerror(int err);
/* After dsp_boot(): records, idle frames, ping and memory dump, all bounded;
 * saves /b310e-os/dsp-report.txt, dsp-share.bin and dsp-dump.bin. It leaves the
 * DSP in its dump loop: call dsp_stop() next. */
int dsp_probe(void);
void dsp_selftest(void);        /* not while it runs; leaves it in reset */
void dsp_set_trace(bool on);    /* start-up marks in the program dsp_boot() sends */
const char *dsp_probe_verdict(void);

/* ---- layer 1, receive only ---------------------------------------------- */

/* After dsp_boot(): share memory cleared, the 0x8181 start marker, the
 * DSP image's parameter records sent (the stock layer-1 start order). */
int l1_start(void);

/* What the stock's DSP interrupts do, by polling: the response ring drained,
 * a control-block message taken and acknowledged (the DSP waits for the ack),
 * the status bits cleared. Call it while layer 1 runs, every frame or so. */
void l1_poll(void);

/* This phone's IMEI from its fixed NV in NOR, read only: 15 digits and a NUL,
 * or false (and "") when it is not there or its check digit is wrong. */
bool nv_imei(char out[16]);

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
int l1_sleep_post(int frames);  /* one multi-frame sleep, below 1000 frames */
void l1_ask_more(int back);     /* flag the instruction this many slots back */
int l1_run(void);               /* after l1_start(): start the frame clock */
int l1_fi_free(void);           /* free frame-instruction slots */

typedef struct {
    uint16_t index;             /* arfcn index */
    uint16_t level;             /* bits 11:4 rxlev (0..110), 1/16 steps */
    uint16_t aux;               /* meaning unknown (docs/gsm-plan.md) */
    uint16_t status;            /* 1 measured, 2 / 3 saturated (hi / lo?) */
} l1_pm_t;

int l1_pm_read(l1_pm_t *out, int max);  /* results waiting, oldest first */

/* Sync, receive only (docs/gsm-plan.md, "Next stage"). Type 9: an FCCH
 * search over window frames (26 before camping, 13 camped); type 10: one SCH
 * read. cell: the cell-table index the result is tagged with. */
int l1_fcch_post(uint16_t index, uint8_t gain, int window, int cell);
int l1_sch_post(uint16_t index, uint8_t gain, int cell);

/* Type 2: one burst (0..3) of a 4-burst block. tag: the stock's RxChannelTag
 * (7 came back as a BCCH/CCCH block, 9 as a paging one); tn: the timeslot
 * (0 for broadcast channels); tsc: the training sequence (their BCC). */
int l1_burst_post(uint16_t index, int tag, int burst, int tn, int tsc, int cell);

/* The gain setting every type-2 read of DSP cell `cell` runs at (BCCH, CCCH,
 * SDCCH downlink). The stock reads a cell at its AGC setting, the measured
 * rxlev / 2 (FUN_0005cafe); l1_cell_sync() records the one it synced with.
 * Unset (after l1_start()): the stock's starting 25. */
void l1_cell_gain_set(int cell, uint8_t gain);

/* The frame number the next posted instruction carries (our own count:
 * received blocks report it, so cell FN = it + the offset an SCH gives). */
uint32_t l1_next_fn(void);

/* One received block (+0x40c ring). hdr[0] type (1 SCH, 7 CCCH/BCCH block,
 * 9 ...), hdr[1] bfi (0 good), hdr[2] frame number low, hdr[3] bits 7:0
 * frame number high and 12:8 the cell; hdr[4..8] partly unknown. data: SCH
 * 4 bytes, blocks 23. */
typedef struct {
    uint16_t hdr[9];
    uint8_t data[54];
} l1_rx_t;

int l1_rx_read(l1_rx_t *rx);            /* 1 one block, 0 none, or an error */

/* For experiments (os/modules/radiolab): any frame instruction, the next
 * frame number written into word 1 bits 15:9 and word 2; share memory
 * (offsets below 0x1000) and commands, raw. */
int l1_fi_post(const uint16_t fi[11], const uint16_t ext[3]);
uint16_t l1_share_r16(uint32_t off);
void l1_share_w16(uint32_t off, uint16_t v);
int l1_cmd_post(const uint16_t cmd[5]);

/* 2.15: the transmit minimum-power test (docs/gsm-plan.md "TX power"). The clamp
 * must be armed before l1_start(); the burst poster refuses without it. */
#define L1_APC_BANDS 4
#define L1_APC_N     18
/* TX power interlock modes: OFF refuses all transmit (the safe default), MIN
 * clamps the APC tables to the calibrated minimum, NORMAL sends them as
 * calibrated, MAX pins every setpoint to the calibrated maximum (full power, for
 * a weak-link registration). Set before l1_start(). */
enum { L1_TX_OFF, L1_TX_MIN, L1_TX_NORMAL, L1_TX_MAX };
void l1_tx_min_power(int on);           /* compat: on -> MIN, off -> OFF */
int l1_tx_min_power_on(void);           /* is the MIN clamp armed? */
void l1_tx_power_set(int mode);         /* L1_TX_* */
int l1_tx_power_mode(void);
int l1_tx_band_caps(uint8_t out[4]);    /* NV per-band TX enable: 900,1800,1900,850 */
int l1_cal_readback(uint16_t out[L1_APC_BANDS][L1_APC_N]);
/* bsic: the target cell's BSIC (l1_cell_t.bsic, from its SCH). An access burst's
 * parity is XORed with it (TS 45.003 4.6), so a wrong one is dropped by the BTS.
 * The stock takes it from its cell table's +32 byte; this was taken for an RF-cal
 * byte and sent as 0 until 2026-11-10. */
int l1_access_post(uint16_t index, int slot, uint8_t bsic, uint8_t ra);
/* Word 7 bits 15:10 of the access frame: the stock's 0x3f (its skeleton's 0xffff
 * kept); 0 before 2026-11-10. A test knob (shell txburst w7=) while what the field
 * does is measured on the SDR. */
extern uint8_t l1_access_w7hi;
/* The TX power control level for the bursts that follow (GSM PCL: on 900, 5 =
 * 33 dBm .. 19 = 5 dBm), the stock's way: share +0x3ec double-buffered, flagged
 * in the next TX frame's word 5. Before a RACH: MS_TXPWR_MAX_CCH, not above the
 * phone's class (PCL 5). The APC tables (l1_tx_power_set) still bound it. */
void l1_tx_level_set(uint8_t pcl);
/* The uplink timing advance (0..63) from the grant; apply after the immediate
 * assignment, before the SDCCH uplink. See l1_tx_ta_set in dsp.c. */
void l1_tx_ta_set(uint8_t ta);
/* Uplink normal-burst TX (osmocom DATA_REQ). RE-faithful, not HW-verified - needs
 * a network-assigned dedicated channel (gated). See docs/l1-osmocom.md. bsic: the
 * serving cell's, as for l1_access_post (the stock's FUN_00050426 sends it too). */
/* Frame-synced uplink (l1_dm_sched): post the burst FI ahead, feed the TXBUFQ
 * descriptors just-in-time. See dsp.c. */
int txbuf_free(void);                   /* free TXBUFQ slots (uplink pacing) */
/* Enable A5 ciphering: Kc, algorithm (0=A5/1, 0xff=off), and the synced cell's
 * fn_offset so the DSP ciphers with the cell's absolute GSM frame number. */
void l1_cipher_set(const uint8_t kc[8], uint8_t alg, uint32_t fn_offset);
int l1_uplink_fi(uint16_t index, int tag, int burst, int tn, int tsc, int cell);
int l1_uplink_desc(int tag, int burst, uint8_t bsic, const uint8_t block[23]);
int l1_uplink_post(uint16_t index, int tag, int burst, int tn, int tsc, int cell,
                   uint8_t bsic, const uint8_t block[23]);
/* DM_EST (dedicated-mode establish): the command-3 piece only. Needs the L1
 * state-machine / RF-control layer + scheduler to work. See docs/l1-osmocom.md. */
int l1_dm_est(uint16_t sel, uint8_t gain, uint16_t aux);
/* Camp on DSP cell `cell` the stock's way (command 3, tracking on): its bursts
 * steer the AFC, so the uplink follows the cell's frequency. rxlev: the cell's
 * level (0..110), which the DSP takes the tracked cell's gain from. */
int l1_cell_track(int cell, uint8_t rxlev);
extern uint8_t l1_track_mode;           /* command-3 mode word (test) */
extern uint16_t l1_track_flags;         /* command-3 flags word (test) */

/* Layer 1 for loadable apps (os_api_t.l1, API 2.11). Members only append. */
typedef struct l1_api {
    int (*dsp_boot)(void);
    void (*dsp_stop)(void);
    int (*start)(void);
    int (*run)(void);
    void (*poll)(void);
    int (*fi_free)(void);
    int (*idle_post)(int frames);
    void (*ask_more)(int back);
    uint16_t (*arfcn_index)(int band, int arfcn);
    int (*pm_post)(const uint16_t *index, const uint8_t *gain, int n);
    int (*pm_read)(l1_pm_t *out, int max);
    int (*fcch_post)(uint16_t index, uint8_t gain, int window, int cell);
    int (*sch_post)(uint16_t index, uint8_t gain, int cell);
    int (*burst_post)(uint16_t index, int tag, int burst, int tn, int tsc, int cell);
    uint32_t (*next_fn)(void);
    int (*rx_read)(l1_rx_t *rx);
    int (*fi_post)(const uint16_t fi[11], const uint16_t ext[3]);
    uint16_t (*share_r16)(uint32_t off);
    void (*share_w16)(uint32_t off, uint16_t v);
    int (*cmd_post)(const uint16_t cmd[5]);
    volatile dsp_info_t *info;
    volatile dsp_probe_t *probe;
    /* 2.12: a cell (os/l1cell.h) */
    int (*si_parse)(l1_cell_t *c, const uint8_t d[23]);
    int (*cell_sync)(l1_cell_t *c, uint16_t index, uint8_t gain, int slot, uint32_t budget_ms);
    int (*cell_read)(l1_cell_t *c, int max_blocks, uint32_t want, uint32_t budget_ms);
    void (*cell_idle)(uint32_t ms);
    int (*rxlev_measure)(uint16_t index, int samples, uint32_t budget_ms);
    int (*cell_c1)(const l1_cell_t *c, int rxlev, bool dcs);
    int (*cell_select)(const uint16_t *index, int n, uint16_t mcc, uint16_t mnc, l1_cell_t *cells);
    int (*cell_stay)(l1_cell_t *c, uint32_t ms, l1_stay_t *st);
    /* 2.13: the common control channel */
    int (*cch_classify)(const uint8_t d[23]);
    int (*cch_read)(l1_cell_t *c, uint32_t ms, l1_cch_t *st);
    int (*dm_sched)(l1_cell_t *c, int subch, int tn, int tsc, uint8_t cal,
                    uint32_t ms, const uint8_t *ul, l1_dm_t *st);  /* SDCCH pump */
    /* 2.15: the minimum-power transmit test */
    void (*tx_min_power)(int on);
    int (*tx_min_power_on)(void);
    int (*cal_readback)(uint16_t out[L1_APC_BANDS][L1_APC_N]);
    int (*access_post)(uint16_t index, int slot, uint8_t cal, uint8_t ra);  /* RACH_REQ */
    int (*uplink_post)(uint16_t index, int tag, int burst, int tn, int tsc, int cell,
                       uint8_t cal, const uint8_t block[23]);  /* DATA_REQ uplink */
    int (*dm_est)(uint16_t sel, uint8_t gain, uint16_t aux);  /* DM_EST (cmd-3 piece) */
} l1_api_t;

extern const l1_api_t l1_api;

#endif
