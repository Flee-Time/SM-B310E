/* B310E OS — the development shell over TCP.
 *
 * Port NET_PORT_TELNET (23) talks to telnet clients (PuTTY, Windows
 * telnet, telnet on Linux): the phone echoes and edits the line (WILL
 * ECHO, WILL SUPPRESS-GO-AHEAD; Backspace, Ctrl-C, Ctrl-D), CR LF line
 * ends. Port NET_PORT_RAW (2323) is plain bytes for nc and scripts (no
 * echo, LF), and the only one that moves binary data: put, get, shot.
 * os/tools/b310e-shell.py speaks it.
 *
 * Every prompt is "b310e> "; a command's output comes before it. Binary
 * transfers (raw port only):
 *   put PATH SIZE   -> "READY\n", then SIZE bytes from the PC, then
 *                      "OK SIZE CRC32\n" (written to PATH.part, then
 *                      renamed) or "ERR why\n"
 *   get PATH        -> "DATA SIZE\n", SIZE bytes, "END CRC32\n"
 *   shot            -> the same, the screen's 128 x 160 RGB565 pixels
 * CRC32 is IEEE (zlib's crc32), in hex.
 *
 * peek and poke reach: the PSRAM, IRAM 0x40000000-0x4000ffff, the NOR
 * (read only) and the register blocks the OS keeps clocked (INTC,
 * timers, ADI master, GPIO, APB and AHB control, DMA, and USB while it
 * runs); anything else needs -f. A data abort is caught (probe.S) and
 * reported, but a block whose clock is off stalls the bus instead and no
 * handler runs again: the phone hangs until the battery is pulled. The
 * analog die goes through its mailbox: "adi".
 *
 * run and close are done by the UI thread (ui_call): the screens above
 * "USB network" are popped, then the app is loaded and pushed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "build_info.h"
#include "os/app.h"
#include "os/arch.h"
#include "os/drivers.h"
#include "os/dsp.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "os/sim.h"
#include "os/status.h"
#include "os/ui.h"
#include "os/usb.h"
#include "net/net_internal.h"

#define SESS_N      3
#define LINE_MAX    200
#define ARGS_MAX    8
#define PATH_MAX    160
#define XFER_BUF    4096
#define PROMPT      "b310e> "

int probe_read(uintptr_t addr, int size, uint32_t *out);
int probe_write(uintptr_t addr, int size, uint32_t value);
void usb_net_close_apps(void);          /* apps/usb.c */

typedef struct {
    bool used;
    tcp_conn_t *c;
    bool telnet;
    bool quit;                          /* the session ends */
    bool dead;                          /* the connection failed: nothing more goes out */
    sem_t exited;
    uint8_t in[256];
    int in_len, in_pos;
    int iac;                            /* telnet command parser state */
    bool cr;                            /* the last line ended with CR */
    char out[512];
    int out_len;
    char line[LINE_MAX + 1];
    char cwd[PATH_MAX];
    char path[PATH_MAX], path2[PATH_MAX];
} sess_t;

static sess_t sess[SESS_N];
static thread_t *server;
static sem_t server_exited;

/* ---- output --------------------------------------------------------------- */

static int flush(sess_t *s)
{
    int r = 0;

    if (s->out_len && !s->dead) {
        if (tcp_write(s->c, s->out, s->out_len) < 0) {
            s->quit = s->dead = true;
            r = -1;
        }
    }
    s->out_len = 0;
    return r;
}

static void put_byte(sess_t *s, char ch)
{
    if (s->out_len >= (int)sizeof(s->out) - 2)
        flush(s);
    s->out[s->out_len++] = ch;
}

/* text: LF becomes CR LF and 0xff is doubled on the telnet port */
static void out_text(sess_t *s, const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (s->telnet && p[i] == '\n')
            put_byte(s, '\r');
        else if (s->telnet && (uint8_t)p[i] == 0xff)
            put_byte(s, (char)0xff);
        put_byte(s, p[i]);
    }
}

static void out_raw(sess_t *s, const void *p, int n)
{
    flush(s);
    if (!s->dead && tcp_write(s->c, p, n) < 0)
        s->quit = s->dead = true;
}

static void say(sess_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(sess_t *s, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    out_text(s, buf, n);
}

/* ---- input ---------------------------------------------------------------- */

/* the next byte, telnet commands removed; -1 at the end, NET_EAGAIN */
static int get_byte(sess_t *s, uint32_t timeout_ms)
{
    for (;;) {
        uint8_t b;

        if (s->in_pos == s->in_len) {
            int n;

            if (s->quit)
                return -1;
            n = tcp_read(s->c, s->in, sizeof(s->in), timeout_ms);
            if (n == NET_EAGAIN)
                return NET_EAGAIN;
            if (n <= 0) {
                s->quit = true;
                return -1;
            }
            s->in_len = n;
            s->in_pos = 0;
        }
        b = s->in[s->in_pos++];
        if (!s->telnet)
            return b;
        switch (s->iac) {
        case 0:
            if (b == 255) {
                s->iac = 1;
                continue;
            }
            return b;
        case 1:                         /* after IAC */
            if (b == 255) {
                s->iac = 0;
                return 255;
            }
            s->iac = b >= 251 ? 2 : b == 250 ? 3 : 0;   /* option, SB, or done */
            continue;
        case 2:                         /* the option of WILL/WONT/DO/DONT */
            s->iac = 0;
            continue;
        case 3:                         /* inside SB, until IAC SE */
            if (b == 255)
                s->iac = 4;
            continue;
        case 4:
            s->iac = b == 240 ? 0 : 3;
            continue;
        }
    }
}

/* true when the PC sent something (taken): ends log -f and top */
static bool interrupted(sess_t *s, uint32_t wait_ms)
{
    int b = get_byte(s, wait_ms);

    return b != NET_EAGAIN;
}

/* One line, with the telnet port's echo and editing; false at the end. */
static bool read_line(sess_t *s)
{
    int n = 0;

    for (;;) {
        int b = get_byte(s, WAIT_FOREVER);

        if (b < 0)
            return false;
        if (s->cr && (b == '\n' || b == 0)) {   /* CR LF, CR NUL */
            s->cr = false;
            continue;
        }
        s->cr = false;
        if (b == '\r' || b == '\n') {
            s->cr = b == '\r';
            if (s->telnet)
                out_text(s, "\n", 1);
            s->line[n] = 0;
            return true;
        }
        if (!s->telnet) {
            if (n < LINE_MAX)
                s->line[n++] = (char)b;
            continue;
        }
        if (b == 8 || b == 127) {               /* Backspace */
            if (n) {
                n--;
                out_text(s, "\b \b", 3);
            }
        } else if (b == 3) {                    /* Ctrl-C */
            out_text(s, "^C\n" PROMPT, 3 + sizeof(PROMPT) - 1);
            n = 0;
        } else if (b == 4 && n == 0) {          /* Ctrl-D */
            strncpy(s->line, "exit", sizeof(s->line));
            out_text(s, "\n", 1);
            return true;
        } else if (b >= 32 && b < 127 && n < LINE_MAX) {
            s->line[n++] = (char)b;
            put_byte(s, (char)b);
        }
        if (s->in_pos == s->in_len)
            flush(s);                           /* the echo, now */
    }
}

static int read_exact(sess_t *s, uint8_t *buf, int n)
{
    int got = 0;

    while (got < n) {
        if (s->in_pos < s->in_len) {
            int k = s->in_len - s->in_pos < n - got ? s->in_len - s->in_pos : n - got;

            memcpy(buf + got, s->in + s->in_pos, (size_t)k);
            s->in_pos += k;
            got += k;
            continue;
        }
        int r = tcp_read(s->c, buf + got, n - got, 10000);

        if (r <= 0) {
            /* also on a timeout: what comes next is still the transfer's, and read
             * as commands it could be anything (rm, poke, reboot) */
            s->quit = true;
            return -1;
        }
        got += r;
    }
    return 0;
}

/* ---- helpers -------------------------------------------------------------- */

static bool parse_num(const char *p, uint32_t *v)
{
    uint32_t x = 0;
    int base = 10;

    if (!p || !*p)
        return false;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        if (!*p)
            return false;
    }
    for (; *p; p++) {
        int d;

        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            return false;
        x = x * (uint32_t)base + (uint32_t)d;
    }
    *v = x;
    return true;
}

