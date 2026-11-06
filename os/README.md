# B310E OS

Our own operating system for the SM-B310E (SC6530C). Design and plan:
[docs/os-design.md](../docs/os-design.md). License: GPLv2 or later
([LICENSE](LICENSE)); the rest of the repository stays MIT.

## Status

- **Milestone 1** (skeleton): verified on the phone 2026-11-06 (97.3%
  idle at rest).
- **Milestone 2** (storage) and **the first UI**: verified on the phone
  2026-11-07 (15 GB card: self-test, Files, clock, battery, power-off).
- **Milestone 3** (loadable apps), screen sleep, CPU load: verified on
  the phone 2026-11-07.
- **The Rockbox drivers ported** (2026-11-07): audio (codec, speaker /
  headphones, volume, PCM DMA), mic and FM recording, FM radio, headset
  detection and buttons, torch, vibrator, SIM reader, RTC calendar, USB
  mass storage; pass in QEMU. On the phone (2026-11-07): mic recording and
  playback, FM (NOR tables ok), SIM slot 0 and the torch work.
- **To do (2026-11-08): USB storage on the phone.** Windows enumerates the
  device but the drive never appears in Explorer. Log: 5 SCSI commands,
  2 failed, 1 reset, 0 KB moved. Start from the failed commands (sense
  and bulk halts, untested on hardware: does +bit 29 stall the bulk
  endpoints and does clearing it reset the data toggle?) and compare with
  the Rockbox usb_storage exchange that works on the phone.
- **CPU clock scaling** and the corrected CPU accounting: run on the
  phone 2026-11-07: 1.3% load at rest, 0.2% with the screen dark, the
  tick at 1000 interrupts a second, 3913 KB heap free.

| Part | What there is |
|---|---|
| Start-up | identity MMU with cached PSRAM, the high-vector page, I- and D-cache (`arch/sc6530c/cpu.S`) |
| Kernel | preemptive threads, 32 priorities, round robin within one, 1 ms tick, sleep, semaphores, mutexes with priority inheritance, message queues, heap, CPU clock governor (`kernel/`) |
| Clock | 208, 104 or 26 MHz, chosen by load every 50 ms; 26 MHz only with the panel asleep; Settings → CPU speed: Auto or 208 MHz (`arch/sc6530c/clock.c`, `kernel/cpufreq.c`) |
| Drivers | LCD (ST7735S, panel sleep), backlight and keypad light, vibrator, torch (GPIO7), keypad matrix and END, text console, SD card (multi-block, IRAM bounce, retries), battery voltage and charger, RTC calendar: date and time, set from the UI (`drivers/rtc.c`), SIM reader: activation, ATR, T=0 APDUs, both slots (`drivers/sim.c`) (`drivers/`) |
| Audio | codec power, speaker or headphones by the jack, volume, PCM playback and mono recording on DMA through the VBC, phone mic, 4-pole headset mic, FM line-in; headset detection and its three buttons (KEY_HS_*); all on the ARM, no DSP (`drivers/audio.c`, `pcm.c`, `include/os/audio.h`) |
| USB storage | the card as a USB mass storage device (1209:0001): our own small stack on the Rockbox port's controller driver, all PIO (DMA once wrote stale FIFO words into the card's root directory); Bulk-Only Transport, the SCSI commands Windows and Linux use, two 16 KB buffers; the OS lets go of the card meanwhile (`fs_suspend`) (`drivers/usb.c`, `usb_msc.c`, `apps/usb.c`) |
| FM radio | RTC6211N on I2C 1, tune, seek, RSSI, stereo, mute, the stock bank tables read from the phone's own NOR when their CRC matches (`drivers/fm.c`) |
| Storage | FatFs R0.15a on the card: long names, UTF-8, thread-safe; a card log in `/b310e-os/log.txt` (`fs/`) |
| UI | screen stack with status, title and soft-key bars, lists, dialogs, text view, key repeat, screen timeout: dim, then LEDs off and panel asleep (`ui/`) |
| Apps | built in: Home (clock, date), Menu, Apps, Files, Sound (tone, WAV player, mic recorder), FM radio (with recording), Tools (torch, vibrator, SIM cards, USB storage), System, Settings (brightness, screen timeout, CPU speed, Date & time), Power (`apps/`) |
| Loadable apps | `.app` files from `/b310e-os/apps/`, relocated into the heap, given a versioned API table, freed when closed (`loader/`, `sdk/`, `include/os/app.h`); first app: Stopwatch (`modules/stopwatch/`) |
| Power | power-off (LEDs off, supplies cut), restart to the boot menu |
| Faults | undefined instruction and aborts show the registers on screen |

On start the console shows the card and a self-test (kernel objects and a
32 KB card write and read), then the UI starts on the home screen.

