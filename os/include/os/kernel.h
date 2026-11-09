/* B310E OS — kernel interface.
 *
 * Preemptive, priority-based threads (0 = idle .. 31 = highest), round
 * robin within a priority, one 1 ms tick. Objects: semaphore, mutex with
 * priority inheritance, fixed-size message queue. All waits take a
 * timeout in milliseconds (WAIT_FOREVER, or 0 to poll).
 *
 * Calls marked "irq-safe" may be used from interrupt handlers; nothing
 * else may.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_KERNEL_H
#define OS_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRIO_IDLE     0
#define PRIO_LOW      4
#define PRIO_NORMAL   12
#define PRIO_HIGH     20
#define PRIO_MAX      31

#define WAIT_FOREVER  0xffffffffu

typedef struct thread thread_t;
typedef void (*thread_fn)(void *arg);

/* A wait list: threads blocked on one object, highest priority first. */
typedef struct {
    thread_t *head;
} waitq_t;

struct thread {
    uint32_t *sp;               /* must stay first: the context switch */
    thread_t *next, *prev;      /* ready list or wait list */
    thread_t *tnext;            /* timeout list */
    uint32_t wake;              /* tick to wake at */
    uint8_t prio;               /* effective (raised by inheritance) */
    uint8_t base_prio;
    uint8_t state;
    int8_t result;              /* wait result: 0, or WAIT_TIMEOUT */
    uint8_t timed;              /* on the timeout list */
    waitq_t *waitq;             /* the list it blocks on */
    struct mutex *blocked_on;   /* for priority inheritance chains */
    struct mutex *held;         /* mutexes it owns */
    const char *name;
    uint32_t *stack;
    size_t stack_size;
    uint32_t cycles;            /* 26 MHz cycles run (statistics) */
    thread_t *all_next;         /* every thread, for listings */
    void *owner;                /* the app that started it, or NULL */
    bool kill_pending;          /* ends when it lets go of its last mutex */
};

#define WAIT_OK       0
#define WAIT_TIMEOUT  (-1)

/* threads */
thread_t *thread_create(const char *name, int prio, size_t stack_size,
                        thread_fn fn, void *arg);
thread_t *thread_create_owned(const char *name, int prio, size_t stack_size,
                            thread_fn fn, void *arg, void *owner);
void thread_sleep(uint32_t ms);
void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));
/* Called (interrupts off) when a thread with an owner (an app's) ends. */
extern void (*thread_owned_exit)(void);
thread_t *thread_self(void);
/* End an app's threads wherever they are. A mutex in [lo, hi) (the app's own
 * memory) is let go; one of the OS's means the thread is inside the OS with
 * something half done: it ends when it lets go of that. How many are left. */
int thread_kill_owned(const void *owner, const void *lo, const void *hi);
int thread_count_owned(const void *owner);  /* threads of an app still running */
void thread_set_priority(thread_t *t, int prio);
thread_t *thread_first(void);   /* walk all threads (interrupts off) */
thread_t *thread_next(thread_t *t);
uint32_t kernel_cycles(void);   /* 26 MHz cycles accounted since boot */

/* time */
uint32_t kernel_ticks(void);    /* milliseconds since boot (irq-safe) */
void kernel_start(void) __attribute__((noreturn));
bool kernel_running(void);
uint32_t kernel_idle_permille(void);  /* idle share of the last second */

/* semaphore */
typedef struct {
    int count;
    waitq_t waiters;
} sem_t;

void sem_init(sem_t *s, int count);
int sem_wait(sem_t *s, uint32_t timeout_ms);
void sem_post(sem_t *s);                       /* irq-safe */

/* mutex: owner-recursive, priority inheritance */
typedef struct mutex {
    thread_t *owner;
    unsigned depth;
    waitq_t waiters;
    struct mutex *next_held;    /* owner's held list */
} mutex_t;

void mutex_init(mutex_t *m);
int mutex_lock(mutex_t *m, uint32_t timeout_ms);
void mutex_unlock(mutex_t *m);

/* message queue of fixed-size messages */
typedef struct {
    uint8_t *buf;
    size_t msg_size, capacity, head, count;
    sem_t items;                /* messages waiting */
    waitq_t senders;            /* blocked on a full queue */
} queue_t;

int queue_init(queue_t *q, size_t msg_size, size_t capacity);
int queue_send(queue_t *q, const void *msg, uint32_t timeout_ms);
bool queue_send_isr(queue_t *q, const void *msg);   /* irq-safe, no wait */
int queue_recv(queue_t *q, void *msg, uint32_t timeout_ms);

/* memory */
void heap_init(void *start, void *end);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
size_t heap_free_bytes(void);
void heap_stats(size_t *free, size_t *largest, unsigned *blocks);

/* internal, used by the architecture layer */
extern thread_t *kernel_current;
void kernel_reschedule(void);   /* svc and irq exit, interrupts off */
void kernel_irq_exit(void);
void kernel_tick(void);
void kernel_irq_entry(void);        /* every interrupt, first */
void kernel_set_tickless(bool on);  /* tickless idle (on by default) */
int cpufreq_holds(void);            /* holds keeping the clock up (the drain log) */

/* CPU clock governor (kernel/cpufreq.c) */
enum { CPUFREQ_AUTO, CPUFREQ_MAX };
void cpufreq_set_mode(int mode);
void cpufreq_hold(void);            /* at least 104 MHz until released */
void cpufreq_release(void);
void cpufreq_allow_low(bool on);    /* 26 MHz allowed (the panel asleep) */
void cpufreq_window(uint32_t busy_permille);   /* the tick, every 50 ms */

#endif