static bool streq(const char *a, const char *b)
{
    return !strcmp(a, b);
}

/* PATH_MAX bytes: an absolute path from cwd and p, with . and .. resolved */
static bool resolve(sess_t *s, const char *p, char *out)
{
    char tmp[PATH_MAX * 2];
    int n = 0, o = 0;

    if (!p)
        p = "";
    if (p[0] == '/')
        n = snprintf(tmp, sizeof(tmp), "%s", p);
    else
        n = snprintf(tmp, sizeof(tmp), "%s/%s", s->cwd, p);
    if (n >= (int)sizeof(tmp))
        return false;
    out[0] = 0;
    for (char *seg = tmp; *seg; ) {
        char *end = seg;
        int len;

        while (*end && *end != '/')
            end++;
        len = (int)(end - seg);
        if (len == 0 || (len == 1 && seg[0] == '.')) {
            /* nothing */
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            out[o] = 0;
        } else {
            if (o + 1 + len >= PATH_MAX)
                return false;
            out[o++] = '/';
            memcpy(out + o, seg, (size_t)len);
            o += len;
            out[o] = 0;
        }
        seg = *end ? end + 1 : end;
    }
    if (!o) {
        out[0] = '/';
        out[1] = 0;
    }
    return true;
}

static const char *fr_text(FRESULT r)
{
    static const char *const t[] = {
        "ok", "disk error", "internal error", "card not ready", "no such file",
        "no such path", "bad name", "denied (exists, full or a non-empty folder)",
        "exists", "invalid object", "write protected", "invalid drive",
        "not mounted", "no filesystem", "aborted", "timeout", "locked",
        "out of memory", "too many open files", "invalid parameter",
    };

    return (unsigned)r < ARRAY_SIZE(t) ? t[r] : "error";
}

/* ---- commands ------------------------------------------------------------- */

typedef struct {
    const char *name, *args, *help;
    void (*fn)(sess_t *s, int argc, char **argv);
} cmd_t;

static const cmd_t cmds[];

static void c_help(sess_t *s, int argc, char **argv)
{
    say(s, "commands (numbers: decimal or 0x hex; paths from the current folder):\n");
    for (const cmd_t *c = cmds; c->name; c++)
        say(s, "  %-6s %-22s %s\n", c->name, c->args, c->help);
    say(s, "binary transfers (put, get, shot) on port %d only\n", NET_PORT_RAW);
}

/* GSM auth self-test: RUN GSM ALGORITHM on slot 0 with a fixed test RAND. Reports
 * only success/failure - never the RAND, SRES or Kc. */
static void c_simauth(sess_t *s, int argc, char **argv)
{
    sim_atr_t atr;
    uint8_t rnd[16], sres[4], kc[8];
    int rc;

    for (int i = 0; i < 16; i++)
        rnd[i] = (uint8_t)(0x10 + i);
    rc = sim_activate(0, &atr);
    if (rc != SIM_OK) {
        say(s, "sim: slot 0 activate: %s (%d)\n", sim_strerror(rc), rc);
        return;
    }
    rc = sim_run_gsm_algo(0, rnd, sres, kc);
    sim_deactivate(0);
    (void)sres;
    (void)kc;
    if (rc == SIM_OK)
        say(s, "auth OK: RUN GSM ALGORITHM returned SRES+Kc (not shown)\n");
    else
        say(s, "auth failed: %s (0x%04x)\n", sim_strerror(rc), (unsigned)rc);
}

/* Keep the frame-instruction ring fed for ms (so the DSP clock never runs dry
 * between bursts); the register app's proven feed rule. */
#define TXB_RING_FREE  25               /* l1_fi_free() with the ring empty */
#define TXB_AHEAD      8
static void txb_idle(uint32_t ms)
{
    uint32_t t0 = kernel_ticks();

    while (kernel_ticks() - t0 < ms) {
        int n = l1_fi_free() - (TXB_RING_FREE - TXB_AHEAD);

        if (n > 0 && l1_idle_post(n) == DSP_OK)
            l1_ask_more(3);
        l1_poll();
        thread_sleep(1);
    }
}

/* Transmit test: fire COUNT access bursts (~20 ms apart, a visible train) on the
 * uplink of a band/ARFCN, at the minimum power (default) or, with "norm", the
 * calibrated normal power. For watching emission on an SDR - e.g. a DCS1800 train
 * checked near 900 MHz (its half-frequency). Keys the PA: a deliberate, bounded,
 * developer test. Boots the DSP, fires, and leaves TX off again. */
static void c_txburst(sess_t *s, int argc, char **argv)
{
    static const char *modename[] = { "off", "minimum", "NORMAL", "MAX" };
    uint32_t band = 0, arfcn = 0, count = 0;
    int mode = L1_TX_MIN, ok = 0, rc, pcl = -1, camp = 0, slot = 0;
    uint16_t index;
    long ukhz;

    if (argc < 4 || !parse_num(argv[1], &band) || !parse_num(argv[2], &arfcn) ||
        !parse_num(argv[3], &count)) {
        say(s, "usage: txburst BAND ARFCN COUNT [norm|max] [pcl=N] [w7=V]  (BAND 0=GSM900"
               " 1=DCS1800; pcl: the power control level, the stock's way)\n");
        return;
    }
    l1_access_w7hi = 0x3f;               /* the stock's, unless w7= says otherwise */
    for (int i = 4; i < argc; i++) {
        uint32_t v;

        if (streq(argv[i], "norm"))
            mode = L1_TX_NORMAL;
        else if (streq(argv[i], "max"))
            mode = L1_TX_MAX;
        else if (argv[i][0] == 'w' && argv[i][1] == '7' && argv[i][2] == '=' &&
                 parse_num(argv[i] + 3, &v))
            l1_access_w7hi = (uint8_t)(v & 0x3f);
        else if (argv[i][0] == 'p' && argv[i][1] == 'c' && argv[i][2] == 'l' &&
                 argv[i][3] == '=' && parse_num(argv[i] + 4, &v) && v < 32)
            pcl = (int)v;
        else if (streq(argv[i], "camp"))
            camp = 1;
    }
    if (count > 500)
        count = 500;                    /* hard cap */
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING) {
        say(s, "txburst: the DSP is busy (Radio status / Register?)\n");
        return;
    }
    l1_tx_power_set(mode);               /* before l1_start() */
    if ((rc = dsp_boot()) != DSP_OK || (rc = l1_start()) != DSP_OK ||
        (rc = l1_run()) != DSP_OK) {
        say(s, "txburst: DSP/L1 bring-up failed: %d\n", rc);
        goto stop;
    }
    txb_idle(300);                      /* let the frame clock settle */
    index = l1_arfcn_index(band == 1 ? L1_BAND_DCS1800 : L1_BAND_GSM900, (int)arfcn);
    ukhz = band == 1 ? 1710200L + 200L * ((long)arfcn - 512) :
           890000L + 200L * ((long)arfcn - (arfcn >= 975 ? 1024L : 0L));   /* E-GSM: n - 1024 */
    if (index == L1_NO_INDEX) {
        say(s, "txburst: no arfcn index for band %lu arfcn %lu\n",
            (unsigned long)band, (unsigned long)arfcn);
        goto stop;
    }
    if (camp) {                         /* as the stock transmits: synced and tracked */
        static l1_cell_t tc;
        int rxlev = l1_rxlev_measure(index, 6, 600);
        l1_cch_t st;

        if (l1_cell_sync(&tc, index, l1_gain_for_rxlev(rxlev), 0, 2000) != 1) {
            say(s, "txburst: camp: no sync on index %u (rxlev %d)\n", index, rxlev);
            goto stop;
        }
        slot = tc.slot;
        rc = l1_cell_track(tc.slot, (uint8_t)(2 * tc.gain));
        memset(&st, 0, sizeof st);
        l1_cch_read(&tc, 1000, &st);    /* the AFC settles on the cell */
        say(s, "txburst: camped on index %u, BSIC %u, rxlev %d, command 3 %d, CCCH %lu/%lu\n",
            index, tc.bsic, rxlev, rc, (unsigned long)st.blocks, (unsigned long)st.bad);
    }
    if (pcl >= 0)
        l1_tx_level_set((uint8_t)pcl);  /* flagged in the first burst's word 5 */
    say(s, "txburst: %s power, band %lu arfcn %lu = uplink %ld kHz (half %ld kHz), "
        "index %u, word 7 bits 15:10 = 0x%02x, PCL %d; firing %lu burst(s), ~20 ms apart...\n",
        modename[mode], (unsigned long)band, (unsigned long)arfcn, ukhz, ukhz / 2, index,
        l1_access_w7hi, pcl, (unsigned long)count);
    for (uint32_t i = 0; i < count; i++) {
        if (l1_access_post(index, slot, 0, 0x3f) == DSP_OK) /* BSIC 0: no BTS takes it */
            ok++;
        txb_idle(20);                   /* space the bursts; keep the clock alive */
    }
    say(s, "txburst: fired %d of %lu on %ld kHz; watch %ld kHz for its harmonic\n",
        ok, (unsigned long)count, ukhz, ukhz / 2);
