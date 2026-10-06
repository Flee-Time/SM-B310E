/*
 * B310E-OS — arch/diag_lcd_main.c
 *
 * LCD DIAGNOSTIC boot image (`make diag-lcd` -> os-diag-lcd.bin).
 * Discriminates WHY the panel shows colorful static instead of the banner.
 *
 * Hardware evidence behind this binary:
 *   - os.bin AND os-diag-s0 behave identically: LCD shows colorful static.
 *     diag-s0 runs usb_debug_init -> lcd_init -> lcd_fill(blue) + lcd_print
 *     + lcd_show -> halt (NO scheduler, NO chip init). The blue banner
 *     never reaches the panel -> the bug is inside lcd_init/lcd_show.
 *   - "static then goes black" matches lcd_init step 8
 *     (lcd_fill(0x0000); lcd_show();) — the fill DOES reach the panel at
 *     least once, so lcd_init completes on hardware.
 *   - "static stays" matches the LCDC freezing on the dummy-refresh frame:
 *     lcdc_init_regs() streams one dummy refresh from img.y_base_addr
 *     BEFORE it is set (= address 0 = flash on SC6530 = the static). If a
 *     later refresh never completes, the panel stays frozen on whatever
 *     the previous DMA streamed.
 *   - Prime suspect: lcd_show()'s unbounded `while ((irq.raw & 1) == 0)`
 *     (drivers/lcd.c) hangs when a refresh does not complete.
 *
 * This main() replicates the os-diag-s0 path (usb_debug_init -> lcd_init,
 * NO chip init, no scheduler — the proven-working boot path) but replaces
 * the single banner show with a STAGED COLOR SEQUENCE. Every refresh uses
 * lcd_show_bounded() (exported by drivers/lcd.c): the IDENTICAL DBI
 * handshake as lcd_show, only the completion wait is bounded (~1 s) so a
 * dead DMA can never freeze this image — the panel simply keeps the last
 * frame that DID complete.
 *
 *   Stage A: lcd_fill(0xF800) RED   -> lcd_show_bounded() -> ~300 ms
 *   Stage B: lcd_fill(0x07E0) GREEN -> lcd_show_bounded() -> ~300 ms
 *   Stage C: lcd_fill(0x001F) BLUE  + "DIAG:COLORS OK"    -> lcd_show_bounded()
 *   then halt (for (;;)).
 *
 * Reading the result (user watches the panel through the sequence):
 *   - static throughout, no color ever appears
 *       -> refresh never completes (irq.raw hang / DMA dead): the first
 *          unbounded wait in lcd_init (dummy refresh or black clear) is
 *          where it dies; the LCDC froze on the address-0 stream.
 *   - red appears, then freezes on red
 *       -> the first refresh completes, a later one hangs (H1 partial:
 *          DMA dies after one refresh, or the irq edge is consumed).
 *   - red -> green -> blue all appear
 *       -> lcd_init/lcd_show refresh path is FINE; the static/banner bug
 *          is elsewhere (panel init state / MADCTL / window) -> next step
 *          is panel-window debug.
 *   - colors appear but wrong orientation/pattern
 *       -> panel init partially wrong (MADCTL / window / GRAM layout).
 *
 * SAFETY: same as the other diag images — no 0x8c pinmux writes, no
 * interrupts added, no production driver changes (lcd_show/lcd_init/
 * lcdc_init_regs are untouched; only lcd_show_bounded was ADDED).
 */

#include "os.h"
#include "chip.h"
#include "../drivers/usb_debug.h"
#include "../drivers/lcd.h"

/* On-screen dwell between color stages. */
#define DIAG_STAGE_DELAY_MS 300u

void main(void)
{
    /* 1. fdl_ack FIRST — usb_debug_init() sends it, unblocking spd_dump
     *    before anything risky runs (user's flow depends on it). */
    usb_debug_init();

    /* 2. Full production LCD init: power -> LCM baseline -> safe-freq ->
     *    reset -> ID read (0x7c89f0 BOE) -> cmd7c89f0_init + MADCTL 0xd0 +
     *    window -> lcdc_init_regs (incl. the address-0 dummy refresh) ->
     *    y_base_addr = lcd_fb -> black first frame -> backlight. */
    lcd_init();

    /* 3. Staged color sequence — every refresh BOUNDED (~1 s timeout). */
    kprintf("diag-lcd: stage A red\n");
    lcd_fill(0xf800);                        /* RED */
    lcd_show_bounded();
    lcd_delay_ms(DIAG_STAGE_DELAY_MS);

    kprintf("diag-lcd: stage B green\n");
    lcd_fill(0x07e0);                        /* GREEN */
    lcd_show_bounded();
    lcd_delay_ms(DIAG_STAGE_DELAY_MS);

    kprintf("diag-lcd: stage C blue\n");
    lcd_fill(0x001f);                        /* BLUE */
    lcd_print(4, 20, "DIAG:COLORS OK", 0xffff, 0x001f);
    lcd_show_bounded();

    /* 4. Halt — the panel keeps the last completed frame on screen. */
    for (;;) ;
}
