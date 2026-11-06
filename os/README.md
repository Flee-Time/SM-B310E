# B310E OS

Our own operating system for the SM-B310E (SC6530C). Design and plan:
[docs/os-design.md](../docs/os-design.md). License: GPLv2 or later
([LICENSE](LICENSE)); the rest of the repository stays MIT.

## Status: milestone 1 (skeleton), verified on the phone

Verified 2026-11-06: boots from the SD menu, self-test passes, keys,
load and the END reboot work; 97.3% idle at rest.

| Part | What there is |
|---|---|
| Start-up | identity MMU with cached PSRAM, the high-vector page, I- and D-cache (`arch/sc6530c/cpu.S`) |
| Kernel | preemptive threads, 32 priorities, round robin within one, 1 ms tick, sleep, semaphores, mutexes with priority inheritance, message queues, heap (`kernel/`) |
| Drivers | LCD (ST7735S), backlight and keypad light, keypad matrix and END, 21 × 20 text console (`drivers/`) |
| Power | watchdog stop at boot, reboot to the boot menu by watchdog |
| Faults | undefined instruction and aborts show the registers on screen |

On start it runs a kernel self-test and shows the result, then a live
status line (uptime, idle share, free heap) and every key event.
`5` switches a busy low-priority thread on and off; holding END for 2 s
reboots to the boot menu.

## Build

```
build.ps1 os        (Windows)
bash build.sh os    (Linux)
```

Output: `build/os/b310e-os.bin`, staged as `sdcard/progs/b310e-os.bin`.
The boot menu lists it under progs.

## Test in QEMU

```
python os/tests/test-boot.py
```

Boots the image in the B310E machine and reads the OS's `os_status`
block through QTest: self-test results, tick and idle share, injected
keys, load, and the reboot when END is held. Screenshots go to
`build/os/test-boot/`.

## Layout

```
arch/sc6530c/   start-up, MMU, context switch, interrupts, timer, ADI, power
kernel/         threads and scheduler, sync objects, heap
drivers/        LCD, backlight, keypad, console, font
lib/            string functions, printf
include/os/     interfaces
link/os.ld      memory map
tests/          QEMU tests
main.c          milestone-1 demo and self-test
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
