/* B310E OS — milestone 1: boot, kernel self-test, status, keys.
 *
 * os_status is a fixed structure the QEMU tests read through the ELF
 * symbol table: ticks, idle share, self-test results, keys seen.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "build_info.h"

extern char __heap_start[], __heap_end[];

#define OS_MAGIC 0x534f3342u        /* "B3OS" */

/* self-test result bits */
#define ST_SLEEP   (1u << 0)
#define ST_SEM     (1u << 1)
#define ST_MUTEX   (1u << 2)
#define ST_QUEUE   (1u << 3)
#define ST_HEAP    (1u << 4)
#define ST_ALL     0x1fu

volatile struct {
    uint32_t magic;
    uint32_t ticks;
    uint32_t idle_permille;
    uint32_t selftest_done;         /* bits that ran */
    uint32_t selftest_pass;         /* bits that passed */
    uint32_t pi_wait_ms;            /* high thread's wait behind the mutex */
    uint32_t sem_rounds_per_s;
    uint32_t keys;
    uint32_t last_key;              /* code | down << 8 */
    uint32_t worker_loops;
    uint32_t heap_free;
    uint32_t stray_irqs;
    uint32_t cycles;                /* all accounted cycles */
    uint32_t idle_cycles;
} os_status __attribute__((used));

/* ---- kernel self-test ----------------------------------------------------- */

static void report(uint32_t bit, bool ok, const char *what)
{
    os_status.selftest_done |= bit;
    if (ok)
        os_status.selftest_pass |= bit;
    console_set_color(ok ? RGB(120, 255, 120) : RGB(255, 90, 90), RGB(0, 0, 0));
    console_printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    console_set_color(RGB(255, 255, 255), RGB(0, 0, 0));
    console_flush();
}

static sem_t ping, pong;
static volatile int pong_rounds;

static void pong_main(void *arg)
{
    for (int i = 0; i < 2000; i++) {
        sem_wait(&ping, WAIT_FOREVER);
        pong_rounds++;
        sem_post(&pong);
    }
}

static mutex_t pi_mutex;
static volatile uint32_t pi_wait;
static volatile bool pi_done, pi_medium_stop;

static void busy_ms(uint32_t ms)
{
    uint32_t end = kernel_ticks() + ms;

    while ((int32_t)(kernel_ticks() - end) < 0)
        ;
}

static void pi_low(void *arg)       /* holds the mutex for 50 ms of work */
{
    mutex_lock(&pi_mutex, WAIT_FOREVER);
    thread_sleep(5);                /* let high and medium start */
    busy_ms(50);
    mutex_unlock(&pi_mutex);
}

static void pi_medium(void *arg)    /* would starve low without inheritance */
{
    uint32_t t0 = kernel_ticks();

    while (!pi_medium_stop && kernel_ticks() - t0 < 300)
        ;
}

static void pi_high(void *arg)
{
    uint32_t t0 = kernel_ticks();

    mutex_lock(&pi_mutex, WAIT_FOREVER);
    pi_wait = kernel_ticks() - t0;
    mutex_unlock(&pi_mutex);
    pi_medium_stop = true;
    pi_done = true;
}

