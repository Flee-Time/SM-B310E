# SC6530C Audio/DSP Protocol — static extraction

> Protocol-facts document for the **ARM↔DSP host interface** of the SC6530C.
> Sources: the **vendor SDK** (leaked internal Samsung/Spreadtrum SDK — **never
> committed**; read-only reference for register ground truth, semantics only, no
> code copied) cross-checked against the B310E firmware dump (`dump_firmware.bin`,
> disassembly + Ghidra decompile). Every item that could not be traced to a
> source is marked **UNKNOWN**; nothing is guessed.
>
> The DSP core is a **proprietary TeakLite-family core** (the SDK build flags
> call it `SC6600M3`/`CHIP_SC6600L`). Only the **ARM-side host interface** is
> documented here — never the DSP ISA.

---

## DSP-interface

### 0. Scope and sources

The four DSP sources in the vendor SDK implement: the **DSP firmware download
protocol** (ARM boots the DSP by copying its code over shared memory in
blocks), the **hardware interrupt mailbox** (ARM→DSP / DSP→ARM interrupts), and
the **reset/strap/clock registers**. Sources (read-only; none copied):

| File (in the vendor SDK) | Role |
|---|---|
| `dsp_ctrl_hal.c` | Download-protocol state machine: share-mem control struct, per-block handshake, boot sequence (`_DSP_CTRL_*`, public `DSP_CTRL_DownLoadCode`). |
| `dsp_hal.c` / `dsp_phy.h` | Thin HAL wrapper (`DSP_HAL_*` → `DSP_PHY_*`) + prototypes. |
| `dsp_phy_v5.c` | Physical layer (`DSP_PHY_*`): share-mem base, reset/strap/boot/interrupt registers. |
| `sc6531efm_reg_global.h` | Register bit-field ground truth (APB/GLB); base `GLB_REG_BASE` from `sc6531efm_reg_base.h`. |
| `dsp_drvapi.h` | Enums `DSP_TEAKLITE_STRAP`, `DSP_INT_TYPE_E`, `CLK_DSP_TYPE_E`. |
| `dsp_log.h` | DSP→ARM log shared-memory layout (`SIO_DSP_LOG_INFO_S`), the **second** share-mem consumer. |
| `sc6531efm_audio_cfg.h` | VBC/audio ARM-vs-DSP ownership helpers (`vb_switch_arm_ctl`, `vbc_phy_reset`). |
| Cross-checks | Dump-analysis scripts (DSP_CODE flash region), the audio-HAL plan §2/§3 (DSP-side ADI pool), the B310E dump's own ARM DSP downloader (`dsp_downloader.bin` disassembly), the layer1 DSP build path (`layer1_dsp.mk`). |

**What this interface is and is not.** There is **no numeric "command ID"
opcode set** in these files: the protocol's "commands" are the **bit flags in
the two shared-memory status words** (§4a/§4b). Audio DSP command messages (if
any) are NOT in these four files — audio ownership is register-level (§7) and
the DSP data-path command protocol is a **known gap** (UNKNOWN-7).

### 1. The SC6530C share-memory base — **UNKNOWN** (the critical finding)

**Do not assume a base.** The SDK only pins the value for the
SC6531EFM/UWS6121E family:

- `DSP_PHY_GetShareMemBaseAddr()` returns **`0x30000000`** for
  `PLATFORM_SC6531EFM || PLATFORM_UWS6121E`; every other (older) platform branch
  returns **`0x10000000`** — the only two values this SDK hard-pins.
- The **SC6530C is not covered by a dedicated `dsp_phy` in this SDK snapshot.**
  The only `dsp_phy_vN.c` wired into the build is `dsp_phy_v5.c` (selected for
  `PLATFORM=UWS6121E`); the chip_plf set is
  `sc6531efm/uix8910/uix8910mpw/uws6121e` — **no SC6530C directory exists**. If
  compiled for a SC6530 build it would take the `#else` branch (`0x10000000`),
  but the stock B310E firmware predates this SDK, so which branch its build used
  is unknowable from the SDK alone.
- The SDK does reference SC6530 in the **layer1 (modem) DSP build**, not in
  chip_drv: `layer1_dsp.mk` adds `-DSC6600M3 -DCHIP_SC6600L` for
  `PLATFORM=SC6530`, selects `DSP_simple_pdata_sc6530.c`, maps
  `CHIP_DSP=6500|6531` to `-DLAYER1_SC6530C`. The layer1 DSP *source* files are
  **not present** in this snapshot (only the make file).
- The dump's own ARM DSP downloader (`dsp_downloader.bin`) decodes as ARM Thumb
  and builds a download-parameter struct through indirect calls; the base is not
  an obvious immediate in the first literal scan.
- **Dump cross-check — where the SC6530C DSP firmware lives in flash:** the dump
  analysis maps the B310E dump's `DSP_CODE` partition to **`0x570000`–
  `0x68C000`** (between the `tres0001` resource partition end `0x568318` and the
  FS partition at `0x690000`). That region holds the DSP firmware image (the
  layer1/modem + audio DSP pdata the ARM downloader loads); it is the
  DSP-firmware extraction target, not a share-mem base. The audio NV blocks
  (Handset/Headset) sit at `0x680000+`.

**Decision:** treat the SC6530C share-mem base as **UNKNOWN with two
candidates** — `0x30000000` (SC6531EFM/UWS6121E-family value) and
`0x10000000` (else-branch value; equals `DSP_SIDE_START_ADDR`, §3). Do NOT
hardcode either into a fake; map **both** addresses onto one shared region (a
single RAM region aliased at both candidate bases) so a guest handshake answers
regardless of which value the real chip uses. **The dump cross-map settles the
weight: `0x10000000`** (R3) — the dump's own downloader takes the base from a
runtime `dsp_download_param.pdata` struct (assert string present, no literal),
and the SDK's SC6530-class default is the else-branch `0x10000000`.

### 2. Shared-memory layout (the download control block)

ARM and DSP exchange the download state through a **16-bit control struct at the
share-mem base** (pointer obtained from `CHIP_DSPShareMemBase()`):

```
struct DSP_DL_CTL_T:                    /* 8 bytes total */
    +0  arm_control_status  u16   ARM→DSP status bits (§4a)
    +2  dsp_control_status  u16   DSP→ARM status bits (§4b)
    +4  dl_offset           u16   download offset
    +6  dl_block_size       u16   current block size
    +8  ... download DATA starts here
```

- `SHARE_MEM_CTRL_SIZE = 10` halfwords (20 bytes): init clears the 8-byte
  struct **plus 12 reserved bytes** at the base.
