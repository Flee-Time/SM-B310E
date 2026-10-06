/*
 * B310E-OS — tests/hosttest.c
 *
 * Host-side unit tests for the Wave 5 kernel: scheduler (cooperative
 * round-robin), message queues, module framework, bump allocator, printk.
 *
 * Built with the HOST compiler (`make hosttest` -> build/hosttest.exe)
 * with -DHOST_TEST, so kernel/ sources take their host implementations:
 *   - scheduler: setjmp/longjmp coroutines
 *   - kputc:     overridden by the STRONG definition below (the weak
 *                default in printk.c loses at link time)
 *
 * Acceptance: exit code 0 and the final line "ALL TESTS PASSED (N checks)".
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "os.h"

/* Keypad driver compiled into this TU: register writes are compiled out by
 * HOST_TEST and keypad_poll() reads the injected keypad_test_raw/status,
 * so the decode, edge detection and queue push logic are testable here. */
#include "../drivers/keypad.c"

/* USB debug framing (Wave 6). drivers/ is not in the host test glob
 * (Makefile: HOSTTEST_SRCS = tests/hosttest.c + the kernel sources), so
 * the driver source is compiled in here directly. Under HOST_TEST the
 * hardware paths and the strong kputc override are compiled out (this
 * file defines its own kputc below); the pure framing helpers stay
 * available. */
#include "../drivers/usb_debug.c"

/* Battery fuel gauge (fuel gauge + RTC todo 2026-08-21). drivers/ is not
 * in the host test glob, so the source is compiled in here directly like
 * keypad/usb_debug above. Under HOST_TEST the ADI/ADC hardware paths are
 * compiled out; battery_level_percent() (the pure mV->percent map) is
 * host-testable. */
#include "../drivers/battery.c"

/* ---- test harness ------------------------------------------------------ */

static int s_checks;
static int s_failures;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        s_checks++;                                             \
        if (cond) {                                             \
            printf("  [PASS] %s\n", msg);                       \
        } else {                                                \
            printf("  [FAIL] %s\n", msg);                       \
            s_failures++;                                       \
        }                                                       \
    } while (0)

/* ---- kputc override (strong symbol) ------------------------------------ */
/* Also captures output so the kprintf tests can assert on it. */

static char s_kbuf[1024];
static size_t s_klen;

void kputc(char c)
{
    if (s_klen + 1 < sizeof(s_kbuf))
        s_kbuf[s_klen++] = c;
    fputc(c, stdout);
}

static void kcapture_reset(void)
{
    s_klen = 0;
    s_kbuf[s_klen] = '\0';
}

static int kcapture_is(const char *expected)
{
    s_kbuf[s_klen] = '\0';
    return strcmp(s_kbuf, expected) == 0;
}

static int vtest(char *buf, size_t size, const char *fmt, ...){
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- scheduler test ----------------------------------------------------- */

#define SCH_N      10               /* slices per task */
#define SCH_TASKS  3

static volatile int s_cnt[SCH_TASKS];
static volatile int s_order[SCH_TASKS * SCH_N];
static volatile int s_order_idx;

static void sched_test_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < SCH_N; i++) {
        s_order[s_order_idx++] = id;
        s_cnt[id]++;
        task_yield();
    }
}

static void sched_lone_task(void *arg)
{
    int *n = (int *)arg;
    for (int i = 0; i < 100; i++) {
        (*n)++;
        task_yield();
    }
}

static void test_scheduler(void)
{
    printf("--- scheduler (cooperative round-robin) ---\n");

    /* no tasks: both calls must be safe no-ops */
    task_yield();
    CHECK(1, "task_yield() with no tasks is a no-op");
    sched_start();
    CHECK(1, "sched_start() with no tasks returns immediately");

    /* separate heap for task stacks (uint64_t alignment guarantees the
     * kmem_init pool is 8-aligned with zero alignment loss) */
    static uint64_t sched_heap[2048];   /* 16 KiB */
    kmem_init(sched_heap, sizeof(sched_heap));

    int ids[SCH_TASKS];
    for (int i = 0; i < SCH_TASKS; i++)
        ids[i] = task_create("t", sched_test_task, (void *)(intptr_t)i, 512);

    CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3,
          "task_create returns ids 1, 2, 3");
    CHECK(sched_task_count() == 3, "sched_task_count() == 3 before start");

    sched_start();                     /* runs until every task is DONE */

    CHECK(sched_task_count() == 0, "sched_task_count() == 0 after run");
    CHECK(s_cnt[0] == SCH_N && s_cnt[1] == SCH_N && s_cnt[2] == SCH_N,
          "each task received exactly N CPU slices");
    CHECK((int)s_cnt[0] + (int)s_cnt[1] + (int)s_cnt[2] == 3 * SCH_N,
          "total slices == 3*N");

    /* strict round-robin proof: the interleave is exactly A,B,C,A,B,C,... */
    {
        int ok = 1;
        for (int i = 0; i < SCH_TASKS * SCH_N; i++) {
            if (s_order[i] != i % SCH_TASKS) {
                ok = 0;
                break;
            }
        }
        CHECK(ok, "tasks interleaved strictly A,B,C,A,B,C,...");
        CHECK((int)s_order_idx == SCH_TASKS * SCH_N,
              "order array fully populated (3*N entries)");
    }

    /* a lone task that yields 100 times must finish without a peer */
    {
        static int n = 0;
        int id = task_create("lone", sched_lone_task, &n, 512);
        CHECK(id == 4, "single-task create works");
        sched_start();
        CHECK(n == 100, "lone task ran all 100 slices then DONE");
        CHECK(sched_task_count() == 0, "lone task consumed");
    }
}

