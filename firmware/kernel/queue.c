/*
 * B310E-OS — kernel/queue.c
 *
 * Ring queue implementation (see queue.h). Plain FIFO, no locks — the
 * cooperative scheduler guarantees a single producer/consumer per queue
 * in v1 (message passing between tasks happens only at yield points).
 */

#include "queue.h"

void q_init(msg_queue_t *q, uint32_t *buf, uint32_t size)
{
    if (q == NULL)
        return;
    q->buf = buf;
    q->size = size;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

int q_push(msg_queue_t *q, uint32_t msg)
{
    if (q == NULL || q->buf == NULL || q->size == 0)
        return -1;
    if (q->count == q->size)         /* full */
        return -1;

    q->buf[q->tail] = msg;
    q->tail = (q->tail + 1) % q->size;
    q->count++;
    return 0;
}

int q_pop(msg_queue_t *q, uint32_t *out)
{
    if (q == NULL || out == NULL)
        return -1;
    if (q->count == 0)               /* empty */
        return -1;

    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->size;
    q->count--;
    return 0;
}

uint32_t q_count(msg_queue_t *q)
{
    return (q == NULL) ? 0 : q->count;
}