**Keys:** Center or LSoft opens and selects, RSoft goes back, the D-pad
moves (Up/Down repeat when held, Left/Right page), END goes home, END
held for 2 s powers off, 0 held for 1 s switches the torch (with a short
buzz). The first key after the screen went dark only wakes it. A 4-pole
headset's buttons arrive as KEY_HS_CENTER, KEY_HS_PLUS, KEY_HS_MINUS
(Sound and FM radio: start/stop or mute, volume).

**On the card** the OS writes only into `/b310e-os/`: `log.txt` (rotated
to `log-old.txt` past 256 KB), `settings.txt` and `selftest.bin`
(32 KB, rewritten at every boot). It loads apps from `/b310e-os/apps/`;
recordings go to `/b310e-os/rec/RECnnnn.WAV` (16 kHz mono from the mic,
32 kHz from the radio).

## Writing an app

One folder in `modules/` per app; every `.c` in it is built and linked
with `sdk/start.c` into `build/os/modules/<folder>.app` (staged as
`sdcard/b310e-os/apps/`):

```c
#include "app.h"

static void draw(screen_t *s)
{
    gfx_text_centered(UI_TOP + 40, "Hello", C_TEXT, FONT_LARGE, 2);
}

static screen_t screen = { .title = "Hello", .right = "Close", .draw = draw };

static screen_t *start(void)
{
    return &screen;             /* RSoft pops it; then the app is freed */
}

APP("Hello", start, NULL);
```

The OS names (`gfx_*`, `ui_*`, `list_*`, `f_*`, `snprintf`, `malloc`,
`ticks`, `klog` ...) are macros for the API table (`sdk/app.h`). A screen
with `tick_ms` set gets `tick()` that often (the stopwatch uses 100 ms).
Close your files before your last screen goes; memory from `malloc` is
freed for you.

## Build

```
build.ps1 os        (Windows)
bash build.sh os    (Linux)
```

Output: `build/os/b310e-os.bin`, staged as `sdcard/progs/b310e-os.bin`.
The boot menu lists it under progs. The first build downloads FatFs
R0.15a from elm-chan.org into `build/fatfs/` (checked against its
SHA-256); the OS uses its own `fs/ffconf.h`.

## Test in QEMU

```
python os/tests/test-boot.py
```

Boots the image with a FAT32 card image (with the stopwatch app on it)
and reads the OS's `os_status` block through QTest: self-test results,
card, idle share, then drives the UI with key presses (Menu, Apps and the
stopwatch, Files, System, Settings, END home, screen dim, dark and wake,
Power off) and checks the card image for the self-test file, the saved
settings and the log. Screenshots of every screen go to `build/os/test-boot/`.

```
python os/tests/test-audio.py
python os/tests/test-sim.py
python os/tests/test-rtc.py --build build/os
```

