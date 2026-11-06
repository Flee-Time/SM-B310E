# USB device controller and Rockbox mass storage

The SC6530C has one full-speed USB device controller ("sx_udc" in the stock
sources). Rockbox now drives it as a mass-storage device exposing the SD card.
Mass storage was chosen over MTP. Rockbox's USB stack ships it, every desktop
OS mounts it without drivers, and the stock firmware uses the same class.

**Status (2026-11-02):** mass storage works on the phone: the card mounts on
Windows, and unplugging is clean. The fourth build fixes the HID failure
and the resets behind the slowness found on hardware; that build is
verified in QEMU only. The hardware reports below track what was learned.

**Hardware report 1 (2026-11-02):** plugging in any USB cable froze Rockbox.
QEMU cannot reproduce this: in the model, every register access and every
interrupt behaves. Three suspects:

1. **Bus stall.** The project's hard rule is that touching an unpowered or
   clock-gated block stalls the AHB for good. The card boot never runs the
   stock firmware, and an earlier note already suspected "USB unpowered"
   (`firmware/` builds `os-sd.bin` without USB for that reason).
2. **Interrupt storm.** Line 25 may stay asserted by a source the driver
   doesn't clear.
3. **Extra bring-up steps.** The first driver pulsed the controller's AHB
   reset and toggled the LDO a second time. The stock code does neither.

The second build follows the stock bring-up step for step and drops both
extras. It also guards line 25: 500 interrupts with nothing to do within one
tick mask the line and switch to servicing from the tick (tested in QEMU with
`--irq-stuck`). Finally, it shows its progress on screen (below).

**Hardware report 2 (2026-11-02):** plugging in no longer froze, and
Windows enumerated the device. That confirms bring-up, addressing and
descriptors on the phone. Two problems remained:

- **Code 10 "SPRD DIAG (COM12)".** Windows bound the UNISOC driver
  installed for spd_dump to Spreadtrum's id `1782:4d01`. The device now
  uses the pid.codes test id `1209:0001`, which no vendor driver claims,
  so Windows loads its own class drivers. That id is fine for private use;
  a published build needs a pid.codes PID of its own.
- **Freezing on unplug.** QEMU cannot reproduce it: unplug and re-plug pass
  there, with and without a stuck interrupt. The exit path now leaves the
  USB AHB clock on (reaching a gated block stalls the bus, the same reason
  the RK27xx driver never gates its clock) and logs each exit step.

Enumeration was also slow because the first diagnostic build waited 3 s
inside Rockbox's USB thread, the thread that answers the host.

**Hardware report 3 (2026-11-02):** mass storage works on the phone. The
card mounted as a Windows drive, files opened, and unplugging no longer
froze. Two problems remained: the HID interface failed with Code 10, and
access was very slow (about 30 s before Explorer opened the drive). The
card log (`usb-log.txt`) showed why:

- **SET_ADDRESS and SET_CONFIGURATION never raise a SETUP.** The
  controller completes both itself. Mass storage worked only through the
  fallback that treats the first class request (GET_MAX_LUN) as
  "configured".
- **Long EP0 answers were cut off.** Every request answered in one 64-byte
  packet succeeded. The HID report descriptor (118 bytes; Windows asks for
  182) failed three times, and each failure made Windows reset the device,
  which explains the slowness. The driver had set EP0 `TR_SIZE` per packet.
  The boot loader (0xa1a4) sets it to the whole data stage and steps on
  each packet's bit 0. So the controller evidently ends the data stage at
  `TR_SIZE` bytes.

Fixes in the fourth build:
- EP0 follows the boot loader: `TR_SIZE` = whole stage, next packet on
  bit 0, and no software ZLP.
- The core is told the device is configured on the first request for an
  interface or endpoint, or the first class or vendor request.
- A 64-byte device descriptor read on a configured device (how hosts open
  enumeration after a reset) is treated as a bus reset.

The QEMU model now behaves the same way (no SETUP for either request,
`TR_SIZE`-bounded data stage), and `test-usb.py` fetches the HID report
descriptor as Windows does.

**Hardware report 4 (2026-11-02):** HID now starts. Then, after a while:
- HID turns to Code 10;
- the drive appears about 30 s after plug-in and reports "insert a disk".

The log shows a clean first enumeration, the card handed over, and then
Windows repeating the 64-byte and 18-byte device descriptor reads hundreds
of times: a reset it never recovered from. The 32-entry SETUP ring
overflowed, so the reset's trigger isn't logged. Two driver bugs kept the
device from recovering:

1. **The bus-reset handler called `usb_core_bus_reset()`.** The core defers
   that to its USB thread and drops every SETUP until then, including the
   requests the host re-enumerates with. It also never reprogrammed the
   endpoints, which a bus reset wipes on this controller. B310E-OS's USB
   link already hit this on replug.
2. **Re-running SET_CONFIGURATION** for the request that implies it drops
   the transfer completions queued before it. The core's
   `usb_clear_pending_transfer_completion_events()` throws that very
   request away.

Fixes in the fifth build:
- A detected reset reprograms EP0 and the endpoints in use and drops the
  transfers in flight. It doesn't touch the core's reset path.
- The request that implies the configuration is held until the core calls
  `usb_drv_ep_init()` for the new configuration, then delivered.
- The log now keeps 256 SETUPs and 16 KiB of pending lines, and writes
  counters every 5 s while connected (packets in/out, detected resets).

The QEMU model now wipes the endpoint setup on a bus reset, and
`test-usb.py` resets the configured device and checks that mass storage
recovers. The fourth build fails that phase as it failed on the phone; the
fifth passes.

