/* B310E OS — SC6530 interrupt controller.
 *
 * The INTC has a masked status (+0), raw status (+4), a read/write enable
 * mask (+8) and a write-ones disable (+0xc). It has no acknowledge: each
 * handler clears its own peripheral's request. A line that fires with no
 * handler is masked and counted, so it cannot storm.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"

static void (*handlers[32])(void);
volatile uint32_t irq_stray_count;

void irq_register(unsigned line, void (*handler)(void))
{
    handlers[line & 31] = handler;
}

void irq_enable_line(unsigned line)
{
    uint32_t s = arch_irq_save();
    REG32(INTC_ENABLE) |= 1u << line;
    arch_irq_restore(s);
}

void irq_disable_line(unsigned line)
{
    REG32(INTC_DISABLE) = 1u << line;
}

/* IRQ mode, interrupts off, the interrupted thread's context saved. */
void irq_dispatch(void)
{
    uint32_t pending;

    while ((pending = REG32(INTC_STATUS)) != 0) {
        unsigned line = 31 - __builtin_clz(pending);

        if (handlers[line]) {
            handlers[line]();
        } else {
            REG32(INTC_DISABLE) = 1u << line;
            irq_stray_count++;
        }
    }
    kernel_irq_exit();
}
