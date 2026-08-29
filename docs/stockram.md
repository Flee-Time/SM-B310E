# Stock firmware RAM-boot (stock-ram.bin) — B310E-OS

This packages the **original Samsung firmware** as a `.bin` that spd_dump can
send over USB — the phone then boots the **stock main OS from PSRAM** instead
of NOR. No flash is written; a reboot returns the phone to normal.

```
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram.bin ram
```

## Status (2026-08-23, hardware-verified)

- **The stock main OS BOOTS from PSRAM.** The diag markers showed it reaches
  its own keylight/LED management and takes over the keylight before stopping.
  The core goal — load the original firmware via spd_dump and run it from RAM
  (zero NOR writes, zero brick risk) — is achieved.
- **The LCD stays blank** (a display-path issue, not a boot issue): the stock
  OS's framebuffer lives at its PSRAM window 0x04000000, but we boot with
  MEM_REMAP=1 (PSRAM physically at 0x34000000), so physical 0x04000000 is
  unmapped. The CPU-side MMU alias works, but the LCDC **DMA bypasses the
  MMU** and reads the framebuffer at the physical 0x04000000-based address →
  nothing reaches the panel. The framebuffer base is runtime-computed (not a
  patchable constant).
- Fix options for a future iteration (see learnings 2026-08-23 #3): clear
  MEM_REMAP from IRAM for the stock layout, or locate + patch the OS's
  runtime framebuffer address to 0x34000000-based. Both are hardware tests.

## What was discovered in the stock bootloader (PBL)

Disassembling `dump_firmware.bin[0..0x10000]` (the PBL) revealed the boot
architecture:

- **Boot vector @ flash 0x0**: 8× `ldr pc, [pc, #24]`, entry pointer @ 0x20
  = `0x46e4`.
- **PBL entry @ 0x46e4** checks a magic at PSRAM `0x04000000`:
  ```
  46e8: mov r0, #0x04000000      ; read *(uint32_t*)0x04000000
  46fc: adds r1, r0, #0x1AE63FC  ; == 0xFE519C04 -> Z set
  4700: bne 0x4718               ; no magic -> full init path (0x7a80)
  4704: bl 0x2e98                ; magic set -> chip-id check
  4710: bxeq r0 = 0x10000        ; if ok -> jump straight to main OS vector
  ```
  The magic at `0x04000000` is a **warm-boot fast path**: skip all PBL init
  and jump directly to the main OS at flash 0x10000. (0x2e98 returns 1 only
  if `chip_id & 31 == 23`; SC6530C gives 0 → the fast path is taken.)
- **MEM_REMAP**: `0x205000e0` bit 0 switches the PSRAM window base between
  `0x04000000` (stock default, 3MB per the firmware's memory_map) and
  `0x34000000` (the window spd_dump's FDL loads into, ~4MB). Same physical
  RAM, two windows — this is why our os.bin runs at 0x34000000 while the
  stock OS uses 0x04000000.

## Why the shim (and why it aliases exactly these windows)

The stock main OS is **XIP-linked at flash VA 0x0** (boot vector @ 0x10000,
entry 0xbf150) and additionally uses PSRAM `0x04000000` (its init_table
high-water is `0x0423bba8 + 0xa50b8` ≈ 2.9MB). To run it from RAM we must
provide BOTH windows from our 4MB PSRAM:

| Window | Size | Mapped to | Contents |
|---|---|---|---|
| VA `0x00000000`-`0x00100000` | 1MB | PA `0x34000800` | XIP stub + entry + init code (flash 0x0-0x100000 copy) |
| VA `0x04000000`-`0x04300000` | 3MB | PA `0x35000800` | the stock OS's own PSRAM runtime |
| everything else | — | identity | real NOR (resource reads at 0x12e524+ are identical bytes), IRAM, peripherals |

4MB budget: 1MB image + 3MB OS window = exactly the 4MB PSRAM. Reads past
the 1MB alias hit **real NOR** — the init payloads (`src 0x1095ec+`) and the
CAPN/DRPS resources are identical bytes whether read from RAM or NOR, so the
OS boots normally with its code staged in RAM.

## The shim (`tools/stockram/shim.s`)

Loaded at 0x34000000 (the FDL `ram` target), runs with MMU off:

1. SVC mode, IRAM stack, invalidate I/D-cache + TLB.
2. Build a full 4GB TTB at IRAM `0x40000000` (16KB — below the FDL1 at
   `0x40004000`), identity-mapped sections.
3. Override: VA `0x0`-`0x100000` → `0x34000800`; VA `0x04000000`-`0x04300000`
   → `0x35000800`.
4. Enable the MMU, `bx` to VA `0x10000` (main OS boot vector → RAM copy).

The image follows the shim at offset 0x800. All knobs (image offset, alias
size, OS PSRAM window) are constants in the shim header.

### DEBUG marker

After enabling the MMU the shim lights the **keypad light** (`0x82001224 =
0xe0`) before jumping to the OS. This bisects failures on the phone:

| Keypad light | LCD | Meaning |
|---|---|---|
| off | blank | Shim crashed before MMU-on (TTB/stack/descriptor issue) |
| on | blank | Shim fine; the STOCK OS crashed after entry (deeper issue) |
| on | Samsung boot | OS took over (and likely disabled the MMU → real-NOR boot) |

### Diagnostic build (`make stockram-diag` → `stock-ram-diag.bin`)

Observed state (2026-08-23): shim OK (keylight on), but the boot vector
never executed — even with the vibrator/level markers removed, the keylight
stayed ON. So the diagnostic now answers the deeper question first: **does
the aliased section 0 execute at all?** The diag image splices three pieces:

- `vectors.s` @ image 0x0 — exception table (any fault → keylight OFF + hang)
- `aliastest.s` @ image 0x100 — entered by the shim's `bx 0x100`; holds the
  keylight OFF ~0.8 s then ON, then jumps to the real boot vector
- `diag-stub.s` @ image 0xE000 — instrumented stock entry (boot vector
  patched 0xbf150 → 0xE000): ADI mailbox read + compare, keylight signals

All keylight writes use the **proven stock RMW pattern** (led_keylight_set,
HW-verified) — the earlier direct writes may not take effect.

| Keylight sequence | Meaning |
|---|---|
| ON (~0.3 s) then **stays ON** | Aliased section 0 does NOT execute — MMU mapping problem in the shim |
| ON (~0.3 s) → OFF (~0.6 s) → **ON** (final) | Aliased execution WORKS; stub compare MATCH → boot-mode/assert path (0x105bc) |
| ON (~0.3 s) → OFF (~0.6 s) → **OFF** (final) | Aliased execution WORKS; stub compare MISMATCH → full boot path (0xbf1b0); ADI ok; hang deeper |
| ON (~0.3 s) → OFF immediately (no ~0.6 s) | An exception fired (vector table caught it) |

Watch ~2 seconds. The ~0.3 s ON and ~0.6 s OFF are the timing anchors — the
final steady state after that is the answer. (First version had a ~10 s
delay from a miscounted loop — the shim ON was invisible; the delays above
are corrected.)

Build + test:
```powershell
make stockram-diag
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram-diag.bin ram
```
The stub hands off to the real entry (0xbf150) so the natural dispatch still
runs. All splice locations (0x0, 0x100, 0xE000) are in the dead PBL region —
never executed by the RAM-boot path, not read by the main OS.

## Build + test

```powershell
make stockram          # needs dump_firmware.bin in the repo root
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\stock-ram.bin ram
```

Expected if it works: the phone runs the stock Samsung OS but the LCD shows
the normal boot (no Samsung logo animation is guaranteed — the resources are
read from NOR, so behavior should match a normal boot; the difference is
invisible from outside, which is the point of the sandbox: RAM-running the
stock OS lets us probe its live memory, DSP commands, etc. via later tooling).

## Known unknowns — hardware iteration needed

- **Does the post-init OS run entirely from PSRAM/IRAM?** The init_table
  copies ~1.3MB of code to `0x04000010`, strongly implying yes, but if some
  post-init code path stays XIP beyond the 1MB alias, it reads real NOR
  (benign — identical bytes).
- **PSRAM high-water**: if the OS touches `0x04300000+` (beyond the 3MB
  window we map), that VA faults. The memory_map says 3MB, so it should not.
- **MMU interaction**: if the stock OS disables the MMU mid-boot, we fall
  back to physical addressing — PSRAM is at `0x34000000` (MEM_REMAP stayed
  1), so the OS would read real NOR and effectively boot stock from NOR
  (benign). Keep MEM_REMAP=1 (the FDL's default) — do NOT clear it in the
  shim, or the executing shim would lose its own memory.
- **Flash writes**: the OS's NV writes go through the SFC to **real NOR**,
  identical to a normal boot (the OS writes the same NV it always writes).
  Low risk, but watch for unexpected writes.
- First boot should be observed with `libc_server` + a `read_mem` dump of
  `0x34000000`/`0x04000000` to confirm which path was taken.

## Safety

Nothing writes NOR beyond what the stock OS does in a normal boot. Reboot
(battery out/in) restores the phone completely. `stock-ram.bin` contains
the first 1MB of the copyrighted stock firmware — **gitignored, never
commit it**.
