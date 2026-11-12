# Stock DSP call logger over USB (NOR patch)

Patches the stock firmware (e52q7a) so that it reports everything it hands
the DSP and everything it reads back from it on a USB port of its own while
it runs normally on the phone: real SIM, real cell, real RF. The log is the ground truth for our own layer 1
([docs/stock-first-association.md](../../../docs/stock-first-association.md),
[docs/dsp-arm-interface.md](../../../docs/dsp-arm-interface.md)).

On the PC the patched phone is two COM ports (`1782:3d00`; Windows binds the
Spreadtrum serial driver and names them after what the unpatched firmware
puts there):

| Port | Carries |
|---|---|
| `SPRD AT` (interface 0) | the logger: one text line per DSP call or read. AT commands no longer work here |
| `SPRD DIAG` (interface 1) | the stock's own log, untouched, in its 0x7e framing |

**Status (2026-11-12): works on the phone.** Version 6 is flashed (8 sectors,
verified by read-back).

- `build/stocklog/capture/v6-boot.dsp.log`: version 6, both directions, from
  5 s after power-on for three and a half minutes: 72,949 records, 22 dropped (one
  burst when the port was opened), no crash: 43,193 power-measure results,
  17,832 frame instructions, 8,710 responses, 1,164 bit-error reports and
  472 RX results. It holds a complete location update in both directions
  (uplink blocks in `~T`, downlink blocks in `~R`): IMMEDIATE ASSIGNMENT,
  AUTHENTICATION REQUEST, IDENTITY REQUEST, CIPHERING MODE COMMAND, LOCATION
  UPDATING ACCEPT, CHANNEL RELEASE, at ticks 191593..195194.
- `build/stocklog/capture/run8.dsp.log` + `run8.diag.log`: version 5, the
  first two minutes after power-on. 11,100 records on the logger's port, 66
  dropped (one burst when the port was opened), one cut line, no logger
  lines on DIAG, no crash.
- `run6.raw`: version 3 (lines on DIAG): two boots, each from the DSP set-up
  through a complete location update to `MN_PHONE_NORMAL_SERVICE`; 15,565
  records, none dropped. `usblog.py --decode run6.raw` prints them.

How it got here, because each step is a rule for the next change:

- **Version 1** formatted and sent each line inside the hook. The phone
  crashed three times (after 7 s, 143 s and 33 s of logging), each time
  right after a logger line. Hooks must only store a record.
- **Version 2** moved formatting and sending to the scheduler's idle loop,
  one record per pass. No crash, but the idle loop gets about 60 passes a
  second during a cell search, so 30% of the records were dropped.
- **Version 3** sent a batch per pass, on the DIAG port, by appending to the
  stock's log queue. No drops, no crash.
- **Version 4** moved the lines to the AT port and handed the USB driver the
  whole batch as one transfer. The phone crashed the moment the PC opened
  the port: on this port the stock never sends more than 64 bytes at once.
- **Version 5** sends 64 bytes at a time through the stock's own port write,
  the next piece from the port's transfer-done callback.
- **Version 6** (this source) adds what the ARM reads back from the DSP, and
  shortens two kinds of line on the wire to make room for it.

| File | Purpose |
|---|---|
| `patch.py` | checks the image, builds `logger.c`, writes the patched NOR and the changed 4 KiB sectors |
| `logger.c` | the code placed in free NOR: one function per hook, formats a line and queues it |
| `usblog.py` | PC side: reads the logger's COM port, optionally the DIAG port too, and decodes older captures |
| `../../../emulator/qemu/scripts/test-stock-usblog.py` | boots the patched image in QEMU with an emulated USB host |

## The ports

- The **UCOM** USB service (`ucom_main.c`, 0x41bd8..0x42a34) is a two-port
  virtual COM device, `1782:3d00`, vendor class (config at NOR 0xcb474):
  interface 0 = EP 0x81 in / 0x02 out, interface 1 = EP 0x83 in / 0x04 out.
  Its bind function (0x42080, mode 2) puts the SIO data port (`COM_DATA`, AT
  commands) on UCOM port 0 and the SIO debug port (`COM_DEBUG`, the log) on
  UCOM port 1. The controller has no further endpoints, so there is no room
  for a third port.
- A UCOM port sends nothing until the host **opens** it: class request
  0x21 / 0x22 with wValue 0x0201 for port 0, 0x0401 for port 1 (handler
  0x7dd9c). The Windows driver sends it when a program opens the COM port.