/* ---- message queue test ------------------------------------------------ */

static void test_queue(void)
{
    printf("--- message queue ---\n");

    static uint32_t buf[4];
    msg_queue_t q;
    uint32_t v;

    q_init(&q, buf, 4);
    CHECK(q_count(&q) == 0, "fresh queue is empty");

    CHECK(q_push(&q, 10) == 0, "push 10");
    CHECK(q_push(&q, 20) == 0, "push 20");
    CHECK(q_push(&q, 30) == 0, "push 30");
    CHECK(q_push(&q, 40) == 0, "push 40");
    CHECK(q_count(&q) == 4, "count == 4 when full");
    CHECK(q_push(&q, 50) == -1, "push on full returns -1");

    CHECK(q_pop(&q, &v) == 0 && v == 10, "pop 10 (FIFO order)");
    CHECK(q_pop(&q, &v) == 0 && v == 20, "pop 20");
    CHECK(q_push(&q, 50) == 0, "push after pop (tail wraps)");
    CHECK(q_pop(&q, &v) == 0 && v == 30, "pop 30");
    CHECK(q_pop(&q, &v) == 0 && v == 40, "pop 40");
    CHECK(q_pop(&q, &v) == 0 && v == 50, "pop 50");
    CHECK(q_count(&q) == 0, "empty after all pops");
    CHECK(q_pop(&q, &v) == -1, "pop on empty returns -1");

    /* order preserved across a full head/tail wrap */
    q_init(&q, buf, 3);
    CHECK(q_push(&q, 100) == 0 && q_push(&q, 101) == 0 &&
          q_push(&q, 102) == 0, "wrap: fill 3 slots");
    CHECK(q_pop(&q, &v) == 0 && v == 100, "wrap: pop 100");
    CHECK(q_push(&q, 103) == 0, "wrap: push 103");
    CHECK(q_pop(&q, &v) == 0 && v == 101, "wrap: pop 101");
    CHECK(q_pop(&q, &v) == 0 && v == 102, "wrap: pop 102");
    CHECK(q_pop(&q, &v) == 0 && v == 103, "wrap: pop 103");
    CHECK(q_count(&q) == 0, "wrap: empty again");
}

/* ---- module framework test --------------------------------------------- */

static int s_mod_order[8];
static int s_mod_n;

static int mod_a_init(void)
{
    s_mod_order[s_mod_n++] = 1;
    return 0;
}
static int mod_b_init(void)
{
    s_mod_order[s_mod_n++] = 2;
    return 0;
}
static int mod_c_init(void)
{
    s_mod_order[s_mod_n++] = 3;
    return 0;
}
static int mod_fail_init(void)
{
    s_mod_order[s_mod_n++] = 9;
    return -1;
}

static const module_t s_mod_a   = { "mod_a", mod_a_init };
static const module_t s_mod_b   = { "mod_b", mod_b_init };
static const module_t s_mod_c   = { "mod_c", mod_c_init };
static const module_t s_mod_fail = { "mod_fail", mod_fail_init };

static void test_module(void)
{
    printf("--- module framework ---\n");

    module_register(&s_mod_a);
    module_register(&s_mod_b);
    module_register(&s_mod_c);

    s_mod_n = 0;
    CHECK(module_init_all() == 0, "module_init_all returns 0 (no failures)");
    CHECK(s_mod_n == 3, "all 3 inits ran");
    CHECK(s_mod_order[0] == 1 && s_mod_order[1] == 2 && s_mod_order[2] == 3,
          "inits ran in registration order");

    module_register(&s_mod_fail);
    s_mod_n = 0;
    CHECK(module_init_all() == 1, "module_init_all counts the failing init");
    CHECK(s_mod_n == 4, "all 4 inits still ran despite one failure");
}

/* ---- bump allocator test ----------------------------------------------- */

