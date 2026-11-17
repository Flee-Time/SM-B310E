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

## CORRECTED: A5/3 IS present — via a separate command path, not the FI engine (2026-11-12)
**The earlier "no A5/3, A5/1-only" conclusion below was WRONG** — refuted by a v6
both-directions on-phone capture (peer session, commit `77337d0`): stock, commanded
A5/3 by TT (CIPHER MODE setting 0x05), **deciphers the downlink** (a valid plaintext
MM LU ACCEPT comes back). So the handset does A5/3. My error was extrapolating from
"the FI word-5 cipher engine is A5/1-only" to "the DSP has no A5/3 at all," without
tracing the **separate cipher command path** that actually carries A5/3:

- **cmd 5** `{5, enable, alg}` — enable ciphering + select algorithm (`alg` 0=A5/1,
  1=A5/2, 2=A5/3). Builder `FUN_0004b4b4`.
- **cmd 0x1e** `{0x1e, Kc0:Kc1, Kc2:Kc3, Kc4:Kc5, Kc6:Kc7, 0}` — loads the raw 64-bit
  Kc, two bytes per word high-byte-first (**no `Kc‖Kc` expansion, no COUNT in the
  command**). Builder `FUN_0004b828` (verified 2026-11-12).

Stock drives ciphering through **these commands**, NOT the FI word-5 bits. The FI
word-5 engine (0x8640, `alg==1`→A5/1 only) is real and A5/1-only, but it is **not the
only cipher path** — that was the whole mistake. The DSP keeps the A5 COUNT itself in
working regs ~`0xec4c..0xec50` (multi-word COUNT arithmetic at dis `0x15A2..0x16BC`),
seeded from the share COUNT window (ARM `+0x3f0`).

The old "no KASUMI S-boxes / no key-schedule constants in 60 images" search results
still stand as *searches* — their correct reading is that the KASUMI core is most
likely a **hardware crypto block** the DSP drives (or computed without lookup tables),
**not** that A5/3 is absent. Mechanism (HW accel vs DSP code) is unresolved and does
not matter to the ARM interface: cmd 5 + cmd 0x1e is the path.

**DSP-side structure (RE 2026-11-12, two clean facts):**
1. The algorithm field `0xfbf2` that the A5/1 engine `0x8640` gates on (`==1`) is
   written at **exactly one site, `0x8457`, inside the FI word-5 decoder** — never
   from a command. So cmd 5's `alg` does not reach the `0x8640` engine; the **A5/3
   unit is separate** and the `0x8640` engine is bypassed for A5/3 (matches stock
   keeping FI w5 = 0).
2. The A5/1 COUNT reg `0xec4c` is referenced at **only 3 sites (`0x15A2`/`0x1645`/
   `0x16A0`), all in the FI-w5/A5/1 setup**, and nowhere in the `0xA000–0xA400` cipher
   region. So `0xec4c` (fed by share `+0x3f0`) is **bound to the A5/1 engine**.