**Hardware report 5 (2026-11-02):** HID failed with Code 10 straight away,
and no drive appeared. The log shows the first enumeration completing,
then SET_IDLE and the HID report descriptor request (182 bytes), which
failed before any reset. Windows then reset the device repeatedly. The
fourth build got past this request. QEMU, now replaying Windows' exact
request order, passes, so the failure depends on controller behavior the
model doesn't capture.

The sixth build:
- returns to the fourth build's first-enumeration path; the request that
  implies the configuration is held back only after a reset, when the core
  is already configured;
- logs EP0 at transfer level (below), so the next report shows how far a
  failing reply gets.

**Hardware report 6 (2026-11-02):** Windows listed the "Rockbox Internal
Storage" disk, but no volume, and HID still failed. The EP0 transfer log
pinned it down:

- Every reply of up to 64 bytes completed.
- The 118-byte HID report descriptor was abandoned at 0 of 118 bytes. Its
  first packet left, but the driver never queued the second.
- The driver moved on only once TX start had cleared. On the phone, TX
  start evidently stays set until `TR_SIZE` bytes have gone out; the boot
  loader (0xa1a4) steps on bit 0 alone.
- The failed reply made Windows reset the device over and over, so mass
  storage only ever received 4 commands.

The seventh build:
- steps EP0 on bit 0 / bit 9 alone;
- drops HID: the controller declares bulk transfers only, so Rockbox builds
  mass storage without its HID class (buttons as a PC keyboard or media
  remote);
- logs the first 400 bulk transfers of each connection.

The QEMU model now keeps EP0 TX start set until `TR_SIZE`. The sixth build
fails the HID phase against it, as on the phone. A HID-enabled seventh
build sends all 118 bytes (`ep0 irq sts 00000001 ctrl 08040000` mid-reply).

**Hardware report 7 (2026-11-02): mass storage works on the phone.** The
volume mounts in Explorer, but only after a long wait.

The log shows a fast bring-up: about 0.6 s from plug-in to the card being
handed to the host. What follows is Windows reading about 8 MB in roughly
95 s (about 85 KB/s), most likely the FAT of a large FAT32 card. The rate,
not enumeration, is the problem.

Two suspects:
- **The SD path:** a single-block CMD17 per 512 bytes, a whole D-cache
  clean per block, and an IRAM bounce copy. (Since 2026-11-06 the SD
  driver moves up to 32 blocks per command, CMD18/CMD25; see
  `docs/rockbox-recording.md`.)
- **The USB path:** one interrupt per 64-byte packet.

The eighth build measures which one before anything changes. Every tick
it samples whether the SD driver (`sd_busy_now`), a bulk IN transfer or a
bulk OUT transfer is busy, and every 5 s it logs throughput and those
shares. The log thread also keeps running while the PC owns the card,
buffering in RAM, so the session's lines carry real timestamps.

**Hardware report 8 (2026-11-02):** the sampling located the slow mount:

| Item | Value |
|---|---|
| Throughput while mounting | 225 KB/s |
| Time sending bulk IN packets | 83% |
| Time in the SD driver | 16% |
| Data Windows reads before Explorer opens the drive | about 7.7 MB (about 30 s) |

At about 3.5 packets per millisecond, the device spends roughly 0.28 ms per
64-byte packet. The CPU work per packet is a few microseconds, so the host
is evidently told "not ready" between packets and comes back later.

The ninth build feeds bulk packets from the interrupt that ended the
previous one. It polls for each to leave and loads the next at once, as the
boot loader does on EP0: at most 32 packets per interrupt, and 3000 polls
per packet before falling back to the interrupt. The rate line counts how
many packets went out this way (`burst N of M packets`). If bursts don't
lift the rate, the host side sets the pace and the next step is DMA.

**Hardware report 9 (2026-11-02):** bursts fed 46% of packets without
waiting for an interrupt, but throughput stayed at exactly 224–225 KB/s
(about 3,600 packets/s). That rules out the driver's turnaround time. With
a single 64-byte FIFO the device has to answer "not ready" until software
reloads it, and the host then comes back on its own schedule. The tenth
build therefore moves bulk IN by DMA, as the stock driver does.

**Hardware report 11 (2026-11-05): the twelfth build failed.**
- Rockbox panicked: `dc_writeback_callback() could not write sector`, a
  disk-cache write to the card failing. The card's filesystem checked
  clean afterwards (`chkdsk E:`).
- Windows then reported Code 43 ("A request for the USB device descriptor
  failed").
- The card log shows no SETUP at all, in any session of that build,
  including the first plug before the panic (`irq 0`, `setups 0`). The
  bring-up stages all complete; the controller simply never sees the
  host's first request.
- The phone stayed on battery throughout, and the bring-up never resets the
  controller, so a controller left wedged by the DMA experiment would stay
  wedged across Rockbox reboots. That is a hypothesis, not confirmed.

The thirteenth build is PIO with bursts again (`USB_SC6530_DMA` 0),
identical to the eleventh, with the card log on. Before testing it, pull
the battery so the USB block powers down completely.

**Hardware report 12 (2026-11-06): USB works again.** After a day of
torch, recording and FM work, mass storage enumerated with no change to
the USB driver. The build was PIO with bursts and the card log on. Windows
copied 84 files (268 MB) to the phone at about **347 KB/s**, faster than
the 225 KB/s measured earlier.

The 2026-11-05 failure covered every mode, including the boot ROM, and
D- measured about 0.6 V. It was therefore not permanent SoC damage; the
likely cause is an intermittent fault on the D+/D- path or the new port,
not the driver. If it returns, check the port's solder joints before
suspecting the driver.

**TODO: bulk DMA.** Both DMA builds moved data, but:

| Build | Setup | Result |
|---|---|---|
| Tenth | 32-byte requests | 70 KB/s, below PIO's 225 KB/s |
| Twelfth | 64-byte requests, endpoint +0x1c = 400 | the panic above |

Open questions:
- the meaning of the endpoint register at +0x1c;
- whether a DMA-driven endpoint needs the stock interrupt bit 13 flow (the
  stock code starts its DMA from an IN-token interrupt, 0x1fb3c);
- whether USB DMA contends with the SD controller's own DMA (this board
  already has two known SD-contention hangs);