static void test_malloc(void)
{
    printf("--- bump allocator ---\n");

    /* uint64_t array -> guaranteed 8-byte aligned, no kmem_init loss */
    static uint64_t heap[(8192u + 7u) / 8u];
    kmem_init(heap, sizeof(heap));

    CHECK(kmem_free_bytes() == 8192, "free bytes == pool size after init");

    void *a = kmalloc(1);
    void *b = kmalloc(7);
    void *c = kmalloc(100);
    void *d = kmalloc(0);
    CHECK(a != NULL && b != NULL && c != NULL && d != NULL,
          "small allocations succeed");
    CHECK(((uintptr_t)a & 7u) == 0 && ((uintptr_t)b & 7u) == 0 &&
          ((uintptr_t)c & 7u) == 0 && ((uintptr_t)d & 7u) == 0,
          "allocations are 8-byte aligned");

    /* bump: 1->8, 7->8, 100->104, 0->8 = 128 bytes consumed */
    CHECK(kmem_free_bytes() == 8192 - 128, "free bytes tracked correctly");

    void *big = kmalloc(8192 - 128 - 64);
    CHECK(big != NULL, "large tail allocation succeeds");
    CHECK(kmem_free_bytes() == 64, "free bytes == remaining 64");

    CHECK(kmalloc(8192) == NULL, "oversized alloc returns NULL");
    CHECK(kmalloc(65) == NULL, "alloc larger than remainder returns NULL");

    void *y = kmalloc(64);
    CHECK(y != NULL, "exact remainder allocation succeeds");
    CHECK(kmem_free_bytes() == 0, "pool exhausted cleanly");
    CHECK(kmalloc(1) == NULL, "alloc on exhausted pool returns NULL");
}

/* ---- printk test ------------------------------------------------------- */

static void test_kprintf(void)
{
    printf("--- kprintf ---\n");

    kcapture_reset();
    kprintf("%d", 42);
    CHECK(kcapture_is("42"), "%%d positive");

    kcapture_reset();
    kprintf("%d", -7);
    CHECK(kcapture_is("-7"), "%%d negative");

    kcapture_reset();
    kprintf("%u", 4000000000u);
    CHECK(kcapture_is("4000000000"), "%%u unsigned");

    kcapture_reset();
    kprintf("%x", 0xdeadbeefu);
    CHECK(kcapture_is("deadbeef"), "%%x lowercase hex");

    kcapture_reset();
    kprintf("%s", "hello kernel");
    CHECK(kcapture_is("hello kernel"), "%%s string");

    kcapture_reset();
    kprintf("%c", 'Z');
    CHECK(kcapture_is("Z"), "%%c char");

    kcapture_reset();
    kprintf("%p", (void *)(uintptr_t)0x1234);
    CHECK(kcapture_is("0x1234"), "%%p pointer");

    kcapture_reset();
    kprintf("100%% done");
    CHECK(kcapture_is("100% done"), "%% literal percent");

    kcapture_reset();
    kprintf("[%d] %s=0x%x%c%u", -1, "reg", 0x1f, '!', 7u);
    CHECK(kcapture_is("[-1] reg=0x1f!7"), "combined format");

    /* vsnprintf direct: return value, exact fit, truncation, size-1 */
    {
        char buf[8];
        int n = vtest(buf, sizeof(buf), "%d-%d-%d", 1, 2, 3);
        CHECK(n == 5, "vsnprintf returns full length");
        CHECK(strcmp(buf, "1-2-3") == 0, "vsnprintf exact fit");

        char buf2[5];
        int m = vtest(buf2, sizeof(buf2), "%d-%d-%d", 1, 2, 3);
        CHECK(m == 5, "vsnprintf length reported despite truncation");
        CHECK(strcmp(buf2, "1-2-") == 0, "vsnprintf truncates + NUL");

        char buf3[1];
        int k = vtest(buf3, 1, "abc");
        CHECK(k == 3 && buf3[0] == '\0', "vsnprintf size-1 yields empty");
    }
}

/* ---- USB debug framing test (libc_server protocol) --------------------- */

/* Golden vectors computed independently (python fastchk16 implementation):
 * frame = [0x80, len, chk_lo, chk_hi, payload...] with
 * chk = fastchk16(0x5a5a + 0x80 + (len << 8), payload, len) */
