/* SC6530C SIM card reader (sim-sc6530c.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SIM_TARGET_H
#define SIM_TARGET_H

#include <stdbool.h>
#include <stdint.h>

enum sim_status {
    SIM_NO_CARD,        /* no ATR at 1.8 V or 3 V */
    SIM_BAD_ATR,        /* bytes arrived but no valid TS */
    SIM_ATR_OK,         /* ATR read, file reads failed */
    SIM_ICCID_OK,
    SIM_IMSI_OK,
};

struct sim_info {
    int slot;
    enum sim_status status;
    bool three_volt;    /* answered only at 3 V */
    bool pin_required;  /* IMSI read refused: CHV1 (PIN) is enabled */
    uint8_t atr[33];
    int atr_len;
    unsigned sw_select, sw_iccid, sw_imsi;
    char iccid[21];
    char imsi[16];
};

/* Power the slot (0 or 1), read ATR/ICCID/IMSI, power it down again.
 * Takes up to a few seconds; thread context only. */
void sim_read(int slot, struct sim_info *info);

#endif