- Block data is copied to **`share_mem + 8`**.
- `DSP_DL_BLOCK_SIZE = 1024` halfwords = **2048 bytes per block**;
  `DSP_DL_MAX_BLOCK_SIZE = 2048 − 8/2 = 2044` halfwords (defined but unused).
  **All data-size units are halfwords** (`DSP_CTRL_DownLoadCode` doc: "the unit
  of input data_size is half-word").
- **Access discipline:** every status-word write/read is done **twice** with a
  verify loop (`SHARE_MEM_WRITE`/`SHARE_MEM_READ`) — *shared memory runs at the
  DSP clock* (§6), so the double access guards against a stale first read in the
  async bridge.
- `DSP_DL_WAIT_TIME = 10000`: every poll loop re-asserts its "ready" bits after
  10000 iterations (a keepalive, see §5) rather than failing.

### 3. The second share-mem consumer: the DSP→ARM log protocol

The DSP writes a debug-log ring into shared memory; the ARM side walks it via
`SIO_DSP_LOG_INFO_S` (offsets are struct order):

```
+0x00 TPWrite_idx        +0x10 memDataWrite_idx       +0x20 statusFlag
+0x04 TPRead_idx         +0x14 memDataRead_idx        +0x24 dspNvSet
+0x08 TPBuffBaseAddr     +0x18 memDataBuffBaseAddr    +0x28 dumpDspBlkNum
+0x0C TPBuffSize         +0x1C memDataBuffSize        +0x2C dump_dsp_blk[8]  (SIO_DUMP_DSP_BLK_INFO ×8, 8 B each)
```

- `statusFlag`: bit0 TP-force-flush, bit1 MEM-force-flush, **bit7
  DSP-assert-busy** (ARM must wait). Default ring sizes: TP `75*1024` B, MEM
  `25*1024` B, MEM send threshold `8*1024` B.
- **Address translation:** the DSP writes its buffer addresses in **DSP-side
  Z-space**; ARM computes `(dsp_addr | DSP_ARM_ADD_OFFSET) − DSP_ARM_ADD_OFFSET +
  CHIP_GetDSPShareMemVirBaseAddr()`. `DSP_ARM_ADD_OFFSET = 0xC0000000` and
  **`DSP_SIDE_START_ADDR = 0x10000000`** is the DSP's own base for this region.
  `CHIP_GetDSPShareMemVirBaseAddr()` is **undefined in this SDK snapshot** (only
  the `DSP_PHY_GetDSPShareMemVirBaseAddr()` wrapper exists and returns 0) → the
  ARM-side virtual base is UNKNOWN-6.
- **Not the audio path:** this log protocol is debug only; it does not carry
  audio commands.

### 4. Mailbox/command registers and status bits

#### 4a. ARM→DSP status bits — `arm_control_status`

| Bit | Name | Meaning |
|---|---|---|
| 0x0001 | `DSP_DL_ARM_READY` | ARM is ready to send data |
| 0x0002 | `DSP_DL_ARM_DATA_READY` | ARM has copied data to share memory |
| 0x0004 | `DSP_DL_ARM_START_COPY` | ARM is beginning to copy (start of a block transfer) |
| 0x0008 | `DSP_DL_ARM_BOOT_DONE` | DSP code was downloaded successfully (last block) |

#### 4b. DSP→ARM status bits — `dsp_control_status`

| Bit | Name | Meaning |
|---|---|---|
| 0x0001 | `DSP_DL_DSP_READY` | DSP is ready to receive data |
| 0x0002 | `DSP_DL_DSP_READY_TO_COPY` | DSP is ready to copy the next block |
| 0x0004 | `DSP_DL_DSP_COPY_DONE` | DSP has copied the current block |
| 0x0008 | `DSP_DL_DSP_RUN` | DSP is running |

There are **no numeric command IDs** — the "command" is the bit pattern in
these two halfwords.

#### 4c. Hardware registers (the interrupt mailbox + control). Base `GLB_REG_BASE = 0x8B000000`.

| Address | SDK name | Bit fields (name = bit) | Used by (SDK fn) |
|---|---|---|---|
| `0x8B000140` | `APB_INT_STS0` | **DSP_IRQ = b2, DSP_FRQ = b3** (DSP set by pulse); also VBCDA_IRQ b5, VBCAD_IRQ b4, MCU_IRQ b0 | DSP→ARM interrupt status — the target of `DSP_is_DSP_IRQ_EN()` (UNKNOWN-3) |
| `0x8B000160` | `APB_INT_SET_CLR0` | **MCU_IRQ_SET = b0** (write 1 → send IRQ to DSP), MCU_FRQ_SET = b1, **DSP_IRQ_CLR = b2, DSP_FRQ_CLR = b3** (write 1 → clear DSP→ARM); also VBCDA_IRQ_CLR b5, VBCAD_IRQ_CLR b4 | `DSP_PHY_SetMcuIrqInt`, `DSP_PHY_ClrDspIrqInt` |
| `0x8B001068` | `APB_RST0_SET` | **DSP_SOFT_RST_SET = b16** | `DSP_PHY_DspinResetMode(TRUE)` — hold DSP |
| `0x8B002068` | `APB_RST0_CLR` | **DSP_SOFT_RST_CLR = b16** | `DSP_PHY_DspinResetMode(FALSE)` — release DSP |
| `0x8B0001A0` | `APB_MCU_CTL0` | **ARM_BOOT_ADDR = [15:0]** (DSP boot vector), **ARM_BOOT_EN = b16**, ARM_MEM1_EN = b19 (IRAM switch), ARM_MEM0/2/3_EN | `DSP_PHY_SetDspBootVector`, `DSP_PHY_DspBootEnable`, `DSP_PHY_IRAMSwitchtoARM` |
| `0x8B0001C0` | `APB_DSP_CTL0` | **STRAP_BITS = [7:3]** (TeakLite strap, §4d), **DSP_EXT_Z = b2** (external Z-space), **DSP_CLK_FORCE_ON = b1** (§6), STC_RTC_EB = b8 | `DSP_PHY_SetStrapMode` |
| `0x8B0001C4` | `APB_PERI_CTL0` | **AUD_CTRL_SEL = b10, CLK_AUD_ARM_CTRL = b9** (audio/VBC ownership), **ARM_VB_ACC = b2**, IIS_MUX_SEL = b1, UART1_MUX_SEL = b0, ARM_VB_* = b3-b8, b11 | `vb_switch_arm_ctl`; the `0x600` register in the audio plan |

Bit-field source: the SDK's `sc6531efm_reg_global.h` INT_STS0 / INT_SET_CLR0 /
MCU_CTL0 / DSP_CTL0 / PERI_CTL0 field sections.

#### 4d. Strap modes (TeakLite boot strap, `APB_DSP_CTL0[7:3]`)

| Value | Name | Meaning |
|---|---|---|
| 0x0 | `EXT_PROG_MODE` | external program mode |
| 0x1 | `TEST_MODE` | test mode |
| 0x2 | `BOOT_MODE` | boot mode, start from address 0xFFFE |
| 0x3 | `DEBUG_MODE` | debug mode |
| 0x4 | `USER_RST_MODE` | user reset to OCEM |

`DSP_INT_TYPE_E`: `DSP_INT_IRQ0=0`, `DSP_INT_FRQ0=1`, `DSP_INT_IRQ1=2`,
`DSP_INT_FRQ1=3` — only IRQ0/FRQ0 are implemented in the v5 PHY; IRQ1/FRQ1 are
empty.

### 5. Send/recv flow — the download handshake

The full sequence, with the SDK's own `@PROTOCOL_N` markers.

**Entry** (`_DSP_CTRL_Download`):
1. `s_dl_ctl_status = CHIP_DSPShareMemBase()`; hold DSP reset (`DSP_in_Reset()`).
2. **PROTOCOL_0** — clear the share-mem control block (first 20 bytes)
   (`_DSP_CTRL_SM_Init`).
3. **PROTOCOL_1** — `arm_control_status = ARM_READY`.
4. **PROTOCOL_2** — `_DSP_CTRL_InitHardware()`: still-held reset →
   `DSP_Set_Boot_Vector(0xA800)` (`DSP_DL_BASE_ADDRESS`, written to
   `APB_MCU_CTL0[15:0]`) → `DSP_Set_DSP_Strap_Mode(USER_RST_MODE)` →
   `DSP_Boot_Enable()` → re-assert `ARM_READY` → **release reset**
   (`DSP_in_Release()`, "let DSP starts running") → `DSP_IRQ_CLR()`.
5. **PROTOCOL_3** — poll `dsp_control_status & DSP_READY`; keepalive re-asserts
   `ARM_READY` every 10000 iters (`_DSP_CTRL_DL_Loop`).
6. Download loop (`_DSP_CTRL_DL_Loop`): `block_num = ceil(size/1024)`; each
   non-last block via `_DSP_CTRL_DL_Data(offset, 1024, last_block=0)`, then
   clear `START_COPY`; the last block via
   `_DSP_CTRL_DL_Data(offset, last_block_size, last_block=1)`, clear
   `START_COPY`, then final `arm_control_status = BOOT_DONE`.
7. After the loop: `DSP_Set_DSP_Strap_Mode(EXT_PROG_MODE)`, `DSP_Boot_Disable()`,
   `DSP_in_Reset()`.

**Per block** (`_DSP_CTRL_DL_Data`):
- **PROTOCOL_6**: write `dl_offset` + `dl_block_size`; copy `size×2` bytes to
  `share_mem + 8`; set `arm_control_status = READY|DATA_READY`; poll
  `dsp_control_status & DSP_READY_TO_COPY` (keepalive re-asserts
  `READY|DATA_READY`).
- **PROTOCOL_8**: clear `DATA_READY`; if last block OR-in `BOOT_DONE`; set
  `START_COPY`; poll `dsp_control_status & DSP_COPY_DONE` (keepalive re-asserts
  `BOOT_DONE` if last else `START_COPY`).
- **PROTOCOL_10**: wait the DSP IRQ — `while(!DSP_is_DSP_IRQ_EN()){}` then
  `DSP_IRQ_CLR()` + `DSP_FIQ_CLR()` (`_DSP_CTRL_DspIsResponse`).

**DSP side (the fake's obligations, inferred — DSP firmware is not
available):** after release of reset, boot at the strap/boot-vector config,
clear/own the same status words, set `DSP_READY`, and for each block: take
`READY_TO_COPY` high once it sees `DATA_READY`, take `COPY_DONE` high after
consuming the block when `START_COPY` is set, and finally pulse the DSP→ARM
IRQ — **inferred from the ARM-side state machine** (UNKNOWN-5).

### 6. Clock discipline for shared memory (critical for a fake)

`APB_DSP_CTL0` (`0x8B0001C0`), **`DSP_CLK_FORCE_ON = b1`** — register doc:
*"Force to open clock of DSP. (This bit is added for shared memory. Shared
memory runs at DSP clock). Before access shared memory, this bit should be set
to 1; after access, clear it to 0 (MUST)."*

Implication: the ARM side must force the DSP clock on **before** touching the
share-mem region and clear it after. A fake should model this bit (store + echo
is sufficient). The v5 `DSP_PHY_DspClockEnable` is **empty**, so enforcement is
up to the caller of `DSP_CTRL_DownLoadCode`.

### 7. Audio ownership (ARM vs DSP)

Audio peripherals on this SoC are **muxed between ARM and DSP**; ownership is
`APB_PERI_CTL0 = 0x8B0001C4`:

| Bit | Name | Meaning |
|---|---|---|
| b10 | `AUD_CTRL_SEL` | DSP/ARM9 AUD control |
| b9 | `CLK_AUD_ARM_CTRL` | VBC SW control (ARM clock ownership) |
| b2 | `ARM_VB_ACC` | ARM vs DSP **VBC** access |
| b1 | `IIS_MUX_SEL` | DSP/ARM9 IIS control |
| b0 | `UART1_MUX_SEL` | DSP/ARM9 UART control |
| b3-b8, b11 | `ARM_VB_ADC0ON/1ON`, `ARM_VB_DA0ON/1ON`, `ARM_VB_MCLKON`, `ARM_VB_ANAON`, `ARM_VB_RST` | VBC SW control |

- `vb_switch_arm_ctl(TRUE)` → `VB_CLK_CTL |= ARM_VB_ACC`; `FALSE` → `&= ~ARM_VB_ACC`
  (`VB_CLK_CTL` = `APB_PERI_CTL0`).
- **`0x600` = bits 9-10** (`CLK_AUD_ARM_CTRL | AUD_CTRL_SEL`) = "ARM owns codec".
- VBC interrupt routing is also on these two registers: status `APB_INT_STS0`
  b4/b5, clear `APB_INT_SET_CLR0` b4/b5; VBC soft-reset via `APB_RST0_SET`/`CLR`.
- The SoC framework switches audio back to ARM before asserting:
  `VB_SWTICH_ARM_CTL(TRUE)`.

**Dump cross-check (the `0x82001140` pool is DSP-side):** the B310E dump's
`0x0C29B8` pool `{0x82001140, 0x82000018, 0x8200001c}` is **DSP-side**
(referenced only from code @`0x0BEFFC`), i.e. the DSP uses the same ADI mailbox
(`0x82000018` index / `0x8200001C` data, ANA register `0x1140`) — not the
ARM-side helper (`0x302B6`/`0x3034E`). `0x82001140` is plausibly
audio/DSP-relevant — **UNKNOWN-8**.