stop:
    dsp_stop();
    l1_access_w7hi = 0x3f;
    l1_tx_power_set(L1_TX_OFF);          /* restore the safe default */
}

/* NV per-band TX-enable bytes: whether each band's cal is uploaded to the DSP
 * (so whether the DSP can key that band at all). */
/* Trigger a registration attempt (the Register app worker) over the link, so
 * the attempt can be driven and iterated without the on-device UI. It TRANSMITS
 * on the live network, owner-cleared; watch the log for the result. */
void reg_trigger(void);
int reg_is_busy(void);
static void c_reg(sess_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (reg_is_busy()) {
        say(s, "reg: already running\n");
        return;
    }
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING) {
        say(s, "reg: the DSP is busy\n");
        return;
    }
    reg_trigger();
    say(s, "reg: started - watch the log (register: prog ...)\n");
}

static void c_nvband(sess_t *s, int argc, char **argv)
{
    static const char *bn[4] = { "GSM900", "DCS1800", "PCS1900", "GSM850" };
    uint8_t caps[4];

    if (l1_tx_band_caps(caps) != 0) {
        say(s, "nvband: NV class item (3) not found\n");
        return;
    }
    say(s, "NV per-band TX enable (item 3):\n");
    for (int i = 0; i < 4; i++)
        say(s, "  %-8s = 0x%02x (%s)\n", bn[i], caps[i], caps[i] ? "ENABLED" : "disabled");
}

/* Receive-only: sync a band/ARFCN (FCCH + SCH) and read its System Information,
 * to prove the band's RX path and the arfcn-index->band mapping. No transmit. */
static void c_rxcell(sess_t *s, int argc, char **argv)
{
    uint32_t band = 0, arfcn = 0, gain = 0, cch_s = 0, track = 0;
    l1_cell_t *c;
    uint16_t index;
    int rc, synced, rxlev = -1;

    if (argc < 3 || !parse_num(argv[1], &band) || !parse_num(argv[2], &arfcn)) {
        say(s, "usage: rxcell BAND ARFCN [GAIN] [CCH_S]  (BAND 0=900 1=1800; GAIN 1-63,"
               " 0 = the stock rule rxlev/2 (default); CCH_S: seconds of CCCH to count)\n");
        return;
    }
    if (argc > 3)
        parse_num(argv[3], &gain);
    if (argc > 4)
        parse_num(argv[4], &cch_s);
    if (argc > 5)
        parse_num(argv[5], &track);
    if (cch_s > 30)
        cch_s = 30;
    if (dsp_info.state == DSP_BOOTING || dsp_info.state == DSP_RUNNING) {
        say(s, "rxcell: the DSP is busy\n");
        return;
    }
    if (!(c = kmalloc(sizeof *c))) {
        say(s, "rxcell: out of memory\n");
        return;
    }
    l1_tx_power_set(L1_TX_OFF);          /* receive only */
    if ((rc = dsp_boot()) != DSP_OK || (rc = l1_start()) != DSP_OK ||
        (rc = l1_run()) != DSP_OK) {
        say(s, "rxcell: DSP/L1 bring-up failed: %d\n", rc);
        goto stop;
    }
    txb_idle(300);
    index = l1_arfcn_index(band == 1 ? L1_BAND_DCS1800 : L1_BAND_GSM900, (int)arfcn);
    if (index == L1_NO_INDEX) {
        say(s, "rxcell: no index for band %lu arfcn %lu\n",
            (unsigned long)band, (unsigned long)arfcn);
        goto stop;
    }
    rxlev = l1_rxlev_measure(index, 6, 600);
    if (!gain)
        gain = l1_gain_for_rxlev(rxlev);
    {
        uint32_t ts = kernel_ticks();

        synced = l1_cell_sync(c, index, (uint8_t)gain, 0, 2000);
        say(s, "rxcell: band %lu arfcn %lu index %u rxlev %d (%d dBm) gain %lu -> sync %d"
            " in %lu ms\n", (unsigned long)band, (unsigned long)arfcn, index, rxlev,
            rxlev - 110, (unsigned long)gain, synced, (unsigned long)(kernel_ticks() - ts));
    }
    if (synced == 1) {
        say(s, "  SCH ok: BSIC %u (NCC %u BCC %u), frame offset %lu\n", c->bsic,
            c->bsic >> 3, c->bsic & 7, (unsigned long)c->fn_offset);
        l1_cell_read(c, 24, L1_SI3 | L1_SI4, 3000);
        say(s, "  SI 0x%08lx, MCC %u MNC %0*u, LAC %u, cell %u; %lu good / %lu bad blocks\n",
            (unsigned long)c->si_seen, c->mcc, (int)c->mnc_digits, c->mnc, c->lac,
            c->ci, (unsigned long)c->good, (unsigned long)c->bad);
        /* receive only: the channel the grant comes on; with TRACK, again after
         * command 3 (camping: the AFC follows the cell), to compare */
        for (int pass = 0; cch_s && pass < (track ? 2 : 1); pass++) {
            l1_cch_t st;
            int32_t h7;
            uint32_t n7;

            if (pass == 1)
                say(s, "  command 3 (track, rxlev %u): %d\n", 2 * c->gain,
                    l1_cell_track(c->slot, (uint8_t)(2 * c->gain)));
            memset(&st, 0, sizeof st);
            l1_cch_hdr7(&h7, &n7);      /* reset */
            rc = l1_cch_read(c, cch_s * 1000u, &st);
            l1_cch_hdr7(&h7, &n7);
            say(s, "  CCCH %lus%s: %lu good / %lu bad, %lu grants, %lu paging, hdr+0e avg %ld"
                " (rc %d)\n", (unsigned long)cch_s, pass ? " tracked" : "",
                (unsigned long)st.blocks, (unsigned long)st.bad,
                (unsigned long)st.imm_assign, (unsigned long)st.paging,
                n7 ? (long)(h7 / (int32_t)n7) : 0L, rc);
        }
    }
stop:
    dsp_stop();
    kfree(c);
}

