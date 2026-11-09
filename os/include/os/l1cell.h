/* B310E OS — layer 1, a cell: sync and broadcast channel, receive only.
 *
 * What the phone showed works (docs/gsm-plan.md, "BCCH decodes"): one FCCH
 * search, the SCH it returns, then the 4 bursts at cell FN mod 51 = 2..5.
 * Call between l1_run() and dsp_stop(), from one thread; these keep the frame
 * ring fed while they run, and the caller must go on feeding it between calls.
 *
 * System Information is what a cell broadcasts to every phone. Nothing else on
 * these channels is kept: si_parse() turns the rest down without storing it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_L1CELL_H
#define OS_L1CELL_H

#include <stdbool.h>
#include <stdint.h>

#define L1_CELL_ARFCNS 32               /* more are counted, not listed */

/* Bits of si_seen, by message type */
#define L1_SI1    (1u << 0x19)
#define L1_SI2    (1u << 0x1a)
#define L1_SI3    (1u << 0x1b)
#define L1_SI4    (1u << 0x1c)
#define L1_SI13   (1u << 0x00)
#define L1_SI2BIS (1u << 0x02)
#define L1_SI2TER (1u << 0x03)
#define L1_SI2QUATER (1u << 0x07)
#define L1_SI_IDLE (L1_SI1 | L1_SI2 | L1_SI3 | L1_SI4)  /* enough to select a cell */

/* The channel-description format (TS 44.018 10.5.2.1b). Only bit map 0 is
 * decoded; the range formats are a tree encoding whose decode has no reference
 * to check here and no cell on this network to try it on, so they are named but
 * not expanded (docs/gsm-plan.md). */
enum {
    L1_LIST_BITMAP0, L1_LIST_R1024, L1_LIST_R512, L1_LIST_R256, L1_LIST_R128,
    L1_LIST_VARBITMAP, L1_LIST_UNKNOWN
};

typedef struct {
    uint16_t arfcn[L1_CELL_ARFCNS];
    uint8_t count;                      /* of the list, all if it fits and decoded */
    uint8_t format;                     /* L1_LIST_* */
    bool decoded;                       /* true: bit map 0, arfcn[] filled */
} l1_arfcn_list_t;

const char *l1_list_format(uint8_t format);     /* "bit map 0", "range 1024", ... */

typedef struct {
    /* the sync (l1_cell_sync) */
    uint16_t index;                     /* arfcn index */
    uint8_t gain;                       /* the setting: 25 to start, docs/gsm-plan.md */
    uint8_t slot;                       /* the DSP's cell number */
    bool synced;
    uint8_t bsic;                       /* NCC << 3 | BCC */
    uint32_t fn_offset;                 /* cell FN = our frame number + this, mod FN_MOD */

    /* blocks (l1_cell_read) */
    uint32_t good, bad, other;          /* other: good CRC, not System Information */

    /* System Information (si_parse) */
    uint32_t si_seen;                   /* L1_SI*: which types came */
    uint16_t mcc, mnc, lac, ci;         /* SI 3, 4; ci from SI 3 */
    uint8_t mnc_digits;
    uint8_t ccch_conf, bs_ag_blks_res, bs_pa_mfrms; /* SI 3; bs_pa_mfrms is the count */
    uint8_t t3212;                      /* decihours, 0: none */
    bool att;
    uint8_t hysteresis, ms_txpwr_max_cch, rxlev_access_min;     /* SI 3, 4 */
    bool acs, neci;
    bool barred, reestablish_off;       /* RACH control: the first decides selection */
    uint8_t max_retrans;                /* RACH control: 0..3 (1, 2, 4, 7 attempts) */
    uint8_t tx_integer;                 /* RACH control: the spreading code 0..15 */
    uint16_t ac;                        /* RACH control: EC and access classes 0..15 */
    uint8_t ncc_permitted;              /* SI 2 */
    bool ba_ind;
    l1_arfcn_list_t channels;           /* SI 1: the cell's own */
    l1_arfcn_list_t neighbours;         /* SI 2 */

    /* selection (l1_cell_select) */
    int16_t rxlev;                      /* 0..63, -1 not measured */
    int16_t c1;                         /* path loss criterion, dB */
    bool suitable;
} l1_cell_t;

/* One 23-byte block with a good CRC. The message type if it is System
 * Information it knows or skips (its bit set in si_seen), else -1 and the cell
 * unchanged. */
