/*
 * B310E-OS — kernel/module.c
 *
 * Module registry (see module.h). Simple bounded array of pointers into
 * static const module_t objects — the modules themselves are compiled in,
 * there is no dynamic loading.
 */

#include "module.h"

#include <stdint.h>

#include "printk.h"

static const module_t *s_modules[MODULE_MAX_REGISTRATIONS];
static uint32_t s_module_count;

void module_register(const module_t *m)
{
    if (m == NULL || s_module_count >= MODULE_MAX_REGISTRATIONS)
        return;
    s_modules[s_module_count++] = m;
}

int module_init_all(void)
{
    int failures = 0;

    for (uint32_t i = 0; i < s_module_count; i++) {
        const module_t *m = s_modules[i];
        kprintf("[kernel] module init: %s\n",
                (m->name != NULL) ? m->name : "(unnamed)");
        if (m->init != NULL && m->init() != 0)
            failures++;
    }
    return failures;
}