static void test_usb_debug_frame(void)
{
    uint8_t out[16];

    printf("--- usb_debug framing (libc_server protocol) ---\n");

    {
        static const uint8_t gold[] = { 0x80, 0x01, 0x1b, 0x5c, 'A' };
        int n = usb_debug_frame_for_test('A', out, sizeof(out));
        CHECK(n == 5, "frame_for_test('A') returns 5 bytes");
        CHECK(memcmp(out, gold, sizeof(gold)) == 0,
              "frame_for_test('A') matches golden vector");
    }

    {
        static const uint8_t gold[] = { 0x80, 0x01, 0x34, 0x5c, 'Z' };
        int n = usb_debug_frame_for_test('Z', out, sizeof(out));
        CHECK(n == 5 && memcmp(out, gold, sizeof(gold)) == 0,
              "frame_for_test('Z') matches golden vector");
    }

    {
        static const uint8_t gold[] = { 0x80, 0x01, 0xe4, 0x5b, '\n' };
        int n = usb_debug_frame_for_test('\n', out, sizeof(out));
        CHECK(n == 5 && memcmp(out, gold, sizeof(gold)) == 0,
              "frame_for_test('\\n') matches golden vector");
    }

    /* multi-byte payload: header carries the length, checksum covers it */
    {
        static const uint8_t gold[] = { 0x80, 0x02, 0x1b, 0x9f, 'A', 'B' };
        int n = usb_debug_frame_build("AB", 2, out, sizeof(out));
        CHECK(n == 6, "frame_build(\"AB\") returns 6 bytes");
        CHECK(memcmp(out, gold, sizeof(gold)) == 0,
              "frame_build(\"AB\") matches golden vector");
    }

    /* 255-byte payload is the protocol cap; length byte wraps otherwise */
    {
        uint8_t bigout[4 + USB_DBG_MAX_PAYLOAD];
        char big[USB_DBG_MAX_PAYLOAD];
        int n;
        memset(big, 'x', sizeof(big));
        n = usb_debug_frame_build(big, (int)sizeof(big), bigout,
                                  (int)sizeof(bigout));
        CHECK(n == 4 + USB_DBG_MAX_PAYLOAD, "frame_build caps at 255 payload");
        CHECK(bigout[1] == 255, "length byte == 255 at the cap");
        CHECK(bigout[2] != 0 || bigout[3] != 0,
              "checksum is non-trivial at the cap");
    }

    /* payload longer than the cap is clamped, not overflowed */
    {
        uint8_t bigout[4 + USB_DBG_MAX_PAYLOAD];
        char big[300];
        int n;
        memset(big, 'y', sizeof(big));
        n = usb_debug_frame_build(big, (int)sizeof(big), bigout,
                                  (int)sizeof(bigout));
        CHECK(n == 4 + USB_DBG_MAX_PAYLOAD,
              "frame_build clamps oversize payload");
    }

    /* undersized output buffer fails cleanly */
    {
        uint8_t tiny[4];
        CHECK(usb_debug_frame_for_test('A', tiny, sizeof(tiny)) == -1,
              "frame_for_test with 4-byte buffer returns -1");
        CHECK(usb_debug_frame_build("AB", 2, tiny, sizeof(tiny)) == -1,
              "frame_build with 4-byte buffer returns -1");
    }

    /* "connected" greeting — the CMD_MESSAGE frame sent the INSTANT the
     * device drains libc_server's HOST_CONNECT (fpdoom _debug_msg right
     * after the connect byte). chk = fastchk16(0x5a5a + 0x80 + 9<<8,
     * "connected", 9) = 0x0aea. */
    {
        static const uint8_t gold[] = {
            0x80, 0x09, 0xea, 0x0a,
            'c', 'o', 'n', 'n', 'e', 'c', 't', 'e', 'd'
        };
        int n = usb_debug_connect_greeting(out, sizeof(out));
        CHECK(n == 13, "connect_greeting returns 13 bytes");
        CHECK(memcmp(out, gold, sizeof(gold)) == 0,
              "connect_greeting matches golden vector");
    }
}

/* ---- keypad driver test ------------------------------------------------ */

