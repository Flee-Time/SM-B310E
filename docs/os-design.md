# B310E OS — design (draft, 2026-11-06)

A new operating system for the SM-B310E (SC6530C: ARM926EJ-S at 208 MHz,
4 MB PSRAM, 256 KB IRAM, TeakLite DSP), written from scratch in a fresh
tree. It replaces neither Rockbox nor the stock firmware on the card: the
boot menu launches it like any other payload in `payloads/`.

`firmware/` (the old `os.bin`) was a hardware test image; it was removed
on 2026-11-06 and stays in git history (e2c0e83). The new OS takes from it
only what the hardware proved; its structure starts over.

## Goals

- **Phone first:** GSM calls and SMS through our own layer 1 and an open
  layer 2/3 (see [gsm-plan.md](gsm-plan.md)). Rockbox stays a separate
  boot for music.
- **Full control:** every line that runs on the ARM is ours or open
  source we can read and change. The only stock pieces are data read
  from the owner's own NOR at runtime, never shipped: the DSP firmware
  blob and the RF calibration.
- **Fast:** no pager, no LZMA at run time, interrupt latency in
  microseconds, cached PSRAM.
- **Modular:** drivers, services and apps are separate units with small
  interfaces. Apps load from the SD card at run time and unload again.
- **Power efficient:** tickless idle with the CPU in wait-for-interrupt,
  each block clocked only while used, the radio duty-cycled by the DSP.

## Constraints

| Item | Fact | Consequence |
|---|---|---|
| RAM | 4 MB PSRAM, 256 KB IRAM | No demand paging; apps loaded on demand; budgets per layer (below) |
| CPU | ARM926EJ-S, ARMv5TE, Thumb-1, MMU, 16 KB I/D caches | Thumb for size, ARM for hot paths; cache maintenance around DMA |
| Boot | the boot loader's menu (boot/) copies a raw image and jumps to it in ARM state, CP15 V=1 (high vectors) | Same contract as Rockbox: identity MMU with the high-vector page mapped |
| SDIO | DMA only into IRAM (DMA into PSRAM hangs) | 16 KB IRAM bounce buffer, as the Rockbox driver |
| USB | bulk DMA corrupts data on this phone | PIO, one interrupt per packet (docs/usb.md) |
| Landmines | pinmux `0x8c0002a4`; stock pinmap replay; IF0 `0x82001180` | Never written (docs list them) |
| GSM | layer 1 must feed the DSP ~2 frames ahead, frame = 4.615 ms | A top-priority layer-1 thread woken by DSP interrupt 13 |

## Layers

```
 apps (SD modules)      phone, SMS, contacts, settings, cell monitor, ...
 ───────────────────────────────────────────────────────────────────────
 services               ui (windows, fonts, input focus) · fs · power
                        · audio · gsm (L2/L3) · usb (mass storage, CDC)
 ───────────────────────────────────────────────────────────────────────
 drivers                lcd · keypad · sdio/sd · usb · codec/vbc · adc
                        · battery/charger · backlight · rtc · fm · sim
                        · dsp · gsm-l1 · vibrator · torch
 ───────────────────────────────────────────────────────────────────────
 kernel                 threads · priorities · preemption · timers
                        · mutex · semaphore · message queue · events
 ───────────────────────────────────────────────────────────────────────
 arch/sc6530c           start · MMU/cache · INTC · timer · ADI · DMA
```

Each layer only calls down. Services and apps talk to each other through
message queues, not direct calls, so a service can be replaced or
restarted without its clients relinking.

## Kernel

Our own, small and preemptive (estimate under 2,000 lines):
- **Threads** with fixed priorities (32 levels) and round-robin within a
  level. A higher-priority thread that becomes ready preempts at once,
  including from an interrupt.
- **Interrupts:** top halves stay short and wake a thread. Layer 1's
  interrupt 13 wakes the highest-priority thread in the system.
- **Tickless:** a one-shot hardware timer is programmed for the next
  timeout; with nothing to run, the CPU waits for an interrupt.