int si_parse(l1_cell_t *c, const uint8_t d[23]);

/* FCCH search and its SCH on one arfcn index. Clears c. slot: the DSP's cell
 * number. 1 synced, 0 no SCH in budget_ms, or a DSP_E* error. */
int l1_cell_sync(l1_cell_t *c, uint16_t index, uint8_t gain, int slot, uint32_t budget_ms);

/* Up to max_blocks BCCH blocks of a synced cell, parsed, until want's types have
 * come (0: all the blocks); 8 blocks cover one round of the cycle. The number of
 * good blocks, or a DSP_E* error. */
int l1_cell_read(l1_cell_t *c, int max_blocks, uint32_t want, uint32_t budget_ms);

/* Idle frames for ms, the rings looked after: between the calls above. */
void l1_cell_idle(uint32_t ms);

/* The level of one channel: type 11 frames with the stock's gain rule, averaged.
 * RXLEV 0..63, or a DSP_E* error. */
int l1_rxlev_measure(uint16_t index, int samples, uint32_t budget_ms);

/* C1 (TS 45.008 6.4), from its SI 3 or SI 4: (rxlev - RXLEV_ACCESS_MIN)
 * - max(MS_TXPWR_MAX_CCH - P, 0), P being this phone's maximum. dcs: the 1800 band's
 * power table and P. */
int l1_cell_c1(const l1_cell_t *c, int rxlev, bool dcs);

/* Each candidate measured, synced and read into cells[i], rxlev, c1 and suitable
 * set: System Information seen, not barred, C1 above 0, and mcc and mnc matching
 * unless mcc is 0. The index in cells of the suitable one with the highest C1, or
 * -1. Only what the caller names is looked at. */
int l1_cell_select(const uint16_t *index, int n, uint16_t mcc, uint16_t mnc, l1_cell_t *cells);

/* Staying on a synced cell for ms: BCCH blocks and a level now and then, one more
 * search when blocks stop coming good. */
typedef struct {
    uint32_t good, bad;                 /* blocks in this call */
    uint32_t resyncs;                   /* searches because blocks went bad */
    uint32_t slips;                     /* a search gave another frame offset */
    uint32_t measures;
    int16_t rxlev_min, rxlev_max, rxlev_last;
    bool lost;                          /* no SCH when it searched again: stopped */
} l1_stay_t;

int l1_cell_stay(l1_cell_t *c, uint32_t ms, l1_stay_t *st);

/* The common control channel (paging and access grant) of a synced cell. The
 * phone hears the network here, receive only: nothing is sent, and a paging
 * request's mobile identities (other phones') are never read or stored. Blocks
 * are counted by kind. */
typedef struct {
    uint32_t blocks;                    /* good-CRC CCCH blocks */
    uint32_t bad;                       /* CRC failures */
    uint32_t paging;                    /* paging request type 1, 2, 3 */
    uint32_t imm_assign;                /* immediate assignment, extended or reject */
    uint32_t filler;                    /* L2 fill / null frames: no one is being called */
    uint32_t other;                     /* another RR message type */
    uint8_t last_type;                  /* the last message type seen, 0 none */
    uint8_t last_other;                 /* the last type counted as "other", 0 none */
} l1_cch_t;

/* The RR message type of a good CCCH block, or -1 for a fill/null frame. It reads
 * the type byte only; the identities in a paging request are not looked at. */
int l1_cch_classify(const uint8_t d[23]);

/* CCCH blocks of a synced cell for ms, counted into st. 0, or a DSP_E* error. */
int l1_cch_read(l1_cell_t *c, uint32_t ms, l1_cch_t *st);

/* Dedicated channel (SDCCH/8) traffic for ms: downlink blocks received and
 * uplink bursts sent. The osmocom DATA_IND/DATA_REQ pump; HW-testable only at
 * registration. */
typedef struct {
    uint32_t dl_blocks;                 /* downlink blocks received (DSP-decoded) */
    uint32_t dl_bad;                    /* CRC failures */
    uint32_t ul_bursts;                 /* uplink bursts sent */
    uint8_t last_dl[23];                /* the last good downlink block */
    int has_dl;
} l1_dm_t;
int l1_dm_sched(l1_cell_t *c, int subch, int tn, int tsc, uint8_t cal,
                uint32_t ms, const uint8_t *ul, l1_dm_t *st);

#endif
