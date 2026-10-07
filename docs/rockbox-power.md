# Rockbox idle power on the SM-B310E

## Implemented

The SC6530C target now selects Rockbox's generic ARM wait-for-interrupt
`core_sleep()` instead of its empty fallback. The scheduler masks IRQ
before checking runnable threads, waits with CP15 `c7,c0,4`, then enables
IRQ to service the wake source. This preserves the existing scheduler's
lost-wakeup protection. Sleep happens whenever no thread has work, during
playback as well as while paused.

Matrix keypad IRQ8 is enabled without replacing other INTC enables. Its
handler captures press/release edges into the held-key mask and acknowledges
all keypad flags, including long-press flags. Rockbox's normal button tick
continues to provide debounce, repeat and the analog EIC END-key poll.
The SD block-transfer resume path captures pending matrix edges before
acknowledging them, so a press/release arriving during a seek refill is
not discarded by its former unconditional keypad ACK. Matrix scanning
still pauses around SD DMA, as required by the existing hardware workaround.
The stock NOR keypad ISR at `0x351de` reads `0x87000008` and acknowledges
`0x87000010`; the emulator's stock interrupt mapping places it on IRQ8.

After the configured backlight timeout, both lights turn off and the panel
enters sleep immediately (`HAVE_LCD_SLEEP`, `LCD_SLEEP_TIMEOUT=0`). The
driver serializes framebuffer transfers and panel state changes, lets the
last transfer settle, disables the LCDC engine, switches DBI to command
mode and sends `0x28` (display off), then `0x10` (sleep in). Framebuffer
updates stop while the display is inactive. After panel settling it clears
only the LCDC AHB gate (bit12 at `0x20500070`), matching the stock sequence
at `0x27d54..60`; wake restores that bit through `0x20500060`, as at
stock `0x27cd4`. LCM and shared clocks stay available for wake.

On wake it sends `0x11`, waits at least 120 ms, sends `0x29`, restores
MADCTL/window/RAMWR and DMA mode, then transfers the latest Rockbox
framebuffer and signals LCD activation before relighting the panel. The
framebuffer remains in RAM. It does not reset the panel or erase settings.
Delays yield to the scheduler, allowing audio and timer interrupts to run.

