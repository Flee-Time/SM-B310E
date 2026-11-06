/* B310E OS — USB mass storage: Bulk-Only Transport and SCSI on the SD card.
 *
 * One LUN, 512-byte blocks straight from sd_read()/sd_write(). Commands:
 * TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SENSE(6) and (10), PREVENT
 * ALLOW MEDIUM REMOVAL, START STOP UNIT (eject), READ FORMAT CAPACITIES,
 * READ CAPACITY(10), READ(10), WRITE(10), SYNCHRONIZE CACHE(10); anything
 * else fails with ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE.
 *
 * Reads and writes move up to 32 blocks (16 KB, one SD command) per chunk
 * through two buffers: the card reads the next chunk while the interrupt
 * handler sends the last one, and writes one while the next arrives (the
 * Rockbox port's double buffering, which reached 710 KB/s on the phone).
 * A chunk reaches the card only once all of it has arrived.
 *
 * Data the host expects but does not get ends with a short packet or a
 * halted endpoint (BOT 6.7), so the host always reads the CSW next.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/usb.h"
#include "drivers/usb_internal.h"

#define CHUNK_BLOCKS  32
#define CHUNK_BYTES   (CHUNK_BLOCKS * 512)

#define CBW_SIG       0x43425355u       /* "USBC" */
#define CSW_SIG       0x53425355u       /* "USBS" */

enum { ST_GOOD = 0, ST_FAIL = 1, ST_PHASE = 2, ST_ABORT = -1 };

/* sense keys and additional sense codes */
#define SK_NOT_READY        0x02
#define SK_MEDIUM_ERROR     0x03
#define SK_ILLEGAL_REQUEST  0x05
#define ASC_WRITE_ERROR     0x0c
#define ASC_READ_ERROR      0x11
#define ASC_INVALID_OPCODE  0x20
#define ASC_LBA_RANGE       0x21
#define ASC_INVALID_FIELD   0x24
#define ASC_LUN             0x25
#define ASC_NO_MEDIUM       0x3a

typedef struct {
    uint32_t gen, tag, host_len, residue;
    bool host_in;               /* the host's data direction */
    bool data_ended;            /* a short IN packet ended the data stage */
    const uint8_t *cb;
} cmd_t;

static uint8_t *buf[2];
static uint32_t cbw[16];        /* 64 bytes: a full packet is caught */
static uint32_t csw[4];
static uint32_t reply[16];
static volatile struct { uint8_t key, asc, ascq; } sense;
static bool ejected;

bool usb_msc_init(void)
{
    if (!buf[0])
        buf[0] = kmalloc(CHUNK_BYTES);
    if (!buf[1])
        buf[1] = kmalloc(CHUNK_BYTES);
    if (buf[0] && buf[1])
        return true;
    usb_msc_exit();
    return false;
}

void usb_msc_exit(void)
{
    kfree(buf[0]);
    kfree(buf[1]);
    buf[0] = buf[1] = NULL;
}