### 8. Handshake pseudocode (implement a DSP fake from this alone)

The fake below is the ARM-side protocol transcribed from `dsp_ctrl_hal.c` so a
fake can be tested against it. Registers as §4c; shared-mem as §2. The fake's
responses are marked `FAKE:`. Everything marked `FAKE:` is the *inferred DSP
obligation* (UNKNOWN-5), not SDK verbatim — the SDK contains only the ARM side.

```
// Shared-memory control words (16-bit) at SHARE_BASE:
//   +0 arm_ctl, +2 dsp_ctl, +4 dl_offset, +6 dl_block_size ; data at SHARE_BASE+8
// Hardware (APB, 32-bit):
//   INT_STS0  = 0x8B000140 ; INT_SET_CLR0 = 0x8B000160
//   RST0_SET  = 0x8B001068 ; RST0_CLR      = 0x8B002068
//   MCU_CTL0  = 0x8B0001A0 ; DSP_CTL0      = 0x8B0001C0
// Bits: ARM{READY=1,DATA_READY=2,START_COPY=4,BOOT_DONE=8}
//       DSP{READY=1,READY_TO_COPY=2,COPY_DONE=4,RUN=8}
//       INT{MCU_IRQ_SET=b0, DSP_IRQ_CLR=b2, DSP_FRQ_CLR=b3} ; STS{DSP_IRQ=b2}
//       MCU_CTL0{BOOT_ADDR=[15:0], BOOT_EN=b16} ; DSP_CTL0{STRAP=[7:3], DSP_EXT_Z=b2, DSP_CLK_FORCE_ON=b1}

ARM_download(fw_data, size_halfwords):            // DSP_CTRL_DownLoadCode
    DSP_CTL0 |= DSP_CLK_FORCE_ON                   // §6: shared mem runs at DSP clock
    RST0_SET  = DSP_SOFT_RST                       // hold DSP
    clear SHARE_BASE[0..20)                         // PROTOCOL_0 (SM_Init)
    arm_ctl = ARM_READY                            // PROTOCOL_1
    MCU_CTL0 = (MCU_CTL0 & ~0xFFFF) | 0xA800        // PROTOCOL_2: boot vector = DSP_DL_BASE_ADDRESS
    DSP_CTL0 = (DSP_CTL0 & ~0xF8) | (USER_RST_MODE << 3)  // strap USER_RST_MODE=4
    MCU_CTL0 |= ARM_BOOT_EN                         // Boot_Enable
    arm_ctl = ARM_READY
    RST0_CLR = DSP_SOFT_RST                         // release -> DSP boots
    INT_SET_CLR0 = DSP_IRQ_CLR                      // clear any pending DSP IRQ
    while !(dsp_ctl & DSP_READY):                   // PROTOCOL_3; keepalive: arm_ctl=ARM_READY
        if timeouts > 10000: arm_ctl = ARM_READY
    for block in ceil(size/1024) blocks:            // _DSP_CTRL_DL_Loop/_DL_Data
        last = (block == last_block)
        SHARE_BASE.dl_offset      = offset
        SHARE_BASE.dl_block_size  = blocksize
        memcpy(SHARE_BASE+8, fw_data+offset, blocksize*2)
        arm_ctl = ARM_READY | ARM_DATA_READY        // PROTOCOL_6
        while !(dsp_ctl & DSP_READY_TO_COPY):       // keepalive re-asserts READY|DATA_READY
            if timeouts > 10000: arm_ctl = ARM_READY|ARM_DATA_READY
        arm_ctl &= ~ARM_DATA_READY                  // PROTOCOL_8
        if last: arm_ctl |= ARM_BOOT_DONE
        arm_ctl |= ARM_START_COPY
        while !(dsp_ctl & DSP_COPY_DONE):           // keepalive: last->BOOT_DONE else START_COPY
            if timeouts > 10000: arm_ctl |= (last ? ARM_BOOT_DONE : ARM_START_COPY)
        while !(INT_STS0 & DSP_IRQ): pass           // PROTOCOL_10: DSP IRQ pulse
        INT_SET_CLR0 = DSP_IRQ_CLR | DSP_FRQ_CLR    // _DSP_CTRL_DspIsResponse
        if not last: arm_ctl &= ~ARM_START_COPY
    arm_ctl = ARM_BOOT_DONE                         // final (DL_Loop:372-373)
    DSP_CTL0 = (DSP_CTL0 & ~0xF8) | (EXT_PROG_MODE << 3)   // strap back to EXT_PROG_MODE=0
    MCU_CTL0 &= ~ARM_BOOT_EN                        // Boot_Disable
    RST0_SET  = DSP_SOFT_RST                        // hold again (download complete)
    DSP_CTL0 &= ~DSP_CLK_FORCE_ON                   // §6: release clock

FAKE_DSP_loop():                                    // inferred DSP obligations (UNKNOWN-5)
    on reset-release with BOOT_EN + strap USER_RST:
        dsp_ctl = 0
        ready the downloader (boot vector 0xA800)
    loop:
        if arm_ctl & ARM_DATA_READY:                // DATA in SHARE_BASE+8, size in dl_block_size
            dsp_ctl |= DSP_READY_TO_COPY            // "ready to copy next block"
            if arm_ctl & ARM_START_COPY:            // consume the block (copy from SHARE_BASE+8)
                dsp_ctl &= ~DSP_READY_TO_COPY
                dsp_ctl |= DSP_COPY_DONE            // "copied current block"
                pulse DSP_IRQ (INT_STS0 bit2)       // PROTOCOL_10 target
                if arm_ctl & ARM_BOOT_DONE: dsp_ctl |= DSP_RUN
        if arm_ctl == ARM_BOOT_DONE and not running:
            dsp_ctl |= DSP_RUN                      // boot complete
```

### 9. UNKNOWN list (untraceable items — do not guess in the fake)

- **UNKNOWN-1 (SHARE-MEM BASE, SC6530C):** the share-mem base for the B310E's
  SC6530C. SDK pins `0x30000000` for SC6531EFM/UWS6121E and `0x10000000` for the
  else-branch; no SC6530-specific `dsp_phy` exists in this SDK snapshot; the
  dump's own downloader does not expose a constant in a quick scan. **Resolved
  in the dump cross-map (R3): weight `0x10000000`, both bases mapped.**
  Candidates: `0x30000000` / `0x10000000`.
- **UNKNOWN-2 (`CHIP_DSPShareMemBase()`):** macro/function used at the download
  entry but **not defined anywhere in the SDK tree** (usage only).
- **UNKNOWN-3 (`DSP_is_DSP_IRQ_EN()`):** used in the PROTOCOL_10 wait,
  **undefined in the SDK**; the register docs imply it polls `APB_INT_STS0` b2
  (`DSP_IRQ`) but the exact check is unverified.
- **UNKNOWN-4 (`DSP_IRQ_CLR()`/`DSP_FIQ_CLR()` as functions):** the SDK defines
  **`DSP_IRQ_CLR` as a bit mask** (`BIT_2`) and a function symbol is not
  present — the function's body (writes `APB_INT_SET_CLR0`?) is inferred, not
  sourced.
- **UNKNOWN-5 (DSP-side behavior):** every `FAKE:` response in §8
  (READY/READY_TO_COPY/COPY_DONE/RUN timing, the end-of-download IRQ) is
  inferred from the ARM state machine. The DSP firmware is proprietary; only the
  ARM half is in the SDK.
- **UNKNOWN-6 (`CHIP_GetDSPShareMemVirBaseAddr()`):** used by the log-address
  translation, **undefined in the SDK**; the `DSP_PHY_GetDSPShareMemVirBaseAddr()`
  wrapper returns 0.
- **UNKNOWN-7 (audio DSP command protocol):** the four DSP files contain no
  audio command message/opcode set. Audio ownership is register-level (§7); a
  DSP-command interface for audio, if it exists, is **not documented by these
  sources** — the dump's `dsp_codec` walker (`0x87764`) and the layer1 DSP
  sources (absent from this snapshot) are the leads. **Partially resolved:** the
  NV analysis (NV §4) identifies the concrete shape — the `audio_dsp_info`
  token from the dsp_codec walker; DSP-side interpretation is still open (→N4).
- **UNKNOWN-8 (`0x82001140` ANA register):** the dump shows a DSP-side ADI pool
  `{0x82001140, 0x82000018, 0x8200001c}` at `0x0C29B8` (referenced from
  `0x0BEFFC`); what ANA register `0x1140` is (or whether it is audio) is not
  sourced.
- **UNKNOWN-9 (SC6530C DSP core):** the SDK build flags the layer1 DSP as
  `SC6600M3`, but whether the SC6530C's audio DSP core matches, and its Z-space
  memory map, are not in this SDK snapshot.
- **UNKNOWN-10 (`INTER_SHARE_MEM_BEGIN`):** used in the SDK's share-mem init
  paths, **undefined in the SDK** (usage only).
- **UNKNOWN-11 (`DSP_in_Reset`/`DSP_in_Release`/`DSP_Set_Boot_Vector`/
  `DSP_Set_DSP_Strap_Mode`/`DSP_Boot_Enable`/`DSP_Boot_Disable`):** used
  throughout the download flow, undefined in the SDK; their register targets are
  **inferred** to §4c from the v5 PHY functions (`DspinResetMode`,
  `SetStrapMode`, `SetDspBootVector`, `DspBootEnable`).
- **UNKNOWN-12 (IRQ1/FRQ1 + clock/status stubs):** `DSP_PHY_DspClockEnable`,
  `GetDspIrqSts`, `DSPIRQFIQINTEnable`, `RegisterHandle`, `ARM_DSLEEP_EN`,
  `ARM_TO_DSP_ASHB_EN`, `ExceptionCheck`, and the IRQ1/FRQ1 branches are
  **empty** — semantics unknown.

