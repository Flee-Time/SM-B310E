/*
 * B310E-OS — kernel/malloc.c
 *
 * Bump allocator. Allocations carve forward through a fixed pool; there is
 * no free() — v1 kernel objects (TCB stacks, module tables) are never
 * released, so fragmentation cannot occur. kmem_init() may be called again
 * to reset the pool (host tests use this between test groups).
 */

#include "malloc.h"

#include <stddef.h>

static uint8_t *s_pool_base;        /* first usable byte (8-aligned)      */
static uint8_t *s_pool_end;         /* one past the last usable byte      */
static uint8_t *s_bump;             /* next allocation address             */

void kmem_init(void *base, uint32_t size)
{
    uintptr_t p = (uintptr_t)base;
    uintptr_t aligned = (p + 7u) & ~(uintptr_t)7u;
    uint32_t align_loss = (uint32_t)(aligned - p);
    uint32_t avail = (size > align_loss) ? (size - align_loss) : 0;

    s_pool_base = (uint8_t *)aligned;
    s_pool_end = s_pool_base + avail;
    s_bump = s_pool_base;
}

void *kmalloc(uint32_t size)
{
    if (size == 0)
        size = 1;                    /* always hand out a usable slot */
    uint32_t need = (size + 7u) & ~7u;

    uint8_t *p = s_bump;
    uint8_t *q = p + need;
    if (q > s_pool_end || q < p)     /* exhausted, or 32-bit wrap-around */
        return NULL;

    s_bump = q;
    return p;
}

uint32_t kmem_free_bytes(void)
{
    return (uint32_t)(s_pool_end - s_bump);
}
