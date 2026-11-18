/*
 * b310e-prof: exact guest instruction counts for the B310E OS benchmark
 * (os/tests/bench-instr.py), and a pacing hold that makes a QEMU run with
 * -icount shift=N,sleep=off repeatable to the instruction.
 *
 * Counting: every guest instruction translated gets a counter of its own
 * (keyed by its virtual address, so a retranslation counts on in the same
 * place) and an inline add of 1 before it runs. A dump writes the nonzero
 * counters as "vaddr size count" lines; the driver subtracts two dumps and
 * attributes the addresses to functions with the ELF symbol table. Not
 * counted twice: nothing; counted once more than run: an instruction that
 * starts an MMIO access in the middle of a block (icount recompiles the
 * block from it, and it runs again), which is the same every run.
 *
 * Pacing: the wait-for-interrupt instruction (ARMv5 "mcr p15, 0, rX, c7, c0,
 * 4", or the v6 WFI) gets a callback too. When the hold condition is met
 * there, the vCPU thread stops inside the callback, before the WFI, and the
 * plugin takes commands from files in its control folder. With icount the
 * virtual clock only moves with instructions or, idle, by warping to the
 * next timer, so while held the guest's time stands still: a key sent then
 * (QMP sendkey) reaches the guest at the same instruction every run, and a
 * dump taken then is exact.
 *
 *   -plugin b310e-prof.dll,ctl=DIR,ticks=0xADDR
 *
 *   ticks  the guest's millisecond counter (kernel/thread.c `ticks`)
 *   ctl    the folder: the driver writes cmd-N (N = 1, 2, ...), the plugin
 *          deletes it, acts and writes ack-N ("N ticks total wfi"):
 *            run         go on, hold at the next WFI          (ack: then)
 *            until T     go on, hold at the first WFI with ticks >= T
 *            dump PATH   write the counters to PATH            (ack: at once)
 *            free        go on and never hold again            (ack: at once)
 *          ack-0 is written at the first hold (the plugin starts holding).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define CHUNK 4096                      /* counters a scoreboard holds */

typedef struct {
    uint64_t vaddr;
    uint32_t size;
} Slot;

enum { MODE_HOLD_NEXT, MODE_UNTIL, MODE_FREE };

static GMutex lock;                     /* the slot table: translation, dumps */
static GHashTable *by_vaddr;            /* vaddr -> slot index + 1 */
static GArray *slots;                   /* Slot */
static GPtrArray *chunks;               /* struct qemu_plugin_scoreboard * */
static char *ctl_dir;
static uint64_t ticks_addr;
static GByteArray *ticks_buf;
static int mode = MODE_HOLD_NEXT;
static uint32_t until_ticks;
static unsigned pending_seq;            /* the command acked at the next hold */
static unsigned next_seq = 1;
static uint64_t wfi_seen, holds;

static qemu_plugin_u64 slot_counter(guint i)
{
    qemu_plugin_u64 e = {
        .score = g_ptr_array_index(chunks, i / CHUNK),
        .offset = (i % CHUNK) * sizeof(uint64_t),
    };
    return e;
}

static guint slot_for(uint64_t vaddr, uint32_t size)
{
    gpointer v = g_hash_table_lookup(by_vaddr, GSIZE_TO_POINTER(vaddr));
    Slot s = { vaddr, size };
    guint i;

    if (v) {
        return GPOINTER_TO_UINT(v) - 1;
    }
    i = slots->len;
    if (i / CHUNK >= chunks->len) {
        g_ptr_array_add(chunks, qemu_plugin_scoreboard_new(CHUNK * sizeof(uint64_t)));
    }
    g_array_append_val(slots, s);
    g_hash_table_insert(by_vaddr, GSIZE_TO_POINTER(vaddr), GUINT_TO_POINTER(i + 1));
    return i;
}

static uint32_t guest_ticks(void)
{
    g_byte_array_set_size(ticks_buf, 0);
    if (!ticks_addr || !qemu_plugin_read_memory_vaddr(ticks_addr, ticks_buf, 4) ||
        ticks_buf->len < 4) {
        return 0;
    }
    return (uint32_t)ticks_buf->data[0] | (uint32_t)ticks_buf->data[1] << 8 |
           (uint32_t)ticks_buf->data[2] << 16 | (uint32_t)ticks_buf->data[3] << 24;
}

static uint64_t total_insns(void)
{
    uint64_t t = 0;

    g_mutex_lock(&lock);
    for (guint i = 0; i < slots->len; i++) {
        t += qemu_plugin_u64_sum(slot_counter(i));
    }
    g_mutex_unlock(&lock);
    return t;
}

static void write_file(const char *path, const char *text, gsize len)
{
    char *tmp = g_strconcat(path, ".tmp", NULL);

    if (g_file_set_contents(tmp, text, len, NULL)) {
        g_remove(path);
        g_rename(tmp, path);
    }
    g_free(tmp);
}

