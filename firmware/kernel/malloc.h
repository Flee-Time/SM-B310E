/*
 * B310E-OS — kernel/malloc.h
 *
 * Simple bump allocator over a caller-provided pool. v1 semantics:
 * allocate-only, no free() (kernel objects and task stacks live for the
 * whole session). 8-byte aligned so it is safe for the ARM EABI and for
 * 64-bit host types.
 *
 * On the phone the pool is the SDRAM region right after the loaded image
 * (base = __bss_end, from link/os.ld); host tests pass a static buffer.
 */

#ifndef B310E_OS_MALLOC_H
#define B310E_OS_MALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void kmem_init(void *base, uint32_t size);
void *kmalloc(uint32_t size);       /* 8-aligned; NULL when exhausted */
uint32_t kmem_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_MALLOC_H */