static void test_keypad(void)
{
    printf("--- keypad driver ---\n");
    int map_ok = 1, used = 0;
    uint32_t v;
    unsigned k;

    /* every assigned position in the REAL keytrn decodes to itself:
     * s_keytrn[k] with k = row*8+col; the status byte for that key is
     * (row<<4)|col at byte offset (col&3)*8, and the PRESS int_raw bit is
     * (col & 3) — the controller maps logical column c to bit (c & 3) for
     * press and bit (c & 3) + 4 for release (fpdoom sys_event math). */
    for (k = 0; k < 64; k++) {
        uint32_t colbits, rowbits, byte;
        unsigned col = k & 7u, row = k >> 3;

        if (s_keytrn[k] == 0xff)
            continue;
        used++;
        byte = (row << 4) | col;
        colbits = 1u << (col & 3u);           /* press bit = col mod 4 */
        rowbits = byte << ((col & 3u) * 8u);
        if (keypad_decode_for_test(rowbits, colbits) != s_keytrn[k])
            map_ok = 0;
    }
    CHECK(map_ok, "all assigned positions decode to their key codes");
    CHECK(used == 20, "real keymap has 20 assigned positions");

    /* fptest ground truth on this phone: physical key -> KEY_* code.
     * Each vector is the real (col,row) from the dump with fpdoom's byte
     * encoding, so these checks pin the exact hardware mapping. */
    {
        static const struct { uint32_t col, row; int code; } gt[] = {
            { 0, 0, KEY_CENTER }, { 0, 1, KEY_1 }, { 0, 2, KEY_4 },
            { 0, 3, KEY_7 }, { 0, 4, KEY_STAR },
            { 1, 0, KEY_DIAL }, { 1, 1, KEY_2 }, { 1, 2, KEY_5 },
            { 1, 3, KEY_8 }, { 1, 4, KEY_0 },
            { 2, 1, KEY_3 }, { 2, 2, KEY_6 }, { 2, 3, KEY_9 },
            { 2, 4, KEY_HASH },
            { 3, 1, KEY_LSOFT }, { 3, 2, KEY_RSOFT },
            { 4, 0, KEY_LEFT }, { 4, 1, KEY_DOWN }, { 4, 3, KEY_UP },
            { 4, 4, KEY_RIGHT },
        };
        unsigned i;
        int gt_ok = 1;

        for (i = 0; i < sizeof(gt) / sizeof(gt[0]); i++) {
            uint32_t byte = (gt[i].row << 4) | gt[i].col;
            uint32_t colbits = 1u << (gt[i].col & 3u);  /* press bit */
            uint32_t rowbits = byte << ((gt[i].col & 3u) * 8u);

            if (keypad_decode_for_test(rowbits, colbits) != gt[i].code)
                gt_ok = 0;
        }
        CHECK(gt_ok, "fptest ground truth: physical key -> correct code");
    }

    CHECK(keypad_decode_for_test(0, 0) == 0, "no key -> 0");
    /* fpdoom sys_event: status bit 3 is a controller bit, whole frame
     * skipped (`if (status & 8) return EVENT_END`). */
    CHECK(keypad_decode_for_test(1u << 3, 1u) == 0,
          "status bit 3 skips the frame");

    /* priority: CENTER (col0) beats DIAL (col1) */
    {
        uint32_t rb = 0x00u | (0x01u << 8);
        uint32_t cb = (1u << 0) | (1u << 1);
        CHECK(keypad_decode_for_test(rb, cb) == KEY_CENTER,
              "priority: lowest column wins");
    }

    CHECK(strcmp(key_name(KEY_DIAL), "DIAL") == 0, "key_name DIAL");
    CHECK(strcmp(key_name(KEY_CENTER), "CENTER") == 0, "key_name CENTER");
    CHECK(strcmp(key_name(KEY_0), "0") == 0, "key_name 0");
    CHECK(strcmp(key_name(KEY_9), "9") == 0, "key_name 9");
    CHECK(strcmp(key_name(KEY_STAR), "STAR") == 0, "key_name STAR");
    CHECK(strcmp(key_name(KEY_HASH), "HASH") == 0, "key_name HASH");
    CHECK(strcmp(key_name(KEY_MUSIC_PLAY), "MUSIC_PLAY") == 0,
          "key_name MUSIC_PLAY");
    CHECK(strcmp(key_name(0x7f), "?") == 0, "key_name unknown");

    /* keypad_poll: HARDWARE edge model (fpdoom sys_event port). The SC6530
     * controller is edge-triggered: int_raw bits 0-3 = press events, bits
     * 4-7 = release events (column i's release = bit i+4, decoding the
     * same key_status row byte via the (i & 3) wrap). No software
     * held-state. */
    keypad_init();
    CHECK(q_count(&keypad_queue) == 0, "keypad queue empty after init");

    /* LSOFT = col3 row1: press = int_raw bit 3, status byte 3 = 0x13. */
    keypad_test_raw = 1u << 3;
    keypad_test_status = 0x13u << 24;
    CHECK(keypad_poll() == KEY_LSOFT, "poll returns code on press edge");
    CHECK(q_count(&keypad_queue) == 1, "press pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_LSOFT,
          "queue pops KEY_LSOFT");

    /* held: controller reports nothing (no repeat) */
    keypad_test_raw = 0;
    keypad_test_status = 0;
    CHECK(keypad_poll() == 0, "held key is not re-emitted");
    CHECK(q_count(&keypad_queue) == 0, "no repeat push");

    /* release LSOFT = int_raw bit 3+4 = 7, same status byte -> UP event */
    keypad_test_raw = 1u << 7;
    keypad_test_status = 0x13u << 24;
    CHECK(keypad_poll() == (KEY_LSOFT | (int)KEY_EVENT_UP_FLAG),
          "release edge (bit 4-7) emits code | KEY_EVENT_UP_FLAG");
    CHECK(q_count(&keypad_queue) == 1, "release pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 &&
          v == (KEY_LSOFT | KEY_EVENT_UP_FLAG),
          "queue pops LSOFT up event");

    /* re-press after release emits again (bit 0-3) */
    keypad_test_raw = 1u << 3;
    keypad_test_status = 0x13u << 24;
    CHECK(keypad_poll() == KEY_LSOFT, "re-press after release emits again");
    CHECK(q_count(&keypad_queue) == 1, "re-press pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_LSOFT,
          "queue pops KEY_LSOFT (re-press)");

    /* DIAL = col1 row0: press bit 1, release bit 1+4 = 5, status byte 1
     * = 0x01. A multi-bit frame (press DIAL while pressing LSOFT) pushes
     * BOTH events; the return value is the first (lowest bit). */
    keypad_test_raw = (1u << 1) | (1u << 3);
    keypad_test_status = (0x01u << 8) | (0x13u << 24);
    CHECK(keypad_poll() == KEY_DIAL, "multi-press frame: first (bit 1) wins");
    CHECK(q_count(&keypad_queue) == 2, "multi-press pushes both events");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_DIAL,
          "queue pops KEY_DIAL first");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_LSOFT,
          "queue pops KEY_LSOFT second");

    /* release DIAL (bit 5) while LSOFT still held (no release bit yet) */
    keypad_test_raw = 1u << 5;
    keypad_test_status = (0x01u << 8) | (0x13u << 24);
    CHECK(keypad_poll() == (KEY_DIAL | (int)KEY_EVENT_UP_FLAG),
          "release DIAL (bit 5) emits DIAL up event");
    CHECK(q_count(&keypad_queue) == 1, "DIAL release pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 &&
          v == (KEY_DIAL | KEY_EVENT_UP_FLAG),
          "queue pops DIAL up event");

    /* release LSOFT (bit 7) -> up event; queue fully drained */
    keypad_test_raw = 1u << 7;
    keypad_test_status = 0x13u << 24;
    CHECK(keypad_poll() == (KEY_LSOFT | (int)KEY_EVENT_UP_FLAG),
          "release LSOFT (bit 7) emits LSOFT up event");
    CHECK(q_pop(&keypad_queue, &v) == 0 &&
          v == (KEY_LSOFT | KEY_EVENT_UP_FLAG),
          "queue pops LSOFT up event (after DIAL release)");

    /* EIC power button (the B310E hangup/END — not in the matrix): the
     * pure edge helpers plus keypad_poll() integration via injected pb.
     * keypad_pb_event_for_test classifies BOTH edges (END_Down / END_Up);
     * the old keypad_pb_to_end_for_test stays as the press-edge-only
     * back-compat shape. */
    CHECK(keypad_pb_event_for_test(1, 0) == KEY_END,
          "pb 0->1 edge -> END_Down (KEY_END)");
    CHECK(keypad_pb_event_for_test(0, 1) == (KEY_END | (int)KEY_EVENT_UP_FLAG),
          "pb 1->0 edge -> END_Up (KEY_END|FLAG)");
    CHECK(keypad_pb_event_for_test(1, 1) == 0, "pb held -> no repeat");
    CHECK(keypad_pb_event_for_test(0, 0) == 0, "pb idle -> no event");
    CHECK(keypad_pb_to_end_for_test(1, 0) == KEY_END,
          "old helper: 0->1 edge -> KEY_END");
    CHECK(keypad_pb_to_end_for_test(0, 1) == 0,
          "old helper: release -> no event");
    CHECK(keypad_pb_to_end_for_test(1, 1) == 0,
          "old helper: held -> no repeat");
    CHECK(keypad_pb_to_end_for_test(0, 0) == 0,
          "old helper: idle -> no event");

    /* poll integration: matrix inputs cleared so the EIC is the only
     * source; keypad_poll() reads keypad_test_pb under HOST_TEST. */
    keypad_test_raw = 0;
    keypad_test_status = 0;
    keypad_test_pb = 0;
    CHECK(keypad_poll() == 0, "pb idle -> poll returns 0");
    keypad_test_pb = 1;
    CHECK(keypad_poll() == KEY_END, "pb press edge -> poll returns END_Down");
    CHECK(q_count(&keypad_queue) == 1, "pb press pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_END,
          "queue pops KEY_END (down)");
    CHECK(keypad_poll() == 0, "pb held -> no repeat");
    CHECK(q_count(&keypad_queue) == 0, "pb held -> no push");
    keypad_test_pb = 0;
    CHECK(keypad_poll() == (KEY_END | (int)KEY_EVENT_UP_FLAG),
          "pb release edge -> poll returns END_Up");
    CHECK(q_count(&keypad_queue) == 1, "pb release pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 &&
          v == (KEY_END | KEY_EVENT_UP_FLAG),
          "queue pops KEY_END|FLAG (up)");
    keypad_test_pb = 1;
    CHECK(keypad_poll() == KEY_END, "pb re-press after release emits again");
    CHECK(q_count(&keypad_queue) == 1, "pb re-press pushed to queue");
    CHECK(q_pop(&keypad_queue, &v) == 0 && v == KEY_END,
          "queue pops KEY_END again");
    keypad_test_pb = 0;
    CHECK(keypad_poll() == (KEY_END | (int)KEY_EVENT_UP_FLAG),
          "pb release after re-press emits up event");
    CHECK(q_pop(&keypad_queue, &v) == 0 &&
          v == (KEY_END | KEY_EVENT_UP_FLAG),
          "queue pops END_Up after re-press");

    /* event encoding helpers + names: low byte = KEY_* code, bit 8 marks
     * release, and names carry the _DOWN/_UP suffix */
    CHECK(keypad_event_is_up((uint32_t)KEY_LSOFT) == 0,
          "event_is_up on plain code is 0");
    CHECK(keypad_event_is_up((uint32_t)KEY_LSOFT | KEY_EVENT_UP_FLAG) == 1,
          "event_is_up on flagged event is 1");
    CHECK(keypad_event_code((uint32_t)KEY_LSOFT | KEY_EVENT_UP_FLAG) ==
          KEY_LSOFT, "event_code strips the up flag");
    CHECK(keypad_event_code((uint32_t)KEY_5) == KEY_5,
          "event_code passes plain codes through");
    CHECK(strcmp(key_name_event((uint32_t)KEY_LSOFT), "LSOFT_DOWN") == 0,
          "key_name_event LSOFT_DOWN");
    CHECK(strcmp(key_name_event((uint32_t)KEY_LSOFT | KEY_EVENT_UP_FLAG),
                 "LSOFT_UP") == 0, "key_name_event LSOFT_UP");
    CHECK(strcmp(key_name_event((uint32_t)KEY_END), "END_DOWN") == 0,
          "key_name_event END_DOWN");
    CHECK(strcmp(key_name_event((uint32_t)KEY_END | KEY_EVENT_UP_FLAG),
                 "END_UP") == 0, "key_name_event END_UP");
    CHECK(strcmp(key_name_event((uint32_t)KEY_5), "5_DOWN") == 0,
          "key_name_event 5_DOWN");
    CHECK(strcmp(key_name_event((uint32_t)KEY_5 | KEY_EVENT_UP_FLAG),
                 "5_UP") == 0, "key_name_event 5_UP");
    CHECK(strcmp(key_name_event(0x7fu | KEY_EVENT_UP_FLAG), "?_UP") == 0,
          "key_name_event unknown base keeps the suffix");
}