static void selftest(void)
{
    uint32_t t0, dt;

    /* sleep: 100 ms within 2 ms */
    t0 = kernel_ticks();
    thread_sleep(100);
    dt = kernel_ticks() - t0;
    report(ST_SLEEP, dt >= 100 && dt <= 102, "sleep 100 ms");

    /* semaphore ping-pong with a second thread */
    sem_init(&ping, 0);
    sem_init(&pong, 0);
    pong_rounds = 0;
    thread_create("pong", PRIO_NORMAL + 1, 1024, pong_main, NULL);
    t0 = kernel_ticks();
    for (int i = 0; i < 2000; i++) {
        sem_post(&ping);
        if (sem_wait(&pong, 100) != WAIT_OK)
            break;
    }
    dt = kernel_ticks() - t0;
    os_status.sem_rounds_per_s = dt ? 2000u * 1000u / dt : 0;
    report(ST_SEM, pong_rounds == 2000, "semaphore x2000");

    /* priority inheritance: low holds, high waits, medium spins */
    mutex_init(&pi_mutex);
    pi_done = pi_medium_stop = false;
    thread_create("pi-low", PRIO_LOW, 1024, pi_low, NULL);
    thread_sleep(2);                /* low takes the mutex */
    thread_create("pi-med", PRIO_NORMAL + 2, 1024, pi_medium, NULL);
    thread_create("pi-high", PRIO_NORMAL + 4, 1024, pi_high, NULL);
    t0 = kernel_ticks();
    while (!pi_done && kernel_ticks() - t0 < 500)
        thread_sleep(10);
    pi_medium_stop = true;
    os_status.pi_wait_ms = pi_wait;
    report(ST_MUTEX, pi_done && pi_wait < 80, "mutex inherit");

    /* queue: 100 messages in order through a 4-slot queue */
    {
        queue_t q;
        bool ok = queue_init(&q, sizeof(uint32_t), 4) == 0;

        for (uint32_t i = 0; ok && i < 100; i++) {
            uint32_t v;
            ok = queue_send(&q, &i, 10) == WAIT_OK &&
                 queue_recv(&q, &v, 10) == WAIT_OK && v == i;
        }
        kfree(q.buf);
        report(ST_QUEUE, ok, "queue order");
    }

    /* heap: free bytes come back after a mixed alloc/free pattern */
    {
        size_t before = heap_free_bytes();
        void *p[16];

        for (int i = 0; i < 16; i++)
            p[i] = kmalloc(32u << (i % 6));
        for (int i = 0; i < 16; i += 2)
            kfree(p[i]);
        for (int i = 1; i < 16; i += 2)
            kfree(p[i]);
        report(ST_HEAP, heap_free_bytes() == before, "heap reuse");
    }
}

/* ---- status line and keys ------------------------------------------------- */

static void status_main(void *arg)
{
    for (;;) {
        uint32_t t = kernel_ticks(), idle = kernel_idle_permille();

        os_status.ticks = t;
        os_status.idle_permille = idle;
        os_status.heap_free = heap_free_bytes();
        os_status.stray_irqs = irq_stray_count;
        os_status.cycles = kernel_cycles();
        for (thread_t *th = thread_first(); th; th = thread_next(th))
            if (th->prio == PRIO_IDLE)
                os_status.idle_cycles = th->cycles;
        console_line(1, "up %lu.%lus idle %lu.%lu%%",
                     (unsigned long)(t / 1000), (unsigned long)(t / 100 % 10),
                     (unsigned long)(idle / 10), (unsigned long)(idle % 10));
        console_line(2, "heap %luK keys %lu",
                     (unsigned long)(heap_free_bytes() / 1024),
                     (unsigned long)os_status.keys);
        console_flush();
        thread_sleep(250);
    }
}

static volatile bool worker_on;

static void worker_main(void *arg)  /* background load, toggled with 5 */
{
    for (;;) {
        if (worker_on)
            os_status.worker_loops++;
        else
            thread_sleep(50);
    }
}

static void main_thread(void *arg)
{
    uint32_t end_down = 0;
    key_event_t ev;

    console_set_color(RGB(255, 210, 80), RGB(0, 0, 0));
    console_line(0, "B310E OS 0.1");
    console_set_color(RGB(255, 255, 255), RGB(0, 0, 0));
    console_printf("built %s\n", BUILD_DATE);
    selftest();
    console_printf("keys echo below\n5: load on/off\nhold End: reboot\n");
    console_flush();

    thread_create("status", PRIO_NORMAL, 1536, status_main, NULL);
    thread_create("worker", PRIO_LOW, 1024, worker_main, NULL);

    for (;;) {
        if (keypad_get(&ev, 100) == WAIT_OK) {
            os_status.keys++;
            os_status.last_key = ev.code | (uint32_t)ev.down << 8;
            console_printf("%s %s\n", keypad_name(ev.code), ev.down ? "down" : "up");
            if (ev.code == KEY_5 && ev.down) {
                worker_on = !worker_on;
                console_printf("load %s\n", worker_on ? "on" : "off");
            }
            if (ev.code == KEY_END)
                end_down = ev.down ? ev.time : 0;
            console_flush();
        }
        if (end_down && kernel_ticks() - end_down > 2000) {
            console_printf("rebooting\n");
            console_flush();
            backlight_set(0);
            arch_reboot();
        }
    }
}

void kmain(void)
{
    watchdog_stop();
    heap_init(__heap_start, __heap_end);
    os_status.magic = OS_MAGIC;

    lcd_init();
    console_init();
    console_reserve(3);
    backlight_set(60);
    console_line(0, "B310E OS 0.1");
    lcd_update();

    timer_init(1000, kernel_tick);
    keypad_init();
    thread_create("main", PRIO_NORMAL, 4096, main_thread, NULL);
    kernel_start();
}
