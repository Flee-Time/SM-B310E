# T2 TOOLING.md — TeakLite-family disassembler: selection + first DSP disassembly attempt

> Plan: `.omo/plans/b310e-audio-eq-tune.md` todo 2 (Part B).
> Date: 2026-08-26. Executor: T2 (Wave 1). Input: `.omo/dumpmine/dsp/dsp_code_full.bin`
> (dump slice `0x570000`–`0x68C000`, 0x11C000 B — T1 byte-verified).
> Evidence: `.omo/evidence/b310e-audio-eq-tune/task-2-b310e-audio-eq-tune.txt`.

---

## 0. TL-I vs TL-II determination (step 0 of the task)

### Determination: **TeakLite-I (Oak-family), with a mandatory correction to the plan's discriminator**

**Correction (evidence-backed, from GBATEK itself).** The plan (and this task's
framing) says "TL-I/Oak is 16-bit, TL-II is 24-bit". That is **factually wrong**.
GBATEK's "DSi TeakLite II Instruction Set Encoding" page
(http://problemkaputt.de/gbatek-dsi-teaklite-ii-instruction-set-encoding.htm) opens with:

> "The opcodes are 16bits wide (some followed by an additional 16bit parameter
> word, namely those with "@16" operands)."

The same page lists **both** families in one table — rows marked `TL` (TeakLite I)
and rows marked `TL2` (TeakLite II) — all 16-bit encodings. **A "16-bit vs 24-bit
density/signature" check therefore CANNOT discriminate TL-I from TL-II.** The
real discriminator is the opcode **family** (TL rows vs TL2-only rows) in actual
instruction streams — implemented in `tools/dsp/tl_family_classify.py` (parses the
GBATEK table, computes `(base,mask)` patterns for all 621 rows, and classifies each
16-bit word of a region as TL/TL2/BOTH/NEITHER).

**Historical evidence → TeakLite-I.**
- The B310E's SC6530C DSP core is the **SC6600M3**:
  `tools/mocor-zw217/BASE/make/layer1_dsp/layer1_dsp.mk:17-25` adds
  `-DSC6600M3 -DCHIP_SC6600L` for `PLATFORM=SC6530` and builds
  `DSP_simple_pdata_sc6530.c` (`:52`).
- The 2003 CEVA press release (cited in `.omo/dumpmine/dsp/librarian-report-audio.md:85`)
  states Spreadtrum's SC6600 is powered by **CEVA-TeakLite** (the first generation).
- MPR (Microprocessor Report, 1999-08-02, cited at `librarian-report-audio.md:84`)
  identifies **TeakLite-I as the Oak-ISA family**.

**From the image itself: no instruction-signature determination is possible.**
The presumed code region `0x570000`–`0x680000` contains **no DSP code** (see §2) —
it is audio resource data. There is no genuine instruction stream to measure a
TL/TL2 signature against. What the image does show is 16-bit-word-oriented content
everywhere (MIDI files, UTF-16 strings, 32-bit pointer tables — all 16-bit-word
structures) and **zero 24-bit encodings** anywhere in the partition. That is
consistent with the Teak family but does not by itself separate TL-I from TL-II.

**Net:** TeakLite-I is the best-supported determination (historical evidence;
no counter-evidence exists in the image because no code exists in the presumed
region). If T3/T6 later locates a genuine DSP instruction stream, the discriminator
to use is `tl_family_classify.py` against the GBATEK table, calibrated against
teakra's own XpertTeak binaries (baseline: genuine XpertTeak decodes ≈ 80-88% TL,
2-7% TL2-only, 10-13% NEITHER).

---

## 1. Per-tool verdicts (tried in the required order)

### 1.1 teakra (wwylele) — **PASS as a tool / FAIL on the B310E input** (the blocker)