static void ack(unsigned seq)
{
    char name[32], *path, *text;

    g_snprintf(name, sizeof(name), "ack-%u", seq);
    path = g_build_filename(ctl_dir, name, NULL);
    text = g_strdup_printf("%u %" PRIu32 " %" PRIu64 " %" PRIu64 "\n", seq,
                           guest_ticks(), total_insns(), wfi_seen);
    write_file(path, text, strlen(text));
    g_free(text);
    g_free(path);
}

static void dump(const char *path)
{
    GString *out = g_string_sized_new(1 << 20);
    uint64_t total = 0;
    char *head;

    g_mutex_lock(&lock);
    for (guint i = 0; i < slots->len; i++) {
        uint64_t n = qemu_plugin_u64_sum(slot_counter(i));
        const Slot *s = &g_array_index(slots, Slot, i);

        if (n) {
            g_string_append_printf(out, "%" PRIx64 " %u %" PRIu64 "\n", s->vaddr,
                                   s->size, n);
            total += n;
        }
    }
    g_mutex_unlock(&lock);
    head = g_strdup_printf("# total %" PRIu64 " ticks %" PRIu32 " wfi %" PRIu64
                           " holds %" PRIu64 "\n", total, guest_ticks(), wfi_seen, holds);
    g_string_prepend(out, head);
    g_free(head);
    write_file(path, out->str, out->len);
    g_string_free(out, TRUE);
}

/* At a hold: commands until one lets the guest go on */
static void hold(void)
{
    holds++;
    ack(pending_seq);
    for (;;) {
        char name[32], *path, *text = NULL, *arg;

        g_snprintf(name, sizeof(name), "cmd-%u", next_seq);
        path = g_build_filename(ctl_dir, name, NULL);
        if (!g_file_get_contents(path, &text, NULL, NULL)) {
            g_free(path);
            g_usleep(200);
            continue;
        }
        g_remove(path);
        g_free(path);
        g_strstrip(text);
        arg = strchr(text, ' ');
        if (arg) {
            *arg++ = 0;
        }
        if (!strcmp(text, "dump") && arg) {
            dump(arg);
            ack(next_seq++);
        } else if (!strcmp(text, "run")) {
            mode = MODE_HOLD_NEXT;
            pending_seq = next_seq++;
            g_free(text);
            return;
        } else if (!strcmp(text, "until") && arg) {
            mode = MODE_UNTIL;
            until_ticks = (uint32_t)strtoul(arg, NULL, 0);
            pending_seq = next_seq++;
            g_free(text);
            return;
        } else if (!strcmp(text, "free")) {
            mode = MODE_FREE;
            ack(next_seq++);
            g_free(text);
            return;
        } else {
            ack(next_seq++);            /* unknown: answered, ignored */
        }
        g_free(text);
    }
}

static void on_wfi(unsigned int vcpu_index, void *udata)
{
    wfi_seen++;
    if (mode == MODE_HOLD_NEXT ||
        (mode == MODE_UNTIL && (int32_t)(guest_ticks() - until_ticks) >= 0)) {
        hold();
    }
}

static bool is_wfi(struct qemu_plugin_insn *insn)
{
    uint32_t w = 0;

    if (qemu_plugin_insn_size(insn) != 4 ||
        qemu_plugin_insn_data(insn, &w, 4) != 4) {
        return false;
    }
    w = GUINT32_FROM_LE(w);
    return (w & 0x0fff0fffu) == 0x0e070f90u ||    /* mcr p15, 0, rX, c7, c0, 4 */
           (w & 0x0fffffffu) == 0x0320f003u;      /* wfi (v6K and later) */
}

static void tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t k = 0; k < n; k++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, k);
        guint i;

        g_mutex_lock(&lock);
        i = slot_for(qemu_plugin_insn_vaddr(insn), (uint32_t)qemu_plugin_insn_size(insn));
        g_mutex_unlock(&lock);
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
            insn, QEMU_PLUGIN_INLINE_ADD_U64, slot_counter(i), 1);
        if (is_wfi(insn)) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_wfi, QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (eq && !strncmp(argv[i], "ctl", (size_t)(eq - argv[i]))) {
            ctl_dir = g_strdup(eq + 1);
        } else if (eq && !strncmp(argv[i], "ticks", (size_t)(eq - argv[i]))) {
            ticks_addr = g_ascii_strtoull(eq + 1, NULL, 0);
        } else {
            fprintf(stderr, "b310e-prof: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    if (!ctl_dir) {
        fprintf(stderr, "b310e-prof: ctl=DIR is needed\n");
        return -1;
    }
    by_vaddr = g_hash_table_new(g_direct_hash, g_direct_equal);
    slots = g_array_new(FALSE, FALSE, sizeof(Slot));
    chunks = g_ptr_array_new();
    ticks_buf = g_byte_array_new();
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    return 0;
}
