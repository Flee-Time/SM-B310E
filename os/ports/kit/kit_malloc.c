/* The game kit's malloc: the game's block (kos_arena) as a heap of its own,
 * in place of newlib-nano's. Nano's realloc moves a block it cannot grow,
 * so a buffer grown a little at a time (gnuboy reads a ROM 4 KB a realloc)
 * needs twice its size at the end: a 2 MB ROM did not fit. Here a block
 * grows into the free space after it (the block's end of the heap too),
 * and free blocks merge with free neighbours.
 *
 * Blocks: a header word (the block's size; bit 0 set when in use, bit 1
 * when the block before it is free), the data, and in a free block the
 * size again in its last word (so the block after it finds it) and the
 * free list's links. 8-byte aligned; best fit (the smallest free block
 * that fits: a game's small blocks kept do not split the big holes its
 * passing ones leave) from a single free list. The game and the kit's sound thread do not
 * allocate at once (the sound thread never does).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <malloc.h>
#include <reent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kit.h"
#include "kit_os.h"

#define HDR      8u                     /* the header (two words: 8-byte data) */
#define MIN_BLK  ((HDR + 2 * sizeof(void *) + 4 + 7) & ~7u)  /* header, links, footer: 24 */
#define USED     1u
#define PREV_FREE 2u

typedef struct blk {
    uint32_t pad;                       /* keeps the data 8-byte aligned */
    uint32_t size;                      /* the whole block's; USED in bit 0 */
    struct blk *next, *prev;            /* free blocks only */
} blk_t;

static uint8_t *heap_lo, *heap_hi;
static blk_t *free_list;

#define SIZE(b)   ((b)->size & ~7u)
#define NEXT(b)   ((blk_t *)((uint8_t *)(b) + SIZE(b)))
#define DATA(b)   ((void *)((uint8_t *)(b) + HDR))
#define BLOCK(p)  ((blk_t *)((uint8_t *)(p) - HDR))
#define FOOT(b)   (((uint32_t *)NEXT(b))[-1])

/* free bytes now, and the fewest there have been (the log's: what a game really needs) */
static size_t free_now, free_low = ~(size_t)0;

static void unlink_free(blk_t *b)
{
    free_now -= SIZE(b);
    if (b->prev)
        b->prev->next = b->next;
    else
        free_list = b->next;
    if (b->next)
        b->next->prev = b->prev;
}

static void link_free(blk_t *b)
{
    b->size &= ~USED;
    free_now += SIZE(b);
    FOOT(b) = SIZE(b);
    NEXT(b)->size |= PREV_FREE;         /* the next one (or the end marker) */
    b->prev = NULL;
    b->next = free_list;
    if (free_list)
        free_list->prev = b;
    free_list = b;
}

/* the block before b if it is free (its footer), else NULL */
static blk_t *free_before(blk_t *b)
{
    if (!(b->size & PREV_FREE) || (uint8_t *)b <= heap_lo)
        return NULL;
    return (blk_t *)((uint8_t *)b - ((uint32_t *)b)[-1]);
}

static int heap_ready(void)
{
    size_t size;
    uint8_t *a;
    blk_t *b;

    if (heap_lo)
        return 1;
    a = kos_arena(&size);
    if (!a || size < 4096)
        return 0;
    heap_lo = (uint8_t *)(((uintptr_t)a + 7) & ~(uintptr_t)7);
    heap_hi = (uint8_t *)(((uintptr_t)a + size - HDR) & ~(uintptr_t)7);
    ((blk_t *)heap_hi)->size = USED;    /* an end marker: a used, empty block */
    b = (blk_t *)heap_lo;
    b->size = (uint32_t)(heap_hi - heap_lo);
    link_free(b);
    return 1;
}

static size_t need(size_t n)
{
    size_t s = (n + HDR + 7) & ~7u;

    return s < MIN_BLK ? MIN_BLK : s;
}

/* take s bytes of the free block b (in use from then on), the rest freed */
static void note_low(void)
{
    if (free_now < free_low)
        free_low = free_now;
}

static void *take(blk_t *b, size_t s)
{
    unlink_free(b);
    if (SIZE(b) - s >= MIN_BLK) {
        blk_t *rest = (blk_t *)((uint8_t *)b + s);

        rest->size = SIZE(b) - (uint32_t)s;
        link_free(rest);
        b->size = (uint32_t)s;
    }
    b->size |= USED;
    NEXT(b)->size &= ~PREV_FREE;
    return DATA(b);
}

size_t kit_heap_largest(void);
static struct mallinfo info(void);

/* the log says so the first few times (a game then usually stops) */
static void no_memory(size_t n)
{
    static int told;
    char text[96];
    struct mallinfo mi = info();

    if (told++ >= 3)
        return;
    snprintf(text, sizeof(text), "malloc: no %lu bytes (largest %lu free, %lu free in all)",
             (unsigned long)n, (unsigned long)kit_heap_largest(), (unsigned long)mi.fordblks);
    kit_log(text);
}