/* ---- IRQ dispatch table test ------------------------------------------- */

static int s_irq_calls[32];
static int s_irq_order[32];
static int s_irq_order_idx;

static void irq_test_handler_n(int n)
{
    s_irq_calls[n]++;
    s_irq_order[s_irq_order_idx++] = n;
}

/* irq_register takes plain void(*)(void) — bind the line into the record */
static void irq_test_handler_3(void) { irq_test_handler_n(3); }
static void irq_test_handler_5(void) { irq_test_handler_n(5); }

static void test_irq(void)
{
    printf("--- IRQ dispatch table ---\n");

    CHECK(irq_line_from_pending(0) == -1, "no pending -> -1");
    CHECK(irq_line_from_pending(1u) == 0, "pending bit 0 -> line 0");
    CHECK(irq_line_from_pending(0x80000000u) == 31, "pending bit 31 -> line 31");
    CHECK(irq_line_from_pending(0x104u) == 2, "lowest set bit wins (0x104 -> 2)");

    CHECK(irq_dispatch_for_test(0) == -1, "dispatch with nothing pending -> -1");

    /* unregistered line: dispatch skips it (no handler to ack the source)
     * and returns -1 — nothing ran */
    memset(s_irq_calls, 0, sizeof(s_irq_calls));
    CHECK(irq_dispatch_for_test(1u << 7) == -1, "unregistered line skipped (-1)");
    CHECK(s_irq_calls[3] == 0 && s_irq_calls[5] == 0,
          "no handler called for an unregistered line");

    /* registered handler runs and dispatch returns its line */
    irq_register(5, irq_test_handler_5);
    s_irq_order_idx = 0;
    CHECK(irq_dispatch_for_test(1u << 5) == 5, "registered line 5 dispatches");
    CHECK(s_irq_calls[5] == 1, "line 5 handler ran");
    CHECK(s_irq_order_idx == 1 && s_irq_order[0] == 5, "dispatch order records line 5");

    /* an unhandled LOW line must not starve a registered HIGH line: the
     * SC6530 INTC pending register reflects disabled lines (keypad/EIC
     * edges vs the 1ms timer line 23), and dispatching the NULL line would
     * leave the timer source un-acked -> IRQ re-entry storm (the freeze) */
    memset(s_irq_calls, 0, sizeof(s_irq_calls));
    s_irq_order_idx = 0;
    CHECK(irq_dispatch_for_test((1u << 3) | (1u << 5)) == 5,
          "unhandled low line skipped, registered line 5 dispatches");
    CHECK(s_irq_calls[5] == 1, "line 5 handler ran despite unhandled line 3");
    CHECK(s_irq_order_idx == 1 && s_irq_order[0] == 5, "dispatch order records line 5");

    /* lowest pending REGISTERED line wins when several are set */
    irq_register(3, irq_test_handler_3);
    s_irq_order_idx = 0;
    CHECK(irq_dispatch_for_test((1u << 3) | (1u << 5)) == 3, "lowest pending line dispatches");
    CHECK(s_irq_calls[3] == 1 && s_irq_calls[5] == 1, "only line 3 handler ran");
    CHECK(s_irq_order_idx == 1 && s_irq_order[0] == 3, "dispatch order records line 3");

    /* unregister works */
    irq_register(5, NULL);
    s_irq_calls[5] = 0;
    CHECK(irq_dispatch_for_test(1u << 5) == -1 && s_irq_calls[5] == 0,
          "unregistered line 5 skipped (-1), not called");

    /* out-of-range registers are rejected */
    irq_register(32, irq_test_handler_5);
    irq_register(-1, irq_test_handler_5);
    CHECK(irq_dispatch_for_test(1u << 9) == -1 && s_irq_calls[5] == 0,
          "out-of-range register calls are no-ops");
}

