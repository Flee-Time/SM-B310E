/* B310E OS — semaphores, mutexes with priority inheritance, queues.
 *
 * Before kernel_start() there is only one flow of control: mutexes are
 * no-ops then and waits do not block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "kernel/internal.h"

/* ---- semaphore ------------------------------------------------------------ */

void sem_init(sem_t *s, int count)
{
    s->count = count;
    s->waiters.head = NULL;
}

int sem_wait(sem_t *s, uint32_t timeout_ms)
{
    uint32_t irq = arch_irq_save();
    int r = WAIT_OK;

    if (s->count > 0)
        s->count--;
    else if (timeout_ms == 0 || !kernel_running() || arch_in_irq())
        r = WAIT_TIMEOUT;
    else
        r = k_block(&s->waiters, timeout_ms);
    arch_irq_restore(irq);
    return r;
}

void sem_post(sem_t *s)
{
    uint32_t irq = arch_irq_save();

    if (s->waiters.head)
        k_wake(s->waiters.head, WAIT_OK);
    else
        s->count++;
    k_preempt();
    arch_irq_restore(irq);
}

/* ---- mutex ------------------------------------------------------------------ */

void mutex_init(mutex_t *m)
{
    m->owner = NULL;
    m->depth = 0;
    m->waiters.head = NULL;
    m->next_held = NULL;
}

static void held_push(thread_t *t, mutex_t *m)
{
    m->next_held = t->held;
    t->held = m;
}

static void held_remove(thread_t *t, mutex_t *m)
{
    for (mutex_t **pp = &t->held; *pp; pp = &(*pp)->next_held) {
        if (*pp == m) {
            *pp = m->next_held;
            break;
        }
    }
    m->next_held = NULL;
}

/* The priority a thread should run at: its own, or that of the highest
 * thread waiting for a mutex it holds. */
int k_mutex_ceiling(thread_t *t)
{
    int p = t->base_prio;

    for (mutex_t *m = t->held; m; m = m->next_held)
        if (m->waiters.head && m->waiters.head->prio > p)
            p = m->waiters.head->prio;
    return p;
}

void k_mutex_inherit(mutex_t *m, int prio)
{
    if (m->owner && prio > m->owner->prio)
        k_set_prio(m->owner, prio);
}

int mutex_lock(mutex_t *m, uint32_t timeout_ms)
{
    uint32_t irq;
    thread_t *cur;
    int r;

    if (!kernel_running())
        return WAIT_OK;
    irq = arch_irq_save();
    cur = kernel_current;
    if (!m->owner) {
        m->owner = cur;
        m->depth = 1;
        held_push(cur, m);
        arch_irq_restore(irq);
        return WAIT_OK;
    }
    if (m->owner == cur) {
        m->depth++;
        arch_irq_restore(irq);
        return WAIT_OK;
    }
    if (timeout_ms == 0 || arch_in_irq()) {
        arch_irq_restore(irq);
        return WAIT_TIMEOUT;
    }
    cur->blocked_on = m;
    k_mutex_inherit(m, cur->prio);
    r = k_block(&m->waiters, timeout_ms);
    cur->blocked_on = NULL;
    if (r != WAIT_OK && m->owner)
        k_set_prio(m->owner, k_mutex_ceiling(m->owner));
    arch_irq_restore(irq);
    return r;
}

void mutex_unlock(mutex_t *m)
{
    uint32_t irq;
    thread_t *cur, *w;

    if (!kernel_running())
        return;
    irq = arch_irq_save();
    cur = kernel_current;
    if (m->owner != cur || --m->depth) {
        arch_irq_restore(irq);
        return;
    }
    held_remove(cur, m);
    w = m->waiters.head;
    if (w) {
        m->owner = w;
        m->depth = 1;
        w->blocked_on = NULL;
        k_wake(w, WAIT_OK);
        held_push(w, m);
        k_set_prio(w, k_mutex_ceiling(w));
    } else {
        m->owner = NULL;
    }
    k_set_prio(cur, k_mutex_ceiling(cur));
    if (cur->kill_pending && !cur->held)
        thread_exit();
    k_preempt();
    arch_irq_restore(irq);
}

void k_mutex_drop(thread_t *t, mutex_t *m)
{
    thread_t *w = m->waiters.head;

    held_remove(t, m);
    m->depth = 0;
    if (w) {
        m->owner = w;
        m->depth = 1;
        w->blocked_on = NULL;
        k_wake(w, WAIT_OK);
        held_push(w, m);
        k_set_prio(w, k_mutex_ceiling(w));
    } else {
        m->owner = NULL;
    }
}

/* ---- message queue -------------------------------------------------------- */

int queue_init(queue_t *q, size_t msg_size, size_t capacity)
{
    q->buf = kmalloc(msg_size * capacity);
    if (!q->buf)
        return -1;
    q->msg_size = msg_size;
    q->capacity = capacity;
    q->head = q->count = 0;
    sem_init(&q->items, 0);
    q->senders.head = NULL;
    return 0;
}

static void put(queue_t *q, const void *msg)
{
    size_t tail = (q->head + q->count) % q->capacity;

    memcpy(q->buf + tail * q->msg_size, msg, q->msg_size);
    q->count++;
}

int queue_send(queue_t *q, const void *msg, uint32_t timeout_ms)
{
    uint32_t irq = arch_irq_save();

    while (q->count == q->capacity) {
        int r = WAIT_TIMEOUT;

        if (timeout_ms && kernel_running() && !arch_in_irq())
            r = k_block(&q->senders, timeout_ms);
        if (r != WAIT_OK) {
            arch_irq_restore(irq);
            return r;
        }
    }
    put(q, msg);
    arch_irq_restore(irq);
    sem_post(&q->items);
    return WAIT_OK;
}

bool queue_send_isr(queue_t *q, const void *msg)
{
    uint32_t irq = arch_irq_save();
    bool ok = q->count < q->capacity;

    if (ok)
        put(q, msg);
    arch_irq_restore(irq);
    if (ok)
        sem_post(&q->items);
    return ok;
}

int queue_recv(queue_t *q, void *msg, uint32_t timeout_ms)
{
    uint32_t irq;
    int r = sem_wait(&q->items, timeout_ms);

    if (r != WAIT_OK)
        return r;
    irq = arch_irq_save();
    memcpy(msg, q->buf + q->head * q->msg_size, q->msg_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    if (q->senders.head)
        k_wake(q->senders.head, WAIT_OK);
    k_preempt();
    arch_irq_restore(irq);
    return WAIT_OK;
}