static void c_info(sess_t *s, int argc, char **argv)
{
    uint32_t t = kernel_ticks(), idle = kernel_idle_permille(), load = idle < 1000 ? 1000 - idle : 0;
    const fs_info_t *fi = fs_info();

    say(s, "B310E OS, built " BUILD_DATE "\n");
    say(s, "up %lu.%03lu s, CPU %d MHz, load %lu.%lu%%, %lu interrupts\n",
        (unsigned long)(t / 1000), (unsigned long)(t % 1000), clock_mhz(),
        (unsigned long)(load / 10), (unsigned long)(load % 10), (unsigned long)irq_count);
    say(s, "battery %lu mV%s\n", (unsigned long)os_status.battery_mv,
        charger_present() ? ", charger" : "");
    say(s, "card: %s, %lu MB free\n", fi->mounted ? "mounted" : "not mounted",
        (unsigned long)(fi->free_kib / 1024));
    say(s, "net: %lu frames in, %lu out, %lu dropped, %lu retransmits\n",
        (unsigned long)net_stat.rx_frames, (unsigned long)net_stat.tx_frames,
        (unsigned long)net_stat.rx_dropped, (unsigned long)net_stat.tcp_retransmits);
}

#define PS_MAX 32

typedef struct {
    thread_t *t;
    const char *name;
    uint32_t cycles, stack_size, stack_free;
    uint8_t prio, base, state;
} ps_t;

static int ps_snap(ps_t *p, bool stacks)
{
    uint32_t irq = arch_irq_save();
    int n = 0;

    for (thread_t *t = thread_first(); t && n < PS_MAX; t = thread_next(t), n++) {
        p[n].t = t;
        p[n].name = t->name;
        p[n].cycles = t->cycles;
        p[n].stack_size = (uint32_t)t->stack_size;
        p[n].prio = t->prio;
        p[n].base = t->base_prio;
        p[n].state = t->state;
        p[n].stack_free = 0;
        if (stacks && t->stack) {
            uint32_t k = 0;

            while (k < t->stack_size / 4 && t->stack[k] == 0x57a57a57)
                k++;
            p[n].stack_free = k * 4;
        }
    }
    arch_irq_restore(irq);
    return n;
}

static void ps_once(sess_t *s, ps_t *a, ps_t *b)
{
    uint32_t c0 = kernel_cycles(), c1, span;
    int na = ps_snap(a, false), nb;

    thread_sleep(500);
    c1 = kernel_cycles();
    nb = ps_snap(b, true);
    span = c1 - c0;
    say(s, "%-10s %-4s %-5s %6s  %s\n", "thread", "prio", "state", "cpu", "stack used/size");
    for (int i = 0; i < nb; i++) {
        uint32_t d = 0, pm;

        for (int j = 0; j < na; j++)
            if (a[j].t == b[i].t)
                d = b[i].cycles - a[j].cycles;
        pm = span >= 1000 ? d / (span / 1000) : 0;
        say(s, "%-10s %3u%c %-5s %3lu.%lu%%  %lu/%lu\n", b[i].name ? b[i].name : "?",
            b[i].prio, b[i].prio != b[i].base ? '^' : ' ',
            b[i].t == thread_self() ? "run" : b[i].state == 1 ? "ready" :
            b[i].state == 2 ? "wait" : "dead",
            (unsigned long)(pm / 10), (unsigned long)(pm % 10),
            (unsigned long)(b[i].stack_size - b[i].stack_free),
            (unsigned long)b[i].stack_size);
    }
    {
        uint32_t idle = kernel_idle_permille(), load = idle < 1000 ? 1000 - idle : 0;

        say(s, "load %lu.%lu%% (last second), CPU %d MHz\n",
            (unsigned long)(load / 10), (unsigned long)(load % 10), clock_mhz());
    }
}

static void c_ps(sess_t *s, int argc, char **argv)
{
    ps_t *a = kmalloc(2 * PS_MAX * sizeof(ps_t));

    if (!a) {
        say(s, "ERR out of memory\n");
        return;
    }
    ps_once(s, a, a + PS_MAX);
    kfree(a);
}

static void c_top(sess_t *s, int argc, char **argv)
{
    ps_t *a = kmalloc(2 * PS_MAX * sizeof(ps_t));

    if (!a) {
        say(s, "ERR out of memory\n");
        return;
    }
    say(s, "every second; any key stops\n");
    do {
        say(s, "\n");
        ps_once(s, a, a + PS_MAX);
        flush(s);
    } while (!s->quit && !interrupted(s, 500));
    kfree(a);
}

static void c_mem(sess_t *s, int argc, char **argv)
{
    extern char _vectors[], __bss_start[], __bss_end[], __heap_start[], __heap_end[];
    size_t free, largest;
    unsigned blocks;

    heap_stats(&free, &largest, &blocks);
    say(s, "PSRAM 0x%08lx-0x%08lx (4 MB)\n", (unsigned long)PSRAM_BASE,
        (unsigned long)(PSRAM_BASE + PSRAM_SIZE - 1));
    say(s, "  image   0x%08lx  %lu KB code and data, %lu KB bss and stacks\n",
        (unsigned long)_vectors, (unsigned long)((__bss_start - _vectors) / 1024),
        (unsigned long)((__bss_end - __bss_start) / 1024));
    say(s, "  heap    0x%08lx  %lu KB\n", (unsigned long)__heap_start,
        (unsigned long)((__heap_end - __heap_start) / 1024));
    say(s, "  frame   0x%08lx  LCD frame, 40 KB\n", (unsigned long)FRAME_ADDR);
    say(s, "  ttb     0x%08lx  MMU table, 16 KB\n", (unsigned long)TTB_ADDR);
    say(s, "heap: %lu bytes free in %u pieces, largest %lu; %lu bytes used\n",
        (unsigned long)free, blocks, (unsigned long)largest,
        (unsigned long)((size_t)(__heap_end - __heap_start) - free));
    say(s, "apps loaded: %d\n", app_count());
}

static void c_log(sess_t *s, int argc, char **argv)
{
    bool follow = argc > 1 && streq(argv[1], "-f");
    uint32_t lines = follow ? 10 : 40, pos, end;
    char *buf = kmalloc(8192 + 1);
    size_t n;
    int start;

    if (argc > 1 && !follow && !parse_num(argv[1], &lines)) {
        say(s, "usage: log [N] | log -f\n");
        kfree(buf);
        return;
    }
    if (!buf) {
        say(s, "ERR out of memory\n");
        return;
    }
    end = klog_pos();
    pos = end > 8192 ? end - 8192 : 0;
    n = klog_read(&pos, buf, 8192);
    start = (int)n;
    for (uint32_t k = 0; start > 0; start--)    /* back over N line ends */
        if (buf[start - 1] == '\n' && start != (int)n && ++k == lines)
            break;
    out_text(s, buf + start, (int)n - start);
    if (follow) {
        say(s, "-- following the log; any key stops --\n");
        flush(s);
        while (!s->quit) {
            n = klog_read(&pos, buf, 8192);
            if (n) {
                out_text(s, buf, (int)n);
                flush(s);
            }
            if (interrupted(s, 200))
                break;
        }
    }
    kfree(buf);
}

/* ---- peek and poke ---------------------------------------------------------- */