- **Repo**: https://github.com/wwylele/teakra — cloned at `build\teakra` (commit `3d697a1`).
  Implements the Teak/XpertTeak-family processor (16-bit, GBATEK-documented; the
  DSi/3DS DSP decoder). "TeakLite-II" per GBATEK naming, but teakra's README argues
  the XpertTeak is the original Teak; for our purposes it is the closest published
  Teak-family disassembler.
- **Build**: expected at `build\teakra\build\teak_disasm.exe`
  (MinGW Makefiles, WinLibs gcc; `teak_disasm` is teakra's own raw-file disassembler,
  `tools/teak_disasm.cpp`). Clone + build if missing:
  `git clone https://github.com/wwylele/teakra.git build\teakra` then cmake-build it.
- **Invocation**:
  ```
  teak_disasm.exe <file> <start_offset_hex> <count> <base_addr_hex>
  teak_disasm.exe dsp_code_full.bin 0 4096 0
  ```
  Reads 16-bit LE words, decodes each (with 16-bit expansion words for `@16`-operand
  opcodes), prints `ADDR  WORD  <mnemonic> <operands>` per line.
- **On the B310E image**: produces output (55-80 mnemonics per block, 0-4% `[ERROR]`),
  but the decode is **invalid** — the presumed code region is NOT code (§2). The
  "good decode stats" are the known false-positive signature of decoding MIDI/Thumb/
  pointer data with a dense 16-bit ISA decoder. Confirmed by cross-checks:
  - The anchor region bytes are Standard MIDI File data (`MThd`/`MTrk`/`FF 2F 00`
    markers, running-status note events) — see §2.
  - Branch-target self-consistency (the acceptance criterion) is not credible:
    the `brr`/`callr` targets are dominated by the repeating 12-byte MIDI
    patterns, not by instruction flow.
  - The TL-family classifier (`tl_family_classify.py`) shows 17-29% NEITHER on the
    region vs 10-13% on genuine XpertTeak, and the top "TL2-only" words are
    pointer-table high words / ASCII (0x0008, 0x0010, 0x0023, 0x0028…), not code.
- **Garbage-file QA** (required by the task):
  - Random 4 KB file → decodes 2048 lines of nonsense (`mov`/`sqra`/`[ERROR]` mix),
    completes with no hang (documented in the evidence file).
  - Tiny 3-byte file → decodes 1 word, clean exit code 0.
- **Verdict**: teakra is a working, validated Teak-family disassembler (PASS as a
  tool; usable the moment a genuine DSP code region is located), but it cannot
  produce a valid disassembly of the presumed region because that region contains no
  DSP code. This is the Part B tooling blocker (see §2).

### 1.2 Ghidra TeakLite-SLEIGH / ghidra-xpertteak-lang — **NOT USED (vetoed with reason)**

- `TeakLite-SLEIGH` (https://github.com/SachinVin/TeakLite-SLEIGH) is a **WIP**
  Ghidra processor for the "DSi/3DS DSP's TeakLite ISA" (its own README) — TL2/DSi
  oriented. `ghidra-xpertteak-lang` (Pokechu22) is the same family.
- **Veto reason**: importing `dsp_code_full.bin` into Ghidra with a TeakLite SLEIGH
  would decode the same non-code region with the same invalid result as teakra
  (§2) — the import would not produce a genuine DSP disassembly. There is no
  genuine code region in the presumed partition to make the import meaningful.
  Recorded as a deliberate non-attempt; the Ghidra environment (12.1.3, project
  `<ghidra-project>\B310E.gpr`) is ready if a real code region
  is found (the `ghidra` MCP verified working this session — `decompile 0x46e4`
  reproduces the PBL fast path).

### 1.3 cevamoo (plutooo) — **NOT USED (vetoed with reason)**

- https://github.com/plutooo/cevamoo — "TeakLite Emulator Prototype" (18-bit PC,
  40-bit accs a0/a1/b0/b1, X/Y/Z mem per the librarian report).
- **Veto reason**: it is an emulator prototype that loads a TeakLite binary in its
  expected layout. No genuine DSP binary was located to feed it (§2), so there is
  nothing to emulate/disassemble. Not attempted.

