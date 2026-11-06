# SIM card reader

Rockbox can power either SIM slot and read the card directly, without the
modem or DSP:
- the ATR;
- the ICCID (card number), which is always readable;
- the IMSI, or "PIN required" if the card's PIN is enabled.

It is on the **View I/O ports** test screen (System → Debug): press **#**.
The result is shown on screen and appended to `.rockbox/torch-b310e.txt`.

**Status: verified on the phone (2026-11-06).** Slot 0 read a real card
at 1.8 V: a valid ATR, ICCID and IMSI, status 9000. Slot 1 (empty)
reports no card.

## Hardware (stock `sim_drv.c`, NOR 0x36400–0x37400)

Tags: **[C]** read in stock code or data, **[L]** likely.

**Controller.** One controller at **0x85000000** serves both slots [C].
The slot is selected in APB `0x8b0001e0` bits 3:2 [C]; bit 1 of the same
register is the FM I2C pad select, so the driver preserves the other bits.
Each slot's clock-stop level is `0x8b000220` bit 1/2/3 [C]. Interrupt line
10 (`TB_SIM0_INT`) [C]; the reader polls.

**Clocks** (stock clock devices "SIM0"/"SIM1", ops `0x673c0`):
- APB enable `0x8b0000a0` bit 25 (SIM0) and bit 2 (SIM1) [C]. Which one
  feeds the controller is not known, so the driver sets both.
- Reset pulse `0x8b000060`/`0x64` bit 20 [C].

**Supply** (`g_ldo_ctl_tab`, LDO ids 10/11 for slots 0/1) [C]:
- **VSIM0:** on = ANA `0x82001184 &= ~2`, then `0x820011a4 |= 2`; off
  reverses it.
- **VSIM1:** the same with bit `0x4`.
- **Voltage:** `0x820011c8`, VSIM0 bits 7:4, VSIM1 bits 11:8. `0xA` gives
  1.8 V and `0x6` gives 3 V (stock maps 1.8 V to level 0 and 3 V to
  level 2) [C].

**Registers (offsets from 0x85000000)** [C offsets; names from the SDK]:

| Off | Name | Use |
|---|---|---|
| 00 | TX | write a byte |
| 04 | RX | read a byte |
| 08 / 0c | STS0 / STS1 | raw status: b0 RX at mark, b1 TX empty, b2/b3 parity, b5/b6 card in/out, b9 RX timeout |
| 10 / 14 / 30 | IE / ICLR / IMSK | |
| 18 | CTL0 | b0–1 convention, b2 parity, b4/b5 RX/TX FIFO reset, **b6 RST**, b7 power, b9 RX en, b10 TX en, b13, b15 I/O low |
| 1c | CTL1 | [10:8] clock divide (÷2^(n+1)), **b11 clock enable**, b12 polarity |
| 20 / 34 | RX / TX clock divider | 0xBA0 = 8 × 372 (one ETU in 26 MHz ticks) |
| 24 | SHE | retransmit limits (RX [3:0], TX [7:4]): 0x0A for ATR, 0x33 for commands |
| 28 / 2c / 38 | TGC / WDT / WDT1 | guard time 0x505; watchdog 0x802 (0x803 armed), 0x100 |
| 3c | CTL2 | FIFO marks: RX [5:0], TX [13:8] |
| 40 | STS2 | FIFO counts: RX [6:0], TX [14:8] |

**Pins.** The SIM pads are set by the boot pinmux table (SIMCLK `0x26c+`,
SIMDA `0x278+` with pull-up, SIMRST `0x284+`), and the boot menu applies
it. The driver writes no pinmux.

## Sequence

**Activation** (stock `0x36ea0`, `0x36d90`, `0x36dd0`, plus ISO 7816-3):
1. Clocks and reset; select the slot.
2. VSIM on at 1.8 V; wait 20 ms.
3. Controller setup:
   - CTL1 clock divide 2 (26 MHz / 8 = **3.25 MHz**); both dividers 0xBA0;
   - CTL0 = 0x2003, TGC 0x505, WDT 0x802, WDT1 0x100;
   - drain RX; SHE 0x0A.
4. Clock on, RST low, RX/TX enable and power, I/O released.
5. RST high after 20 ms.
6. **ATR:** the first byte within 300 ms, the rest until 50 ms of silence.
   TS `0x3B` is direct convention; `0x03` means inverse (toggle CTL0 bits
   0–1).
7. **No ATR:** deactivate and retry at 3 V.

**Commands (T=0, GSM class A0).** Per command:
1. Reset the FIFOs; set the RX mark to 1; enable RX and TX; SHE = 0x33;
   arm the watchdog (WDT = 0x803).
2. Send the 5-byte header, wait for the TX FIFO to drain, then clear TX
   enable to turn the line around.
3. Read procedure bytes:
   - `0x60` (NULL): keep waiting.
   - **INS:** send or receive the rest.
   - **~INS:** one byte at a time.
   - `6X`/`9X`: SW1, followed by SW2.

The reader then runs:
1. SELECT MF (`3F00`), then EF_ICCID (`2FE2`), then READ BINARY 10 bytes.
2. SELECT DF_GSM (`7F20`), then EF_IMSI (`6F07`), then READ BINARY 9 bytes.
   `98 04` means CHV1 (the PIN) is enabled.

Both use BCD with swapped nibbles; the first IMSI nibble is the parity
digit.

**Deactivation** (stock `0x36faa`): IE off, RST low, clock off, I/O low,
VSIM off, voltage back to level 0, APB clocks off.

## QEMU model and test

`emulator/qemu/machine/hw/misc/sc6530_sim.c` models the controller and a
GSM test card:
- an ATR on cold reset;
- T=0 with procedure bytes;
- SELECT, READ BINARY and GET RESPONSE over MF, DF_GSM, EF_ICCID and
  EF_IMSI.

Properties:
- `sim-slots`: a bitmask of populated slots, checked against the live
  slot-select register. The default is 0 (no card), because stock's own
  SIM driver needs interrupt line 10, which the model does not raise.
- `sim-pin`: EF_IMSI answers `98 04`.
- `sim-iccid`, `sim-imsi`.

`ports/rockbox/tests/test-sim.py` opens the I/O screen, presses #, and
checks the dump. Slot 0 must show the ATR, ICCID and IMSI; slot 1 must
show "no card"; the PIN-locked card must report `pin=1`.

## Unconfirmed

- Which APB gate (bit 25 or bit 2) clocks the controller, and which
  connector (SIM200 or SIM201) is slot 0.
- The units of the WDT/WDT1 timeout registers.
- The stock protocol layer (ATR parsing, PPS, T=0 loop) is in the
  compressed image, so the T=0 handling here follows ISO 7816-3. There is
  no PPS: the card stays at F=372 / D=1.
- Whether the controller echoes transmitted bytes into RX. The driver
  assumes it does not; if it does, the first "procedure byte" would be
  the echoed header.
