/* B310E OS — kernel heap: first fit, address-ordered free list with
 * coalescing, 8-byte aligned blocks. Interrupts off while the list
 * changes (short); never call from an interrupt handler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"
#include "os/lib.h"

typedef struct block {
    size_t size;                /* whole block, header included */
    struct block *next;         /* free list only */
} block_t;

#define HDR       sizeof(block_t)
#define MIN_SPLIT 32

static block_t *free_list;
static size_t free_bytes;

void heap_init(void *start, void *end)
{
    uintptr_t s = ((uintptr_t)start + 7) & ~7u, e = (uintptr_t)end & ~7u;

    free_list = (block_t *)s;
    free_list->size = e - s;
    free_list->next = NULL;
    free_bytes = e - s;
}

void *kmalloc(size_t size)
{
    size_t need = (size + HDR + 7) & ~7u;
    block_t **pp, *b = NULL;
    uint32_t irq;

    if (!size)
        return NULL;
    irq = arch_irq_save();
    for (pp = &free_list; *pp; pp = &(*pp)->next) {
        if ((*pp)->size >= need) {
            b = *pp;
            if (b->size - need >= MIN_SPLIT) {
                block_t *rest = (block_t *)((uint8_t *)b + need);
                rest->size = b->size - need;
                rest->next = b->next;
                *pp = rest;
                b->size = need;
            } else {
                *pp = b->next;
            }
            free_bytes -= b->size;
            break;
        }
    }
    arch_irq_restore(irq);
    return b ? (uint8_t *)b + HDR : NULL;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    block_t *b, **pp, *prev = NULL;
    uint32_t irq;

    if (!p)
        return;
    b = (block_t *)((uint8_t *)p - HDR);
    irq = arch_irq_save();
    free_bytes += b->size;
    for (pp = &free_list; *pp && *pp < b; pp = &(*pp)->next)
        prev = *pp;
    b->next = *pp;
    *pp = b;
    if (b->next && (uint8_t *)b + b->size == (uint8_t *)b->next) {
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (prev && (uint8_t *)prev + prev->size == (uint8_t *)b) {
        prev->size += b->size;
        prev->next = b->next;
    }
    arch_irq_restore(irq);
}

size_t heap_free_bytes(void)
{
    return free_bytes;
}