static bool addr_ok(uintptr_t a, uint32_t size, bool write)
{
    static const struct { uint32_t lo, hi; bool rw; } safe[] = {
        { 0x00000000u, 0x007fffffu, false },    /* NOR */
        { PSRAM_BASE, PSRAM_BASE + PSRAM_SIZE - 1, true },
        { 0x40000000u, 0x4000ffffu, true },     /* IRAM */
        { 0x20100000u, 0x20102fffu, true },     /* DMA */
        { 0x20500000u, 0x20500fffu, true },     /* AHB control */
        { 0x80000000u, 0x80000fffu, true },     /* interrupt controller */
        { 0x81000000u, 0x81000fffu, true },     /* timers */
        { 0x82000000u, 0x82000fffu, true },     /* ADI master */
        { 0x8a000000u, 0x8a002fffu, true },     /* GPIO */
        { 0x8b000000u, 0x8b000fffu, true },     /* APB control */
    };
    uint32_t end = (uint32_t)a + size - 1;

    if (end < (uint32_t)a)
        return false;
    if (usb_running() && a >= 0x20300000u && end <= 0x20300fffu)
        return true;
    for (unsigned i = 0; i < ARRAY_SIZE(safe); i++)
        if (a >= safe[i].lo && end <= safe[i].hi)
            return !write || safe[i].rw;
    return false;
}

static int width_of(const char *w)
{
    return streq(w, "b") ? 1 : streq(w, "h") ? 2 : streq(w, "w") ? 4 : 0;
}

/* args without the flags: -f (any address) and b/h/w (the width) */
static int flags(int argc, char **argv, bool *force, int *width)
{
    int n = 0;

    *force = false;
    *width = 4;
    for (int i = 0; i < argc; i++) {
        if (streq(argv[i], "-f"))
            *force = true;
        else if (i > 0 && width_of(argv[i]))
            *width = width_of(argv[i]);
        else
            argv[n++] = argv[i];
    }
    return n;
}

static void c_peek(sess_t *s, int argc, char **argv)
{
    bool force;
    int w;
    uint32_t a, count = 1;

    argc = flags(argc, argv, &force, &w);
    if (argc < 2 || !parse_num(argv[1], &a) || (argc > 2 && !parse_num(argv[2], &count))) {
        say(s, "usage: peek ADDR [COUNT] [b|h|w] [-f]\n");
        return;
    }
    if (a & (uint32_t)(w - 1)) {
        say(s, "ERR not aligned to %d\n", w);
        return;
    }
    if (count > 1024)
        count = 1024;
    if (!force && !addr_ok(a, count * (uint32_t)w, false)) {
        say(s, "ERR 0x%08lx is outside the safe ranges (see help; -f to force: an "
            "unclocked block hangs the phone)\n", (unsigned long)a);
        return;
    }
    for (uint32_t i = 0; i < count && !s->quit; i++) {
        uint32_t v, at = a + i * (uint32_t)w;

        if (i % (16 / (uint32_t)w) == 0)
            say(s, "%s%08lx:", i ? "\n" : "", (unsigned long)at);
        if (probe_read(at, w, &v)) {
            say(s, " data abort\n");
            return;
        }
        say(s, w == 4 ? " %08lx" : w == 2 ? " %04lx" : " %02lx", (unsigned long)v);
    }
    say(s, "\n");
}

static void c_poke(sess_t *s, int argc, char **argv)
{
    bool force;
    int w;
    uint32_t a, v, back;

    argc = flags(argc, argv, &force, &w);
    if (argc < 3 || !parse_num(argv[1], &a) || !parse_num(argv[2], &v)) {
        say(s, "usage: poke ADDR VALUE [b|h|w] [-f]\n");
        return;
    }
    if (a & (uint32_t)(w - 1)) {
        say(s, "ERR not aligned to %d\n", w);
        return;
    }
    if (!force && !addr_ok(a, (uint32_t)w, true)) {
        say(s, "ERR 0x%08lx is not writable without -f\n", (unsigned long)a);
        return;
    }
    if (probe_write(a, w, v)) {
        say(s, "ERR data abort\n");
        return;
    }
    if (probe_read(a, w, &back))
        say(s, "written; reading back aborted\n");
    else
        say(s, "0x%08lx = 0x%lx (read back)\n", (unsigned long)a, (unsigned long)back);
}

static void c_adi(sess_t *s, int argc, char **argv)
{
    uint32_t a, v;

    if (argc < 2 || !parse_num(argv[1], &a) || (argc > 2 && !parse_num(argv[2], &v)) ||
        a < 0x82001000u || a > 0x82003fffu || (a & 3)) {
        say(s, "usage: adi ADDR [VALUE]   (analog die, 0x82001000-0x82003ffc)\n");
        return;
    }
    if (argc > 2 && !adi_write(a, v)) {
        say(s, "ERR the ADI did not take the write\n");
        return;
    }
    say(s, "0x%08lx = 0x%04lx\n", (unsigned long)a, (unsigned long)adi_read(a));
}

/* What breaks the USB link? One second of each; the watchdog in ip.c notices,
 * and apps/usb.c connects again, about a minute later. */
static void c_usbstall(sess_t *s, int argc, char **argv)
{
    int how = argc < 2 ? -1 : streq(argv[1], "irq") ? 0 : streq(argv[1], "low") ? 1 : -1;

    if (how < 0) {
        say(s, "usage: usbstall irq|low   (a second without the ARM, or also without the PLL)\n");
        return;
    }
    klog("shell: usbstall %s", argv[1]);
    usb_test_stall(how);
    say(s, "OK\n");
}

/* USB off for a while, the cable in (apps/usb.c): this connection ends with it */
static void c_bench(sess_t *s, int argc, char **argv)
{
    uint32_t seconds;
    bool dark = false, nosleep = false;

    for (int i = 2; i < argc; i++) {
        if (streq(argv[i], "dark"))
            dark = true;
        else if (streq(argv[i], "nosleep"))
            nosleep = true;
        else
            argc = 0;
    }
    if (argc < 2 || !parse_num(argv[1], &seconds) || !seconds || seconds > 86400) {
        say(s, "usage: bench SECONDS [dark] [nosleep]   (USB off that long; the screen dark at "
               "once; chip sleep off, to compare)\n");
        return;
    }
    if (!usb_auto_network()) {
        say(s, "ERR bench is for the network in the background, not the USB screen\n");
        return;
    }
    say(s, "OK: USB off for %lu s, the log says how it went\n", (unsigned long)seconds);
    flush(s);
    tcp_flush(s->c, 1000);
    usb_bench_request(seconds, dark, nosleep);
}

/* ---- files ------------------------------------------------------------------ */

static bool need_card(sess_t *s)
{
    if (fs_info()->mounted)
        return true;
    say(s, "ERR no card mounted\n");
    return false;
}

static void c_ls(sess_t *s, int argc, char **argv)
{
    DIR d;
    FILINFO fi;
    FRESULT r;
    unsigned n = 0;

    if (!need_card(s) || !resolve(s, argc > 1 ? argv[1] : "", s->path)) {
        return;
    }
    r = f_opendir(&d, s->path);
    if (r != FR_OK) {
        say(s, "ERR %s: %s\n", s->path, fr_text(r));
        return;
    }
    while (!s->quit && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR)
            say(s, "%10s  %04u-%02u-%02u %02u:%02u  %s/\n", "-",
                (fi.fdate >> 9) + 1980, fi.fdate >> 5 & 15, fi.fdate & 31,
                fi.ftime >> 11, fi.ftime >> 5 & 63, fi.fname);
        else
            say(s, "%10lu  %04u-%02u-%02u %02u:%02u  %s\n", (unsigned long)fi.fsize,
                (fi.fdate >> 9) + 1980, fi.fdate >> 5 & 15, fi.fdate & 31,
                fi.ftime >> 11, fi.ftime >> 5 & 63, fi.fname);
        n++;
    }
    f_closedir(&d);
    say(s, "%u entries in %s\n", n, s->path);
}

