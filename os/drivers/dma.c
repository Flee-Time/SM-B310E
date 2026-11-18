/* B310E OS — the system DMA engine (0x20100000): quiet at boot.
 *
 * 32 standard channels at 0x20101000 + n * 0x40 (stock init NOR 0x32580
 * walks all of them): +8 configuration (bit 0 enables), +12 interrupt
 * status and acknowledge (0x1f000000). Peripheral requests are bound to a
 * channel through the table at 0x20102000 + 4 * (request - 1), 64 entries
 * (one-based channel; 0 unbound).
 *
 * The stock firmware and the boot menu run before the OS and may leave
 * channels configured and requests bound. On the phone (2026-11-07) such
 * a channel woke once the audio code switched the engine's clock on for
 * the headset check, and wrote small values (codec samples) into the
 * OS's memory: the UI's screen stack and key timers. The OS then crashed
 * leaving the Stopwatch, and later started with the torch on and powered
 * off at once. QEMU runs no firmware before the OS and never showed it.
 * So every channel is disabled and every request unbound at boot, before
 * anything enables the engine for real; pcm.c binds only its own.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"

#define DMA_AHB_EN     0x20500060u      /* AHB enable SET; bit 0 the DMA */
#define DMA_CH(n, off) REG32(0x20101000u + (n) * 0x40u + (off))
#define DMA_MAP(req)   REG32(0x20102000u + ((req) - 1) * 4u)
#define DMA_CHANNELS   32
#define DMA_REQUESTS   64
#define DMA_ACK_ALL    0x1f000000u

void dma_quiesce(void)
{
    uint32_t s = arch_irq_save();

    REG32(INTC_DISABLE) = 1u << 20;     /* its interrupt line */
    REG32(DMA_AHB_EN) = 1;              /* registers need the clock */
    for (unsigned r = 1; r <= DMA_REQUESTS; r++)
        DMA_MAP(r) = 0;
    for (unsigned n = 0; n < DMA_CHANNELS; n++) {
        DMA_CH(n, 8) = 0;
        DMA_CH(n, 12) = DMA_ACK_ALL;
    }
    arch_irq_restore(s);
}

/* The engine's AHB clock (bit 0), counted: the codec (audio.c) and the USB
 * controller (usb.c, which never programs a channel but enables it as stock
 * does) each take it while they run. Closed when neither does (power audit
 * A3, 2026-11-18; before, open from boot). Every DMA register access is
 * between a get and its put: a gated block stalls the bus for good. */
static unsigned dma_users;
/* dev (poked): 0 never closes it (the pre-2026-11-18 behaviour); the users
 * are counted either way */
volatile uint32_t dma_gate_dev = 1;

void dma_clock_get(void)
{
    uint32_t s = arch_irq_save();

    if (dma_users++ == 0)
        REG32(DMA_AHB_EN) = 1;
    arch_irq_restore(s);
}

void dma_clock_put(void)
{
    uint32_t s = arch_irq_save();

    if (dma_users && --dma_users == 0 && dma_gate_dev)
        REG32(DMA_AHB_EN + 0x10u) = 1;  /* the clear register, 0x20500070 */
    arch_irq_restore(s);
}

/* the boot's quiesce done: closed until a user takes it */
void dma_clock_idle(void)
{
    uint32_t s = arch_irq_save();

    if (!dma_users && dma_gate_dev)
        REG32(DMA_AHB_EN + 0x10u) = 1;
    arch_irq_restore(s);
}

bool dma_clock_on(void)
{
    return (REG32(0x20500080u) & 1u) != 0;
}