/* ---- battery fuel gauge (pure mV->percent logic) ------------------------ */
/* battery_level_percent() is the host-testable half of the fuel-gauge
 * driver (the ADC/ADI hardware path compiles out under HOST_TEST). The
 * map is linear over 3.3 V..4.2 V; these checks pin the edges + midpoint. */

static void test_battery(void)
{
    printf("test_battery\n");

    CHECK(battery_level_percent(0) == 0, "0 mV -> 0%");
    CHECK(battery_level_percent(3300) == 0, "3.3 V -> 0% (empty)");
    CHECK(battery_level_percent(3299) == 0, "just below 3.3 V clamps to 0%");
    CHECK(battery_level_percent(4200) == 100, "4.2 V -> 100% (full)");
    CHECK(battery_level_percent(5000) == 100, "above 4.2 V clamps to 100%");
    CHECK(battery_level_percent(3300 + 450) == 50,
          "3.75 V (midpoint) -> 50%");
    CHECK(battery_level_percent(3300 + 900) == 100,
          "4.2 V boundary -> 100%");
    CHECK(battery_level_percent(3300 + 90) == 10, "3.39 V -> 10%");

    /* battery_mv_from_raw: the HW-calibrated ADC mapping (raw*4390)/1000 —
     * full-scale 1023 -> 4491 mV, a 4.15V cell at raw ~946. */
    CHECK(battery_mv_from_raw(0) == 0, "raw 0 -> 0 mV");
    CHECK(battery_mv_from_raw(1023) == 4491, "raw 1023 (full-scale) -> 4491 mV");
    CHECK(battery_mv_from_raw(1000) == 4390, "raw 1000 -> 4390 mV");
    CHECK(battery_mv_from_raw(946) == 4153, "raw 946 -> 4153 mV (~4.15V cell)");
}