- The `usbservice` task (0x43630) starts UCOM at boot only if byte 0xa0 of
  NV item 22 is 4 (getter 0x2c46a; this phone has 5), and the bind happens
  only if the assert-mode byte (0x0425de8c) is 1 or 0x35 (this phone: 0x35).
  Two byte edits force both (`EDITS` in `patch.py`): 0x43678 `beq` -> `b`,
  and 0x42208 `cmp r0,#1` -> `cmp r0,r0`. The power-off charging boot (cable
  in, END not held) is left alone.
- With UCOM up the firmware switches its own `SCI_TraceLow` text on
  (0x0422d4d4 = 1): that is the stock log on `SPRD DIAG` (SIM, charger, RR/MM
  states, layer 1, ...).
- UCOM port 0 is written with `FUN_000426b0(port, buf, len)`: at most 64
  bytes, sent from the caller's buffer after the call returns; the port's
  callback (`+0x1c` in its block at 0x04277804) is then called with 1. The
  logger replaces that callback and the SIO data port's write (0x04251200),
  which cuts the AT channel off. Before the patch the AT port answered `AT`
  and `ATE1` and returned `+CME ERROR` to everything else tried;
  `AT+CFUN=1,1` restarted the phone.

## The hooks and the drain

Each hook replaces 4 bytes with a Thumb `BL` to a stub in the cave. The stub
saves r0-r7 and lr, calls the logger, restores everything and either jumps
to the call it replaced or runs the two instructions it replaced. The CPU is
an ARM926EJ-S: Thumb-1 only.

A hook runs in the caller's context (the layer-1 task, the DSP interrupt),
so it only copies a 40-byte record into a ring, with interrupts masked for
the copy. The ring and the drain's stack are the stock's layer-1 debug
buffer (0x23a entries of 16 bytes at 0x0423e728, pointer at 0x0422c940):
the stock writes it only when its assert-mode byte is 1, and this phone has
0x35. It holds 177 records.

The drain runs from the scheduler's idle loop (NOR 0x118178, in PSRAM at
0x0400eb9c), which calls an empty function through a veneer; `patch.py`
points the veneer's literal (NOR 0x11d92c) at the drain. The idle loop is
neither a thread nor an interrupt, has a 200-byte stack, and an interrupt
taken in it does not return to it. So the drain switches to its own stack,
turns the waiting records into text in its transmit buffer (1.5 KB, about
16 lines) without changing any other state, and commits with interrupts
masked: it advances the ring and writes the first 64 bytes to the port. The
transfer-done callback writes each following piece; the drain builds the
next batch once the buffer is out.

| Line | Hooked function | Site | Fields after the tick |
|---|---|---|---|
| | **ARM to DSP** | | |
| `~C` | `post_cmd` FUN_0005f474 | 0x5f478 | the 5 command words (word 0 = id) |
| `~F` | `post_fi` FUN_0005ef76 | 0x5ef7a | 11 frame-instruction words, 3 extension words |
| `~T` | `tx_desc_post` FUN_0005f332 | 0x5f33a | tag, block type, length, the block's bytes |
| | **DSP to ARM** (what the ARM reads back) | | |
| `~R` | RX ring read FUN_0005f692 | 0x5f6d8 | 9 header words, 54 data bytes (a decoded block, an SCH) |
| `~A` | RESP ring read FUN_0005f722 | 0x5f774 | the 5 response words |
| `~P` | power-measure read FUN_0005ed64 | 0x5edc8 | the 4 words of one result |
| `~B` | bit-error report read FUN_0005edec | 0x5ee3a | the 2 words of one report |
| `~M` | DSP message handler FUN_0003ab0a | 0x3ab0c | code, parameter, ack word |
| | **other** | | |
| `~S` | `SetState` FUN_0002f348 | 0x2f34c | old state, new state |
| `~H` | `usbservice`, a service starts | 0x436b6 | service number, `v1` |

Details that matter when reading a log:

- A line ends in CR LF. The tick in it is the time of the hook, not of the
  send.
- Two kinds are shortened on the wire, because this port is slow, and
  `usblog.py` writes them out in full: a `~F` line ends in `*` where the
  rest of its 14 words are `ffff`, and a `~R` result comes as three lines
  `~R tick offset total bytes`. A plain terminal shows the short forms.
- The read hooks sit on the copy out of the share window (its memcpy, or
  for `~A` the index write after it), so a line is what the ARM took, at the
  moment it took it. They log only while the PC has the port open.
- Nothing is sent until the PC opens the logger's port. Until then the ring
  keeps the first 140 records since boot, which covers the DSP set-up; `~F`
  and `~M` are not recorded before the port is open, so they cannot crowd
  those out.
- `~D tick count` reports records lost to a full ring. On this port the
  phone sends 64 bytes per USB interrupt, so a long burst can overflow it.
