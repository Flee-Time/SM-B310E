# Rockbox controls on the SM-B310E

The directional pad, Center and soft keys cover normal navigation and
playback. The same physical buttons have these defaults:

| Button | While a track is displayed | In menus / file browser |
| --- | --- | --- |
| Center / OK | Play or pause | Select |
| Hold Center | Track context menu | Item context menu |
| Up / Down | Raise / lower volume; hold to repeat | Move up / down; hold to repeat |
| Left / Right | Previous / next track | Back / enter |
| Hold Left / Right | Seek backward / forward; release to resume | List scrolling, according to the list setting |
| Left soft key | Main menu | Main menu |
| Right soft key | Back to file browser; playback continues | Back / cancel |
| DIAL | Play or pause | Return to the playing track |
| END | Stop playback | Stop / cancel |
| Hold END | Shut down | Shut down |
| Hold STAR | Quickscreen | Quickscreen |
| Hold 0 (1 s) | Torch on / off | Torch on / off |

Rockbox's previous-track behavior can restart the current track if it has
already played past its configured skip threshold. Release after seeking
finishes the seek; it does not also skip a track. The number-key shortcuts
remain available: 2/8 for volume and 4/6 for tracks.

## Torch

**Status: verified on hardware** (2026-11-05): holding 0 lights the
torch. The first two hardware builds did
not light the torch. The test screen's dump showed why: every GPIO
register read 0 on the phone, even right after Rockbox wrote it. The
GPIO block's APB clock was never on. The port enabled bit 19 of
`0x8b0000a0`, which is half of the keypad clock (`0x80040`). fpdoom
enables the GPIO block with `0x800080` ("GPIO_D"), and so does the port
now (`sc6530_gpio_clock_on()` in `system-target.h`). Before this fix
the charger status pins (GPIO 8/9) and the headset output pin (GPIO 0)
were not working either. QEMU does not model this clock gate, so it
could not catch the bug.

Holding 0 for a second toggles the torch LED from any screen, plugins
included; a short press does nothing. The keypad driver handles it
(`button-sc6530c.c`), and `torch_set()` in `power-sc6530c.c` drives the
pin. Power-off and reboot turn the torch off.

The torch is GPIO 7 (bank `0x8a000000`, bit 7, active high). The service
manual's block diagram shows one `TORCH_EN` line from the SoC to U202
(LD6816CX4C33P, an LDO with an enable pin) feeding the LED. The pin
number comes from the stock firmware in QEMU: a long press of Up on the
home screen writes bank 0 DATA bit 7 high, and the next long press
writes it low (writer at NOR `0x34e1e`, no other register changes).
Stock boot leaves bit 7 set in DMSK and DIR. Rockbox sets those itself,
so it does not depend on stock initialisation, and it writes no pinmux
registers.

**Test screen:** System > Debug (Keep Out!) > View I/O ports. It shows
the GPIO 7 state, how many times the hold-0 toggle fired, bank 0 GPIO
registers and the analog LED control register (`0x82001220`). Its keys:

| Key | Action |
| --- | --- |
| 1 | GPIO 7 on/off |
| 3 | `0x82001220`: set `0x8800`, clear `0x4400` / restore |
| 5 | `0x82001220`: clear `0x1100` (fpdoom's "flashlight off" bits) / restore |
| 9 | Append a register dump to `.rockbox/torch-b310e.txt` |
| Back | Exit, restoring `0x82001220` |

Opening the screen also writes a dump. The dump only reads the pinmux
bank (`0x8c000000`–`0x8c0002fc`); comparing it with the stock pinmap
table at NOR `0xc6ef0` shows whether the boot menu applied the stock pin
functions.

In the on-screen keyboard, Center chooses a character, Right soft deletes,
DIAL accepts the text and END cancels. Center also accepts the color
chooser, matching the other settings screens.

The full-player test `ports/rockbox/tests/test-idle-power.py --controls`
checks playback pause/resume, actual volume changes, track paths, forward
and backward seeking, and leaving the playing screen through Menu/Back.
It uses normal emulated keypad input and reads player state without
patching guest RAM.
