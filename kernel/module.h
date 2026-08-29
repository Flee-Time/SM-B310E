/*
 * B310E-OS — kernel/module.h
 *
 * Module framework: static registration of init functions. Later waves
 * (LCD, keypad, USB, SD, ...) declare a module_t and call module_register()
 * from their own init; the kernel runs all registered inits in order at
 * boot via module_init_all(). Stable interface — do not break it.
 */

#ifndef B310E_OS_MODULE_H
#define B310E_OS_MODULE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*module_init_fn)(void);   /* return 0 on success */

typedef struct {
    const char *name;
    module_init_fn init;
} module_t;

#define MODULE_MAX_REGISTRATIONS 32u

void module_register(const module_t *m);
/* Runs every registered init in registration order; prints each name via
 * kprintf. Returns the number of inits that failed (0 = all ok). */
int module_init_all(void);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_MODULE_H */
