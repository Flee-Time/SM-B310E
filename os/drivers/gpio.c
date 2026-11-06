/* B310E OS — GPIO bank 0 (0x8a000000) and what hangs on it: the torch.
 *
 * Data +0, data mask +4, direction +8 (1 = output). The bank's APB clock
 * is enabled by writing 0x800080 to 0x8b0000a0 (the enable-set register),
 * as the Rockbox port does (system-target.h).
 *
 * Torch: GPIO7 enables the U202 LD6816 LDO that feeds LED200 (Rockbox
 * power-sc6530c.c, verified on the phone 2026-11-06).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"

#define GPIO_DATA   0x8a000000u
#define GPIO_DMSK   0x8a000004u
#define GPIO_DIR    0x8a000008u
#define APB_EN_SET  0x8b0000a0u
#define TORCH_EN    (1u << 7)

static bool torch_on;

void gpio_clock_on(void)
{
    REG32(APB_EN_SET) = 0x800080u;
}

void torch_set(bool on)
{
    uint32_t s = arch_irq_save();

    torch_on = on;
    gpio_clock_on();
    REG32(GPIO_DMSK) |= TORCH_EN;
    if (on)
        REG32(GPIO_DATA) |= TORCH_EN;
    else
        REG32(GPIO_DATA) &= ~TORCH_EN;
    REG32(GPIO_DIR) |= TORCH_EN;
    arch_irq_restore(s);
}

bool torch_get(void)
{
    return torch_on;
}