- **Objects:** mutex (with priority inheritance), counting semaphore,
  message queue (fixed-size messages), event flags, timers.
- **Memory:** a small kernel heap for objects, a page allocator for app
  images and large buffers; no allocation in interrupt context.

FreeRTOS (MIT) would also fit and is the fallback if our kernel becomes a
time sink; the kernel API above is kept close enough that the swap stays
contained in `kernel/`.

## Memory map (first budget)

| Region | Use | Size |
|---|---|---|
| PSRAM 0x04000000 | kernel, drivers, services (one image) | ≤ 768 KB |
| | GSM layer 1 + layer 2/3 | ≤ 768 KB |
| | LCD frame buffer (128×160 RGB565) | 40 KB |
| | page pool: app images, heaps, audio/file buffers | the rest, ~2.3 MB |
| IRAM 0x40000000 | boot menu's vectors/stacks (until we move them), SD bounce 16 KB, hot code (IRQ entry, context switch, L1 frame handler) | 256 KB |
| DSP share-mem 0x10000000 | layer-1 queues (frame instructions, power monitor, RX blocks, commands) | DSP-defined |

## Modules

*Built 2026-11-07 (milestone 3, in QEMU).*

- **Format** (`os/include/os/app.h`): a `.app` file is a 48-byte header
  (magic, API version, sizes, CRC-32, name), the image linked at address
  0 (`os/sdk/app.ld`, with `app_info` first) and a list of word offsets
  to relocate. `os/sdk/mkapp.py` makes it from the ELF linked with `-q`:
  it keeps only R_ARM_ABS32 against image symbols (PC-relative types stay
  valid anywhere) and refuses anything else, so a module cannot need an
  import table.
- **API:** apps call nothing in the OS directly. `start()` gets a
  versioned table of function pointers (`os_api_t`: C library, memory,
  time, log, battery, drawing, screens and widgets, FatFs). Minor versions
  only append; the SDK header (`os/sdk/app.h`) maps the usual names to the
  table, so app code reads like OS code.