void *malloc(size_t n)
{
    size_t s;

    if (!heap_ready() || n > 0x7ffffff0u) {
        errno = ENOMEM;
        return NULL;
    }
    s = need(n);
    {
        blk_t *best = NULL;

        for (blk_t *b = free_list; b; b = b->next)     /* the smallest that fits */
            if (SIZE(b) >= s && (!best || SIZE(b) < SIZE(best))) {
                best = b;
                if (SIZE(b) == s)
                    break;
            }
        if (best) {
            void *p = take(best, s);

            note_low();                 /* (whole again: a block's split is done) */
            return p;
        }
    }
    no_memory(n);
    errno = ENOMEM;
    return NULL;
}

void free(void *p)
{
    blk_t *b, *n, *pb;

    if (!p)
        return;
    b = BLOCK(p);
    b->size &= ~USED;                   /* (link_free below marks the next) */
    n = NEXT(b);
    if ((uint8_t *)n < heap_hi && !(n->size & USED)) {     /* merge the next */
        unlink_free(n);
        b->size += SIZE(n);
    }
    if ((pb = free_before(b))) {                           /* into the one before */
        unlink_free(pb);
        pb->size += SIZE(b);
        b = pb;
    }
    link_free(b);
}

void *realloc(void *p, size_t n)
{
    blk_t *b, *nx;
    size_t s, have;
    void *q;

    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    b = BLOCK(p);
    s = need(n);
    have = SIZE(b);
    if (s <= have)
        return p;                       /* it fits (kept as it is) */
    nx = NEXT(b);
    if ((uint8_t *)nx < heap_hi && !(nx->size & USED) && have + SIZE(nx) >= s) {
        unlink_free(nx);                /* grows into the free one after it */
        b->size = (uint32_t)(have + SIZE(nx)) | USED | (b->size & PREV_FREE);
        NEXT(b)->size &= ~PREV_FREE;
        if (SIZE(b) - s >= MIN_BLK) {
            blk_t *rest = (blk_t *)((uint8_t *)b + s);

            rest->size = SIZE(b) - (uint32_t)s;
            b->size = (uint32_t)s | USED | (b->size & PREV_FREE);
            link_free(rest);
        }
        note_low();
        return p;
    }
    if (!(q = malloc(n)))
        return NULL;
    memcpy(q, p, have - HDR);
    free(p);
    return q;
}

void *calloc(size_t n, size_t size)
{
    void *p;

    if (size && n > 0x7ffffff0u / size) {
        errno = ENOMEM;
        return NULL;
    }
    if ((p = malloc(n * size)))
        memset(p, 0, n * size);
    return p;
}

void *memalign(size_t align, size_t n)
{
    uint8_t *p;
    blk_t *b;

    if (align <= 8)
        return malloc(n);
    if (!(p = malloc(n + align + MIN_BLK)))
        return NULL;
    b = BLOCK(p);
    if (((uintptr_t)p & (align - 1)) == 0)
        return p;
    {
        uint8_t *q = (uint8_t *)(((uintptr_t)p + MIN_BLK + align - 1) & ~(uintptr_t)(align - 1));
        blk_t *a = BLOCK(q);
        uint32_t lead = (uint32_t)((uint8_t *)a - (uint8_t *)b);

        a->size = (SIZE(b) - lead) | USED;
        b->size = lead | USED | (b->size & PREV_FREE);
        free(DATA(b));                  /* the lead becomes a free block */
        return q;
    }
}

/* the largest block malloc can give (Doom's zone takes it, less a margin) */
size_t kit_heap_largest(void)
{
    size_t best = 0;

    if (!heap_ready())
        return 0;
    for (blk_t *b = free_list; b; b = b->next)
        if (SIZE(b) > best)
            best = SIZE(b);
    return best > HDR ? best - HDR : 0;
}

static struct mallinfo info(void)
{
    struct mallinfo mi;

    memset(&mi, 0, sizeof(mi));
    if (heap_ready()) {
        mi.arena = heap_hi - heap_lo;
        for (blk_t *b = free_list; b; b = b->next) {
            mi.fordblks += SIZE(b);
            mi.ordblks++;
        }
        mi.uordblks = mi.arena - mi.fordblks;
    }
    return mi;
}

/* newlib's own (stdio's buffers ...) call these */
void *_malloc_r(struct _reent *r, size_t n) { (void)r; return malloc(n); }
void _free_r(struct _reent *r, void *p) { (void)r; free(p); }
void *_realloc_r(struct _reent *r, void *p, size_t n) { (void)r; return realloc(p, n); }
void *_calloc_r(struct _reent *r, size_t n, size_t s) { (void)r; return calloc(n, s); }
void *_memalign_r(struct _reent *r, size_t a, size_t n) { (void)r; return memalign(a, n); }

struct mallinfo mallinfo(void) { return info(); }

/* free in the game's heap, all told, and the least it has been (kit_os.c's log) */
size_t kit_heap_free(void) { return free_now; }
size_t kit_heap_low(void) { return free_low == ~(size_t)0 ? free_now : free_low; }