void usb_msc_reset(void)
{
    sense.key = sense.asc = sense.ascq = 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int fail(uint8_t key, uint8_t asc)
{
    sense.key = key;
    sense.asc = asc;
    sense.ascq = 0;
    return ST_FAIL;
}

static bool ready(void)
{
    return !ejected && sd_present();
}

static void activity(int what, uint32_t bytes)
{
    usb_stat.activity = (uint32_t)what;
    usb_stat.last_data_ms = kernel_ticks();
    if (what == USB_READING)
        usb_stat.bytes_read += bytes;
    else
        usb_stat.bytes_written += bytes;
}

/* a small reply: at most what the host asked for (and the allocation
 * length); a short packet ends the stage, or the endpoint is halted */
static int reply_in(cmd_t *c, const void *data, uint32_t len, uint32_t alloc)
{
    uint32_t n = len < alloc ? len : alloc;

    if (!c->host_len)
        return ST_GOOD;
    if (!c->host_in)
        return ST_PHASE;
    if (n > c->host_len)
        n = c->host_len;
    if (n) {
        if (usb_bulk_start_in(c->gen, data, (int)n) < 0 || usb_bulk_wait_in() < 0)
            return ST_ABORT;
    }
    c->residue = c->host_len - n;
    c->data_ended = n % USB_PKT != 0;
    return ST_GOOD;
}

static int cmd_read(cmd_t *c, uint32_t lba, uint32_t count)
{
    uint32_t bytes = count * 512, sent = 0;
    unsigned n, m, cur = 0;

    if (!ready())
        return fail(SK_NOT_READY, ASC_NO_MEDIUM);
    if (lba >= sd_sectors() || count > sd_sectors() - lba)
        return fail(SK_ILLEGAL_REQUEST, ASC_LBA_RANGE);
    if (!count)
        return ST_GOOD;
    if (!c->host_in || bytes > c->host_len)
        return ST_PHASE;
    n = count < CHUNK_BLOCKS ? count : CHUNK_BLOCKS;
    if (sd_read(lba, buf[0], n)) {
        usb_stat.sd_errors++;
        return fail(SK_MEDIUM_ERROR, ASC_READ_ERROR);
    }
    for (;;) {
        int rd = 0;

        if (usb_bulk_start_in(c->gen, buf[cur], (int)(n * 512)) < 0)
            return ST_ABORT;
        count -= n;
        lba += n;
        m = count < CHUNK_BLOCKS ? count : CHUNK_BLOCKS;
        if (m)                          /* while the interrupt sends */
            rd = sd_read(lba, buf[cur ^ 1], m);
        if (usb_bulk_wait_in() < 0)
            return ST_ABORT;
        sent += n * 512;
        c->residue = c->host_len - sent;
        activity(USB_READING, n * 512);
        if (!m)
            return ST_GOOD;
        if (rd) {
            usb_stat.sd_errors++;
            return fail(SK_MEDIUM_ERROR, ASC_READ_ERROR);
        }
        cur ^= 1;
        n = m;
    }
}

static int cmd_write(cmd_t *c, uint32_t lba, uint32_t count)
{
    uint32_t bytes = count * 512, got = 0;
    unsigned n, m, cur = 0;
    bool err = false;

    if (!ready())
        return fail(SK_NOT_READY, ASC_NO_MEDIUM);
    if (lba >= sd_sectors() || count > sd_sectors() - lba)
        return fail(SK_ILLEGAL_REQUEST, ASC_LBA_RANGE);
    if (!count)
        return ST_GOOD;
    if (c->host_in || bytes > c->host_len)
        return ST_PHASE;
    n = count < CHUNK_BLOCKS ? count : CHUNK_BLOCKS;
    if (usb_bulk_start_out(c->gen, buf[0], (int)(n * 512)) < 0)
        return ST_ABORT;
    for (;;) {
        int r = usb_bulk_wait_out();

        if (r < 0)
            return ST_ABORT;
        got += (uint32_t)r;
        c->residue = c->host_len - got;
        if ((uint32_t)r != n * 512) {   /* the host stopped early */
            c->data_ended = true;
            return ST_PHASE;
        }
        count -= n;
        m = count < CHUNK_BLOCKS ? count : CHUNK_BLOCKS;
        if (m && usb_bulk_start_out(c->gen, buf[cur ^ 1], (int)(m * 512)) < 0)
            return ST_ABORT;
        if (!err) {                     /* while the next chunk arrives */
            if (sd_write(lba, buf[cur], n)) {
                err = true;
                usb_stat.sd_errors++;
            } else {
                activity(USB_WRITING, n * 512);
            }
        }
        lba += n;
        if (!m)
            break;
        cur ^= 1;
        n = m;
    }
    return err ? fail(SK_MEDIUM_ERROR, ASC_WRITE_ERROR) : ST_GOOD;
}

static int execute(cmd_t *c)
{
    const uint8_t *cb = c->cb;
    uint8_t *r = (uint8_t *)reply;
    uint32_t blocks = sd_sectors();

    switch (cb[0]) {
    case 0x00:                          /* TEST UNIT READY */
        return ready() ? ST_GOOD : fail(SK_NOT_READY, ASC_NO_MEDIUM);

    case 0x03: {                        /* REQUEST SENSE */
        int st;

        memset(r, 0, 18);
        r[0] = 0x70;                    /* current, fixed format */
        r[2] = sense.key;
        r[7] = 10;
        r[12] = sense.asc;
        r[13] = sense.ascq;
        st = reply_in(c, r, 18, cb[4]);
        if (st == ST_GOOD)
            usb_msc_reset();
        return st;
    }

    case 0x12:                          /* INQUIRY */
        if (cb[1] & 1)                  /* no vital product data pages */
            return fail(SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD);
        memset(r, 0, 36);
        r[0] = 0x00;                    /* direct access block device */
        r[1] = 0x80;                    /* removable */
        r[2] = 0x02;
        r[3] = 0x02;
        r[4] = 31;
        memcpy(r + 8, "B310E   ", 8);
        memcpy(r + 16, "SD card         ", 16);
        memcpy(r + 32, "0.3 ", 4);
        return reply_in(c, r, 36, (uint32_t)cb[3] << 8 | cb[4]);

    case 0x1a:                          /* MODE SENSE(6) */
    case 0x5a: {                        /* MODE SENSE(10) */
        unsigned page = cb[2] & 0x3f;

        if (page != 0x3f && page != 0)  /* the header only, no pages */
            return fail(SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD);
        memset(r, 0, 8);
        if (cb[0] == 0x1a) {
            r[0] = 3;                   /* length; medium 0, not protected */
            return reply_in(c, r, 4, cb[4]);
        }
        r[1] = 6;
        return reply_in(c, r, 8, (uint32_t)cb[7] << 8 | cb[8]);
    }

    case 0x1e:                          /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x35:                          /* SYNCHRONIZE CACHE(10): writes are synchronous */
        return ST_GOOD;

    case 0x1b:                          /* START STOP UNIT */
        if (cb[4] & 2) {                /* load/eject */
            ejected = !(cb[4] & 1);
            usb_stat.ejected = ejected;
        }
        return ST_GOOD;

    case 0x23:                          /* READ FORMAT CAPACITIES */
        if (!ready())
            return fail(SK_NOT_READY, ASC_NO_MEDIUM);
        memset(r, 0, 12);
        r[3] = 8;                       /* one capacity descriptor */
        put_be32(r + 4, blocks);
        r[8] = 0x02;                    /* formatted media */
        r[10] = 0x02;                   /* 512-byte blocks */
        return reply_in(c, r, 12, (uint32_t)cb[7] << 8 | cb[8]);

    case 0x25:                          /* READ CAPACITY(10) */
        if (!ready())
            return fail(SK_NOT_READY, ASC_NO_MEDIUM);
        put_be32(r, blocks - 1);
        put_be32(r + 4, 512);
        return reply_in(c, r, 8, 8);

    case 0x28:                          /* READ(10) */
        return cmd_read(c, be32(cb + 2), (uint32_t)cb[7] << 8 | cb[8]);

    case 0x2a:                          /* WRITE(10) */
        return cmd_write(c, be32(cb + 2), (uint32_t)cb[7] << 8 | cb[8]);
    }
    return fail(SK_ILLEGAL_REQUEST, ASC_INVALID_OPCODE);
}

void usb_msc_run(void)
{
    const uint8_t *b = (const uint8_t *)cbw;
    uint32_t gen = 0;
    bool armed = false;

    ejected = false;
    usb_msc_reset();
    while (!usb_stopping()) {
        cmd_t c;
        int n, st;

        if (!armed) {
            if (!usb_wait_configured(&gen))
                continue;
            if (usb_bulk_start_out(gen, cbw, sizeof(cbw)) < 0)
                continue;
        }
        armed = false;
        n = usb_bulk_wait_out();
        if (n < 0 || !usb_gen_valid(gen))
            continue;
        if (n != 31 || le32(b) != CBW_SIG || b[14] < 1 || b[14] > 16) {
            /* not a valid CBW: halt both until reset recovery (BOT 6.6.1) */
            usb_bulk_halt(true);
            usb_bulk_halt(false);
            usb_wait_unhalted(gen);
            continue;
        }
        c.gen = gen;
        c.tag = le32(b + 4);
        c.host_len = c.residue = le32(b + 8);
        c.host_in = (b[12] & 0x80) != 0;
        c.data_ended = false;
        c.cb = b + 15;
        usb_stat.commands++;
        st = (b[13] & 0x0f) ? fail(SK_ILLEGAL_REQUEST, ASC_LUN) : execute(&c);
        if (st == ST_ABORT)
            continue;
        if (st != ST_GOOD)
            usb_stat.failed++;
        /* data the host still expects (or still sends): halt the pipe; the
         * host clears it and reads the CSW (BOT 6.7.2 / 6.7.3) */
        if (c.residue && !c.data_ended)
            usb_bulk_halt(c.host_in);
        csw[0] = CSW_SIG;
        csw[1] = c.tag;
        csw[2] = c.residue;
        csw[3] = (uint32_t)st;
        if (usb_bulk_start_in(gen, csw, 13) < 0)
            continue;
        /* the next CBW may arrive as soon as the CSW has gone */
        armed = usb_bulk_start_out(gen, cbw, sizeof(cbw)) == 0;
        if (usb_bulk_wait_in() < 0)
            continue;
    }
}