- **Apps** run in the UI thread as screens, not as threads: `start()`
  returns the first screen, and every callback runs with the app marked
  current. Screens it pushes and memory from `api->malloc` belong to it;
  when its last screen leaves the stack, `stop()` runs and the app and
  its memory are freed (the unload waits for the UI loop, never inside the
  app's own code). Threads for apps come with the first app that needs
  one.
- **Card:** `/b310e-os/apps/*.app`, listed under Menu → Apps by the name
  in the header. The first one is a stopwatch (`os/modules/stopwatch`).
- **Drivers and services** are linked into the OS image at first; the
  same format can load optional ones later.
- **Isolation:** none at first (single address space). The MMU could give
  each app its own mapping later if crashes become a problem.

## Drivers: where each comes from

The Rockbox port holds the newest hardware-verified register facts.
Drivers are rewritten for the new kernel's interfaces, reusing that code
where its license allows (see License).

| Driver | Source of truth | Status there |
|---|---|---|
| LCD (ST7735S, LCDC DMA) | Rockbox `lcd-sc6530c.c` | verified |
| Keypad, side keys, headset buttons | `button-sc6530c.c`, `audiohw-sc6530c.c` | verified |
| SD (multi-block, IRAM bounce) | `sd-sc6530c.c`, `sdio-sc6530c.c` | verified |
| USB device (PIO) | `usb-drv-sc6530c.c` | verified |
| Codec, VBC, mic, FM line-in | `audiohw`, `pcm` | verified |
| Battery, charger, power-off | `powermgmt`, `power` | verified |
| Backlight, keypad light, vibrator, torch | `backlight`, GPIO 7 | verified |
| RTC | `rtc-sc6530c.c` | verified |
| FM (RTC6211N) | `rtc6211.c`, `fmradio-i2c` | verified |
| SIM | `sim-sc6530c.c` | verified |
| DSP boot | `firmware/arch/diag_dsp_main.c` (removed with the old test OS; in git at e2c0e83) | verified (2026-08-27) |
| GSM layer 1 | new (gsm-plan.md) | in reverse engineering |

## GSM

- **Layer 1 (ours):** boots the DSP with the blob from NOR, loads the RF
  calibration, and runs the frame-instruction ring in DSP share-memory
  (layout in gsm-plan.md). A top-priority thread fills instructions two
  frames ahead and drains results (power measurements, decoded blocks).
- **Layer 2/3:** OsmocomBB's `mobile` (LAPDm, RR, MM, CC, SMS, SIM),
  ported to the kernel with L1CTL as an internal interface.
- **Development wiring:** before layer 2/3 runs on the phone, layer 1
  speaks L1CTL over USB (CDC-ACM) to an unmodified OsmocomBB on a PC.
- **Transmit** waits until receive works, and is first tested against a
  private network (osmo-bts with an SDR, shielded): transmitting on a
  public network with unapproved firmware is not legal in most places.

## Power

- Tickless idle with wait-for-interrupt whenever no thread is ready.
- Drivers gate their clocks when idle (LCD, codec, SD, USB, FM).
- The display service dims and blanks on timeouts; keys wake it at once.
- Idle-mode GSM: the DSP wakes for paging blocks; the ARM sleeps between.

## License

Open decision. GPLv2-or-later for the OS tree would let it link
OsmocomBB's layer 2/3 (GPL) and reuse the GPL-derived parts of our Rockbox
drivers directly. Keeping MIT means writing GSM layers 2/3 ourselves and
rewriting those drivers clean. The rest of the repository keeps its MIT
license either way.

## Build and test

- `os/` at the repository root, built by `build.ps1 os` / `build.sh os`
  with arm-none-eabi-gcc, output `sdcard/payloads/b310e-os.bin`.
- QEMU runs it like Rockbox (`boot-mode=rockbox` loads a raw image at the
  PSRAM base); every driver gets a QEMU test where the model allows. The
  DSP is not emulated, so layer-1 radio work is tested on the phone.
- A card log, as in Rockbox, for hardware runs.

## Milestones

1. **Skeleton:** start-up, MMU, interrupts, timer, kernel threads and
   preemption, text on the LCD, keypad input; boots in QEMU and on the
   phone from the menu. *Done in QEMU (2026-11-06, `os/tests/test-boot.py`):
   self-test passes (priority inheritance holds the high thread 50 ms,
   exactly the low thread's hold), 99.5% idle at rest, keys, load,
   watchdog reboot. **Verified on the phone the same day:** text, self-test,
   keys, load and the END reboot all work; idle 97.3% at rest (the 1 ms
   tick and four status redraws a second), 0.0% with the load thread on.
   The tick is still periodic; tickless comes with power work.*
2. **Storage and logging:** SD driver, FAT (FatFs, BSD-style license),
   card log, power-off. *Verified on the phone 2026-11-07 (15 GB FAT32
   card: self-test, Files, clock, battery, colours, power-off). The first
   boot sat 15 s in the free-space count (the card's FSINFO had none, and
   FatFs read the FAT one ~1 ms sector at a time); the FAT is now read
   ahead 32 sectors at a time and the count runs after boot.* A first UI
   came with it (home, menu, files, system, settings, power; `os/ui`,
   `os/apps`), ahead of milestone 7.
3. **Modules:** loader, exported API, a first app loaded from the card.
   *Verified on the phone 2026-11-07 (the stopwatch).* CPU clock scaling
   (208/104/26 MHz by load, 26 MHz only with the panel asleep) came next:
   *runs on the phone (2026-11-07): 1.3% load at rest, 0.2% dark.*
4. **Layer-1 power scan:** DSP boot from the NOR blob, layer-1 start-up,
   signal levels per channel on screen. Receive only.
5. **USB:** mass storage and CDC-ACM (L1CTL to a PC).
6. **Receive GSM:** sync, broadcast channels, idle mode with OsmocomBB on
   the PC.
7. **Audio, FM, the rest of the drivers;** UI service and first apps.
8. **Transmit and layer 2/3 on the phone,** in a test network first.
