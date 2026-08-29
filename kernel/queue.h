/*
 * B310E-OS — kernel/queue.h
 *
 * Fixed-size ring message queue. Stores 32-bit messages; no dynamic
 * allocation, no blocking in v1 (full push and empty pop return -1).
 * FIFO order is preserved, including across the head/tail wrap.
 */

#ifndef B310E_OS_QUEUE_H
#define B310E_OS_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *buf;                   /* caller-provided slot array */
    uint32_t size;                   /* number of slots */
    uint32_t head;                   /* next slot to pop           */
    uint32_t tail;                   /* next slot to push          */
    uint32_t count;                  /* messages currently stored  */
} msg_queue_t;

void q_init(msg_queue_t *q, uint32_t *buf, uint32_t size);
int q_push(msg_queue_t *q, uint32_t msg);   /* 0 ok, -1 full */
int q_pop(msg_queue_t *q, uint32_t *out);   /* 0 ok, -1 empty */
uint32_t q_count(msg_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_QUEUE_H */