static void c_cd(sess_t *s, int argc, char **argv)
{
    FILINFO fi;

    if (!resolve(s, argc > 1 ? argv[1] : "/", s->path))
        return;
    if (strcmp(s->path, "/") && (f_stat(s->path, &fi) != FR_OK || !(fi.fattrib & AM_DIR))) {
        say(s, "ERR %s: not a folder\n", s->path);
        return;
    }
    strncpy(s->cwd, strcmp(s->path, "/") ? s->path : "", sizeof(s->cwd));
}

static void c_pwd(sess_t *s, int argc, char **argv)
{
    say(s, "%s\n", s->cwd[0] ? s->cwd : "/");
}

static void c_cat(sess_t *s, int argc, char **argv)
{
    FIL f;
    FRESULT r;
    char buf[512];
    UINT n;

    if (argc < 2) {
        say(s, "usage: cat FILE\n");
        return;
    }
    if (!need_card(s) || !resolve(s, argv[1], s->path))
        return;
    r = f_open(&f, s->path, FA_READ);
    if (r != FR_OK) {
        say(s, "ERR %s: %s\n", s->path, fr_text(r));
        return;
    }
    while (!s->quit && f_read(&f, buf, sizeof(buf), &n) == FR_OK && n)
        out_text(s, buf, (int)n);
    f_close(&f);
}

static void simple_fs(sess_t *s, int argc, char **argv, int what)
{
    FRESULT r;

    if (argc < (what == 2 ? 3 : 2)) {
        say(s, "usage: %s\n", what == 0 ? "rm PATH" : what == 1 ? "mkdir PATH" : "mv FROM TO");
        return;
    }
    if (!need_card(s) || !resolve(s, argv[1], s->path) ||
        (what == 2 && !resolve(s, argv[2], s->path2)))
        return;
    r = what == 0 ? f_unlink(s->path) : what == 1 ? f_mkdir(s->path)
                                                  : f_rename(s->path, s->path2);
    if (r != FR_OK)
        say(s, "ERR %s: %s\n", s->path, fr_text(r));
    else
        say(s, "OK\n");
}

static void c_rm(sess_t *s, int argc, char **argv) { simple_fs(s, argc, argv, 0); }
static void c_mkdir(sess_t *s, int argc, char **argv) { simple_fs(s, argc, argv, 1); }
static void c_mv(sess_t *s, int argc, char **argv) { simple_fs(s, argc, argv, 2); }

static void c_df(sess_t *s, int argc, char **argv)
{
    DWORD clusters;
    FATFS *fs;

    if (!need_card(s))
        return;
    if (f_getfree("", &clusters, &fs) != FR_OK) {
        say(s, "ERR cannot read the FAT\n");
        return;
    }
    say(s, "%lu MB free of %lu MB\n", (unsigned long)(clusters * fs->csize / 2048),
        (unsigned long)((fs->n_fatent - 2) * fs->csize / 2048));
}

/* date: the clock; date UNIX: set it (seconds since 1970, UTC; the time
 * zone from the settings), as os/tools/b310e-shell.py time does */
static void c_date(sess_t *s, int argc, char **argv)
{
    rtc_datetime_t t;

    if (argc > 1) {
        uint32_t v = 0;

        for (const char *p = argv[1]; *p; p++) {
            if (*p < '0' || *p > '9') {
                say(s, "ERR usage: date [UNIX-SECONDS]\n");
                return;
            }
            v = v * 10 + (uint32_t)(*p - '0');
        }
        if (rtc_set_unix(v, ui_settings.tz_min)) {
            say(s, "ERR the clock did not take it\n");
            return;
        }
        rtc_save();
        klog("shell: clock set to %lu (UTC%+d min)", (unsigned long)v, ui_settings.tz_min);
    }
    rtc_get(&t);
    say(s, "%04d-%02d-%02d %02d:%02d:%02d, UTC%+d:%02d, unix %lu\n", t.year, t.month, t.day,
        t.hour, t.min, t.sec, ui_settings.tz_min / 60,
        (ui_settings.tz_min < 0 ? -ui_settings.tz_min : ui_settings.tz_min) % 60,
        (unsigned long)rtc_unix(ui_settings.tz_min));
}

static void c_crc(sess_t *s, int argc, char **argv)
{
    FIL f;
    FRESULT r;
    uint8_t *buf;
    UINT n;
    uint32_t crc = 0xffffffffu, total = 0;

    if (argc < 2) {
        say(s, "usage: crc FILE\n");
        return;
    }
    if (!need_card(s) || !resolve(s, argv[1], s->path))
        return;
    if (!(buf = kmalloc(XFER_BUF))) {
        say(s, "ERR out of memory\n");
        return;
    }
    r = f_open(&f, s->path, FA_READ);
    if (r == FR_OK) {
        while (!net_stopping && (r = f_read(&f, buf, XFER_BUF, &n)) == FR_OK && n) {
            crc = net_crc32(crc, buf, n);
            total += n;
        }
        f_close(&f);
    }
    if (r != FR_OK)
        say(s, "ERR %s: %s\n", s->path, fr_text(r));
    else
        say(s, "%08lx %lu %s\n", (unsigned long)~crc, (unsigned long)total, s->path);
    kfree(buf);
}

static bool raw_only(sess_t *s)
{
    if (!s->telnet)
        return true;
    say(s, "ERR binary transfers only on port %d (tools/b310e-shell.py)\n", NET_PORT_RAW);
    return false;
}

static void c_put(sess_t *s, int argc, char **argv)
{
    uint32_t size, left, crc = 0xffffffffu;
    FIL f;
    FRESULT r = FR_OK;
    uint8_t *buf;
    bool open = false;
    UINT n;

    if (argc < 3 || !parse_num(argv[2], &size)) {
        say(s, "usage: put PATH SIZE, then SIZE bytes\n");
        return;
    }
    if (!raw_only(s) || !need_card(s) || !resolve(s, argv[1], s->path))
        return;
    if (snprintf(s->path2, sizeof(s->path2), "%s.part", s->path) >= (int)sizeof(s->path2) ||
        !(buf = kmalloc(XFER_BUF))) {
        say(s, "ERR path too long or no memory\n");
        return;
    }
    r = f_open(&f, s->path2, FA_WRITE | FA_CREATE_ALWAYS);
    open = r == FR_OK;
    say(s, "READY\n");
    flush(s);
    /* all of it is read, also after an error: the stream stays in step */
    for (left = size; left && !s->quit; ) {
        uint32_t k = left < XFER_BUF ? left : XFER_BUF;

        if (read_exact(s, buf, (int)k) < 0) {
            r = FR_INT_ERR;
            break;
        }
        crc = net_crc32(crc, buf, k);
        if (open && r == FR_OK && (f_write(&f, buf, k, &n) != FR_OK || n != k))
            r = FR_DENIED;
        left -= k;
    }
    if (open && f_close(&f) != FR_OK && r == FR_OK)
        r = FR_DISK_ERR;                /* the last of it did not reach the card */
    if (r == FR_OK && !left) {
        f_unlink(s->path);
        r = f_rename(s->path2, s->path);
    } else if (open) {
        f_unlink(s->path2);
    }
    kfree(buf);
    if (r != FR_OK || left) {
        say(s, "ERR %s: %s\n", s->path, left ? "the transfer stopped" : fr_text(r));
        return;
    }
    net_stat.files_in++;
    klog("net: %s received, %lu bytes", s->path, (unsigned long)size);
    say(s, "OK %lu %08lx\n", (unsigned long)size, (unsigned long)~crc);
}

static void send_blob(sess_t *s, const uint8_t *p, uint32_t size)
{
    uint32_t crc = net_crc32(0xffffffffu, p, size);

    say(s, "DATA %lu\n", (unsigned long)size);
    out_raw(s, p, (int)size);
    say(s, "END %08lx\n", (unsigned long)~crc);
}

