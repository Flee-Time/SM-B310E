/* B310E OS — USB mass storage: the SD card as a removable disk on a PC.
 *
 * usb_start() brings the SC6530C's full-speed device controller up (PIO
 * only, never DMA), connects to the host and serves the card through the
 * Bulk-Only Transport with a small SCSI command set; usb_stop() takes it
 * off the bus again. The caller owns the card in between: fs_suspend()
 * before usb_start(), fs_resume() after usb_stop(), and cpufreq_hold()
 * for as long as the controller runs (apps/usb.c does all of this).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_USB_H
#define OS_USB_H

#include <stdbool.h>
#include <stdint.h>

enum {
    USB_OFF,            /* not started, or stopped */
    USB_STARTING,       /* controller bring-up (about 300 ms) */
    USB_WAITING,        /* on the bus, no request from a host yet */
    USB_ENUMERATING,    /* the host is reading descriptors */
    USB_CONNECTED,      /* configured: mass storage is being served */
};

enum { USB_IDLE, USB_READING, USB_WRITING };

/* Counters and state, read by the UI and by tests (QEMU reads the
 * `usb_stat` symbol). Field order is the tests' contract: append only. */
typedef struct {
    uint32_t state;             /* USB_OFF .. USB_CONNECTED */
    uint32_t activity;          /* the last data command: USB_IDLE/READING/WRITING */
    uint32_t last_data_ms;      /* kernel_ticks() at its last block */
    uint32_t bytes_read;        /* card -> host, this session */
    uint32_t bytes_written;     /* host -> card, this session */
    uint32_t commands;          /* SCSI commands, this session */
    uint32_t failed;            /* commands answered with a failure */
    uint32_t setups;            /* control requests the driver saw */
    uint32_t bus_resets;        /* resets inferred from the requests */
    uint32_t irqs;
    uint32_t sessions;          /* usb_start() calls since boot */
    uint32_t ejected;           /* the host ejected the medium */
    uint32_t polled;            /* interrupt storm: line 25 masked, polled */
    uint32_t sd_errors;         /* failed card reads or writes */
} usb_stat_t;

extern volatile usb_stat_t usb_stat;

/* 0 when started (or already running); -1 without a card or memory. */
int usb_start(void);
/* Off the bus; returns when mass storage has finished with the card. */
void usb_stop(void);
bool usb_running(void);
/* VBUS: the micro-USB socket is the only power input, so the charger. */
bool usb_cable(void);

/* The "USB storage" screen (apps/usb.c). */
struct screen *app_usb(void);

/* The last mass storage commands of the session (opcode, page, direction,
 * length, result, sense) to the card log; the USB screen calls it once
 * the card is back. */
void usb_msc_trace_log(void);

#endif