- how to recover the controller (the stock clock device's reset, 0x67c9c).

### Bulk DMA again, IN and OUT (2026-11-06)

**Why again.** The twelfth build's two failures turned out to have other
causes. Its disk-cache writeback panic was the SD driver giving up on
slow card writes (fixed: longer waits, retries, multi-block writes). Its
Code 43 / "no SETUP" happened on 2026-11-05, the day the phone's USB was
dead in every mode, boot ROM included (hardware report 12). Stock's DMA
setup was therefore never really measured.

**What runs by DMA now** (`USB_SC6530_DMA` 1):
- **Bulk IN** exactly as the twelfth build (stock EP1 `0x1f288`).
- **Bulk OUT**, new, as stock EP2/EP4 (`0x1f454`, `0x1f524`): channel
  from the FIFO into the buffer, 64 B requests, `+0x1c` = 400, packets of
  64, `TRANS_SIZE` the whole length, buffer ready once; transfer end is
  the completion. Channel ids 9 (EP2) and 10 (EP4). The fragment
  register is `0xA4100040`: decoding stock's encoder `0x32a68`, bit 20
  alone means "source fixed" (bits 20+21 "destination fixed", the IN
  case), and the byte swap `[27:26]` = 1 comes from one struct field
  that stock sets to 1 for both directions.

**Every OUT transfer starts by PIO.** Rockbox posts the same 512-byte
receive for a 31-byte CBW and for a one-block WRITE. A short packet does
not fill a 64 B DMA request; the first QEMU run left a CBW's tail in the
FIFO and mass storage stalled both endpoints. So the first packet always
comes by PIO, and only a full first packet with a long, packet-aligned
remainder hands the rest to the DMA. The DMA only ever sees full packets.

**Fallbacks.** An IN transfer the DMA doesn't finish within 0.5 s starts
over by PIO (as before). An OUT transfer with no packet for a whole second
stops its channel, takes what the DMA moved (length minus the channel's
remaining count), drains a packet left in the FIFO by PIO and carries on
by PIO. Either way DMA stays off for the rest of the connection; the log
shows `dma timeout`.

**QEMU** (`test-usb.py`, now also a 128 KiB write verified in the image):

| Run | Reads | Writes |
|---|---|---|
| DMA | 941 KB/s | 1153 KB/s |
| `--no-dma` (IN times out, all PIO) | 300 KB/s | 211 KB/s |
| `--no-dma-out` (OUT salvage, then PIO) | 1050 KB/s | 256 KB/s |
| previous PIO-only build | 224 KB/s | 256 KB/s |

QEMU's speeds do not predict the phone (the tenth build was fast in QEMU
too and measured 70 KB/s on hardware); they only show the paths work. The
model now drains OUT packets by DMA, keeps buffer ready armed until
`TRANS_SIZE`, and has `dma-out-requests=off` for the salvage test.

**Hardware report 13 (2026-11-06): DMA is slower than PIO, and the OUT
salvage damaged the card.** From the card log:

| Phase | Measured |
|---|---|
| Mount (bulk IN by DMA) | ~138 KB/s, about 48 DMA transfers per 5 s; Explorer needed about a minute |
| A 2 GB copy to the phone (bulk OUT by DMA) | ~0 KB/s: one 4032-byte remainder trickled in over ~18 s, then `dma timeout` |
| Afterwards | the host reset the bus and only re-read descriptors; the copy never resumed |

The watchdog's salvage read the channel's BLOCK register as a remaining
byte count. It read 0 ("4032 moved") while the controller had counted
3968 bytes in, so the code trusted a buffer the DMA had not fully
written. The 4 KB write it completed landed in the root directory:
`chkdsk` found an invalid long-name entry (System Volume Information),
two garbage `m.m` entries with invalid clusters, and ~401 MB in lost
chains (the copy's clusters, its entry gone). All 1,589 other files
checked clean.

**Conclusion:** this controller's DMA does not help on the phone: IN is
slower than PIO (as the tenth build found), OUT is far slower and stalls.
Both are off again (`USB_SC6530_DMA` 0) and the log is off. The code
stays for reference; it must not be turned on again without a
hardware-verified way to know how many bytes an OUT DMA really moved.

### Stock-style bulk OUT DMA (2026-11-06)

**Stock on the same phone and card** (Explorer copies): 789 KB/s to the
phone, 355 KB/s from it (sometimes bursts to 1.4 MB/s). Reads match our
PIO, so IN stays PIO (`USB_SC6530_DMA_IN` 0). Writes are twice ours.

**Stock's OUT sequence** (`0x1fc4a`, EP2's interrupt handler; EP4
`0x1fe80` is the same) never arms a receive ahead of time:
1. The host's packet is refused while buffer ready is clear, which sets
   endpoint interrupt **bit 12**.
2. On bit 12: start the DMA for the whole request (`0x1f454`), set buffer
   ready, enable bit 9 instead of bit 12, clear bit 12.
3. On transfer end (bit 9): read the count at `+0x04`, stop the channel,
   complete. Stock never reads the channel's BLOCK register.

The first attempt skipped step 1 and armed the DMA at once; packets then
trickled in.

**This driver** (`USB_SC6530_DMA_OUT` 1):
- the first packet by PIO, as before (CBW vs data, above);
- after a full first packet with a long remainder, buffer ready stays
  clear and only bit 12 (plus 9) is enabled; bit 12 starts the DMA;
