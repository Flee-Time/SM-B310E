# Stock DSP command logger (NOR patch)

Capture the **stock** firmware's real ARM→DSP command sequence during a genuine
first cell-tower association, by patching the stock NOR to log the command ring to
a RAM buffer and reading it back. This sidesteps QEMU entirely: on the real phone
the modem runs (real SIM, real cell, real RF), so the sequence is authentic ground
truth — exactly what the registration work needs. See
[docs/stock-first-association.md](../../../docs/stock-first-association.md).

## What it does
`patch.py` installs a trampoline on **`post_cmd` (FUN_0005f474)** — the single
function every ARM→DSP *command* passes through. The hook (`hook.s`, 60 bytes)
copies each 5-word command into a capture buffer in RAM, then re-runs post_cmd's
original prologue and continues. Commands only (setup, wake, frame-clock start,
dedicated-establish, RACH, PA-key — the decisive structure); per-frame FIs are
higher volume and can be added later with a second hook on `post_fi`
(FUN_0005ef76).

Buffer layout: word 0 = write offset (starts at 4), then packed 10-byte records
(the 5 command halfwords). Capped at 64 KiB.

## Build / apply (produces a patched image; does NOT flash)
```bash
export PATH="/d/Toolchains/arm-none-eabi/bin:$PATH"   # arm-none-eabi-as/ld/objcopy
python3 patch.py full-backup.bin full-backup-patched.bin --buf 0x10010000
```
`patch.py` assembles `hook.s` with the buffer base baked in, verifies the post_cmd
prologue (`f8 b5 04 00`) and the cave (zero-filled, 0x121390 in the XIP kernel), and
writes the patched NOR. The trampoline was verified by disassembly.

## Read the capture back
- **QEMU:** `pmemsave <buf> 0x10000 cap.bin` over the monitor, then decode word 0 as
  the offset and the rest as 10-byte records. (Note: QEMU **cannot** exercise this
  hook — the stock modem/L1 never runs there, so `post_cmd` is never called and the
  buffer stays empty. QEMU only confirms the patch applies and the image still boots.)
- **Hardware:** read the buffer region with spd_dump after an association, then decode.

## Deploying to the phone — open items (need care + owner OK before flashing)
1. **Buffer address (`--buf`).** The QEMU default `0x10010000` is the upper DSP-share
   window, which is 128 KiB in the model but only **4 KiB on real hardware**. On the
   phone pick a **free PSRAM** address (`0x34000000..0x343fffff`) the stock does not
   use — determine it first (dump PSRAM usage), or the log will corrupt live data.
2. **Flashing** the patched NOR (spd_dump / the NOR loader). Recoverable via the
   backup + our loader, but it is a real flash — do not do it without the owner's OK.
3. Optionally add the `post_fi` hook for the full FI stream.

## Why not just run it in QEMU
The stock modem subsystem never initializes in QEMU (the SIM driver needs an
unraised IRQ 10, so MM/registration never starts) — so there is no L1 traffic to
log there. The patch is the way to get the real sequence off real hardware.
