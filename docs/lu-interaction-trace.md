# Location Update — the whole interaction, RX↔TX correlated

The complete power-on → LOCATION UPDATING ACCEPT exchange for the stock SC6530C,
correlating the **receive** path (what the ARM does with each decoded block) with
the **transmit** path (what it posts back), and naming the code that runs at each
step. It stitches together the RE in [dsp-arm-interface.md](dsp-arm-interface.md)
(RX §5.6, TX §6, states, cipher §7) and [stock-first-association.md](stock-first-association.md).

Confidence: **[C]** = code-traced in this image (resident kernel); **[L3]** = the
decision is in the demand-paged protocol stack (`user_raw.bin`/`drps`), so the
*trigger/response* is known but the dispatch code is not in this image; **[air]** =
the network's reply, guessed from the GSM procedure. The DSP never parses L3 — every
downlink block of a given channel takes the same tag handler (§5.6); only the TX
*response* differs, chosen by L3.

## The loop
Registration is one request/response loop: **receive a block → L1 forwards it by tag
→ L3 decides → L1 transmits the response → receive the next block.** The RX side is
uniform (tag dispatch); the intelligence is which TX primitive L3 picks next.

## The sequence

| # | Dir | Channel (tag) | Message | DSP cmd / FI | Code that runs |
|---|---|---|---|---|---|
| 0 | — | — | bring-up | cmd 0 ×records/NV, cmd 2/0x1b/0x2f, **cmd 1** | `dsp_boot`→records; `FUN_0002f11a` (wake); `FUN_0005a874`→`SetState(1)` (frame clock) **[C]** |
| 1 | RX | — | **find the cell** | FI **11** (power) | `FUN_0004f646` post → POWERMONQ → `rxlevmon`; "is the cell there?" = rxlev per ARFCN **[C]** |
| 2 | RX | SCH (1) | sync | FI **9/10** | `FUN_0005096e` post → `rx_read` → tag 1 → **`FUN_0004ce38`** (BSIC+FN, cell locked) **[C]** |
| 3 | RX | BCCH (7) | System Information | FI **2** tag 7 | tag 7 → **`FUN_0004d91a`** → SI parse → PLMN/LAI, C1 suitability **[C]** |
| 4 | — | — | **decide to register** | — | MM sees SIM + suitable cell → LOCATION UPDATING; asks RR for a dedicated channel **[L3]** |
| 5 | TX | — | enter dedicated | **cmd 3** + RF writes | `FUN_003b13c6`→**`FUN_000589d8`**: cmd 3 (serving cell, mode) + `FUN_00062dc2/62d78/62dce` + **`SetState(4)` SDCCH** **[C]** |
| 6 | TX | RACH (6) | CHANNEL REQUEST | FI **6** + TXBUFQ + **cmd 4** | `FUN_0004fe0a` (type-6 access burst `{0,bsic,0,ra}`) + cmd 4 (PA key); retransmit per RACH ctrl **[C]** |
| 7 | RX | AGCH (3/7/8) | **IMMEDIATE ASSIGNMENT** `[air]` | FI 2 tag 3/7 | tag 3 → **`FUN_0004c788`** (grant timing + channel/subch `FUN_00062c42`); block → L3 RR matches our request-ref, extracts SDCCH (TN/subch/TSC/TA/hop) **[C]+[L3]** |
| 8 | TX | SDCCH (9) | **SABM: LU REQUEST** | FI **7** ×4 + TXBUFQ type-3 + cmd 4; cmd 3 re-track | L3 builds LU REQUEST (LAI+classmark+IMSI/TMSI); L2 wraps SABM; `l1_uplink_post`-equiv `FUN_000504f8` **[C]+[L3]** |
| 9 | RX | SDCCH (9) | **UA** `[air]` | FI 2 tag 9 | `rx_read` → tag 9 → **`FUN_0004dd7c`** → up to L2 → LAPDm **ESTABLISHED** **[C]** |
| 10 | RX | SDCCH (9) | **AUTHENTICATION REQUEST** `[air]` | FI 2 tag 9 | tag 9 → `FUN_0004dd7c` → L3 MM (has RAND) **[C]+[L3]** |
| 11 | TX | SDCCH (9) | **AUTH RESPONSE** | FI 7 + cmd 4 | SIM RUN GSM ALGORITHM (RAND→SRES,Kc); SRES uplink via the type-7 path **[C]+[L3]** |
| 12 | RX | SDCCH (9) | **CIPHER MODE COMMAND** (RR 0x35) `[air]` | FI 2 tag 9 | tag 9 → `FUN_0004dd7c` → L3 RR → **`MPH_CIPHER_MODE_REQ`** down to L1 **[C]+[L3]** |
| 13 | — | — | **turn ciphering on** | share `+0x3f8`, FI **word-5** | L1 MPH handler writes **Kc[8] → +0x3f8** and sets word-5 bits (bit15 load, 14:13=A5/1) = `0xA000` — exactly `l1_cipher_set` (§7) **[C]** |
| 14 | TX | SDCCH (9) | **CIPHER MODE COMPLETE** (ciphered) | FI 7 w/ word-5 cipher | first enciphered uplink block **[C]+[L3]** |
| 15 | RX | SDCCH (9) | **LOCATION UPDATING ACCEPT** (ciphered) `[air]` | FI 2 tag 9 (DSP deciphers via word-5) | tag 9 → `FUN_0004dd7c` → L3 MM → **registered** (+new TMSI) **[C]+[L3]** |
| 16 | TX | SDCCH (9) | TMSI REALLOCATION COMPLETE (if TMSI) | FI 7 + cmd 4 | normal uplink **[C]+[L3]** |
| 17 | — | — | release | DM_REL → back to camp | RR releases SDCCH; L1 to idle/camp **[L3]** |