- transfer end is a success **only with the full count**;
- a short count, or no progress for a second (no bit 12, or no data)
  **fails the receive**: Rockbox's mass storage then writes nothing, the
  host resets the device and retries, and DMA stays off for the session.
  There is no salvage.

**QEMU:** the model raises bit 12 for a refused OUT packet. `test-usb.py`
now retries a failed write after a bus reset and re-enumeration, as
Windows does:

| Run | Writes | Result |
|---|---|---|
| normal | 1233 KB/s | 128 KiB write intact in the image |
| `--no-dma-out` | 168 KB/s | first write failed, retried by PIO, intact |
| `--no-dma` | 160 KB/s | the same |

**Hardware report 14:** that build froze the phone while starting an
Explorer copy. The card was clean afterwards (chkdsk found nothing).

### Serialized write DMA and a freeze recorder (2026-11-06)

**Suspected cause of the freeze.** Rockbox's mass storage writes chunk k
to the card while it receives chunk k+1, so the USB OUT DMA into PSRAM
ran alongside the SDIO DMA. This board already hangs on SDIO DMA
contention (SDIO DMA into PSRAM, a held key during SDIO DMA). The second
suspect was an interrupt storm from bit 12.

**Serialized:**
- The SD driver (`transfer_sectors`) waits while a USB OUT DMA runs
  (`sc6530_usb_out_dma`, bounded at 2 s) before it claims `sd_busy_now`.
- A bit 12 that arrives while `sd_busy_now` is set masks bit 12 and holds
  the start back. The SD driver calls `sc6530_usb_sd_idle()` when its
  transfer ends, and that starts the held DMA.
- The OUT watchdog does not count time spent held back.

**Guarded:**
- Bit 12 is acted on only when it is enabled; the status latches a
  refused send even while it is off (the next CBW at every transfer end).
- An enabled bit 12 that nothing waits for masks itself.

**Freeze recorder** (only with `USB_SC6530_DIAG`):
- **Breadcrumbs:** a ring of 64 `{tick, code, value}` at IRAM `0x4003f000`
  (bank 3, which the boot menu enables; nothing else uses it). Magic
  `USBC` is set at connect (stage 9) and cleared at clean exit.
  - `S` session start
  - `W` waiting for the host's send (remaining bytes)
  - `T` bit 12 started the DMA
  - `K`/`k` held for the card / started after it
  - `D` DMA started (length)
  - `E` transfer end (count)
  - `U` unexpected bit 12 (status)
  - `F` OUT timeout (rx count)
- **Watchdog:** the hardware watchdog (ADI `0x82001480`, as `system_reboot`
  sets it up) is armed for ~4 s at connect, fed every 0.5 s from the tick
  task and stopped at exit. A hang with interrupts off or a stuck tick
  resets the phone. Since report 15 it is armed only with
  `USB_SC6530_WDG` as well: it was never verified on the phone, and a
  wrong reset during a write could damage the card. `--freeze` needs a
  build with both `USB_SC6530_DIAG` and `USB_SC6530_WDG`.
- **On the next session:** the first lines of `/usb-log.txt` read
  "previous USB session never ended" with the steps. If the boot chain or
  the reset clears IRAM, only the reboot itself is the sign.
- **Rate line:** each one now says how many write DMAs were held for the
  card.

**QEMU:**
- **Watchdog model:** the ADI model has the watchdog (`-global
  sc6530_adi.watchdog=on`): lock, LOAD reload on a LOAD_LOW write, and a
  machine reset on expiry.
- **Machine reset:** a run-time reset now also resets the CPU and reloads
  the Rockbox image, as the real boot chain does; IRAM is kept.
- **Test results:**

| Run | Writes | Result |
|---|---|---|
| `--watchdog` | 1362 KB/s | intact; 4 of 9 write DMAs held for the card; no watchdog reset |
| `--watchdog --no-dma-out` | 174 KB/s | first write failed, retried by PIO, intact |
| `--watchdog --no-dma` | 153 KB/s | the same |
| `--freeze` | – | all interrupts masked after a 64 KiB write: reset after 4.02 s, Rockbox reconnected, and the card log listed the frozen session's 22 steps (`S`, then `W T D E` per chunk) |

**Hardware report 15 (2026-11-06): bulk OUT DMA is not safe; off.**
- **No freeze this time.** Every copy, to or from the phone, stuck at 0;
  the phone stayed usable.
- **The log** (two sessions, reads by PIO at 230 KB/s): the first write
  that went to DMA started (`1 out transfers`, nothing held for the
  card) and never finished. Its 1 s timeout never fired because data
  trickled in and counted as progress. Windows reset the device after
  ~16 s.
