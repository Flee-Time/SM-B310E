/* B310E OS - GSM Mobility Management (04.08), MS side: the location-updating
 * (registration) procedure. The key outgoing message is LOCATION UPDATING
 * REQUEST, which rides in the SABM contention resolution on the SDCCH; the
 * network answers with ACCEPT or REJECT, possibly after AUTHENTICATION,
 * IDENTITY and TMSI-reallocation exchanges.
 *
 * Pure stdint encode/parse, no OS deps, host-unit-testable. GSM 04.08 layouts;
 * confirmed end-to-end on the live network at registration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_GSM_MM_H
#define OS_GSM_MM_H

#include <stdint.h>

#define MM_PD                   0x05    /* protocol discriminator: MM */

/* MM message types (octet after PD; the low 6 bits are the type) */
#define MM_IMSI_DETACH          0x01
#define MM_LU_ACCEPT            0x02
#define MM_LU_REJECT            0x04
#define MM_LU_REQUEST           0x08
#define MM_AUTH_REJECT          0x11
#define MM_AUTH_REQUEST         0x12
#define MM_AUTH_RESPONSE        0x14
#define MM_IDENTITY_REQUEST     0x18
#define MM_IDENTITY_RESPONSE    0x19
#define MM_TMSI_REALLOC_CMD     0x1a
#define MM_TMSI_REALLOC_COMPL   0x1b
#define MM_CM_SERVICE_REQUEST   0x24

/* Location updating type (octet 3, low nibble) */
#define MM_LU_TYPE_NORMAL       0
#define MM_LU_TYPE_PERIODIC     1
#define MM_LU_TYPE_IMSI_ATTACH  2

/* Mobile identity type (10.5.1.4) */
#define MI_TYPE_NONE            0
#define MI_TYPE_IMSI            1
#define MI_TYPE_IMEI            2
#define MI_TYPE_IMEISV          3
#define MI_TYPE_TMSI            4

/* ---- encoders (return the number of octets written) ---- */
/* Mobile identity from BCD digits (IMSI; type MI_TYPE_IMSI/IMEI). digits are 0-9. */
int mm_encode_mi_digits(uint8_t *out, int type, const uint8_t *digits, int n);
/* Mobile identity for a 32-bit TMSI (5 octets). */
int mm_encode_mi_tmsi(uint8_t *out, uint32_t tmsi);
/* Location Area Identification (5 octets). mnc3=1 for a 3-digit MNC. */
void mm_encode_lai(uint8_t out[5], uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac);
/* LOCATION UPDATING REQUEST into out (<= 20 for an SDCCH SABM). Returns length. */
int mm_encode_lu_request(uint8_t *out, int lu_type, int cksn,
                         uint16_t mcc, uint16_t mnc, int mnc3, uint16_t lac,
                         uint8_t classmark1, const uint8_t *imsi_digits, int n);

/* ---- parsing ---- */
/* MM message type of an L3 message (its PD must be MM), or -1. */
int mm_msg_type(const uint8_t *l3, int len);
/* LOCATION UPDATING REJECT -> reject cause. 0, or -1. */
int mm_parse_lu_reject(const uint8_t *l3, int len, uint8_t *cause);
/* AUTHENTICATION REQUEST -> the 16-octet RAND. 0, or -1. */
int mm_parse_auth_request(const uint8_t *l3, int len, uint8_t rand[16]);
/* AUTHENTICATION RESPONSE from the 4-octet SRES. Returns length (6). */
int mm_encode_auth_response(uint8_t *out, const uint8_t sres[4]);
/* IDENTITY RESPONSE carrying a BCD identity (IMSI/IMEI). Returns length. */
int mm_encode_identity_response(uint8_t *out, int type, const uint8_t *digits, int n);
/* TMSI REALLOCATION COMPLETE. Returns length (2). */
int mm_encode_tmsi_realloc_complete(uint8_t *out);

#define MM_SELFTEST_TOTAL 13
int mm_selftest(void);

#endif