## Correlations worth seeing
- **The grant closes the RACH loop.** Step 6 TX (CHANNEL REQUEST, a random
  reference `ra`) and step 7 RX (IMMEDIATE ASSIGNMENT) are matched by L3 RR on the
  *request reference + frame number* — the tag-3 handler `FUN_0004c788` carries the
  grant's timing (it compares frame numbers, `FUN_00062dee`) and the channel
  description, which RR checks against the RACH it sent. Our `os/net/gsm_lu.c`
  already does this match (`ref_of_fn` within `LU_REF_SLACK`).
- **One receive handler, many messages.** Steps 9–15's downlink (UA, AUTH REQUEST,
  CIPHER MODE COMMAND, LU ACCEPT) *all* enter through the single tag-9 handler
  `FUN_0004dd7c`; the phone tells them apart only after L2/L3, and responds with a
  different TX each time. So "what does it do on cipher vs classmark" is **not** an
  L1 branch — it is the same receive, a different L3-chosen uplink.
- **Cipher is the only step that changes the DSP.** Every other response is just
  another type-7 uplink block. CIPHER MODE COMMAND (step 12) is special only because
  L3's response is an **MPH signal that writes the DSP cipher regs** (step 13), after
  which both directions carry the word-5 cipher bits (§7) — no per-message L1 logic.
- **CLASSMARK ENQUIRY** (RR 0x16), if the network sends it, is the same shape:
  tag-9 receive → L3 RR → **CLASSMARK CHANGE uplink** (an ordinary type-7 block), no
  DSP change. (Our OS sends an *early* CLASSMARK CHANGE proactively to force A5/1.)

## What our OS must reproduce
The L1↔DSP mechanics (steps 0–3, 5–9, 11, 13–16) are all **[C]** — RACH, dedicated
establish (cmd 3), the SDCCH pump, `l1_cipher_set`, the uplink path — and are
already built. The missing intelligence is the **[L3]** decisions (steps 4, 7-match,
8, 10-12, 15): decode each block, pick the next uplink. That logic lives in the
paged stack here; our `os/net/{rr,mm,lapdm,gsm_reg,gsm_lu}.c` reimplements exactly
these decisions over the same L1 primitives — which is why the gap has always been
L3 reliability (LAPDm sequencing, the cipher COUNT, uplink robustness), never the
DSP interface.

## Recovering the [L3] steps (the paged stack is RE-able)
The `[L3]` decisions above are not a dead end. The RR/MM/LAPDm/L3 code is in the
**demand-paged user firmware** `SM-B310E_Backup/.omo/dumpmine/user_raw.bin`
(identical to `dsp/drps_12E524_0.bin`, 3.6 MB Thumb-2), which — unlike the stripped
resident kernel — keeps **assertion strings that name the source files and
variables** (`rr_adapter.c`, `mm_msg.c`, `mm_specific_procedure.c`,
`rr_res.sapi0_chan`, `est_mode == ESTABLISH_NORMAL`, `MPH_FAB_RACH_IND`,
`DL:package SABM`, `T3212_EXP_IND`, …). Import it into Ghidra at **base
`0x27f30000`** (empirically recovered; the `gsm-plan` guess `0x08000000` is wrong)
and anchor on those strings to identify the handlers. Demonstrated: `DL:package
SABM` → `FUN_27f746e6` = the **LAPDm SABM/uplink builder** (decompiles cleanly).
So each `[L3]` step — the immediate-assignment match, the AUTH/cipher/classmark
dispatch, LU ACCEPT handling — is recoverable from this image; see
`[[b310e-ghidra-headless]]` for the import recipe. (Ciphering itself may be
numeric-trace there — no `Cipher` string in `user_raw` — but the MM/RR/LAPDm
procedures all carry filename anchors.)