### 1.4 IDA "Atmel OAK DSP" processor module — **UNAVAILABLE**

- **IDA is not installed** on this machine (checked `C:\Program Files\IDA Pro`,
  `C:\Program Files\IDA`, `C:\Program Files (x86)\IDA`, `C:\IDA`,
  `%LOCALAPPDATA%\Programs\IDA`). The Atmel OAK DSP module would be
  the best-match decoder for a TL-I/Oak core per the librarian report, but it cannot
  be exercised without IDA. Recorded as unavailable.

### 1.5 GBATEK TeakLite-II hand-decode — **NOT APPLICABLE**

- The plan's last-resort trigger is "hand-decode with GBATEK's TeakLite-II encoding
  ONLY if the image proved TL-II". Two reasons it does not apply:
  1. GBATEK's TL-II encoding is **16-bit**, not 24-bit (§0) — the "24-bit TL-II"
     premise that gates this step is void.
  2. There is no code in the presumed region to hand-decode (§2).
- (A best-effort TL-I/TL2 decoder was nevertheless built from the GBATEK table —
  `tl_family_classify.py` — and used as the family discriminator. It is the 
  hand-decode tooling, usable when a real region exists.)

---

## 2. The blocker — the "DSP code region" is NOT code (T1 correction)

**The plan's T1 section-map claim that `0x570000`–`0x680000` is "TeakLite executable
code, dense 16-bit LE halfwords" is FALSIFIED by direct byte analysis.** The DSP_CODE
partition is a **mixed audio-resource area**:

| File off | Partition addr | Size | Content (verified) |
|---|---|---|---|
| `0x000000` | `0x570000` | 0x2E000 | **Standard MIDI Files** (ringtones): 5× `MThd`, 65× `MTrk`, 66× `FF 2F 00` end-of-track markers; `MThd@0x430` fmt=1/16 tracks/120ppq; `MThd@0x1D88` fmt=0/6 tracks; running-status note-event streams |
| `0x009000`–`0x02E000` | `0x579000` | — | 32-bit LE pointer tables (values `0x0008A618`…, `0x000F3890`…, `0x00102CD8`… — page-tagged DSP-address-like values; **not** instructions) |
| `0x02E000` | `0x59E000` | 0xB3000 | **UTF-16 Indic-script language strings** (275,426 words in U+0900–U+0DFF Devanagari/Gujarati/Tamil/Telugu/Malayalam; also "SIM ", "…" ASCII UTF-16) |
| `0x0E1000` | `0x651000` | 0x4000 | **XML theme data** (`<Color red="255" …/>`, `<Brightness value="1"/>`, `<Opacity`) |
| `0x0E5000` | `0x655000` | 0xA000 | **high-entropy data** (≈7.9-8.0 bit/byte; no bzip2/LZIP/standard-LZMA magic) — plausible **compressed/encrypted DSP program** |
| `0x0EE000` | `0x65E000` | 0x12000 | high-entropy (same class) |
| `0x0F8000` | `0x668000` | 0x12000 | high-entropy (same class) |
| `0x10A000` | `0x67A000` | 0x2000 | table (incl. a `BZh` false positive at `0x103002` — not bzip2, next byte 0xF1 ≠ level digit) |
| `0x10C000` | `0x67C000` | 0x4000 | high-entropy (same class) |
| `0x110000` | `0x680000` | 0xA000 | **NV parameter island** (dsp_codec datasets + EQ_* tables — confirmed DATA per T1/`docs/audio-dsp-protocol.md`) |
| `0x11A000` | `0x68A000` | 0x2000 | 0xFF fill |

The `0x68784E`–`0x68A000` "code tail" is also data: the mode-name table tail, a
`0x000C`-repeated table, and the `V1.0.1-B7` / `Spreadtrum Communication CO.`
version strings — no instruction stream.