- `~M` is logged when the ack word is 0 (a message is waiting) or the code is
  0xffff (DSP assert). On the phone it shows code 5, parameter 0x258,
  several times per frame burst.
- `~H` shows the USB service numbers as the stock starts them (2 is the
  forced start).
- `--no-fi` leaves `post_fi` unpatched; `--no-pm` the power-measure read
  (it is the bulk of the lines while the phone scans).

Not hooked: the DSP's own scalars in the share window that no reader
function copies out (timing advance, the level the DSP's AGC holds), the
parameter-record bodies behind command 0, and the analog and RF register
writes.

RX result tags seen on the phone (header word 0): 1 SCH, 7 and 8 the common
control channels (8 carried the IMMEDIATE ASSIGNMENT), 5 the SDCCH downlink
blocks and 3 the SACCH downlink blocks of a location update, 9 while camped.
The block bytes are stored a halfword at a time, high byte first: swap each
pair to get the frame.

## The cave

`logger.c` goes to NOR **0x40c000**: the stock image ends at 0x3fb3fc, our
boot loader owns 0x3fc000..0x40c000 (`boot/boot.ld`), and the flash is erased
from there to the resource partition at 0x4a0000 in both NOR backups. Thumb
`BL` reaches it from every hook (under 4 MiB).

The earlier version of this tool used 0x121390. That address is inside the
`.data` load image (NOR 0x11eac4..0x12e184, copied to 0x0422c4e8 at boot), so
code there would have become the initial value of stock variables. Its hook
also used Thumb-2 instructions (`b.w`), which this CPU does not have. It was
never run on the phone.

## Build

```bash
export PATH="/d/Toolchains/arm-none-eabi/bin:$PATH"
python tools/dsp/stock-dsp-logger/patch.py tools/spd_dump/full-backup.bin --output build/stocklog
```

`build/stocklog/` then holds `stock-usblog.bin` (the whole NOR, for QEMU),
`sector-<addr>.patched.bin` / `.pristine.bin` for the 8 changed sectors and
`flash.txt` with the spd_dump commands. These files contain stock firmware:
they stay in `build/`.

## Test in QEMU

```bash
python emulator/qemu/scripts/test-stock-usblog.py --qemu build/qemu/build/qemu-system-arm.exe \
  --firmware build/stocklog/stock-usblog.bin --output build/validation/stock-usblog \
  --hold-end 12 --display gtk
```

END must be held through the boot when the cable is in, as on the phone.
What the run shows, and what it does not:

- The device is `1782:3d00` with the two vendor interfaces, and the firmware
  takes the port-open request (port state 2).
- No bytes reach the emulated host: `sc6530_usb` models the controller as
  our own drivers use it, not the stock driver's IN transfers. The test
  therefore reads the logger's state from RAM and checks that the idle-loop
  drain built a batch and handed its first piece to the port.
- A few seconds after power-on the stock runs its cable-removed path (0x1f650
  with 0: it masks INTC line 25), about when the emulated charger shows
  "Charging paused for battery durability". Later requests then go
  unanswered. Open the port right after enumeration, as the test does.
- Do not set `SC6530_DSP_SNIFF` (`--dsp-sniff`) for this test: it leaves the
  DSP interrupt raised and the stock never gets past its logo.
- The emulated modem does not run, so the DSP lines need the phone.

## On the phone

The patch changes 7 sectors of the stock kernel plus one free sector. The
boot ROM's download mode is not affected, so the revert in `flash.txt`
always works. Take the owner's OK before any NOR write.

1. Copy `build/stocklog/sector-*.bin` to `tools/spd_dump/`, put the phone in
   download mode and run the PATCH command from `flash.txt` (each
   `erase_flash` asks for a typed `yes`; keep the cable in from the first
   erase to the last write). The phone answers the handshake once per
   connection: unplug and reconnect in download mode before every spd_dump
   run, or it stops with "ver expected".
2. Read the sectors back (`read_flash 0x80000003 <addr> <size> file`) and
   compare them with the `.patched.bin` files.
3. Start `python tools/dsp/stock-dsp-logger/usblog.py --out dsp.log --diag diag.log`.
   It waits for the two COM ports, prints the logger's lines and saves both
   logs with the PC's time on each line. Any serial terminal on `SPRD AT`
   shows the logger's lines too.
4. Boot the stock firmware with the cable in and END held to the logo.
5. To go back, run the REVERT command from `flash.txt`.

State of the phone on 2026-11-12: patched with version 6. The files that
match it are in `tools/spd_dump/` (`sector-*.patched.bin`, with the
`.pristine.bin` set for the revert); the full NOR backup taken before the
first write is `tools/spd_dump/nor-backup-20261112.bin`.