static void c_get(sess_t *s, int argc, char **argv)
{
    FIL f;
    FRESULT r;
    uint8_t *buf;
    UINT n;
    uint32_t crc = 0xffffffffu, left;

    if (argc < 2) {
        say(s, "usage: get PATH\n");
        return;
    }
    if (!raw_only(s) || !need_card(s) || !resolve(s, argv[1], s->path))
        return;
    r = f_open(&f, s->path, FA_READ);
    if (r != FR_OK) {
        say(s, "ERR %s: %s\n", s->path, fr_text(r));
        return;
    }
    if (!(buf = kmalloc(XFER_BUF))) {
        f_close(&f);
        say(s, "ERR out of memory\n");
        return;
    }
    left = (uint32_t)f_size(&f);
    say(s, "DATA %lu\n", (unsigned long)left);
    flush(s);
    while (left && !s->quit) {
        uint32_t k = left < XFER_BUF ? left : XFER_BUF;

        if (f_read(&f, buf, k, &n) != FR_OK || n != k)
            memset(buf, 0, k);          /* the length was promised: zeros, and a bad CRC */
        else
            crc = net_crc32(crc, buf, k);
        out_raw(s, buf, (int)k);
        left -= k;
    }
    f_close(&f);
    kfree(buf);
    net_stat.files_out++;
    say(s, "END %08lx\n", (unsigned long)~crc);
}

static void c_shot(sess_t *s, int argc, char **argv)
{
    /* a copy first: the LCD driver updates the frame's changed rows at any
     * time (partial updates), and the sum must cover what is sent */
    size_t n = LCD_WIDTH * LCD_HEIGHT * 2;
    uint8_t *copy;

    if (!raw_only(s))
        return;
    copy = kmalloc(n);
    if (!copy) {
        say(s, "ERR no memory\n");
        return;
    }
    memcpy(copy, (const void *)FRAME_ADDR, n);
    send_blob(s, copy, n);
    kfree(copy);
}

/* ---- the UI: apps, keys, reboot -------------------------------------------- */

#define RUN_WAIT_MS   3000u             /* its threads, to end by themselves */
#define RUN_KILLED_MS 1000u             /* after app_kill */

static const char *run_err;
static char run_arg[LINE_MAX + 1];      /* the app's argument, "" none */
static bool run_loaded;

static void ui_run_app(void *arg)
{
    usb_net_close_apps();
    run_err = app_open_fresh((const char *)arg, run_arg[0] ? run_arg : NULL);
    ui_wake();
}

/* Unloading happens in the UI loop, after this call. */
static void ui_check_loaded(void *arg)
{
    run_loaded = app_of_file((const char *)arg) != NULL;
    if (run_loaded)
        ui_collect_apps();
}

static void ui_kill_loaded(void *arg)
{
    app_t *a = app_of_file((const char *)arg);

    if (a) {
        app_kill(a);
        ui_collect_apps();
    }
}

static void ui_close_apps(void *arg)
{
    usb_net_close_apps();
}

static const char *reboot_err;

static void ui_do_reboot(void *arg)
{
    ui_reboot();
}

static void ui_do_chain(void *arg)
{
    reboot_err = ui_chain(OS_IMAGE_PATH, "Restarting");     /* only returns on an error */
}

/* wait for the UI thread, giving up when the stack stops */
static bool on_ui(sess_t *s, void (*fn)(void *), void *arg)
{
    while (!net_stopping) {
        if (ui_call(fn, arg, 200))
            return true;
    }
    say(s, "ERR stopping\n");
    return false;
}

/* false: still loaded after ms, or the stack is stopping (on_ui said so) */
static bool wait_unloaded(sess_t *s, const char *path, uint32_t ms)
{
    uint32_t t0 = kernel_ticks();

    for (;;) {
        if (!on_ui(s, ui_check_loaded, (void *)path))
            return false;
        if (!run_loaded)
            return true;
        if (kernel_ticks() - t0 >= ms)
            return false;
        thread_sleep(20);
    }
}

static mutex_t run_lock;

static void c_run_locked(sess_t *s, int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : NULL;
    size_t n;

    if (!name) {
        say(s, "usage: run APP [\"ARG\"]   (a path, or a name in " APP_DIR ")\n");
        return;
    }
    snprintf(run_arg, sizeof(run_arg), "%s", argc > 2 ? argv[2] : "");
    if (!need_card(s))
        return;
    n = strlen(name);
    if (!strchr(name, '/')) {           /* by name or file, in APP_DIR or a
                                         * folder of its own there */
        snprintf(s->path2, sizeof(s->path2), "%s%s", name,
                 n > 4 && !strcmp(name + n - 4, ".app") ? "" : ".app");
        if (!app_find(name, s->path, sizeof(s->path)) &&
            !app_find(s->path2, s->path, sizeof(s->path)))
            snprintf(s->path, sizeof(s->path), APP_DIR "/%s", s->path2);
    } else if (!resolve(s, name, s->path))
        return;
    /* A copy of it still loaded goes first (a closed app until the UI loop
     * unloads it, one with a thread or a notification): the point of run is
     * the file just uploaded, so old code never starts again. */
    if (!on_ui(s, ui_close_apps, NULL))
        return;
    if (!wait_unloaded(s, s->path, RUN_WAIT_MS)) {
        if (net_stopping || !on_ui(s, ui_kill_loaded, s->path))
            return;
        if (!wait_unloaded(s, s->path, RUN_KILLED_MS)) {
            if (!net_stopping)
                say(s, "ERR %s: the old copy is still loaded\n", s->path);
            return;
        }
    }
    if (!on_ui(s, ui_run_app, s->path))
        return;
    if (run_err) {
        say(s, "ERR %s: %s\n", s->path, run_err);
        return;
    }
    net_stat.apps_run++;
    say(s, "OK %s started\n", s->path);
}

/* run_arg, run_err and run_loaded are one set: one run at a time */
static void c_run(sess_t *s, int argc, char **argv)
{
    mutex_lock(&run_lock, WAIT_FOREVER);
    c_run_locked(s, argc, argv);
    mutex_unlock(&run_lock);
}

static void c_close(sess_t *s, int argc, char **argv)
{
    if (on_ui(s, ui_close_apps, NULL))
        say(s, "OK (%d apps loaded)\n", app_count());
}