The command pair was observed in unmodified stock firmware traces at the
LCM write caller `0x68c5e`. The manufacturer's controller documentation
describes the retained-memory sleep state and its 120 ms transition delay:
[ST7735 controller datasheet, sections 9.13 and 10.1.10–11](https://www.displayfuture.com/Display/datasheet/controller/ST7735S.pdf).
The commands are shared by the port's three supported ST7735 panel IDs.

This provides interrupt-driven CPU idle with a sleeping display, analogous
to suspend-to-idle in scope. It is **not SC6530 deep sleep**: the 100 Hz
kernel tick continues to wake the CPU, PLL and RAM supplies stay enabled,
and audio DMA must continue during playback. A matrix key interrupts idle
directly; END and headset status are still polled by the normal tick.

RTC reads no longer poll raw-status bit0 at `0x82001634`: that is a latched
second interrupt, while time-update acknowledgements occupy bits8–11.
The old read path could burn its million-read budget on each clock/status
refresh. Counter reads also protect the shared ADI mailbox against the
button interrupt. The older calendar conversion and write protocol still
need reconciliation with the stock day counter; this change does not
claim working calendar setting or retention after battery removal.

## Shutdown and USB charging

The old `power_off()` armed a watchdog reboot and then spun forever. Its
watchdog clocks were not enabled, so the reset could fail and leave the
"Shutting down" screen visible. It also left interrupt-driven ADI access
running during its register sequence.

The replacement uses the stock e52q7a shutdown function at NOR `0x1a42e`:
disable IRQ/FIQ, then write `0x1f` to LDO power-down SET1 (`0x82001184`),
followed by `0x3fff` to SET0 (`0x82001180`), as at `0x1a448..52`.
The stock function's callers include `0x16be8`, `0x18184` and `0x2da46`.
The final write removes the CPU/RAM supplies; there is no ADI read/poll
after it. Normal Rockbox shutdown stops audio, closes the codec and
finishes storage work before invoking this target function. The separate
reboot routine now enables SC6530 watchdog clocks through `0x820010e0=4`
and `0x820010e4=2`, with IRQ/FIQ masked.
`HAVE_SW_POWEROFF` is now enabled, making the existing held-END definition
request this normal shutdown through Rockbox's button framework.

The paused idle-timeout test also reproduced a filesystem panic before
`power_off()`: `Updating size on empty dir entry 55`. FAT passes interior
directory-entry pointers to `dc_dirty_buf()`. Its old strided-array pointer
subtraction allowed GCC to assume sector alignment and turn the bounds
check into a byte-offset check ending at the **start** of the final buffer
(`0x3e00` for 32 × 512 bytes). Interior entries in that buffer were never
marked dirty. A lost long-name entry at a cluster boundary caused a later
resume-settings save to clear a cluster holding the open playlist file.
The integration now subtracts integer byte addresses before dividing by
the sector size, preserving the whole final buffer (`0x4000` bytes).
SD requests also serialize the shared controller and IRAM bounce buffer,
and pin initialization preserves the caller's interrupt mask.

QEMU recognizes the complete two-register LDO power-off request as guest
shutdown. This checks the real player reaches the request; it does not
model individual supply voltages, watchdog reset timing or automatic
restart with USB power present. Hardware shutdown remains to be checked
both with USB disconnected and connected.

The reported freeze occurred while paused with USB connected. The former
charger stub let Rockbox's paused idle timeout run while externally powered.
Charger monitoring now enables the normal plug indicator, charge animation
and inhibition of idle shutdown. Manual END shutdown remains available.

The mapping is independently reconstructed from the unmodified e52q7a dump:

| Signal | Stock evidence | Rockbox behavior |
|---|---|---|
| External power | `CHG_PHY_IsChargerPresent` at NOR `0x663d4` reads logical EIC18; the `0xca92c` table maps it to analog `0x82001900` bit2, active high | Enable analog EIC clocks and unmask bit2, preserving END bit3; poll for power presence |
| Charger enable | `0x416fc` returns raw GPIO6; `0x6626c` drives high to disable, `0x66332` low to enable | Preserve its mask/direction/value; observe an explicitly configured high disable |
| Power good / charge complete | `0x41704` returns raw GPIO8 (PGB), `0x41700` GPIO9 (CHGSB); `0x66352` and its `GetCHGDoneStatus` string return complete for GPIO8 low, GPIO9 high | Configure only status pins8/9 as inputs; animate while both are low and power is present; stop for completion, bad power or explicit disable |

This is `CHARGING_MONITOR`: it does not set charger current, voltage or
drive GPIO6. Regulation remains with the charger IC and inherited firmware
setup. On 2026-10-07 the user confirmed that charge detection works on the
phone. Charge-complete indication and the GPIO8/9 electrical levels still
need hardware confirmation; QEMU validates the driver and Rockbox policy
against external inputs.

With USB/DC connected, the phone's channel5 ADC measures the powered rail.
The target now retains its last valid unplugged battery sample while powered.
A powered boot reports unknown battery percentage until USB is removed,
rather than treating that rail as full charge. B310E-specific filter guards
preserve unknown readings and initialize the filter on the first real sample;
externally powered writes remain allowed. Percentage cannot track charge
progress while plugged in with this ADC path. ADI battery reads/writes now
serialize mailbox access against the button/timer interrupt.

`ports/rockbox/tests/test-charger.py` boots the full player and checks cable
insertion/removal, charging/completion/fault/disable states, cached voltage,
powered-boot unknown voltage, first unplugged conversion and inhibition of
a one-minute idle timeout. QEMU provides `sc6530_adi.charger-present` and
the named `charger-input` pin0 (active high). Analog EIC bit2 obeys DMSK;
the input persists over reset and cannot be forced by guest writes. EIC
debounce/edge IRQs, analog clock gating and electrical charging are not modeled.

## Validation

`ports/rockbox/tests/test-audio.py` runs the real ARM startup, interrupt
dispatcher, PCM core, codec, keypad driver and DMA sink. Its waits now use
the same masked-IRQ `core_sleep()` sequence as `core_idle()`, rather than
a separate assembly sleep instruction. The 39 playback/rate/gain/hotplug
cases pass with timer and DMA wakeups.

`ports/rockbox/tests/test-idle-power.py` boots the complete player with
a private FAT32 image, a two-second backlight timeout and the stereo test
WAV. It checks the linked idle instruction, advancing kernel ticks, IRQ8
key delivery without stray IRQs, LCDC disable, stopped display transfers,
panel blanking, keypad wake, held-key repeat, playback with the screen
asleep and paused sleep/resume. It observes registers and uses normal keypad input; it does
not replace guest code or write guest RAM. Example:

```powershell
python ports/rockbox/tests/test-idle-power.py `
  --qemu /path/to/qemu-system-arm.exe `
  --rockbox sdcard/progs/rockbox.bin `
  --elf build/rockbox/build-b310e/rockbox.elf `
  --toolchain /path/to/arm-toolchain/bin `
  --runtime sdcard/.rockbox `
  --output emulator/qemu/logs/idle-power
```

Repeat with `--headset` to cover the other output. QEMU also retains panel
GRAM while blanking a sleeping/off display; `test-display.py` verifies the
state combinations and reset. The stock ringtone capture still produces
sustained, unclipped sound after this model change.

`--controls` additionally tests the [primary control layout](rockbox-controls.md).
`--shutdown manual` holds END after pausing; `--shutdown idle` uses a
one-minute idle timeout on a private image. Both must exit QEMU cleanly
after the ordered stock LDO writes, without programming a watchdog reboot.
After shutdown the test reads the FAT32 image independently: both FAT
copies must agree, playlist/resume settings must remain readable, and the
test WAV must be unchanged. Manual shutdown includes a paused delay so
the periodic resume-settings save runs before END is held.

QEMU does not measure battery current or validate electrical sleep/wake
timing. On the phone, check multiple screen-off/wake cycles with both
speaker and headphones, pause for several minutes, resume, hold keys and
open a different track after waking. Compare the same track, brightness,
volume, output and starting charge against the previous build. A current
measurement is more useful than the port's voltage-based percentage; do
not infer a runtime improvement from emulator host CPU usage.

## Remaining work, in priority order

1. **Power down idle audio.** Pause stops DMA/VBC and mutes, but leaves the
   codec bias, DAC and output power on. Add a delayed, serialized shutdown
   using the existing close/init path, restoring sample rate, route and
   gain before resume. Test pop suppression and repeated resume on hardware.
2. **Trace SC6530 deep sleep and RAM retention.** Establish the actual
   stock PMU/PLL/PSRAM sequence and wake-source masks. A tickless interval
   must account for elapsed time and all Rockbox deadlines; simply stopping
   the tick would lose button polling, timeouts and scheduling. Confirm
   keypad/END/headset/charger wake independently on hardware before gating
   shared clocks or supplies. The emulator's clock register echoes cannot
   establish that RAM or a real wake circuit remains powered.
3. **Reduce active CPU frequency.** The port is fixed at 208 MHz and has
   no adjustable-frequency implementation. Verify divider transitions,
   memory timing and codec performance before enabling scaling.
4. **Verify shutdown and charger status on hardware.**
   `power_off()` follows the stock LDO shutdown above; charger reporting
   now uses the stock EIC/GPIO inputs. Check charge completion, USB removal
   and manual shutdown while externally powered.
   A reliable software firmware/loader handoff could also avoid battery
   removal when switching firmware.
5. **Finish storage power and hotplug.** The existing SDIO command path
   already disables the *card clock* after every command, so another card
   clock-off operation is not a new saving. SD controller/internal clocks,
   storage sleep hooks and safe card replacement remain incomplete. Keep
   card power and filesystem state intact until a safe transition is proven.
6. **Verify RTC calendar handling.** Reconcile the older Rockbox date
   assumptions with the stock day-counter/update-ack register contract.

## RTC backup supply

The available Samsung service-manual copy has troubleshooting circuit
fragments rather than a complete SC6530/SC6500 power pinout. Its PMU figures
show main `VBAT`/`VBATBUCK` inputs; no separate RTC backup connection is
identified in those fragments. That is insufficient to conclude that the
chip has no backup pin. `RTC6211S` in its block diagram is the FM receiver,
not evidence of a separate timekeeping RTC.

A supercap design needs an identified, isolated RTC supply with known
voltage limits and current, charge-current limiting, and isolation from
the main battery rail. A capacitor across the main supply would feed the
phone rather than just the RTC. Retention also depends on capacitor
leakage, not just `C × voltage drop / RTC current`. No board attachment
point or capacitor value is verified here. See the general backup-supply
principles in [Analog Devices' RTC design note](https://www.analog.com/en/resources/design-notes/design-considerations-for-analog-devices-realtime-clocks.html);
its device pinouts are not applicable to this phone. The reference documents remain
outside Git; only independently written findings are included.
