/* B310E OS — the analog die through the ADI mailbox.
 *
 * Read: write the 12-bit register index to +0x18, poll +0x1c until bit 31
 * (busy) clears, check the echoed index in bits 28:16, take bits 15:0.
 * Write: wait while the FIFO is full (+0x20 bit 9), store to the analog
 * address directly, wait until the FIFO is empty (bit 8). Every wait is
 * bounded; reads disable interrupts so a handler cannot interleave.
 *
 * The bus does not run without the PLL (reads return 0, phone, 2026-11-09), so
 * each access starts it if the idle loop stopped it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"

#define ADI_RD_CMD    0x82000018u
#define ADI_RD_DATA   0x8200001cu
#define ADI_STS       0x82000020u
#define ADI_FULL      (1u << 9)
#define ADI_EMPTY     (1u << 8)
#define ADI_BUDGET    1000000u

uint32_t adi_read_errors;

uint32_t adi_read(uint32_t addr)
{
    uint32_t s = arch_irq_save(), data = 0;
    bool ok = false;

    clock_pll_need();
    for (int tries = 0; tries < 3 && !ok; tries++) {
        uint32_t n = ADI_BUDGET;

        while (!(REG32(ADI_STS) & ADI_EMPTY) && --n)
            ;
        REG32(ADI_RD_CMD) = addr & 0xfff;
        n = ADI_BUDGET;
        do {
            data = REG32(ADI_RD_DATA);
        } while ((data >> 31) && --n);
        ok = !(data >> 31) && ((data >> 16) & 0xfff) == (addr & 0xfff);
    }
    if (!ok) {                          /* 0, not another register's value */
        adi_read_errors++;
        data = 0;
    }
    arch_irq_restore(s);
    return data & 0xffff;
}

bool adi_write(uint32_t addr, uint32_t value)
{
    uint32_t s = arch_irq_save(), n = ADI_BUDGET;
    bool ok = false;

    clock_pll_need();
    while ((REG32(ADI_STS) & ADI_FULL) && --n)
        ;
    if (n) {
        REG32(addr) = value;
        n = ADI_BUDGET;
        while (!(REG32(ADI_STS) & ADI_EMPTY) && --n)
            ;
        ok = n != 0;
    }
    arch_irq_restore(s);
    return ok;
}