/* ---- scheduler v2: sched_preempt_check (parked-frame pick) ------------- */

static void test_preempt_check(void)
{
    printf("--- scheduler v2 (sched_preempt_check) ---\n");

    static uint64_t preempt_heap[1024];   /* 8 KiB */

    kmem_init(preempt_heap, sizeof(preempt_heap));

    /* no tick armed -> never a preempt target */
    CHECK(sched_preempt_check() == -1, "v2: no tick -> sched_preempt_check() == -1");

    /* 3 READY tasks; force a tick; the check must pick one and stage the
     * cur/next sp pointers for the asm dance. */
    int ids[3];
    for (int i = 0; i < 3; i++)
        ids[i] = task_create("p", sched_test_task, (void *)(intptr_t)i, 512);
    CHECK(ids[0] > 0 && ids[1] > ids[0] && ids[2] > ids[1],
          "v2: three READY tasks created");

    sched_test_force_preempt();
    {
        int idx = sched_preempt_check();

        CHECK(idx >= 0, "v2: forced tick returns a preempt target");
        CHECK(s_preempt_cur_sp != NULL && s_preempt_next_sp != NULL,
              "v2: cur/next sp pointers staged for the dance");
        CHECK(sched_preempt_check() == -1,
              "v2: s_need_resched consumed -> second call -1");

        /* the picked task is now RUNNING; a second forced pick must advance
         * round-robin to a DIFFERENT task */
        sched_test_force_preempt();
        CHECK(sched_preempt_check() != idx,
              "v2: second pick advances round-robin");
    }
}

/* ---- main -------------------------------------------------------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* see output even on crash */

    printf("B310E-OS kernel host tests\n\n");

    test_scheduler();
    test_preempt_check();
    test_queue();
    test_module();
    test_malloc();
    test_kprintf();
    test_keypad();
    test_usb_debug_frame();
    test_irq();
    test_battery();

    printf("\n");
    if (s_failures == 0) {
        printf("ALL TESTS PASSED (%d checks)\n", s_checks);
        return 0;
    }
    printf("%d of %d checks FAILED\n", s_failures, s_checks);
    return 1;
}
