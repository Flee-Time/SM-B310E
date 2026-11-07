# SC6530C analog EIC interrupt (END key, headset button), from the stock firmware

Tags: **[C]** read from the stock NOR (`SM-B310E_Backup/dump_firmware.bin`, NOR address given),
**[L]** likely (Linux `drivers/gpio/gpio-eic-sprd.c`, or a strong pattern), **[G]** guess.
The analysis used `tools/ghidra-probe.sh` (read-only) plus `arm-none-eabi-objdump -M force-thumb` on the dump.
Ghidra stops disassembling after every `blx 0xc02dc` (a trace call it treats as non-returning),
so most of the EIC phy code had to be read with objdump.

Source files named in stock asserts: `eic_hal.c`, `eic_phy_v5.c` (NOR 0x39074-0x39dd0),
`int_phy_v5.c` / int HAL (0x10730-0x10960, 0x1b5d8-0x1ba9c), `gpio_prod.c` (0x13108-0x13e60),
`adi_phy_v5.c` (0x302a4-0x303cc).

## 1. Analog debounce EIC registers (base 0x82001900, ADI; 16-bit registers)

The layout matches Linux `SPRD_EIC_DBNC_*` exactly. The stock code confirms each offset.

| Addr | Name | Use in stock | Tag |
|---|---|---|---|
| 0x82001900 | DATA | input level, bit n = channel n. Read by `39382` (GetData) | [C] 0x39390 |
| 0x82001904 | DMSK | data mask; set bit n before reading DATA (`393ca`, does `|= 1<<n`) | [C] 0x393d6 |
| 0x82001914 | IEV | trigger level: 1 = high, 0 = low (`3991a`, group type 0) | [C] 0x39944 |
| 0x82001918 | IE | interrupt enable (`392c4`: on = `|=`, off = `&= ~`) | [C] 0x392e6 |
| 0x8200191c | RIS | raw status. The stock code never touches it | [L] |
| 0x82001920 | MIS | masked status, read by the ISR (`39406`, type 0 = base+0x20) | [C] 0x39414 |
| 0x82001924 | IC | interrupt clear. Stock ORs in 1<<n with a read-modify-write (`3932c`); Linux writes 1<<n | [C] 0x3934a |
| 0x82001928 | TRIG | write 1<<n to (re)start that channel's debounce (`395c4`) | [C] 0x395d0 |
| 0x82001940 + 4n | CTRL0(n) | bits[11:0] = debounce time (`39a0a`: `&= 0xfffff000; |= t`); bit 14 = set if debounce is enabled (`398ca`: `|= 0x4000`, else `&= 0xffffbfff`) | [C] 0x39a2c / 0x398f0. Bit 14's exact meaning [G]. Linux defines only bits[11:0] |

There are 16 channels (`chan_id < EICA_DBNC_SIZE`, 0x10).
The ADI write path masks the value to 16 bits (`39c2a` → `0x39da2`) [C].

EIC group table `eic_group_cfg` at NOR **0xca92c** (returned by `0x6a140`: 6 entries of {base, grptype, nchan, is_analog}) [C]:

| # | base | type | n | analog | logical EIC IDs |
|---|---|---|---|---|---|
| 0 | 0x8a001000 | 0 DBNC | 16 | 0 | 0-15 (digital debounce; EIC0 = headset jack) |
| 1 | 0x82001900 | 0 DBNC | 16 | 1 | 16-31 → **EIC18 = ch2 charger, EIC19 = ch3 END, EIC20 = ch4 headset button** |
| 2 | 0x8a001080 | 1 LATCH | 8 | 0 | |
| 3 | 0x8a0010a0 | 2 ASYNC | 8 | 0 | |
| 4 | 0x8a0010c0 | 3 SYNC | 8 | 0 | |
| 5 | 0x82001900 | 1 | 8 | 1 | (second analog entry. Purpose unknown) |

For types 1-3, stock uses IE +0, MIS +8, IC +0xc, polarity/mode +0x10/+0x14/+0x18 [C], close to Linux latch/async.

## 2. How the analog die interrupts the ARM

**ANA_INT block at 0x82001580 (ADI), not 0x82001380:**

| Addr | Name | Tag |
|---|---|---|
| 0x82001580 | ANA_INT masked status. Read by the IRQ entry `1b874` when INTC bit 24 is set (pointer kept in RAM ctx 0x0422ca50, initial value at NOR 0x11f02c) | [C] 0x1b886 |
| 0x82001584 | raw status | [G] (not referenced) |
| 0x82001588 | ANA_INT enable, read-modify-write: `|= mask` (`1b8d0`), `&= ~mask` (`1b95e`), `= 0` at init (`1b8c4`) | [C] 0x1b92c, 0x1b8c0 |

**INTC line 24** is the analog die's summary line [C, 0x1b882: `lsls r0,#7; bpl` tests status bit 24 before reading ANA status].
At boot `1b8b6` writes INTC_DISABLE (0x8000000c) = 0xffffffff and ANA_INT_EN = 0, then enables logic 24 [C].