Consequence: a per-frame `+0x3f0` COUNT reseed is a **no-op for the A5/3 cmd path** (it
feeds the A5/1 COUNT the A5/3 unit doesn't read) — consistent with the COUNT-seed
experiments not helping.

**Stock A5/3 transaction, from the on-phone v6 log (ground truth, 2026-11-12)** —
`build/stocklog/capture/v6-boot.dsp.log`, the stock DSP-call capture:
- RX **CIPHER MODE COMMAND** (`~R`, RR 0x35) at frame `0x2f408` → **same frame**
  `0x2f409`: `~C 0005 0001 0002` (**cmd 5 {enable=1, alg=2=A5/3}**) then
  `~C 001e <Kc>` (**cmd 0x1e**, Kc 2 bytes/word high-byte-first) — enable FIRST, key
  second.
- ~1.7 s ciphered window: normal FIs (types 0xb/5/3/2), **FI word5 = 0 the whole time**
  (the only non-zero w5 is `0x0010` on type-2 RX bursts — a non-cipher bit, not bits
  14:13), PA keys (`~C 0004`) per uplink burst, and the plaintext uplink block handed to
  the DSP in `~T` (the DSP enciphers it). **No COUNT command and no per-frame cipher FI
  bit anywhere.**
- cipher off: `~C 0005 0000` (**cmd 5 {enable=0}**) at frame `0x2fa79`.

Full cipher-window command diff (2026-11-12): the ONLY `~C` commands stock posts in the
window are cmd 5 + cmd 0x1e (setup), cmd 4 (PA key, per uplink burst), and the teardown
(cmd 0x21 {0xf} / cmd 0x0d {0} / cmd 5 {0}) — no cmd 0 parameter record, no other cipher
command. One FI divergence worth noting: stock sets **FI word5 = `0x0010` on every
type-2 (RX) burst in the dedicated phase** (67/67, both pre-cipher and during cipher;
idle/uplink FIs stay 0). That is w5 bit4 → DSP reg `0xeca1`, read at `0xC804` in a
dedicated-channel reset/AFC-reinit routine (it also clears the AFC accumulator
`0xecb9`) — a dedicated-RX flag, **not** the cipher enable (it is on pre-cipher too),
but a stock behaviour to match on the dedicated RX path. The cmd-5/0x1e DSP handler
bodies are unreachable from our artifacts: the dispatch table is in DSP **data** space
(`[0x10fd+id]`), absent from the extracted code-only images.

So the ARM→DSP A5/3 setup is exactly **cmd 5 {1,2} + cmd 0x1e {Kc}, once, right after
the CIPHER MODE COMMAND, w5=0, no COUNT** — which the peer's path already matches. A
tested word5-bits14:13=3 hypothesis is **refuted by this log** (w5 is 0 throughout).
The DSP sources the A5/3 COUNT internally (its own cell-synced TDMA FN). Since the
whole ARM boundary now matches stock, a ciphered exchange that still fails points to
**decode reliability** (the ciphered LU ACCEPT lost to the dedicated-RX AFC ceiling) or
a **frame-count/sync offset**, not the cipher commands. The cmd-5/0x1e DSP handlers sit
behind a data table `[0x10fd+id]` (cmd5=`[0x1102]`, cmd0x1e=`[0x111b]`; dispatcher
`0x9F64`) not in the program `.dis`.

**Implication (corrected):** do NOT force A5/1 via the classmark as "the fix" — the
phone genuinely supports A5/3 and TT can legitimately command it; matching stock means
*implementing* A5/3 via cmd 5 + cmd 0x1e, which the peer does and the v6 log confirms
is the exact stock sequence. Open item (peer): with the ARM boundary matching stock,
the ciphered exchange still not completing is most likely **decode reliability** (the
ciphered LU ACCEPT lost to the dedicated-RX AFC ceiling) or a **frame-count/sync
offset** — not the cmd 5/0x1e sequence, and not the Kc (packing confirmed correct).
"TT requires A5/3" is a real path to complete, not a hard wall and not merely a
classmark bug.

**Keystream verdict — HW, good-signal run (2026-11-13):** logged RX headers across the
dedicated phase. Pre-cipher (plaintext) blocks `bfi=0` at `fn%51=27`; post-cipher
(ciphered) blocks `bfi=1` at the **same** `fn%51=27`, same signal, identical other
header fields. Since decode succeeds on the plaintext blocks at the same position, the
failure is **not** the RF/decode ceiling — it is the **A5/3 keystream** (our decipher
produces garbage → CRC fail → silent). (Note: on our DSP `hdr[4..8]` are constant/zero,
so `hdr[7]` is not a usable SNR field — the same-position pre/post-BFI comparison is the
discriminator instead.)

So the blocker is now narrowed to the DSP-side A5/3 keystream: **COUNT, direction, or
whether cmd 5 arms the unit** — Kc is confirmed correct and the ARM sequence matches
stock. Caution: "silent after COMPLETE" does **not** on its own separate a *symmetric*
fault (wrong COUNT, or unit not armed → TT can't read our COMPLETE → no ACCEPT) from a
*direction* fault (uplink enciphered right → TT sends the ACCEPT → we fail to decipher
the downlink). **Discriminator:** gr-gsm our own downlink and watch our SDCCH subchannel
after CIPHER COMPLETE — downlink turning ciphered/undecodable = TT accepted our COMPLETE
= uplink keystream right = *direction* (downlink-only); downlink staying plaintext
(re-CIPHER MODE COMMAND / RELEASE) = TT rejected it = *symmetric* = COUNT/arming. If
symmetric/COUNT, the fix is likely **not a missing cipher command** (stock sends none —
its DSP already holds the cell's absolute FN from continuous sync); it means getting our
DSP to hold the true absolute FN (T1/T2/T3), the same reason A5/1 needed the `+0x3f0`
absolute-FN injection.

<details><summary>Original (wrong) 2026-11-11 claim, kept for the record</summary>

> Searched for a KASUMI/A5/3 engine… **A5/3 is genuinely absent** — engine 0x8640 has
> no `==2`/`==3` branch; no S7/S9 S-boxes in 60 images; no `C1..C8` key-schedule
> constants. Implication: stock can only do A5/1, force A5/1 via the classmark. —
> *Refuted: the FI engine being A5/1-only does not mean the DSP has no A5/3; the
> cmd-5/0x1e path does A5/3, HW-proven.*
</details>
