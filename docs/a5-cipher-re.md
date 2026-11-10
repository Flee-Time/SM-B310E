# A5 ciphering RE — working notes

Goal: implement `l1_cipher_set(kc[8], alg)` in `os/drivers/dsp.c` (currently a stub)
so registration can answer CIPHERING MODE COMMAND and complete the LOCATION
UPDATING ACCEPT. This is the one remaining gate: the uplink now reliably reaches
AUTHENTICATION + SIM SRES, and after our auth response TT sends CIPHERING MODE
COMMAND (RR 0x35), which we cannot yet honour.

## Assets
- Stock DSP code, disassembled: `SM-B310E_Backup/.omo/dumpmine/stock-fw/dsp-blob-CC874.dis`
  (TeakLite-I; 65768 lines; `PROGADDR  HEXWORD  mnemonic  operands`; `[0xNNNN]` = DSP DATA addr).
- Stock ARM "user" firmware: `SM-B310E_Backup/.omo/dumpmine/dsp/drps_12E524_0.bin` (3.6 MB Thumb-2, no symbols).
- SDK (headers only, no L1 .c): `SM-B310E_Backup/tools/mocor-zw217/`.
- DSP command/FI decode map (authoritative): `docs/gsm-plan.md` ~line 1014-1070.

## What is known
- **FI word 5 bits 14:13 → DSP data `0xec83`** = cipher mode (gsm-plan.md:1028, marked
  `[L]` = low confidence, so re-verify). The FI decoder writes it at dis 0x8450.
- **Cipher frame counters T1/T2/T3 at share `+0x3f0..+0x3f6`**, copied into the cipher
  unit when FI w1 bit 8 = 0 (gsm-plan.md:1024). These are the A5 COUNT input.
- A w5 flag "pulls in" share `+0x3e4/+0x3e8/+0x3ec/+0x3f8` for dedicated mode
  (gsm-plan.md:1028). `+0x3e4` is the TA (we already write it), `+0x3ec` the PCL.
  **HYPOTHESIS (unconfirmed): Kc[8] = 4 words at share `+0x3f8..+0x3fe`**, right after
  the counters, pulled in by a w5 flag. Needs confirmation from the share-copy code.
- DSP command dispatcher at dis 0x9F64: `r2 = cmd_id; r2 += 0x10fd; r2 = [r2]; jump`.
  Valid ids up to at least 0x40 (special-cased at 0x9F5C). A cipher/key command, if one
  exists, is an entry in the 0x10fd table (ids 1/2/3 = 0x9a20/0x9a52/0x9ab4; we use 2/3/4).
- **ARM handler = `MphHandle`** (the L1 MPH signal handler). Its cipher case is at SDK
  source lines ~3910-4275 (from the trace strings in
  `tools/mocor-zw217/common/export/inc/trace_id/base_trc.h`):
  - 3820 `"L1:chan_mode %d, is_change %d"` (id 2309)
  - 3910 `"L1:Cipher mode %d,Cipher_algarithm %d"` (id 2310)
  - 3915 `"L1:Cipher Key %d,%d,%d,%d,%d,%d,%d,%d"` (id 2311)  ← the 8 Kc bytes
  - 4275 `"L1:cipher_mode %d,cipher_algorithm %d"` (id 2318)
  So MphHandle logs mode/algorithm/Kc, then programs the DSP (share write and/or command).
- SDK types (summary): `CIPHER_ALGORITHM_E` A5_1=0..A5_7; `MPH_CIPHER_MODE_REQ { CIPHER_PARAM_T
  cipher_param; KC_T kc; }`, `KC_T = uint8[8]` (ps_layer1_signals_def.h).

## SOLVED (2026-11-10, from dsp-blob-CC874.dis)
Share base on the DSP is word 0xa800 (share BYTE offset N → DSP word 0xa800 + N/2).
Confirmed by the w1-bit copy at dis 0x8379-0x8389: it copies 4 words
DSP 0xa9f8..0xa9fb → 0xec4c..0xec4f = share +0x3f0 (the cipher COUNT working regs).

- **Kc[8] = share `+0x3f8`** (DSP 0xa9fc), **8 words, one Kc byte per word** (low byte).
  The copy is at dis 0x85A5-0x85AD: `if ([0xec70] != 0) { r1=0xec71; r2=0xa9fc;
  bkrep 8 { [r1++] = [r2++] } }` — 8 words from share +0x3f8 into the cipher KEY
  register 0xec71..0xec78. The A5 engine indexes the key via 0xec71 (dis 0x745A/
  0x7588/0x75B3) and 0xec78.