**The same false-positive was excluded at every other candidate location:**
- `DRPS`/`CAPN` container `0x12E524` (decompressed via
  `fphelper drps_dec 0x12e524 0` → `drps_12E524_0.bin`, 3.6 MB): contains the
  **ARM "user" firmware layer** (81,225 Thumb-2 `bl` pairs, JPEG-decoder strings
  "jpeg decoder error", pointer tables) — NOT DSP code. Blob 1 = "sres0001"
  resource index. The 0x089000–0x0A6000 "mpy-heavy" blocks are ARM data tables.
- `fphelper scan` boot-init table (0x10410): the boot decompresses/copies only ARM
  segments; entry 5 copies the 0x1090-byte DRPS header to IRAM `0x400003a0` (the
  container stays XIP, decompressed at runtime).
- Region-wide branch-consistency scans (`tools/dsp/branch_consistency.py`) over
  `0x0`–`0x800000` and the DRPS blob: every "consistent" block is either ARM code,
  zero-fill (`nop`-dominated), or MIDI (whose running-status structure creates
  pseudo-consistent branch targets). No block passes all of {low error, high
  mnemonic diversity, high branch consistency, low ASCII}.

**Exact blocker (as required by the task):** the presumed DSP code input
(`dsp_code_full.bin`, partition `0x570000`–`0x68C000`) contains **no genuine
TeakLite instruction stream** in any locatable region — it is audio resources
(MIDI ringtones, language strings, XML themes, compressed blobs, NV parameter data,
fill). The tool (teakra) works and is validated, but there is no code for it to
disassemble. The DSP program most plausibly lives **compressed/encrypted** in the
high-entropy span `0x655000`–`0x680000` (~172 KB, ≈ the right size for a modem/audio
DSP binary) — decompressing it needs a lead from the ARM download path (T3/T6).

**T3/T6 lead found:** the ARM-side audio_DSP command table at dump `0x733F0`–`0x73430`
pairs `audio_dsp_info` tokens (`0x02300066/67/68/69…`) with DSP program addresses
(`0x0D4B`, `0x0D59`, `0x0D68`…) — the DSP-side consumers of the NV blocks will
reference these addresses once a code region is recovered.

---

## 3. Analysis tooling written this session (all in `tools/dsp/`)

| File | Purpose |
|---|---|
| `tl_family_classify.py` | GBATEK TL-vs-TL2 opcode-family classifier (parses the 621-row table; per-word TL/TL2/BOTH/NEITHER) |
| `region_classify.py` | 4 KB-block content classifier (CODE16/TABLE/ASCII16/UTF16_INDIC/UNIFORM/FILL) |
| `window_highbytes.py` / `analyze_width.py` | 16-vs-24-bit word-density analysis (pre-existing from the planning session; reused) |
| `decode_scan.py` | per-block teak_disasm decode-quality scan (mnemonics/errors/branch targets) |
| `find_dsp_code.py` | full-dump scan for TL-family-match density |
| `find_dsp_markers.py` | DSP-marker-mnemonic density scan |
| `branch_consistency.py` | branch-target self-consistency scan (the acceptance criterion) |
| `verify_extract.py` | T1's extraction verifier (pre-existing; untouched) |

---

## 4. Bottom line

- **Tooling**: teakra = working Teak-family disassembler (**PASS**, ready for use).
  SLEIGH/cevamoo = vetoed (no code to decode). IDA = unavailable. GBATEK hand-decode
  = not applicable (TL-II is 16-bit, and no code exists).
- **Part B first disassembly**: **documented tooling failure** — the presumed DSP
  code region is audio-resource data, not code. No disassembly was produced or
  fabricated; the failure is fully evidenced above.
- **Determination**: **TeakLite-I** (Oak-family) per the historical record
  (SC6600M3 = CEVA-TeakLite; TeakLite-I = Oak ISA), with the plan's "TL-II is 24-bit"
  discriminator corrected (both families are 16-bit per GBATEK).