---

## Codec-driver

### 0. Scope and family caveat

This section documents the **SC6531EFM-family internal codec driver** (the
`v0` PHY layer) as the SDK implements it, cross-referenced against the SC6530C
dump-derived register table. Sources (read-only, none copied):
`sprd_codec_ap_phy_v0.{c,h}` (AP analog-path PHY: power ladder, analog DAC
enable/routing, DAC gains, PA), `sprd_codec_dp_phy_v0.{c,h}` (DP digital-path
PHY: DAC/ADC digital enable, I2S, fs modes, DAC mute/ramp),
`sprd_codec_state_v0.c` (state machine: bring-up order, delays, mute
sequencing), `audio_codec_sprd.c` (codec HAL: `is_dsp_ctl`, PA control, PGA
update, run_callback), `audio_hal.c` (HAL orchestration), `sc6531efm_audio_cfg.h`
(chip-PLF config: codec register bases, `SPRD_CODEC_CTRL_BY_DSP`, reset/clk
macros), `sc6531efm_reg_base.h` (register bases), `sc6531efm_reg_global.h`
(`APB_PERI_CTL0` bits), `adi_hal_internal.h` (ADI access macros — the ANA
mailbox).

**Family caveat:** every *address* below is SC6531EFM-family; only the
*semantics* transfer to the SC6530C — the dump shows the same offset skeleton at
a different base with partly different bit roles (see §5). The SDK v0 codec is
**not** what UWS6121E ships (that platform builds the external `es83xx` codec);
it is the reference for the SC6531EFM/internal-codec family that the B310E's
audio driver is modeled on.

### 1. The driver split: AP (analog) and DP (digital) PHY

The codec is split into two register blocks (both struct-addressed, so every
claim is an offset, not an absolute address):

- **AP block** — the analog-die codec (`SPRD_CODEC_AP_REG_CTL_T`): `audif_enb`
  +0x00, `audif_clr` +0x04, `audif_sync_ctl` +0x08, `audif_shutdown_ctl` +0x14,
  `pmur1` +0x40, `pmur2` +0x44, `pmur3` +0x48, `pmur4` +0x4C, `pmur5` +0x50,
  `pmur6` +0x54, `hibdr` +0x58, `aacr1` +0x5C, `aacr2` +0x60, `aaicr1..3`
  +0x64/68/6C, `acgr` +0x70, **`dacr` +0x74**, `daocr1..3` +0x78/7C/80,
  `dcr1` +0x84, `dcr2` +0x88, `dcr3` +0x8C, `dcr4` +0x90, **`dcgr1` +0x94 (HP),
  `dcgr2` +0x98 (EAR), `dcgr3` +0x9C (PA)**, `pnrcr1..3` +0xA0/A4/A8,
  **`ccr` +0xAC (clocks)**, `ifr1..3` +0xB0/B4/B8.
- **DP block** — the digital codec (`SPRD_CODEC_DP_CTL_T`): `top_ctl` +0x00
  (DAC/ADC enable L/R), `aud_clr` +0x04, `i2s_ctl` +0x08, `dac_ctl` +0x0C
  (fs mode, **DAC mute** div0/div1 + `DAC_MUTE_EN` b15 + `DAC_MUTE_START` b14),
  `sdm_ctl0/1` +0x10/+0x14, `adc_ctl` +0x18, `loop_ctl` +0x1C, `aud_sts0` +0x20
  (MUTE_ST), `int_clr`/`int_en` +0x24/+0x28.
- Access: AP regs via the **ANA (ADI) mailbox**; DP regs are direct CHIP_REG
  reads/writes. This matches the SC6530C ADI mailbox protocol and the dump's
  `audio_dp_dac_on` DP block @ **0x8A002000** — **the DP base is the same
  0x8A002000 on both families**.

### 2. Codec bring-up order as the SDK implements it

The order is driven by the codec state machine (`SPRD_CODEC_SM_Process()`),
which **refuses to run unless the codec is ARM-owned**
(`__sprd_codec_is_arm_ctl()`). Full playback sequence:

1. **Init**: zero the codec struct, create the task, `__sprd_codec_init()` calls
   **`__sprd_codec_switch_to(1)`** = ARM owns codec; `SPRD_CODEC_PGA_Init_Set`;
   `_SPRD_CODEC_PA_Def_Setting()`: classAB, no LDO, no DEMI, `PA_LDO_V_32`
   (3.2 V), `PA_DTRI_F_590`.