Interrupt "logic number" table (RW-init image NOR **0x11f048**, loaded to RAM 0x0422ca6c; 41 entries × 0x18 = {logic, src(0 = INTC, 1 = ANA), mask, isr, hisr, default}) [C].
Its ANA sources (src = 1) are:

| ANA bit | logic | identity |
|---|---|---|
| 0 | 0x20 | ? (Linux sc8810: ADC [L]) |
| 1 | 0x21 | turned on together with logic 6 (digital GPIO, INTC 6) → analog GPIO [L] |
| 2 | 0x22 | RTC (emulator's finding; handler 0x35eec region) [C/L] |
| 3 | 0x23 | ? (WDG [L]) |
| 4 | 0x24 | ? |
| **5** | **0x25 (37)** | **analog debounce EIC: ISR = `EICA_DbncIsr` 0x39602 (pointer 0x39603 at NOR 0x39c54)** [C] |
| 6-8 | 0x26-0x28 | ? |

The INTC sources have logic = line number and mask = 1<<line (for example logic 0x18 → 0x01000000) [C].

**INTC registers as stock uses them** [C, 0x1b904-0x1b926 and 0x1b85a]:
- 0x80000000: IRQ masked status.
- 0x80000008: IRQ enable (read-modify-write `|=`).
- 0x8000000c: IRQ disable (write ones).
- 0x80000020: FIQ status. The FIQ dispatcher `10884` reads it via ctx 0x0422ca5c.
- 0x8000002c: FIQ disable. Stock writes the mask here before setting the IRQ enable, so the line is not routed as an FIQ.
- 0x80000028 is probably FIQ enable [G].

No ack at the INTC or ANA_INT level was found. The source's own clear (EIC IC) drops ANA status bit 5, which drops INTC bit 24 [C: neither the dispatcher (0x1073c-0x107a4) nor `1b874` writes any status register].

Dispatcher `0x1073c` [C]: `1b874` latches INTC status to 0x0422ca54 and ANA status to 0x0422ca58. It walks the 41-entry table and calls the **first** entry whose `status[src] & mask` is set. A return of 0x5ae005a5 activates the HISR.
Since all ANA entries (src = 1) come before logic 24 in the table, ANA bit 5 dispatches straight to `EICA_DbncIsr`. Logic 24 itself has only the default handler (`0x1b81d`, which disables the logic).

## 3. Stock enable sequence

`EIC_HAL` register call `0x3918a(eic_id, trig, dbnc_en, dbnc_time, callback)` [C].
`gpio_prod.c` calls it from `0x138ce`, invoked by GPIO init at **NOR 0x24db8-0x24e44**:

| prod ID | EIC | level (active) | dbnc_en | time | callback |
|---|---|---|---|---|---|
| 0x0d | **19 = analog ch3, END** | 1 (high) | 1 | **50** | 0x13806 |
| 0x13 | **20 = analog ch4, headset button** | 1 (high) | 1 | **50** | 0x13710 `GPIO_HeadsetButtonIntHandler` |
| 0x08 | 18 = analog ch2, charger | 1 | 1 | 200 | 0x132ce |
| 0x11 | 0 = digital, headset jack | 0 (low) | 1 | 140 | 0x1344a |

The product list comes from NOR 0xc4dc4 ({id, type(2 = EIC), level, num, cb} × 18) [C].

For type EIC, `138ce` first reads the current level (`39104`: DMSK |= bit, read DATA). It then arms the **opposite level**: `trig = level ? 0 : 1` [C 0x138fc-0x139a8].

Trigger enum, decoded from the switch at `3912a` (0x39140; table bytes 06 0b 04 0b 06 08 0a 0b) [C]:
- 0 → IEV = 0 (low)
- 1 → IEV = 1 (high)
- 2 → IEV = 0 (low)
- 3, 4, 5 → edge modes (async groups only)

This matches MOCOR `GPIO_INT_DISABLE / LEVEL_HIGH / LEVEL_LOW / EDGES_BOTH / RISING / FALLING` [L].

`0x3918a` does the following, in order [C]:
1. First call only (`0x0422e558 == 0`): `0x39860`. Using the `39bcc` plain-store helper (an ADI write for 0x8200xxxx):
   - write 0x8b0000a0 = 1<<25, then 0x8b0000a0 = 1<<26 (APB EIC clocks)
   - ADI write **0x820010e0 = 0x80**, **0x820010e4 = 0x20**. These are the same writes B310E-OS and Rockbox already make. Whether these are write-1-set registers is unconfirmed.
   - enable logic 6 (INTC 6 + ANA bit 1, see `1b940`)
   - register `EICA_DbncIsr` on logic 37 and enable it → **ANA_INT_EN 0x82001588 |= 0x20**
   - register and enable logic 0 (latch ISR 0x3954a) and logic 7 (async/sync ISR 0x3944c)
   - `0x39288`: 0x8b000060 = 1<<20; spin 100; 0x8b000064 = 1<<20 (APB EIC reset pulse); ADI 0x820010a0 = 0x20; spin 100; ADI 0x820010b0 = 0x20 (analog EIC soft-reset pulse, SET/CLR pair [L])
2. `39162`: IEV bit n set or cleared per trig, then CTRL0(n) = (CTRL0 & 0xfffff000) | dbnc_time.
3. `398ca`: CTRL0(n) |= 1<<14 (dbnc_en = 1).
4. Store the callback in the analog callback table, RAM 0x042773c4[n] (`39b66`; only if the slot is empty).
5. IE |= 1<<n (`392c4`).
6. TRIG |= 1<<n (`395c4`).

Note: stock's enable does NOT write IC before IE (Linux writes IC after changing IEV).

## 4. Stock handler and ack (`EICA_DbncIsr`, NOR 0x39602-0x3985e) [C]

```
for n in 0..15:
  if MIS(0x82001920) & (1<<n):
    IE   &= ~(1<<n)                 # 0x39632
    DMSK |=  (1<<n)                 # 0x39642
    lvl  = (DATA >> n) & 1          # 0x3964a
    callback[n](n, lvl)             # 0x39654  (re-programs IEV, see below)
    IC   |=  (1<<n)                 # 0x39672  (RMW-OR through ADI)
    IE   |=  (1<<n)                 # 0x39848
    TRIG |=  (1<<n)                 # 0x39850  (restart debounce)
```

The END callback `0x13806(ch, lvl)` [C]:
- builds the message {0x81 = press if lvl == active level 1, else 0x80 = release; 2; 0}
- re-arms with `131e8(0xd, lvl ? 2 : 1)`, which ends in `3912a` → IEV = opposite level
- posts the message to the GPIO HISR through `1085c(6, msg)`

The headset-button callback `0x13710` does the same re-arm for prod 0x13. It then debounces against the mic ADC (`136d8`) and uses 350-tick timers.

So stock uses **level-triggered debounce EIC with the polarity flipped on every interrupt** (the same idea as Linux `sprd_eic_toggle_trigger`).

## 5. Implementation recipe for B310E-OS (derived from the stock code; not run on the phone)

Init (after the current keypad EIC clock and DMSK code):
```
for ch in (3, 4):
  c = adi_read(0x82001940 + 4*ch);  adi_write(.., (c & 0xf000) | 50 | 1<<14)
  lvl = adi_read(0x82001900) >> ch & 1
  IEV(0x82001914): bit ch = !lvl
  IC(0x82001924)  = 1<<ch            # harmless pre-clear (Linux does it)
  IE(0x82001918) |= 1<<ch
  TRIG(0x82001928)|= 1<<ch
adi_write(0x82001588, adi_read(0x82001588) | 1<<5)       # ANA_INT EIC
REG32(0x8000002c) = 1<<24;  irq_register(24, eic_isr); irq_enable_line(24)
```

ISR on line 24: if `adi_read(0x82001580) & 0x20`, do the §4 loop for bits 3 and 4. Flip IEV to `!lvl` in place of the callback, and hand `lvl` to the key queue.

Cautions:
- `adi_read`/`adi_write` must then be IRQ-safe. Stock's ADI read runs with IRQ and FIQ disabled (`0x302b6` calls 0x040066b2/0x04006734 ... 0x04006766/0x040066f6).
- If a thread does an ADI access with IRQs on, an ISR can preempt it between the address write and the data read. That breaks the index echo (the stock assert at 0x302b6 checks for this).
- Leave other ANA_INT bits off. Stock clears 0x82001588 to 0 at boot. Unknown bits on line 24 with no handler will fire.
- Charger ch2 could use the same path (stock: 200 ms debounce).

## 6. Wake from WFI

- No stock code found that ties the analog EIC to sleep. `_TrgSleepEn` (+0x14) exists only for LATCH groups ("EIC_DBNC not support seting _TrgSleepEn", 0x39ce0) [C].
- ARM926EJ-S `mcr p15,0,rX,c7,c0,4` (used in `os/arch/sc6530c/cpu.S:282`) wakes when nIRQ is asserted, even with CPSR.I set. So an enabled INTC line 24 should wake WFI [L: ARM926 TRM behaviour; not tested here].
- The debounce needs the EIC clocks (APB 0x8b0000a0 bits 25/26, ANA 0x820010e0/e4) to keep running. Light WFI keeps them. Any deeper clock gating is untested [G].

## 7. Still unknown

- ANA_INT raw-status offset (+0x584 is a guess). There is no ANA-level clear in the stock path.
- CTRL0 bit 14 semantics. The debounce time unit: stock passes 50 / 140 / 200, which look like ms but this is not verified.
- Whether 0x820010e0/e4 and 0x820010a0/b0 are SET/CLR registers.
- Group entry 5 (analog, type 1, 8 channels at the same base): purpose unknown.
- The emulator (`sc6530_adi.c`) models ANA status only for RTC bit 2 (+0x634/+0x630). It has no EIC IE/IEV/MIS/IC/TRIG and no ANA bit 5, so the IRQ path can only be tested on the phone, or after extending the model.
- The prior "m4-ghidra" notes folder was not found anywhere under `C:\Users\koray` (searched to depth 6).