static void c_key(sess_t *s, int argc, char **argv)
{
    static const struct { const char *name; uint8_t code; } keys[] = {
        { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT },
        { "right", KEY_RIGHT }, { "center", KEY_CENTER }, { "ok", KEY_CENTER },
        { "lsoft", KEY_LSOFT }, { "rsoft", KEY_RSOFT }, { "back", KEY_RSOFT },
        { "end", KEY_END }, { "dial", KEY_DIAL }, { "*", KEY_STAR }, { "#", KEY_HASH },
    };

    if (argc < 2) {
        say(s, "usage: key NAME...   (up down left right center lsoft rsoft end dial 0-9 * #)\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        uint8_t code = 0;

        if (argv[i][0] >= '0' && argv[i][0] <= '9' && !argv[i][1])
            code = (uint8_t)(KEY_0 + argv[i][0] - '0');
        for (unsigned k = 0; k < ARRAY_SIZE(keys) && !code; k++)
            if (streq(argv[i], keys[k].name))
                code = keys[k].code;
        if (!code) {
            say(s, "ERR no key %s\n", argv[i]);
            return;
        }
        keypad_inject(code, true);
        thread_sleep(60);
        keypad_inject(code, false);
        thread_sleep(60);
    }
    say(s, "OK\n");
}

/* reboot: a hard reset (the watchdog), back to this OS through the boot loader
 * (ui_reboot). With an argument, what comes after it: menu, os or stock, a
 * request to the loader. reboot soft: the image on the card again with no reset,
 * so the clock stays. */
static void c_reboot(sess_t *s, int argc, char **argv)
{
    static const struct { const char *arg; unsigned req; } requests[] = {
        { "menu", BOOT_REQ_MENU }, { "os", BOOT_REQ_OS }, { "stock", BOOT_REQ_STOCK },
    };
    const char *how = argc > 1 ? argv[1] : NULL;
    unsigned req = 0;

    if (how && !strcmp(how, "soft")) {
        if (!need_card(s))
            return;
        say(s, "rebooting B310E OS from the card, no reset\n");
        flush(s);
        tcp_flush(s->c, 1000);
        if (on_ui(s, ui_do_chain, NULL))
            say(s, "ERR %s\n", reboot_err);
        return;
    }
    if (how) {
        for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i++)
            if (!strcmp(how, requests[i].arg))
                req = requests[i].req;
        if (!req) {
            say(s, "usage: reboot [soft | menu | os | stock]\n");
            return;
        }
        if (!arch_boot_loader()) {
            say(s, "ERR the boot loader is not in NOR\n");
            return;
        }
    }
    say(s, "rebooting: hard reset%s%s\n", how ? ", then " : "", how ? how : "");
    flush(s);
    tcp_flush(s->c, 1000);
    if (req)
        arch_boot_request(req, NULL);
    on_ui(s, ui_do_reboot, NULL);
}

static void c_echo(sess_t *s, int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        say(s, "%s%s", argv[i], i + 1 < argc ? " " : "");
    say(s, "\n");
}

static void c_exit(sess_t *s, int argc, char **argv)
{
    say(s, "bye\n");
    s->quit = true;
}

static const cmd_t cmds[] = {
    { "help", "", "this list", c_help },
    { "info", "", "build, uptime, CPU, battery, card, network", c_info },
    { "simauth", "", "RUN GSM ALGORITHM on slot 0 (auth self-test)", c_simauth },
    { "txburst", "BAND ARFCN COUNT [norm]", "fire a TX burst train (0=900 1=1800)", c_txburst },
    { "reg", "", "run a registration attempt (TX, owner-cleared)", c_reg },
    { "nvband", "", "NV per-band TX enable bytes", c_nvband },
    { "rxcell", "BAND ARFCN [GAIN] [CCH_S] [TRACK]", "sync+read a cell, RX only (0=900 1=1800)", c_rxcell },
    { "ps", "", "threads: priority, state, CPU over 0.5 s, stack", c_ps },
    { "top", "", "ps every second until a key", c_top },
    { "mem", "", "memory map and heap", c_mem },
    { "log", "[N | -f]", "the last N log lines, or follow it", c_log },
    { "peek", "ADDR [N] [b|h|w] [-f]", "read memory or registers", c_peek },
    { "poke", "ADDR VAL [b|h|w] [-f]", "write memory or registers", c_poke },
    { "adi", "ADDR [VAL]", "analog die register", c_adi },
    { "usbstall", "irq|low", "stall the USB link a second", c_usbstall },
    { "bench", "SECONDS [dark] [nosleep]", "USB off that long, cable in", c_bench },
    { "ls", "[DIR]", "list a folder", c_ls },
    { "cd", "[DIR]", "change folder", c_cd },
    { "pwd", "", "current folder", c_pwd },
    { "cat", "FILE", "print a file", c_cat },
    { "rm", "PATH", "delete a file or empty folder", c_rm },
    { "mkdir", "DIR", "make a folder", c_mkdir },
    { "mv", "FROM TO", "rename or move", c_mv },
    { "df", "", "card space", c_df },
    { "crc", "FILE", "CRC32 and size", c_crc },
    { "date", "[UNIX]", "the clock, or set it (UTC seconds)", c_date },
    { "put", "PATH SIZE", "upload (raw port)", c_put },
    { "get", "PATH", "download (raw port)", c_get },
    { "shot", "", "the screen, RGB565 (raw port)", c_shot },
    { "run", "APP [ARG]", "close the open app, load and start APP (with ARG)", c_run },
    { "close", "", "close the open app", c_close },
    { "key", "NAME...", "press keys", c_key },
    { "reboot", "[soft|menu|os|stock]", "hard reset; soft: the card's OS again, no reset", c_reboot },
    { "echo", "TEXT", "print TEXT", c_echo },
    { "exit", "", "end the session", c_exit },
    { NULL, NULL, NULL, NULL },
};

/* ---- sessions ---------------------------------------------------------------- */

static int split(char *p, char **argv)
{
    int n = 0;

    while (*p && n < ARGS_MAX) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            argv[n++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    return n;
}

static void session(void *arg)
{
    sess_t *s = arg;

    net_stat.shell_sessions++;
    net_stat.shell_open++;
    if (s->telnet) {
        static const uint8_t neg[] = { 255, 251, 1, 255, 251, 3 };     /* WILL ECHO, WILL SGA */

        out_raw(s, neg, sizeof(neg));
    }
    say(s, "B310E OS shell, built " BUILD_DATE ". \"help\" lists the commands.\n" PROMPT);
    flush(s);
    while (!s->quit && read_line(s)) {
        char *argv[ARGS_MAX];
        int argc = split(s->line, argv);
        const cmd_t *c;

        if (argc) {
            for (c = cmds; c->name && !streq(c->name, argv[0]); c++)
                ;
            if (c->name) {
                net_stat.shell_commands++;
                c->fn(s, argc, argv);
            } else {
                say(s, "ERR unknown command %s (help lists them)\n", argv[0]);
            }
        }
        if (!s->quit)
            say(s, PROMPT);
        flush(s);
    }
    flush(s);
    if (!net_stopping) {
        tcp_flush(s->c, 1000);
        tcp_close(s->c);
    }
    net_stat.shell_open--;
    s->used = false;
    sem_post(&s->exited);
    thread_exit();
}

static void server_main(void *arg)
{
    while (!net_stopping) {
        tcp_conn_t *c = tcp_accept(1000);
        sess_t *s = NULL;

        if (!c)
            continue;
        for (int i = 0; i < SESS_N && !s; i++)
            if (!sess[i].used)
                s = &sess[i];
        if (!s) {
            static const char busy[] = "busy: three sessions are open\r\n";

            tcp_write(c, busy, sizeof(busy) - 1);
            tcp_flush(c, 500);
            tcp_close(c);
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->c = c;
        s->telnet = tcp_port(c) == NET_PORT_TELNET;
        sem_init(&s->exited, 0);
        if (!thread_create("shell", PRIO_NORMAL - 2, 8192, session, s)) {
            s->used = false;
            tcp_close(c);
        }
    }
    sem_post(&server_exited);
    thread_exit();
}

int shell_start(void)
{
    memset(sess, 0, sizeof(sess));
    mutex_init(&run_lock);
    sem_init(&server_exited, 0);
    if (tcp_listen(NET_PORT_TELNET) || tcp_listen(NET_PORT_RAW))
        return -1;
    server = thread_create("netsh", PRIO_NORMAL - 1, 1536, server_main, NULL);
    return server ? 0 : -1;
}

/* net_stopping is set and every waiter woken: wait for the threads;
 * false when one is still running (its connection must not be freed) */
bool shell_stop(void)
{
    bool ok = true;

    if (server && sem_wait(&server_exited, 3000) != WAIT_OK) {
        klog("net: the shell server did not stop");
        ok = false;
    }
    server = NULL;
    for (int i = 0; i < SESS_N; i++)
        if (sess[i].used && sem_wait(&sess[i].exited, 5000) != WAIT_OK) {
            klog("net: shell session %d did not stop", i);
            ok = false;
        }
    return ok;
}