- **Enable / Kc-load trigger = FI word 5 bit 15** → `0xec70` (decoder dis 0x8497-0x849A:
  `0xec70 = w5 >> 15`). When set, the DSP (re)loads Kc from share +0x3f8. This is the
  same bit gsm-plan.md:1028 noted as "pulls in share +0x3f8" — it is the Kc load.
- **Algorithm select = FI word 5 bits 14:13** → `0xfbf2`; the engine (dis 0x8640-0x8649)
  does `0xfbf4 = ([0xfbf2] == 1) ? 1 : 0`, i.e. **value 1 = A5/1** (the active cipher),
  anything else = off. So A5/1 = bits 14:13 = 0b01.
- **COUNT** = share +0x3f0 (4 words) → 0xec4c, DSP-maintained per frame (the w1-bit copy).
  Working assumption: the DSP tracks the GSM frame number itself, so the ARM need NOT
  write +0x3f0. **This is the main on-air risk to verify** — a wrong COUNT = wrong
  keystream = all ciphered bursts garbage.

### Resulting FI word-5 cipher bits (both RX and TX, once ciphering is on)
`w5 |= (1u << 15) | (1u << 13)` = **0xA000** → bit15 = enable/Kc-load, bits14:13 = 1 = A5/1.
(General: `(1<<15) | ((alg+1) << 13)`, alg 0 = A5/1.)

### THE COUNT is the likely remaining bug (found 2026-11-10, after cipher was reached on-air)
The A5 keystream COUNT (the 22-bit GSM frame number) is assembled by the engine at
dis 0x1645 from DSP 0xec4c/0xec4d/0xec4e: `[r3] = [0xec4c]; [r5] = ([0xec4e]<<8) |
[0xec4d]`. Those are copied from share +0x3f0/+0x3f2/+0x3f4 whenever FI word-1 bit 8
is 0 (gsm-plan.md:1024). PROBLEM: our fi_init sets fi[1] = ((frame_number>>16)&0x7f)
<<9, i.e. **bit 8 = 0 on every FI**, so the DSP reloads the cipher COUNT from share
+0x3f0 EVERY frame - and we never write +0x3f0, and the DSP's own frame counter is
our LOCAL frame_number (0 at l1_start), not the cell's absolute FN. So the COUNT the
MS ciphers with does not match the network's, the keystream is wrong, and the ACCEPT
never comes even when A5/1 is correctly reached+enabled (confirmed on-air: cipher
enables, result stays 6).
FIX SHAPE (to implement + verify): write the cell's current absolute FN into share
+0x3f0 in the 0xec4c/4d/4e packing (determine T1/T2/T3 vs raw split from 0x1645/the
copy code), aligned to the frame the DSP will cipher; and decide the w1-bit-8
load-once-then-advance vs reload-each-frame semantics (does the DSP advance 0xec4c
per frame when bit 8 = 1? - trace the +0x3f0 copy and any per-frame increment of
0xec4c). cell FN = (frame_number + c->fn_offset) mod (26*51*2048). Until this is
right, A5 cannot complete. Also: TT sometimes requests A5/3 (seen: w5 e000); this DSP
path only activates A5/1 (mode==1 at 0x8640), so A5/3 attempts cannot complete - rely
on the A5/1 ones.

### Implementation (done 2026-11-10)
`l1_cipher_set(kc, alg)` in os/drivers/dsp.c: writes kc[0..7] to share +0x3f8..+0x406
(8 words, one byte each) and sets a `cipher_w5` global = (1<<15)|((alg+1)<<13); the FI
builders (l1_burst_post RX, l1_uplink_post TX) OR cipher_w5 into fi[5]. Ciphering turns
on when gsm_lu calls l1_cipher_set on CIPHERING MODE COMMAND; both directions cipher
from then. UNVERIFIED on-air (needs a good-signal registration that reaches the cipher
step). If it fails, suspect (a) the COUNT source, (b) enable-every-frame vs once, (c)
byte order / packing of Kc in share.

## Candidate implementation shape (once the above are pinned)
- `l1_cipher_set(kc, alg)`: `sm_w16` Kc into its 4 share words; set a cipher-mode global
  that `txpwr_w5()`-style logic ORs into FI word 5 bits 14:13 on subsequent RX/TX frames
  (both directions must cipher); ensure the COUNT path is fed. Then CIPHERING MODE COMPLETE
  (already built, `reg_cipher_complete`) must go out **enciphered**, i.e. the first uplink
  block after enabling must already carry the cipher mode bits.
- Risk: a wrong key/mode/count corrupts every ciphered burst — verify on-air at the
  cleared TT registration, watching for the ACCEPT after CIPHER COMPLETE.