- **Root directory damaged.** Two `m` entries appeared again after the
  card had been cleaned. The sector, read raw after chkdsk had marked
  the entries deleted, showed entries 1–5 (the "System Volume
  Information" LFN) followed by **128 bytes (two packets) of one word
  repeated**, `6d 00 65 00`. That is the last word of the preceding
  packet, which is what a read of the empty FIFO returns.
- **So the channel reads the FIFO whether or not a packet has arrived.**
  Its requests are not paced by the controller as set up here (request
  map, `+0x1c` = 400, fragment register). The controller's count still
  says the transfer is complete, so the full-count check cannot catch it.
- **The first DMA build's damage** (the earlier `m` entries) has the same
  signature, so the salvage was not its only cause.
- **Waiting for bit 12 the stock way changed nothing.** The slow bulk IN
  DMA (138 KB/s) probably has the same pacing problem.

`USB_SC6530_DMA_OUT` is 0 and the card log is off again: the shipped
build is all PIO. The serialization and the freeze recorder stay in the
code and are inert without OUT DMA or `USB_SC6530_DIAG`; the QEMU
watchdog model stays. Any further DMA attempt needs a hardware test on a
scratch card first: one known pattern written by DMA and read back raw,
before Windows writes anything.

### PIO without bursts (2026-11-06)

**Measurement build.** It kept the card log on and rotated three ways of
handling a bulk packet's end every ~10 s:
- **A:** the bursts of report 9 (up to 32 packets per interrupt, 3000
  polls each);
- **B:** no bursts, one interrupt per packet;
- **C:** bursts with 300 polls.

Each rate line named the setting and the share of time spent in the USB
interrupt (timed with the tick's 26 MHz counter). The FIFO copies also
went from bytes to whole words (`__builtin_bswap32`) for aligned buffers.

**Hardware report 16:**

| Setting | Reads | USB interrupt (polling) |
|---|---|---|
| A | 230 KB/s | 60–67% (58–64%) |
| B | 230 KB/s | 3% (0%) |
| C | 230 KB/s | 13% (9%) |

- **Reads** run at 230 KB/s whatever the driver does: the PC sets that
  pace (report 9).
- **Writes:** Explorer showed up to **710 KB/s** while the settings
  rotated, against 347 KB/s with A alone (report 12). The write's lines
  never reached the card: the 16 KB RAM buffer for lines logged while
  the PC holds the card filled up first. So which setting gave it is
  inferred, not measured: the word copy saves about a microsecond per
  packet, far too little, which leaves B or C. A's polling, with
  interrupts off, starved the thread that writes the data to the card.

**Shipped:** B. Bursts are removed, the card log is off, and the interrupt
share stays in the log's rate line (`irq N%`). A long write with the log
on would confirm the write speed per setting; the log's RAM buffer would
then need to be larger, or the bulk lines fewer.

### Twelfth build: DMA exactly as stock EP1 does it

A second pass over the stock bulk routines found two differences from the
tenth build:

| | Stock EP1 IN (0x1f288) and EP2/EP4 OUT (0x1f454, 0x1f524) | Tenth build |
|---|---|---|
| Bytes per DMA request | 64 (a whole packet) | 32, copied from the EP3 routine (0x1f1c6) |
| Endpoint register +0x1c | low half set to 400 on every DMA start | never written |

PIO never touches +0x1c, so it belongs to the endpoint's DMA mode. Its
meaning is unknown.

The twelfth build matches stock on both points, with `USB_SC6530_DMA` back
to 1. With 32-byte requests issued only after a "not ready", each packet
cost two host retries; 64-byte requests should cost at most one. If the
phone still falls short of PIO's 225 KB/s, the build goes back to PIO and
bulk speed stays a TODO.

The model stores +0x1c. All QEMU runs pass (1 MB reads, reset, replug,
stuck interrupt, the no-DMA fallback, the charger test).

### Bulk IN by DMA (tenth build; compiled out since the eleventh)

**Stock sequence** (EP3 IN at 0x1f1c6; the siblings at 0x1f288, 0x1f454
and 0x1f524):
1. Allocate a channel (0x1f092, which calls the DMA library at 0x1aa74).
2. Route the endpoint FIFO's request by writing the channel id to
   `0x20102040 + 4·k`, where k is the FIFO index (2 EP1, 3 EP2, 4 EP3,
   5 EP4).
3. Configure the channel through the library's config struct (0x1aa22).
4. Start the channel (0x1a97e), with no channel interrupts (0x1a9fc).
5. Set the endpoint's packet size to 64, set `TRANS_SIZE` to the whole
   length, and start TX once. The endpoint's transfer end (bit 9) is the
   completion.

**The library** (`dma_phy_v5.c`): channel ids run 1–32, with registers at
`0x20100fc0 + id·0x40`. Rockbox's PCM, hardware-verified, uses ids 3 and 4
and routes its requests with the same ids. 0x32a68 encodes the fragment
register differently for chip id 0x65300000. The phone (0x6530c000) takes
the other branch, and the PCM's `0x50300140` decodes exactly under it.

The driver's channel setup (EP1 uses id 7, EP3 id 8):

| Register | Value |
|---|---|
| SRC | buffer, after `commit_dcache_range()` |
| DST | the endpoint's IN FIFO |
| FRAG | `0xA4300020`: 32-bit source and destination, field [27:26] = 1, destination fixed (bits 20+21), 32 bytes per request |
| BLOCK | the length |
| INT | statuses cleared, nothing enabled |
| CFG | 1 |

The AHB DMA clock (`0x20500060` bit 0) is set, as the PCM does.

**When DMA is used:** aligned transfers of at least 512 bytes that are a
multiple of 64, i.e. the mass-storage sector data. Everything else (CSWs,
INQUIRY, EP0) stays PIO. Bulk OUT (writes from the PC) stays PIO too.

**Assumptions to confirm on the phone:**
- **Field [27:26] = 1 is a byte swap into the FIFO.** Only the stock USB
  driver sets it, and the FIFO's wire order is most significant byte first
  (the PIO path swaps every word). If this is wrong, reads arrive
  byte-swapped and Windows sees a damaged volume.
- **The request routing and the channel id base**, as above.

**Fallback:** a DMA transfer that hasn't finished after 0.5 s is restarted
by PIO, and DMA stays off for the rest of the connection. The log then
shows `dma timeout`. QEMU checks this path with `--no-dma`.

**QEMU:** the DMA model serves endpoint FIFO requests
(`sc6530_dma_hw_request`, request 17 + FIFO index) and applies field
[27:26] = 1 as a word byte swap. The USB model fills the FIFO from DMA for
bulk IN packets and bounds bulk transfers by `TRANS_SIZE`. In QEMU:
- 1 MB reads match;
- reset, replug and stuck-interrupt runs pass;
- the no-DMA fallback passes.

**Hardware report 10 (2026-11-02):** DMA works on the phone: no timeouts,
no resets, and Windows read on normally. But it is slower: about 70 KB/s,
one 16 KiB transfer per ~0.2 s, about 1.3 packets per millisecond. The
FIFO evidently raises its DMA request only after the host has been told
"not ready", so the host retries once per frame. Stock uses exactly this
setup, so its own mass storage was presumably as slow.

The eleventh build keeps PIO with bursts (225 KB/s measured) and compiles
DMA out (`USB_SC6530_DMA`, default 0).

**Why mounting takes about 30 s:** Windows' FAT32 driver reads the whole
FAT on mount to build its free-space map, even with a valid FSInfo (Rockbox
keeps FSInfo current; the card is not dirty). The test card is 16 GB FAT32
with 8 KB clusters: about 1.94 M clusters, a 7.8 MB FAT, so about 35 s at
225 KB/s. The mount time scales with the FAT size:

| Cluster size | FAT size | Mount time at 225 KB/s |
|---|---|---|
| 8 KB | 7.8 MB | about 35 s |
| 32 KB | about 2 MB | about 9 s |
| 64 KB | about 1 MB | about 5 s |

Rockbox handles any FAT32 cluster size.

### The diagnostic log

`USB_SC6530_DIAG` (top of `usb-drv-sc6530c.c`) is off since USB works again
(2026-11-06). When defined, it appends to `/usb-log.txt` on the card:
- every bring-up and exit stage;
- every SETUP the host sends, and each EP0 reply's progress;
- the interrupt counters for 10 s after connecting.

Each line is written and the file closed before the next register access.
After a bus stall, the log ends with the last step that completed.

While the PC has the card mounted as a drive, nothing can be written. The
diagnostic thread acknowledges the hand-over like any other Rockbox thread,
keeps logging to RAM, and writes those lines once Rockbox has the card
back. A
log that ends with `card handed to the host` and no `card back` therefore
means the phone froze while the PC owned the card or during unplug.

| Line | Meaning |
|---|---|
| `stage 1 ldo off` … `stage 4 ahb 0x100` | power and clock steps; stage 4's 300 ms wait follows |
| `stage 5 ctrl` | the first USB register access is next |
| `stage 6 ep0`, `stage 7 irq on` | EP0 setup, unmasking line 25 |
| `stage 8 ldo on`, `stage 9 online` | USB LDO on: the host sees the device |
| `setup llllllll hhhhhhhh` | a SETUP request, bytes 0–3 and 4–7 little-endian (`01000680 00400000` = GET_DESCRIPTOR device, 64 bytes) |
| `irq N idle M sts S setups K in I out O resets R` | every 5 s while connected: interrupt counts, last INT_STS, SETUPs, bulk/EP0 packets, detected bus resets; `POLLED` means the storm guard switched to tick polling |
| `card handed to the host` / `card back from the host` | exclusive mass storage begins / ends |
| `ep0 in start N of wLength W` / `ep0 in done N bytes` | an EP0 reply queued / fully taken by the host |
| `ep0 in abandoned D/N ctrl C sts S` | a new SETUP arrived after D of N bytes; EP0 CTRL and INT_STS at that moment |
| `held setup delivered` / `held setup superseded` | the configuration-implying request after a reset: delivered, or overtaken by the host's next request |
| `bus reset detected` | the 64-byte device descriptor read on a configured device |
| `stall ep N in/out on/off` | a stall set or cleared by the core |
| `ep0 irq sts S ctrl C at D` | an EP0 interrupt during a multi-packet reply: INT_STS, CTRL, bytes sent so far |
| `epN in/out start L` / `epN in/out done L` | a bulk transfer queued / finished (first 400 per connection) |
| `R KB/s in, W KB/s out; sd A% in B% out C% other D%; irq E%, N packets, dma T in U out transfers (H held for the card)` | every 5 s while connected: throughput, and where the time went. `sd` means the card driver was in a transfer, `in` a bulk IN reply was being sent, `out` the device waited for a command or write data, `other` none of these. `irq` is the share of time spent in the USB interrupt; `dma` the bulk transfers run by DMA (both off now), `(DMA off)` after a DMA timeout |
| `dma timeout epN len L ctrl C: PIO from now` | a DMA transfer never finished; it was restarted by PIO |
| `stage 10 exit irq` … `stage 13 exit done` | the unplug path |

In QEMU a full plug, use and unplug cycle logs stages 1–9 (8–9 twice:
Rockbox's `usb_attach()` enables the stack a second time), 9 SETUPs, the
hand-over, stages 10–13 and `card back`.

| Piece | File |
|---|---|
| Controller driver (Rockbox `usb_drv_*`) | `ports/rockbox/firmware/target/arm/sc6530c/usb-drv-sc6530c.c` |
| Target glue (`usb_enable`, `usb_detect`) | `ports/rockbox/firmware/target/arm/sc6530c/usb-sc6530c.c` |
| IRQ 25 dispatch | `ports/rockbox/firmware/target/arm/sc6530c/system-sc6530c.c` |
| Config (`USBOTG_SC6530`, VID:PID, buffers) | `ports/rockbox/firmware/export/config/b310e.h`, `scripts/targets/rockbox.sh` |
| QEMU controller model + host link | `emulator/qemu/machine/hw/misc/sc6530_usb.c` |
| End-to-end test (Python USB host) | `emulator/qemu/scripts/test-usb.py` |

## Board: the USB/UART switch

The service manual (`Troubleshooting.pdf`, block diagram and charger page)
shows the micro-USB D+/D− (IFC200) going through **U201, an FSUSB42UMX**
analog switch, labelled "CHARGER & USB & USB/UART SWITCH". U201 routes the
lines either to the SoC's USB pins (USB_DP/USB_DM) or to UART1 (TD/RD),
for Samsung's factory jig mode (JIG_ON, from the ID pin).

**On the author's phone U201 is not populated, and its footprint has no
pads**, only solder mask: that board revision routes D+/D- without the
switch. The firmware facts below still describe what the stock code does.

The stock firmware's `GPIO_SetUsbSwitch` (0x13e6c) drives the select
through logical GPIO 0x5a. In the stock GPIO table (RAM 0x423c8dc, 16 bytes
per entry, read from a QEMU boot), that is SoC **GPIO 94**, active high:
bank 5 at `0x8a000280`, bit 14. After a normal boot the stock firmware
holds it as an output, **low** (DATA 0, DMSK and DIR bit 14 set). Its
`s_mmi_usb_mode` is 0 at that point. 0x22dc8 reads the ID pin on an ADC
("UsbSwitchVoltage", "JIG_UART_MODE", "standard USB cable mode").

Neither Rockbox nor B310E-OS drives GPIO 94. At reset it is an input, so in
the boot ROM's download mode the switch position comes from the board
alone.

If the switch sits on the UART side, UART1's idle-high transmit line on D+
looks to the host like a full-speed pull-up. Windows then reports "device
descriptor request failed" (Code 43), the device never sees a SETUP, and
Rockbox only charges. On 2026-11-05 the phone showed exactly this with
Rockbox, the stock OS and the boot ROM alike, before and after a port
replacement. On that board U201 is absent, so the cause lies elsewhere on
the D+/D- path. On boards that carry U201, check:
- the level on U201's select pin, which should be low;
- U201's supply and output-enable pins;
- the ID pin (pin 4) for a bridge to GND (pin 5).

## Sources in the stock firmware

The addresses below are in the 8 MiB NOR (`tools/spd_dump/full-backup.bin`).
Queries were made with `tools/ghidra-probe.sh`.

| Code | NOR | What it shows |
|---|---|---|
| `sx_udc.c` | 0x1efa8–0x20b44 | Interrupt-driven controller driver with DMA. ISR at 0x20372 (INTC raw bit 25) |
| `vcom_usb.c` | 0x3862c–0x38f14 | Polled bring-up (0x38b62), endpoint init, EP0 GET_DESCRIPTOR and CDC requests |
| Boot loader download stack | 0x9b3c–0xa2cc | Multi-packet EP0 IN loop (0xa1a4) |
| Clock device "USB" | table 0xca000, ops 0x67c6c/0x67c84/0x67c9c | AHB enable, disable and reset bits |
| Mass storage descriptors | 0xcb2d1 (config), 0xcb308 (device) | `1782:4d01`, bulk EP 0x81 IN and 0x02 OUT, 64-byte packets |

## Bring-up (stock 0x38b62 + clock ops)

1. **USB LDO off.** Through ADI: clear bit 4 of `0x820011a4`, then set bit 4 of `0x82001184`. (Bit 0 of the same pair is the SD card LDO.)
2. **AHB clocks on.** `0x20500060 = 0x100` (clock device "USB") and `0x20500060 = 0x10` (vcom init).
3. **PHY bit.** Clear bit 6 of `0x8b000220`.
4. **Wait.** The stock code waits 300 ms.
5. **Controller.** `CTRL |= 3` (enable + connect).
6. **EP0.** Packet size 8, `INT_CLR = 0x3fff`, `INT_CTRL |= 0x100` (SETUP), buffer ready.
7. **EP1–EP4.** For each: `TRANS_SIZE = 0x40`, packet size 64, clear bits 23–24, enable bit 25, `INT_CLR = 0x3fff`, `INT_CTRL |= 0x200`.
8. **USB LDO on.** Clear bit 4 of `0x82001184`, set bit 4 of `0x820011a4`. The host sees the device from here.

Reset (clock op 0x67c9c): `0x20500020 = 0x20`, a short delay, then `0x20500030 = 0x20`.
The vcom bring-up never calls it, and the driver doesn't either.

## Register map (0x20300000)

| Offset | Register | Meaning |
|---|---|---|
| 0x00 | CTRL | bit 0 enable; bit 1 set by stock 0x20b44 (assumed D+ pull-up / connect) |
| 0x0c | FIFO reset | Written before every FIFO fill. Bit 0 EP0-IN, 1 EP0-OUT, 2 EP1, 3 EP2, 4 EP3, 5 EP4 |
| 0x18 | INT_STS | Bits 8–12: EP0–EP4 pending. Bits 0–6: device events, cleared with `0x1c = 0x7f`; meaning unknown |
| 0x28 | TIMEOUT_LMT | 15 (B310E-OS, fpdoom) |
| 0x40 | EP0 TR_SIZE | Transfer length |
| 0x5c / 0x60 | SETUP | Request bytes 0–3 and 4–7, little-endian |
| 0x64 | EP0 CTRL | See endpoint CTRL bits below |
| 0x68 / 0x6c / 0x70 | EP0 INT_CTRL / INT_STS / INT_CLR | bit 0 packet done, bit 8 SETUP, bit 9 transfer done |
| 0x80 + n·0x40 | EPn CTRL | EP1 0xc0, EP2 0x100, EP3 0x140, EP4 0x180 |
| +0x04 | EPn RX count | Received length (DMA path) |
| +0x08 | EPn TRANS_SIZE | Transfer size |
| +0x0c / +0x10 / +0x14 | EPn INT_CTRL / INT_STS / INT_CLR | bit 0 packet done, bit 9 transfer done, bit 2 error |

EP0's interrupt registers follow its CTRL directly. EP1–EP4 have RX count and
transfer size in between.

Endpoint CTRL bits:

| Bits | Meaning |
|---|---|
| 10:0 | Received byte count (vcom reads it) |
| 22:12 | Packet size |
| 24:23 | Cleared at init |
| 25 | Enable |
| 27 | TX start |
| 28 | RX buffer ready |
| 29 | Stall (EP1; the driver assumes the same bit on all endpoints) |

FIFO window at 0x20380000:

| Offset | FIFO |
|---|---|
| +0x00 | EP0 IN |
| +0x04 | EP1 IN |
| +0x08 | EP3 IN |
| +0x0c | EP0 OUT |
| +0x10 | EP2 OUT |
| +0x14 | EP4 OUT |

FIFO access is 32-bit words. The stock code byte-swaps every word, so the
first wire byte is the most significant byte.

## How the driver works

The driver uses programmed I/O, as vcom and the boot loader do. INTC line 25
calls `USB_UDC()`.

- **Bulk IN packet** (each packet is its own transfer):
  1. Clear status.
  2. Set packet size and transfer size to the packet length.
  3. Reset the FIFO and fill it.
  4. Set TX start.
  5. Wait for transfer done (bit 9).
- **EP0 IN data stage** (boot loader 0xa1a4):
  1. Set `TR_SIZE` (0x40) to the whole stage.
  2. For each packet: set the packet size, reset and fill the FIFO, set TX
     start, wait for transaction end (bit 0).
  3. The controller ends the stage at `TR_SIZE`, short answers included, so
     the driver never sends a ZLP on EP0.
- **OUT packet:**
  1. Set buffer ready.
  2. Wait for transfer done.
  3. Read the length from CTRL[10:0] and drain the FIFO.

### The controller handles addressing, configuration and status stages

On the phone, neither SET_ADDRESS nor SET_CONFIGURATION raises a SETUP, and
no stock driver writes a device address or sends a status ZLP. The driver
tells Rockbox's core what it cannot see:

- **Configuration:** the first request for an interface or endpoint, or the
  first class or vendor request, notifies config 1.
- **Bus reset:** a 64-byte device descriptor read on a configured device,
  which is how hosts open enumeration after a reset, cancels transfers and
  resets the core.
- **Status stages** that the core arms are completed in software:
  - a control-write status immediately;
  - a control-read status after its data stage.
- **SET_ADDRESS**, should one ever arrive as a SETUP, notifies the core and
  is treated as a bus reset.

### Cable detection

VBUS is the charger EIC line, because the micro-USB socket is the only power
input. `USB_DETECT_BY_REQUEST` keeps a wall charger from taking the SD card
away: Rockbox switches to the USB screen only after a host sends a request.

## QEMU model and test

`sc6530_usb` models the registers above, the FIFOs, IRQ line 25 and the
controller-side SET_ADDRESS and status stages. A host connects through a
chardev with a small packet protocol, documented in the model's header
comment. Without `-global sc6530_usb.chardev=…` nothing is ever acknowledged,
as with a cable and no host, so `os.bin`'s bounded USB waits behave as before.

```bash
python emulator/qemu/scripts/test-usb.py --qemu build/qemu/build/qemu-system-arm.exe --rockbox sdcard/progs/rockbox.bin --sdcard card.img --output build/validation/usb
```

The script boots Rockbox with VBUS present, then:
1. Enumerates the device like a PC.
2. Sends GET_MAX_LUN and INQUIRY, waits for TEST UNIT READY and reads the capacity.
3. Compares sector 0 with the card image.
4. Times a 256 KiB READ(10).
5. Writes two sectors with WRITE(10).
6. Unplugs: drops VBUS and waits for Rockbox to disconnect.
7. Re-plugs, enumerates again and reads sector 0.
8. After QEMU exits, checks the written sectors in the image file.

`--irq-stuck` holds line 25 high to exercise the polled fallback.

The 2026-11-02 run gave:

| Item | Result |
|---|---|
| Device | `1782:4d01` at the time (now `1209:0001`), "Rockbox.org" / "Rockbox media player" |
| Interfaces | mass storage on EP1 IN and EP2 OUT; HID on EP3 IN (HID dropped in the seventh build) |
| LUN | "Rockbox Internal Storage", 131072 × 512 |
| Read speed | 264 KiB/s |
| Write | verified in the image |

The read speed is limited by the emulator: one socket round trip per 64-byte
packet. It says nothing about the phone.

The model encodes the stock drivers' behavior, so a pass shows the Rockbox
driver matches that interpretation, not the silicon.

## Open questions for the hardware test

- **Status stages:** confirmed. The controller ACKs them unprompted
  (enumeration works).
- **SET_ADDRESS / SET_CONFIGURATION:** confirmed. Neither raises a SETUP.
- **CTRL bit 1:** is it the pull-up? If enumeration never starts, check this
  and the LDO order.
- **Stalls:** do EP0 and the bulk endpoints honor bit 29? Mass storage stalls
  bulk endpoints on errors.
- **Zero-length IN packets:** can TX start send a packet of size 0? Only needed
  when a control read is shorter than wLength and a multiple of 64 bytes.
- **Bus reset:** which of INT_STS bits 0–6 is bus reset? Until known, the
  driver infers resets from the host's first request. What makes Windows
  reset the device mid-session is still unknown (report 4).
- **SD and USB together:** these have only been exercised in emulation. Known
  landmines: SD DMA must use IRAM, and keypad scanning must pause during SD
  transfers.
- **Power:** a charger now powers the controller and LDO on every plug-in.
  If plugging in a charger hangs the phone, suspect the bring-up writes
  (`0x8b000220` bit 6, the LDO pair).

### Hardware test

1. Run `.\build.ps1 rockbox` and copy `sdcard/progs/rockbox.bin` to the
   card's `progs/`.
2. Boot Rockbox from the card menu.
3. Plug into a PC. Expected: the Rockbox USB screen and a new removable drive
   with the card's contents.
4. Also try a wall charger: Rockbox should keep playing and charge.
5. Report what Windows shows (Device Manager error, "device descriptor
   request failed", nothing at all), and whether a charger alone behaves.
