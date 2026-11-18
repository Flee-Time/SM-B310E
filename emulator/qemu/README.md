# SC6530C QEMU machine

`machine/hw/arm/b310e.c` defines the board. `machine/hw/misc/` contains
the peripheral models. `scripts/` holds machine-install helpers, captures
and regression tests. Build implementations live in `scripts/targets/`
at the repository root.

Build with `.\build.ps1 qemu` on Windows or `bash build.sh qemu` on Linux.
The shared build guide lists dependencies and `--qemu-source` overrides:
[build.md](../../docs/build.md). QEMU is pinned to v11.1.0 and builds headless
with PNG captures; no administrator privileges are required.

The stock ROM is local research data and is not committed. Current stock boot,
audio evidence and capture commands are in [emulator-audio.md](../../docs/emulator-audio.md).
The memory map and older boot-path research are in [b310e-qemu.md](../../docs/b310e-qemu.md).
Rockbox SD-image and playback checks are in [rockbox-audio.md](../../docs/rockbox-audio.md).
The DSP model implements the ARM-facing protocol, not a Teak instruction CPU.

## Instruction counts (b310e-prof)

`plugins/b310e-prof.c` is a TCG plugin for the OS benchmark
(`os/tests/bench-instr.py`). It counts every guest instruction at its
address (an inline add before it runs: exact, an instruction icount replays
after MMIO is not counted twice) and, when asked, holds the vCPU just before
a WFI. Run with `-icount shift=2,sleep=off`, the guest's clock then stands
still while held, so keys, dumps and screenshots land on the same
instruction every run and two runs count the same.
`scripts/build-prof-plugin.py` builds it against the QEMU in `build/qemu`
(plugins are on there; QEMU is not rebuilt) into
`build/qemu-plugin/b310e-prof.dll` (`.so` on Linux); the benchmark builds it
when it is missing or older than its source.

```
python os/tests/bench-instr.py --json before.json          # every phase
python os/tests/bench-instr.py --phase list-scroll --phase dialog
python os/tests/bench-instr.py --compare before.json after.json
```

The plugin's control is a folder of files (`ctl=DIR`; the driver writes
`cmd-N`, the plugin answers `ack-N`): `run` (hold at the next WFI),
`until T` (hold at the first WFI with the kernel's `ticks` >= T; `ticks=ADDR`
gives its address), `dump PATH` (the counters as `vaddr size count` lines),
`free`. The analog EIC interrupt is modelled (below), so the OS waits for END
by interrupt as on the phone ("end key: by interrupt" in its log) and the
idle phases no longer count a 20/100 ms END poll; `-global
sc6530_adi.eic-irq=off` brings the old polled path back for a comparison.

## Analog EIC interrupt (END, headset button, charger)

`hw/misc/sc6530_adi.c` models the analog die's debounce EIC block the way
the stock firmware and `os/drivers/keypad.c` use it
([docs/eic-interrupt.md](../../docs/eic-interrupt.md)), 16 channels at ADI
0x82001900: DATA +0x00 (the input levels: END channel 3, headset button 4,
charger 2 when DMSK bit 2 is set), DMSK +0x04, IEV +0x14 (trigger level, 1
high), IE +0x18, RIS +0x1c, MIS +0x20 (RIS & IE), IC +0x24 (write 1 clears
RIS), TRIG +0x28 (write 1 starts a detection), CTRL0(n) +0x40 + 4n
(debounce time in bits 11:0, bit 14 debounce on). A channel's input at its
IEV level for the debounce time (milliseconds of virtual time) latches its
RIS bit; then ANA_INT bit 5 (+0x580 masked, +0x584 raw, enabled by +0x588
bit 5) raises INTC line 24 until IC clears it. Level triggered, one
detection per TRIG write (stock and the OS write TRIG each time they arm).
Guesses, not read anywhere: that a detection needs TRIG and ends at its
latch, the debounce unit (Linux's gpio-eic-sprd uses ms), that IC and TRIG
read 0, that RIS latches with IE off. Reset clears all of it.