2. **OFF→PWR** (the power ladder's outer envelope):
   - register + mclk: `APB_EB0_SET AUD_EB_SET` + `ANA_ARM_CLK_EN0 |=
     ANA_CLK_AUD_IF_EN` (D-die + ANA IF clocks).
   - reset: `APB_RST0_SET AUD_SOFT_RST_SET` (b21) + `ANA_SOFT_RST0 |=
     ANA_AUD_IF_SOFT_RST`, ~10-iter delay, then clear both; **delay 5 ms**;
     open; fs_setting; en(1) → DP sdm_set + AP analog power ladder; **delay
     5 ms** (`SPRD_CODEC_VCOM_TIME`).
3. **AP analog power ladder** — the pmur1 order: `BG_IBIAS_EN` b1 →
   `AUD_BG_EN` b3 → `AUD_VCM_EN` b7 → `VCM_BUF_EN` b6 → `VB_EN` b5 → `VBO_EN`
   b4; then `audif_shutdown_ctl = 0x00` ("Turn OFF all protect"); then PA OVP:
   `pa_ovp_en`, `pa_ovp_ldo_en`, `pa_ovp_v_sel(AUD_OVP_V_465_444)`. Every rail
   also arms the deep-sleep LDO auto-close. **Power OFF** reverses the ladder
   with a 2 ms sleep between VCM off and VCMBUF off.
4. **SLP→DA**: `_sprd_codec_mute_all()` (pa/hp/ep gain-mute), then DA_CLK_S:
   **`ccr` `AUD_DAC_CLK_EN` b4 + `AUD_DRV_CLK_EN` b3**.
5. **DAC enable**: mclk (ANA SCLK/6P5M) → **`dac_en(1,chan)`** (DP `top_ctl`
   `DAC_EN_L` b0/`DAC_EN_R` b2 + AP `audif_enb` `AUDIF_DAC_EN_L` b0/
   `AUDIF_DAC_EN_R` b2) → **`ap_dac(1,chan)`** = **`dacr` `AUD_DACL_EN` b7 /
   `AUD_DACR_EN` b6**.
6. **Mixer/output state** (HP/LOUT/EP): `dcr1` output enables (`AUD_HPL_EN` b7 /
   `AUD_HPR_EN` b6 / `AUD_EAR_EN` b5 / `AUD_AOL_EN` b4 / `AUD_DIFF_EN` b2) +
   `daocr1/2/3` routing bits; HP also runs the **pop ramp** (`pnrcr1/2/3`) with
   its own `ifr1` flag poll.
7. **DAC mute/unmute**: DP-side mute sets `dac_ctl` `DAC_MUTE_EN` b15 + toggles
   `DAC_MUTE_START` b14, then the SM waits for `aud_sts0` `MUTE_ST[1:0]` or the
   DACMUTE IRQ.
8. **run_callback**: after the SM settles: fs_setting, `_sprd_codec_update_pga()`
   (**the gains**, §3), update_sw, update_ldo, notify `AUDIO_HAL_CODEC_OPENED`,
   and — only if `is_dsp_ctl` — `__sprd_codec_switch_to(0)`.

### 3. DAC gains and mute

Gains are 4-bit fields in `dcgr1/2/3` (AP) + `acgr` (ADC):
- dcgr3 `DAC_PA_G` [7:4] / `DAC_PAR_G` [3:0] (PA)
- dcgr2 `DAC_EAR_G` [7:4] (EAR)
- dcgr1 `DAC_HPR_G` [3:0] / `DAC_HPL_G` [7:4] (HP)
- acgr (ADC)

Gain value tables: HP −33…+9 dB step 3 dB; EAR same; PA −21…+21 dB;
ADC −6…+39 dB; **mute = gain value 0 (`DAC_*_PGA_G_MUTE (0)`)**.
⚠️ **Discrepancy (UNKNOWN-C3):** the audio-HAL plan says "mute = gain 0xF"; the
v0 SDK mute value is **0, not 0xF** (the plan's 0xF appears to be an
SC6530C-dump-derived reading or an error). The SC6530C additionally has a
**dedicated PA-mute bit `0x82001290` b0** (cross-map row 3) — a different
mechanism from the SDK's dcgr-gain-0 mute.

### 4. PA (speaker path) — ARM-side control

`AUDIO_PA_Ctl(id,is_open)` is the PA master: `pa_d_en` (dcr2 `PA_D_EN` b7,
class-D), `pa_demi_en` (dcr2 `PA_DEMI_EN` b4 + dcr3 `AUD_DRV_OCP_PA_PD` b7),
`pa_ldo_v_sel` (pmur4 `PA_LDO_V` [5:3]) + `pa_ldo_en` (pmur2 `PA_LDO_EN` b5)
when `is_LDO`, `pa_dtri_f_sel` (dcr2 `PA_DTRI_F` [6:5]), then **`pa_en(1)`**
(pmur2 `PA_EN` b3; on close also `PA_SW_EN` b7 + `PA_LDO_EN` b5 and a 2 ms
sleep). **External PA** is the product GPIO hook (`codec_pa_ctl`) — on the
B310E that is the NCP2817BFC `AMP_EN` (candidate GPIOs 18/39 — UNPROBED). The
SDK itself drives only the internal pmur2/PA stage.

### 5. Cross-reference to the SC6530C register table

| SDK v0 semantic | SDK addr (SC6531EFM) | SC6530C dump addr | Verdict |
|---|---|---|---|
| AP codec block base | `SPRD_CODEC_AP_REG_BASE = ANA_AUDIO_BASE` (token undefined in the SDK tree — UNKNOWN-C1) | **0x82001280** | Base candidate **matches** `ANA_AUD_DIG_BASE = 0x82001280` |
| DP codec block base | `SPRD_CODEC_DP_REG_BASE = AUDIO_BASE = 0x8A002000` | 0x8A002000 | **Same on both families** |
| Power rails | pmur1 b1/b3/b7/b6/b5/b4 | struct[0x40] = **0x820012C0** | Offset skeleton matches |
| DAC enable | dacr b7/b6 @ +0x74 | — | semantics-only |
| DAC gains | dcgr1/2/3 @ +0x94/+0x98/+0x9C | struct[0x94]/[0x98] = **0x82001314/0x1318** | Offset skeleton matches |
| PA mute core | dcgr gain = 0 (§3) | **0x82001290 b0 = PA MUTE (1=muted)** | ⚠️ **Different mechanism** — SC6530C has a dedicated PA-mute bit at +0x10; SDK uses dcgr gain-0 |
| PA enable | pmur2 `PA_EN` + external GPIO | 0x82001450/54 write 1 + `0x8B000060/64` b21 + `0x8B0000A0/A4` b28 | ⚠️ SDK lacks the 0x8B000060/64/0xA0/A4 PA-power halves |
| ARM-vs-DSP ownership | **`APB_PERI_CTL0` 0x8B0001C4 b10 `AUD_CTRL_SEL` + b9 `CLK_AUD_ARM_CTRL`** | **0x600 = ARM owns codec** | **Identical** — the register the diff must capture |

The offset-skeleton match is exact: SC6530C base 0x82001280 + the v0 struct
offsets reproduces the dump table addresses (pmur1 +0x40 → 0x820012C0,
dcgr1/2 +0x94/+0x98 → 0x82001314/0x1318). The **bit roles differ** at some
offsets (notably PA-mute: +0x10 is `audif_otptmr_ctl` in the SDK struct but the
SC6530C PA-MUTE core) — that is the "semantics-only" caveat in action.

### 6. `is_dsp_ctl` — **VERDICT: CONFIRMED `== 0`**

- The field `s_sprd_codec.is_dsp_ctl` is set to 1 in exactly one place: codec
  open, only when `(SPRD_CODEC_CTRL_BY_DSP & fun_flg)`.
- `SPRD_CODEC_CTRL_BY_DSP` is `#define`d to **`AUDIO_FUN_DSP_CTL_CODEC`** in all
  four chip_plf audio-cfg headers.
- **`AUDIO_FUN_DSP_CTL_CODEC` is never defined anywhere in the SDK tree.**
  Exhaustive search: it appears only as the macro *target* — there is **no
  `#define AUDIO_FUN_DSP_CTL_CODEC`** and no enum member with that name (the
  `AUDIO_FUN_E` enum is commented out; only NONE/PLAYBACK/CAPTRUE are defined).
  Its sibling `AUDIO_FUN_DSP` is equally undefined.
- Consequence: `SPRD_CODEC_CTRL_BY_DSP` expands to an undefined token; even if a
  caller constructed `fun_flg` from every defined `AUDIO_FUN_*` bit (max
  0x00000003), the `&` can never be nonzero. **`s_sprd_codec.is_dsp_ctl` stays 0
  on this SDK** — the codec is **ARM-controlled**.
- What the flag *would* do (dead code here): the only consumers are
  `__sprd_codec_run_callback` (hand the codec to the DSP after a successful
  open) and `_SPRD_CODEC_Close` (take it back), via
  `sprd_codec_switch_arm_ctl` writing `APB_PERI_CTL0`
  `AUD_CTRL_SEL|CLK_AUD_ARM_CTRL` (**0x600**) — the same ownership register as
  §DSP-interface §7. The DSP-side VBC has its own **defined**
  `VBC_CTL_BY_DSP = 0x80000000` and `__vbc_arm_acc_switch` — but that gates the
  **VBC data path**, not the analog codec.

### 7. ARM-owned vs DSP-owned split (summary)

| Stage | Owner on this SDK | Evidence |
|---|---|---|
| Init / switch-to-ARM | **ARM** | `__sprd_codec_init()` → `__sprd_codec_switch_to(1)` |
| State machine execution | **ARM (gated)** | `SPRD_CODEC_SM_Process` returns if `!__sprd_codec_is_arm_ctl()` |
| Power ladder, clocks, reset | **ARM** | `__sprd_codec_ap_en` / mclk / reset helpers |
| DAC enable + gains + mute | **ARM** | `dacr`/`dcgr1-3`/`dac_ctl` writes |
| PA (internal + external GPIO) | **ARM** | `AUDIO_PA_Ctl` + `codec_pa_ctl` |
| Codec handoff to DSP | **never on this SDK** | `is_dsp_ctl` stays 0 (§6) |
| VBC data path | switchable | `VBC_CTL_BY_DSP = 0x80000000` + `__vbc_arm_acc_switch` |

**What the ARM side must do** (all of it): the full bring-up of §2 — reset,
clocks, the pmur1 rail ladder, DAC enable, gains, PA, DAC-mute sequencing — plus
(B310E-specific, not in the v0 SDK) the external `AMP_EN` GPIO, and **no codec
command is ever sent to the DSP on this path**. The VBC data path is the only
genuinely switchable audio block.

### 8. Beep/tone path

The v0 codec driver has **no tone generator**. A "beep" on the SDK path is
`AUDIO_HAL_Open(AUDIO_HAL_TONE)`, where `AUDIO_HAL_TONE = 0x80000000` is handled
by the HAL/DAI layer, not the codec ("deal by audio_hal, invalid for
codec/dai"); the DAI produces the tone (e.g. AIF hardware tonegen) and the codec
is opened as a plain playback DAC. On the SC6530C the AIF hardware tonegen does
**not** exist — so the B310E's `audio_beep()` square-wave-via-gain-toggling is
an **ARM-side invention**, not an SDK sequence. It is compatible with the SDK's
DAC path (§2 steps 4-8) but nothing in the v0 SDK emits a tone.

### 9. UNKNOWN list (codec-driver specific)

- **UNKNOWN-C1 (`ANA_AUDIO_BASE`):** `SPRD_CODEC_AP_REG_BASE` is `#define`d to
  `ANA_AUDIO_BASE` but that token has no definition in the SDK tree (searched
  all chip_plf headers). The defined analog audio bases are
  `ANA_AUD_DIG_BASE = 0x82001280` and `ANA_GLB_REG_BASE = 0x82001400`; the
  SC6530C codec AP base (0x82001280) matches `ANA_AUD_DIG_BASE`. The SDK's
  intended AP base is therefore most likely 0x82001280, but the macro is
  unresolved in this snapshot.
- **UNKNOWN-C2 (`codec_pa_ctl` body):** the external-PA hook is a product
  function; the SDK ships no default body. B310E's `AMP_EN` GPIO is the
  candidate 18/39 — UNPROBED.
- **UNKNOWN-C3 (mute-value discrepancy):** the plan says "mute = gain 0xF"; the
  v0 SDK's `DAC_*_PGA_G_MUTE (0)` says **0**. Resolve in the hardware/emulator
  diff against the SC6530C dump's actual mute write (SC6530C has a dedicated
  PA-mute bit `0x82001290` b0 — cross-map row 3, R2).
- **UNKNOWN-C4 (`AUDIO_FUN_DSP`/`AUDIO_FUN_DSP_CTL_CODEC` undefined):** both are
  referenced but never defined in the tree — the DSP-control branch is
  unbuildable-as-shipped and, per §6, `is_dsp_ctl` stays 0.

---

## Cross-map

### 0. Scope, method and anchor

This section maps the **known B310E stock-firmware audio functions**
(disassembled from `dump_firmware.bin`, file offset == address) to their vendor
SDK source counterparts (read-only). Every row cites BOTH the dump-disasm
evidence (register writes decoded from the bytes) AND the SDK function. Where
the dump function cannot be mapped to a named SDK function, the row is marked
**UNKNOWN** with the dump evidence — nothing is guessed.

**Method.** Each target region was extracted from `dump_firmware.bin` and
disassembled with `arm-none-eabi-objdump -D -b binary -m arm -M force-thumb`
(the stock audio functions are **Thumb-1**, not ARM — `-m arm` decodes them as
garbage). Addresses are file offsets into `dump_firmware.bin` (XIP, offset ==
virtual address). Literal pools were read as raw 32-bit LE words.

**Base-address anchor (used by every row):** the SC6530C codec AP register block
base is **0x82001280** — the SDK's `ANA_AUD_DIG_BASE`, and the v0 struct
`SPRD_CODEC_AP_REG_CTL_T` is the offset skeleton: +0x00 audif_enb, +0x08
audif_sync_ctl, +0x10 audif_otptmr_ctl, +0x14 audif_shutdown_ctl, +0x20
audif_int_raw, +0x24 audif_int_mask, +0x40 pmur1, +0x44 pmur2, +0x74 dacr,
+0x94/0x98/0x9C dcgr1/2/3, +0xAC ccr. The dump's pool constants confirm:
0x6A3F8 = 0x820012A0 (= +0x20), 0x6A3FC = 0x82001180, 0x6A400 = 0x82001014,
0x816B4 = 0x82001290 (= +0x10), 0x80C4C = 0x042354B0 (the runtime codec struct
base whose field [0] = 0x82001280).

### 1. The cross-map table

| # | Dump addr | SDK counterpart | Register writes (decoded) | DSP interaction |
|---|---|---|---|---|
| 1 | **0x302B6** (ADI read helper) | `ADI_Analogdie_reg_read` (assert strings `adi_phy_v5.c` / "ADI Read Timeout!" at dump 0x303BC+ match) | poll `0x82000020` bit8; write `0x82000018` = `addr&0xFFF`; poll `0x8200001C` bit31; assert `([0x1C]&0x1FFF0000)==(addr&0xFFF)<<16` (pool 0x303E4 = **0x1FFF0000**); return `[0x1C]&0xFFFF` | none |
| 2 | **0x3034E** (ADI write helper) | `ADI_Analogdie_reg_write` (timeout string "ADI Write Timeout!"; final `REG32(addr)=data`) | poll `0x82000020` bit9 (FIFO-full); **direct 32-bit store** to the ANA address — no mailbox write command | none |
| 3 | **0x81470** (PA mute) | `sprd_codec_switch_arm_ctl` (writes `APB_PERI_CTL0` = **0x8B0001C4**, bits `AUD_CTRL_SEL` b10 + `CLK_AUD_ARM_CTRL` b9 = 0x600); the mute **bit itself** (0x82001290 b0 = `audif_otptmr_ctl` +0x10) has **no SDK write** — the SDK mutes via dcgr3 gain 0 | on: `[0x8B0001C4] |= 0x600`, `adi_write(0x82001290, read&~1)`; off: `[0x8B0001C4] &= ~0x600`, `adi_write(0x82001290, read|1)` (pools 0x816B0 = 0x8B0001C0, 0x816B4 = 0x82001290) | **yes — 0x600 = the ARM-vs-DSP ownership bits**: the stock OS asserts ARM codec ownership inline in the mute path |
| 4 | **0x81516** (PA enable) | no SDK counterpart — the SDK's PA enable is `pmur2 PA_EN` which never touches these | `[0x8B000060]=0x200000`; `adi_write(0x82001450,1)`; delay(10); `[0x8B000064]=0x200000`; `adi_write(0x82001454,1)` (pool 0x816C0 = 0x8B000040) | none — SC6530C-specific APB power-half + ANA LDO pair (**UNKNOWN**: no SDK equivalent) |
| 5 | **0x81562** (PA enable 2) | no SDK counterpart (same class as #4) | on: `[0x8B0000A0]=0x10000000`, `adi_write(0x82001440,4)`; off: `[0x8B0000A4]=0x10000000`, `adi_write(0x82001444,4)` | none — SC6530C-specific (**UNKNOWN**) |
| 6 | **0x80982** (power ladder) | `__sprd_codec_ap_en` — **exact bit order**: bg_ibais(0x02)→bg(0x08)→vcm(0x80)→vcm_buf(0x40)→vb(0x20)→vbo(0x10), then "Turn OFF all protect" = the 0x82001294 write; OFF reverses with 2 ms sleep | 6× RMW on struct[0x40]=0x820012C0 (pmur1): 0x02→0x08→0x80→0x40→0x20→0x10, then `adi_write(0x82001294,0)` (audif_shutdown_ctl); pool 0x80C4C = 0x042354B0 | none — but the ladder **RMW helpers also drive GPIOs 31/30/29/28/2** (dump-only, not in the SDK ladder) |
| 7 | **0x81C0A** (device-on) | `_run_pwr_s` power-on state machine; the SDK refuses to run unless ARM-owned | device-state switch over `{0x81562 PA-en2, DAC enable, 0x80982 ladder}` with `adi_write(struct[0x14],0)` after the ladder; each transition writes the state word + delay(5) | none |
| 8 | **0x06A14C** (LDO/device route) | **UNKNOWN by name** — old-generation `audio_hal.c` absent from the SDK snapshot (R4); SDK functional equivalents map to LDO vol/gain helpers | dev0: `0x820012A0 &= ~8; |= 4`; dev1: `0x82001180 &= ~0x10; 0x820011A0 |= 0x10; 0x82001180 &= 0x7FFF; 0x820011A0 |= 0x8000`; dev2: `0x82001014 &= 0x7FFF; |= 0x4000` (on) / `&= 0xBFFF; |= 0x8000` (off). Strings near: `audio_hal.c` @0x6A823, "Register Update Codec LDO VOL Failed!" @0x6A920, `_WAIT_DSP_EVENT` @0x6A8E0 | **`_WAIT_DSP_EVENT` string present** — the old HAL waits on a DSP event (see UNKNOWN-7) |
| 9 | **0x06A2A4** (device route 2) | same family as #8 (**UNKNOWN by name**; v0 struct +0x20/+0x24 = `audif_int_raw`/`audif_int_mask` — the SC6530C repurposes these as DAC-path/mux regs) | dev0: `0x820012A0 &=~0x20, |=0x10`; dev1: `0x820012A0 &=~0x10, |=0x20` + `0x820012A4 &=~0x40, |=0x80`; dev2: `0x820012A4 &=~0x80, |=0x40` | none visible |
| 10 | **0x06A35A** (device on/off) | same family as #8 (**UNKNOWN by name**; `0x82001288` = +0x08 `audif_sync_ctl` — bit semantics SC6530C-specific) | `0x82001288 |=0x400; &=~0x200` (pools 0x6A408=0xFDFF, 0x6A40C=0xFBFF); on: `0x820012A4 |=0x4000`, `0x8200116C |=0x400`; off: `&=~0x4000`, `&=~0x400` | none visible |
| 11 | **0x87720** `dsp_codec` | `dsp_codec_id`/`NV_AUDIO_DSP_CODEC_*` appear only in the SDK's NV adapter (assert message text matches; the enum is **not defined in-tree**). The per-id table walker is the **ARM-side DSP-codec config lookup** (resolves `dsp_codec_id` → DSP-side config ptr, the UNKNOWN-7 lead) | region starts with a data table (0x00000000, 0x02300101, 0x02300102, `%d`, assert **`(dsp_codec_id<NV_AUDIO_DSP_CODEC_MAX)`**, 0x02300103, 0x0423542C, 0x02300104); walker fn at **0x87764** iterates a 12-byte-stride table (6 iters), compares on `[entry+8]`/`[entry+20]`, returns `[entry]`/`[entry+12]`; not-found → 6 | **yes — this IS the ARM→DSP codec interface**: the id keys DSP-side codec config; the 0x0230010x values are DSP-side/API addresses (see §1 UNKNOWN-1 / NV §4) |
| 12 | **0x0B06A4** VBC | `__vbc_dma_da_chn_en`/`__vbc_dma_ad_chn_en` (`VBDALDMA_EN` 1<<13, `VBDARDMA_EN` 1<<14, `VBADLDMA_EN` 1<<11, `VBADRDMA_EN` 1<<12); FIFO ctl = **`VBDABUFFDTA` = 0x82003018** (`ARM_VBC_BASE` 0x82003000 + 0x18); buffer-size asserts match the dump strings; `__vbc_enable/disable` write `VBENABLE` 1<<15; data via `VBDAL/VBDAR` = 0x82003000/04 | 4× `_vbc_en_op` on `VBDABUFFDTA` 0x82003018 with DMA-EN bits (1<<13/14 DA, 1<<11/12 AD); APB power-half 1<<18 into 0x8B000060/64 (pool 0x8B000040 @0xB0A64, delay(10)); FIFO ctl pool **0x82003018** @0xB0A18 | **yes — VBC is ARM/DSP-switchable**: `__vbc_arm_acc_switch` → `vb_switch_arm_ctl` writes `ARM_VB_ACC` on `APB_PERI_CTL0`; `VBC_CTL_BY_DSP = 0x80000000` gates the DSP-owned data path |

### 2. Reconciliations and flags

**R1 — the ADI helpers are byte-identical to the SDK's `adi_phy_v5.c`.** Pool
`0x303E4` = `0x1FFF0000` = the SDK's `ADI_ARM_RD_ADDR_MASK`; the poll/assert
sequence (bit8 → 0x18 write → bit31 → index-echo → `&0xFFFF`) matches
`ADI_Analogdie_reg_read` line-for-line, and the write (bit9 FIFO-full poll →
direct `REG32(addr)=data`) matches `ADI_Analogdie_reg_write` (the dump's string
pool at 0x303BC+ carries the SDK's exact filename). **This validates the
"direct store, no mailbox command" write path.**

**R2 — PA-mute: the 0x600 ownership dance is the SDK's
`sprd_codec_switch_arm_ctl`, the mute bit itself is SC6530C-specific.**
`0x8B0001C4` = `APB_PERI_CTL0`; bits 9-10 = `CLK_AUD_ARM_CTRL`|`AUD_CTRL_SEL`.
The dump folds that switch into the mute path; the SDK does it at codec
open/close instead. The mute **bit** 0x82001290 (= `audif_otptmr_ctl` +0x10)
has NO SDK writer — the SDK mutes by writing gain 0 to dcgr3. **Flag for the
diff:** the B310E driver's `audio_pa_mute` writes 0x200 (`CLK_AUD_ARM_CTRL`
only); the dump ORs **0x600** (both bits) — verify which the stock path really
leaves set.

**R3 — share-mem base (UNKNOWN-1): the dump does NOT resolve it, but the SDK's
SC6530-class default is 0x10000000.** The dump's own DSP downloader (string
`layer1_dsp_download.c` @0x7480C; regions 0x73E00/0x74800) contains **no
0x30000000 or 0x10000000 literal** — its pool values are 0x00090001 / 0x000DC78C
/ 0x65300000 (string data); the base comes from a runtime param struct
(`dsp_download_param.pdata`, assert @0x74980). The `dsp_codec` region's
0x0230010x values are **DSP-side codec-config table entries**, not an ARM
share-mem base. SDK: the v5 PHY returns `0x30000000` ONLY for
SC6531EFM/UWS6121E, else `0x10000000`; the v1 PHY hardcodes `0x10000000`;
`DSP_SIDE_START_ADDR = 0x10000000`; `SHARE_MEM_BEGIN = 0x30000000` is
SC6531EFM/UWS6121E-only. **Verdict: treat the SC6530C share-mem base as
0x10000000 (SDK default for the SC6530-class) with 0x30000000 still mapped in
the fake** — the real chip's value is runtime-supplied, so a fake must answer
either. Do NOT hardcode one. This is the resolution of UNKNOWN-1.

**R4 — the 0x06A14C family is an OLD-generation `audio_hal.c` absent from this
SDK.** The dump strings `audio_hal.c` / "Register Update Codec LDO VOL
Failed!" / `_WAIT_DSP_EVENT` are not present in any SDK source or prebuilt
library — the dump's SDK generation predates this snapshot. The register/bit
semantics map cleanly onto the v0 codec functions (rows 8-10 cite the current
equivalents), but **no exact name match is possible** — rows are
UNKNOWN-by-name, register evidence intact.

**R5 — VBC row confirms the ARM-vs-DSP VBC switch.** The DMA-enable masks, the
0x82003018 FIFO-ctl reg, and the `ARM_VB_ACC` ownership switch all line up with
the SDK's vbc PHY + DAI (`VBC_CTL_BY_DSP = 0x80000000`). The 1<<18 APB power
half (0x8B000060/64) has no SDK counterpart — SC6530C-specific device gate
(same pattern as PA's 0x200000/0x10000000 halves).

---

## NV

### 0. Scope, method and key finding

This section resolves the **NV (non-volatile) path for the stock audio/DSP
configuration**: the `NV_AUDIO_DSP_CODEC_*` / `dsp_codec_id` read sites in the
dump, the NV partition layout, the **actual NV payload bytes the B310E ships**,
and where the OS consumes them.

**Method.** All payload bytes were read **directly from `dump_firmware.bin`**
(16 MB, gitignored — a dump of the 8 MB NOR XIP flash **replicated twice**,
verified `dump[0..8M) == dump[8M..16M)`). File offset == XIP virtual address in
the first 8 MB copy. Code regions were disassembled with
`arm-none-eabi-objdump -D -b binary -m arm -M force-thumb` (Thumb-1).
**Caveat:** the ARM926EJ-S is ARMv5TE Thumb-1-only, but objdump decodes every
`0xF0xx xxxx` 32-bit pair as a Thumb-2 `bl`/`blx`. The real encoding is a
Thumb-1 **BLX to ARM state**, whose target differs from objdump's — do not
treat any `blx 0x3xxxx` target in a `.dis` file as the runtime target (only the
*role* is established, from the call-site structure).

**Key finding:** the B310E ships its DSP audio NV as **named mode-parameter
blocks embedded in the DSP_CODE flash partition at `0x680000`–`0x68C000`** (NOT
in the FS partition — `0x690000` is erased 0xFF). The consumer is the compiled
`audio_nv_dsp.c` module: `AUDIONVDSP_InitModeManager` loads the blocks into a
RAM mode list (head `0x0423572C`), and
`AUDIONVDSP_GetAudioDspCodecInfoFromRam(dsp_codec_id)` (the walker, fn @
**0x87764**) returns the per-mode **`audio_dsp_info`** token — the
`0x0230010x` family — the ARM→DSP codec-config handoff (§4).

### 1. The read sites — the compiled `audio_nv_dsp.c` module in the dump

The region `0x87660`–`0x87AC0` is the **data + code of the compiled
`audio_nv_dsp.c` module** (filename string `audio_nv_dsp` @ **0x87684**). The
trace-string pool @ **0xCC2C0–0xCC6C0** catalogs every function
(`audio_nv_dsp.c:AUDIONVDSP_<fn>:...`): `InitModeManager` @0xCC2E0/0xCC320/
0xCC380, **`GetAudioDspCodecInfoFromRam`** @0xCC3C0 (@0xCC440 success trace
`audio_dsp_info:0x%x`), `SetAudioDspCodecInfoToRam` @0xCC490/0xCC4E0,
`GetAudioDspCodecInfoFromFlash` @0xCC540, `SetAudioDspCodecInfoToFlash`
@0xCC590, `ReadModeParamFromFlash` @0xCC5E0, `WriteModeParamToFlash`
@0xCC640/0xCC690. Runtime data anchors: `0x0423571C` @ 0x87670 (codec struct
base) and **`0x0423572C`** @ 0x8767C-area/0x8775C/0x87A78 — the RAM **mode-list
head (`spAudioNvDspModeListHead`)** the walker iterates.

**The assert string (the `NV_AUDIO_DSP_CODEC_E` smoking gun), verified bytes @
0x87730:** `(dsp_codec_id<NV_AUDIO_DSP_CODEC_MAX)` (37 chars + NUL; the `%d`
format is at 0x8772C). This is the **assert on the enum id** — the id's type
and MAX bound come from an `NV_AUDIO_DSP_CODEC_E` enum that is, exactly as in
the SDK, **used but not defined in-tree**.

**The walker fn @ 0x87764 =
`AUDIONVDSP_GetAudioDspCodecInfoFromRam(dsp_codec_id)`.** Structure (Thumb-1,
verified bytes): **6 iterations** (r4 = 0,2,4,6,8,10 until `r4 == 12`),
**12-byte effective stride** (`r4*3*4`), each entry holding **two (key, info)
pairs**: `entry[+0]`/`entry[+8]` (key A) and `entry[+12]`/`entry[+20]` (key B).
On a key match it returns the sibling word — `entry[+0]` or `entry[+12]` — the
**`audio_dsp_info`** (per the success trace `audio_dsp_info:0x%x`, @0xCC470).
Not-found → `return 6`. The compare helper is called twice per entry; its own
body is **UNKNOWN** (Thumb-1 BLX caveat, UNKNOWN-N3). `id == 0` short-circuits
to the not-found trace.

**The compile-time mode→info table @ 0x876E0–0x87764** (verified bytes):
DSP-side codec-config tokens `0x023000F4..F6, 0x023000FB, 0x023000EE,
0x023000FC..FD, 0x023000FE..FF, 0x02300100..0x02300104` interleaved with the
short mode-name strings `"ds"`, `"dsd"`, `"ddds"`, `"dd"`, `"dsdd"` and the
runtime struct pointers `0x0423571C`/`0x0423572C`. A byte-search for each
`0x0230010x` value finds it ONLY here and in the +0x100000 mirror copy — they
are never embedded as literals elsewhere (they flow out of the walker). Because
the low nibbles are nonzero (`0x01..0x04`), they are **tokens/parameter IDs in
DSP Z-space, not 4-aligned addresses** (exact DSP-side meaning UNKNOWN-N4).

### 2. NV partition layout

Verified directly in `dump_firmware.bin` (16 MB = 2× 8 MB NOR mirror; all
offsets are XIP addresses in the first copy):

| Offset | Partition / content | State in the B310E dump |
|---|---|---|
| `0x000000`–… | boot/OS/CAPN/resource code (`tres0001` ends `0x568318`) | firmware |
| `0x570000`–`0x68C000` | **`DSP_CODE`** partition (DSP firmware the ARM downloader loads) | firmware |
| **`0x680000`–`0x68C000`** | **DSP audio NV blocks** (named mode-parameter blocks, §3) | **present** (the shipped NV values) |
| `0x68C000` | production/calibration block: serial, `"DOWNLOAD"`, `"WRITESN"`, `"CFT"`, `"ANTENNA"`, `"IMEI"`, `"MobileTV"` | present |
| `0x690000`–`0x6A0000` | **FS partition** | **erased: 0xFF everywhere**, except the end signature `… FF FF AA 55` @ **`0x69FFFE`** |
| `0x6A0000`–`0x800000` | user-data area (contains **FAT12** boot sectors with OEM `"SPRD"` @ **`0x714632`** and **`0x724B30`**) | present |
| `0x800000`–`0x1000000` | exact mirror of `0x000000`–`0x800000` | duplicate |

**Consequence:** the DSP audio NV **is not in the FS/NV filesystem** — it is
factory data baked into the DSP_CODE region. XIP reads of `0x680000+` are plain
NOR-RAM reads (already modeled in the emulator), so
`AUDIONVDSP_InitModeManager`'s flash read is "free". The erased FS partition
explains why the OS must (and does) fall back to the DSP_CODE-embedded blocks:
**the NV VALUES the B310E ships are the `0x680000` blocks, quoted in §3.**

### 3. The actual NV payload bytes the B310E ships (verified hexdumps)

The region is organized as **datasets**: a 16-byte dataset header followed by a
run of named blocks. Dataset 1 (verified boundaries):

```
0x680000  61 5C B3 0D  06 00 02 00  0C 00 00 00  07 00 70 23   a\............p#
0x680010  48 65 61 64 73 65 74 00 00 00 00 00 00 00 00 00   Headset.........   <- block 0 name
0x680020  2A E0 00 00  11 00 00 00  00 00 03 00  02 00 F3 0F   *...............   <- block 0 payload
0x680030  00 00 04 00  00 00 08 00  05 00 01 00  A9 36 CD 3F   .............6.?
0x680040  6D F9 76 0D  82 1A 88 E7  3F 03 76 0D  9C 20 34 C0   m.v.....?.v.. 4.
0x680050  49 03 00 10  00 10 00 10  00 10 74 32  36 3F 00 FE   I.........t26?..
0x680060  7F 16 82 1A  C2 D2 61 FE  3D 19 11 11  00 01 01 81   ......a.=.......
0x680070  01 01 07 02  00 00 E2 FF  03 00 00 00  02 00 00 00   ................
0x680080  16 00 01 00  00 00 40 00  00 00 00 00  B0 04 50 00   ......@.......P.
0x680090  B8 0B 00 00  00 00 00 02  00 04 E0 2E  28 23 70 17   ............(#p.
0x6800A0  B8 0B 0A 00  20 40 00 00  A9 36 41 2D  40 00 10 00   .... @...6A-@...
```

**Dataset header decode** (`0x680000`, 16 bytes): `0x0DB35C61` (u32 —
magic/checksum), `0x0006`/`0x0002` (u16 fields), `0x0000000C` (**u32 block
count = 12**), `0x0007` (u16 — version), **`0x2370` (u16 — total data size =
12 × 0x2F4 = 9072)**. The block stride **`0x2F4` = 756 bytes** is measured
between consecutive block names (`0x680010` → `0x680304` → `0x6805F8` →
`0x6808EC` …); each block = `name[16]` + a ~`0x2E4`-byte DSP audio parameter
struct. Slight stride variance appears at later blocks (0x2F8/0x5E4/0x300) —
the per-mode payload sizes are not perfectly uniform (UNKNOWN-N1).

**Blocks captured in dataset 1** (name @ offset): `Headset` @0x680010,
`Handset` @0x680304, `Handsfree` @0x6805F8, `MP4HFTR` @0x6808EC,
`MP4HFTRHeadset` @0x680BE4, `RecordHeadset` @0x6811C8, `BTHS` @0x6814BC,
`LoopBHeadset` @0x6817B0, `LoopBHandset` @0x681AA4, `LoopBHandfree` @0x681D98,
`BTHSNREC` @0x68208C (11 of the 12; one name is a form the scan did not
capture). Further datasets repeat the same names at higher offsets (`Headset`
@0x68238C, `Handset` @0x682F2C, …).

**The Handset block (block 1), verified @ 0x680304:**

```
0x680304  48 61 6E 64 73 65 74 00 00 00 00 00 00 00 00 00   Handset.........   <- name[16]
0x680314  26 C0 00 00  01 00 00 00  00 00 03 00  02 00 03 0F   &...............   <- payload
0x680324  07 00 07 00  00 00 08 00  03 00 01 00  7F 2E CD 3F   ...............?
0x680334  AA FD FF 0B  82 1A 9E 25  03 FC 00 10  47 F3 34 C0   .......%....G.4.
0x680344  31 FB 1E 14  00 10 00 10  00 10 C3 2A  0B 3C 5D FE  1..........*.<].
0x680354  24 14 82 1A  E2 C5 00 00  73 1C 10 00  00 00 00 00   $.......s.......
```

Payload-shape observations (both blocks): the leading halfwords read as `u16
{0xE02A|0xC026, 0x0011|0x0001, 0x0000, 0x0300, 0x0002, 0x0FF3|0x0F03, …}` — a
mixture of **u16 gain/count fields** and **u32 filter/coefficient words** (e.g.
the `00 00 08 00` u16 pair at `0x680034` and `0x680328`). The `0x82 1A` byte
pairs recur (`0x680042`, `0x680066`, `0x680336`, `0x680354`) — a repeated
per-section marker. **These are the DSP's voice/playback parameter structs**;
the exact field map is NOT in the SDK snapshot (`audio_nv_dsp.c` and its
`AUDIO_NV_DSP_MODE_STRUCT_T` header are absent — only the size-check
`param_len == sizeof(AUDIO_NV_DSP_MODE_STRUCT_T)*2` survives) → **UNKNOWN-N2**.

Every byte above was read directly from `dump_firmware.bin` at the stated
offset (spot-verified byte-for-byte against the extracted `dsp_codec_full.bin`,
same content from `0x87760`).

### 4. Where the OS consumes them

**SDK path (the name-based NV read):** `NV_DEV_GetDspAudioNv_Name` resolves the
device mode to an `NV_AUDIO_E` id (`_LAYER1_GetDspAudioNvId` →
Handset/Headset/Handsfree/BTHS/…), maps it to a mode **name**, then reads the
name's parameter block from flash. `NV_DEV_GetDspCodecNv_Name` (the
`NV_AUDIO_DSP_CODEC_E` variant) is `#if 0`'d out in this SDK — the
`dsp_codec_id` enum is used by the `NV_DEV_*_GetDspCodecId` family and by
`audio_nv_dsp.c`'s `AUDIONVDSP_GetAudioModeName(dsp_codec_id)`. The DSP NV
struct is `AUDIO_NV_DSP_MODE_INFO_T` = `{ uint8 ucModeName[16];
AUDIO_NV_DSP_MODE_STRUCT_T tAudioNvDspModeStruct; }` — the 16-byte name matches
the dump's block name field exactly.

**Dump path (the compiled consumer):**
1. `AUDIONVDSP_InitModeManager` (traces @0xCC2E0–0xCC380) — "Please download new
   NVItem !" on failure — reads the `0x680000` blocks into the RAM mode list
   (`spAudioNvDspModeListHead` = **`0x0423572C`**) at audio-init time.
2. `AUDIONVDSP_GetAudioDspCodecInfoFromRam(dsp_codec_id)` = walker @ 0x87764 —
   asserts `dsp_codec_id < NV_AUDIO_DSP_CODEC_MAX`, walks the RAM list, returns
   the **`audio_dsp_info`** token (`0x0230010x`, e.g. `0x02300101` for the first
   id).
3. The caller (the ARM→DSP codec handoff — UNKNOWN-7's concrete shape) takes
   that DSP-side token and configures the DSP codec with it. The token values
   appear nowhere else as literals, so **all consumption is through this
   walker**; the DSP-side interpretation of the `0x0230010x` space is part of
   the DSP firmware (DSP_CODE, `0x570000+`), not this SDK.

### 5. UNKNOWN list (NV-specific)

- **UNKNOWN-N1 (per-block size variance):** dataset-1 blocks are mostly `0x2F4`
  bytes but later blocks drift (`0x2F8`/`0x5E4`/`0x300` strides). The dataset
  header's `0x2370` total (= 12 × 0x2F4) and count `0x0C` are consistent, so the
  variance is in the per-mode payload size, not the count.
- **UNKNOWN-N2 (`AUDIO_NV_DSP_MODE_STRUCT_T` field map):** the payload's
  per-field meaning (gains vs filter coefficients) is **NOT decoded** — neither
  `audio_nv_dsp.c` nor its header exists in the SDK snapshot; only the size-check
  and name length are evidenced. Do NOT guess fields; treat the blocks as opaque
  parameter blobs.
- **UNKNOWN-N3 (the compare/helper fns):** the walker's `<compare fn>` and
  `<trace fn>` targets are unrecoverable with objdump's Thumb-2 mis-decode
  (§0 caveat); their *roles* are established structurally (returns 0 on key
  match; prints the trace). Their true addresses need a Thumb-1-aware decoder or
  the SC6530 TRM.
- **UNKNOWN-N4 (`0x0230010x` token semantics):** DSP-side codec-config tokens;
  non-aligned (low nibbles set) so not plain addresses. Exact DSP interpretation
  lives in the DSP_CODE firmware.
- **UNKNOWN-N5 (second-and-later datasets):** blocks at `0x682380+` repeat the
  same names with different headers (`0x682380: A9 01 02 00 05 02 00 00 AA 01 10
  1D …`) — presumably per-DSP-application copies (LAYER1/RECORD/MP4RECORD/
  LOOPBACK), not decoded here.

### 6. EQ_* parameter tables — dead data (final verdict)

The EFS NV items **439** (`EQSetInfo`, 0x1B7) / **440** (`untunableEQ`, 0x1B8) /
**441** (`tunableEQ`, 0x1B9) hold 16 EQ records — `EQ_Headset`, `EQ_Headfree`,
`EQ_Handset`, `EQ_Handsfree`, `EQ_BTHS` (+ `_1`, `_2` copies of each) at
`0x6840A8`–`0x68608C`, **stride exactly 0x220**, plus the `EQ_Tunable` custom
slot @ `0x68608C` (0x178 B). **Four byte-identical copies**: `0x6840A8`
(DSP-partition factory default) + user-data EFS sectors `0x6B1D40` / `0x7F4F02`
/ `0x7FCA64`. Copy C @ `0x7F4F02` is UNALIGNED (+2) and straddles two 4 KB
sectors — patching it takes 9 RMW sectors, not 8.

Unit scalings: `fo = raw Hz`, `q = raw/512`, `boostdB = s16 × 0.1 dB`,
`gaindB = s16 × 0.1 dB` (example raw s16: **+10.0 dB = 100 = 0x0064**, **+6.0 =
60 = 0x003C**, **+3.0 = 30 = 0x001E**, **+8.0 = 80 = 0x0050**). Hard cap:
`boostdB` ≤ +10.0 dB. 8-band layout `AUDIO_ENHA_EQ_STRUCT_T`:
`para_name[16] | eq_control u16 (bit15 = 8-band switch) | eq_modes[6] ×
(agc_in_gain u16, band_control u16 (bit15..8 = filter_sw_1..8, bit1 hi-shelf,
bit0 lo-shelf), eq_band[8] × {fo u16, q u16, boostdB s16, gaindB s16}) |
externdArray[59]` → record = 544 = **0x220**. The tunable twin
(`AUDIO_ENHA_TUNABLE_EQ_STRUCT_T`, 0x178 B) adds `level_step` and
current-vs-default boost arrays.

**Final verdict (2026-08-26): the EQ_* tables are dead data.** An exhaustive
static search found **ZERO code references** to the item IDs 439/440/441 as
32-bit literals, the dataset size 0x1FE0, the stride 0x220, the dataset
addresses, or the `aud_enha`/`eq_exp`/`GetEqPara`/`SetEqMode` API strings; of
the 57 `EFS_NV_ReadItem` callers the audio modules read only items
425/426/428/429/430/432 (the dsp_codec mode blocks). The music playback stream
is FLAT — no EQ on the ARM side, no user-reachable EQ UI; the shipped
`eq_control` bit15 (8-band switch) is 0 in `EQ_Headset`. Empirically no-op:
the all-modes/all-copies bass patch produced **NO CHANGE AT ALL** on the
3.5 mm jack; the mic record-gain patch produced **NO AUDIBLE CHANGE**; the
HPF-clear successor path also produced **NO audible effect**. The bass cut is
applied AFTER the NV parameters in the **DSP/VBC data path** (VBC DA HPF
`VBDAHP_EN` bit10 @ `0x82003048`, HPCOEF0-42 @ `0x82003100+`, DSP-programmed,
0 ARM hits). The fix is the custom-OS software EQ.