`test-audio.py` plugs in a 4-pole headset, checks its centre button,
plays the 1 kHz test tone (measured in QEMU's WAV output), records the
model's mic tone to the card and checks the WAV there, plays it back and
starts the FM radio (probe, start-up and seek in the tuner model's
trace). `test-sim.py` reads both slots with a card in either and checks
that only SELECT MF reached the card and nothing identifying was logged.
`test-rtc.py` sets dates through the Date & time screen and reads the
RTC registers back.

## Layout

```
arch/sc6530c/   start-up, MMU, context switch, interrupts, timer, ADI, power
kernel/         threads and scheduler, sync objects, heap
drivers/        LCD, backlight, keypad, console, SD/SDIO, battery, fonts
fs/             FatFs glue (disk, locks, config), card log
loader/         app loader, the API table apps get
ui/             drawing, screen stack, widgets
apps/           built-in screens: home, menu, apps, files, system, settings, power
sdk/            for loadable apps: app.h, app.ld, start.c, mkapp.py
modules/        loadable apps, one folder each
lib/            string functions, printf
include/os/     interfaces
link/os.ld      memory map
tests/          QEMU tests
main.c          start-up; selftest.c: the boot self-test
```

## Notes for the code

- Every assembly routine needs `.type name, %function`: C is built as
  Thumb, and without the type the linker calls ARM routines with `bl`
  instead of `blx` (the first boot in QEMU ran ARM code as Thumb).
- Threads run in SYS mode; interrupts and `svc` save the same 17-word
  frame on the thread's stack (`cpu.S`).
- The LCD transfer is started, not waited for: the LCDC's done flag does
  not report reliably, and polling it cost half the CPU.
- QEMU on Windows delivers about 700 of the 1000 ticks a second, so time
  in QEMU runs slow; the idle share is a ratio of counted cycles and stays
  right.
- Pixels are plain RGB565. Milestone 1 packed them byte-swapped (copied
  from Rockbox's video helper), which only white survived; the QEMU
  model showed it.
- FatFs reads the FAT one sector at a time, and a single-block read
  costs about 1 ms on the phone, so counting a 15 GB card's free space
  took 15 s at the first boot (its FSINFO had no count). `fs/diskio.c`
  reads the FAT region 32 sectors ahead, and when FSINFO has no count
  the log thread counts after boot; FatFs writes it back to FSINFO.
- The screen timeout first only set the LED to 0 and stopped refreshing;
  on the phone the screen stayed lit and froze (2026-11-07). Dark now
  does what Rockbox does (verified there): LED and keypad light off,
  display off and panel sleep (`lcd_sleep`), and the first key wakes it.
- Dark still left a faint glow on the phone: "off" (from fpdoom) only
  cleared the LED's on bit. The stock on/off function (NOR `0x1a186`)
  clears bits 5 and 6 of `0x82001220`, then sets bit 6 for on or bit 5
  for off; `drivers/backlight.c` does the same now.
- A full frame takes about 10.6 ms on the BOE panel (27 bus clocks a
  write at the 150 ns timing, two writes a pixel), not the 6 ms assumed:
  at 8 and then 10 ms spacing the phone showed half-drawn frames and
  screens that lagged until the next redraw. The spacing is now computed
  from the programmed timing plus a quarter (14 ms), and the boot log
  records the real transfer time by the LCDC's done flag
  (`lcd: transfer spacing ... done flag after ... us`).
- Everything draws into a back buffer that `lcd_update()` copies to the
  LCDC's frame just before a transfer, as Rockbox does. Drawing straight
  into that frame showed dark horizontal lines on the phone where text
  was being redrawn (the stopwatch, 10 times a second).
- The System screen shows the CPU clock and load (100% minus the idle
  thread's share), interrupts per second and the two busiest interrupt
  lines (#23 is the 1 ms timer, #8 the keypad).
- CPU time was measured as the tick counter's difference modulo one
  period, which loses every whole period the idle thread sleeps through:
  the phone showed 10% load and 17000 timer interrupts a second at rest
  (really about 1000). `timer_cycles()` is now a monotonic 26 MHz count
  (periods plus the position in the current one, a pending period
  included).
- With the screen dark the UI loop spun: `redraw()` returns early and
  leaves the frame dirty for the wake, and the loop took "dirty" as "do
  not wait". It waits now; the QEMU test checks 99.9% idle while dark.
- Clock levels keep the AHB at 104 MHz at 104 and 208 MHz, so the LCD
  timing, SDIO and ADI run as at boot; only 26 MHz changes the AHB, and
  SD transfers (`cpufreq_hold`) and the awake panel (`cpufreq_allow_low`)
  keep it at 104 MHz or more. Switch order and register meaning are from
  the stock firmware (`arch/sc6530c/clock.c`). If the boot clock is not
  one of the three levels, scaling stays off. If Auto ever misbehaves on
  the phone, `cpu_max=1` in `/b310e-os/settings.txt` pins 208 MHz.
- RTC (from the stock rtc_phy_v5 driver, NOR 0x35dac..0x363d0, and the
  time manager at 0x19594..0x19b9e): seconds, minutes, hours and a 16-bit
  day counter at 0x82001600..0c, day 0 = 1980-01-01; new values go to
  +10..+1c and the RTC acknowledges each in raw status bits 8-11 (+34,
  cleared at +38). FAT timestamps come from it. The Rockbox port's RTC
  driver read the date as packed fields; it uses the day counter too now
  (docs/rockbox-power.md).
- Headset buttons need the mic bias, which needs the codec powered:
  while a 4-pole headset is in, the codec stays on (Rockbox keeps it on
  from boot). A 3-pole plug turns it off again after the check.
- The charger level (analog EIC channel 2) reads 0 until its mask bit is
  set in 0x82001904, which the OS never did: "charging" could not show.
  `charger_present()` sets it now (found while porting USB, which detects
  the cable through the same line).
- FM seeking tunes one channel per UI tick (about 16 a second) and any
  key stops it. It first swept the band inside one key handler: on the
  phone, Up while recording the radio blocked the UI, the recording and
  END for minutes. As a failsafe the END thread powers off by itself when
  END is held for 8 s (the UI does it at 2 s).
- The torch pin is set up (off) at boot, as Rockbox's power_init does:
  configured only on the first toggle, that toggle was lost on the phone.
- Log lines written while USB storage has the card are kept and written
  once it is back.
- Audio, SD transfers and the awake panel hold the CPU clock at 104 MHz
  or more (the AHB feeds their timing); SIM sessions do too.
