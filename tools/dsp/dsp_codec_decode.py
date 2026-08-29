#!/usr/bin/env python3
"""dsp_codec_decode.py - T11 (b310e-audio-eq-tune): decode the B310E's
dsp_codec Record/RecordHeadset NV blocks (mic-path params).

The blocks live in dump_firmware.bin's DSP partition NV-data island, dataset-1
@0x680000 (12 blocks, stride 0x2F4; section map:
.omo/dumpmine/dsp/dsp-section-map.md). Each block = name[16] + a 370-u16
(740-byte) old-generation audio parameter struct. The field map below was
recovered from the SC6531EFM AEC NV module file (tools/mocor-zw217/common/
nv_parameters/audio/uis8910ff_zxf/audio_sc6531efm_AEC.nvm, a MODULE=audio
STRUCT item tree: AudioStructure = 88 named u16 + extend[142] + arm_volume[10]
+ dsp_volume[10] + extend2[120] = 370 u16 = 740 B) and VERIFIED against the
B310E blocks byte-for-byte (Record: 330/370 u16 exact matches, RecordHeadset:
338/370; T11 evidence). The dump is the ground truth -- every block is
name-anchored (16-byte ASCII name at the block start) and the parse FAILS if
a name does not match.

Field index i -> payload byte offset 2*i (block + 16 + 2*i).

Usage:
  python tools\\dsp\\dsp_codec_decode.py --record Record
  python tools\\dsp\\dsp_codec_decode.py --record RecordHeadset
  python tools\\dsp\\dsp_codec_decode.py --record Record --format csv
  python tools\\dsp\\dsp_codec_decode.py --offset 0x680ED4      # name-anchored
  python tools\\dsp\\dsp_codec_decode.py --all                  # all 12 ds1 blocks
  python tools\\dsp\\dsp_codec_decode.py --offset 0x680ED0      # must exit 1

Exit 0: all requested blocks decoded, every name matched.
Exit 1: name-mismatch / read error / stale dump.
Exit 2: usage error.
"""

import argparse
import hashlib
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DUMP = os.path.join(REPO, "dump_firmware.bin")
DUMP_SHA256 = "2B62CE1BFBF9C43F80C917BDE4E1130B1A3D24E014DDF0672F138598B6FEF0E6"

# dataset-1 geometry (T1-verified, name-anchored)
DS1_BASE = 0x680000
DS1_COUNT = 12
BLOCK_STRIDE = 0x2F4          # name[16] + 370-u16 struct = 756 B, exact
PAYLOAD_U16 = 370             # 740 B
PAYLOAD_BYTES = 740

DS1_NAMES = [
    "Headset", "Handset", "Handsfree", "MP4HFTR", "MP4HFTRHeadset",
    "Record", "RecordHeadset", "BTHS", "LoopBHeadset", "LoopBHandset",
    "LoopBHandfree", "BTHSNREC",
]

# --------------------------------------------------------------------------
# field map: (index, base_name, desc) -- recovered from the SC6531EFM AEC
# .nvm AudioStructure item tree (see module docstring); desc = the .nvm
# ITEM_DESC of the leaf (or the array head for extend/arm_volume/dsp_volume).
# Semantics of fields with empty desc are NOT asserted here.
# --------------------------------------------------------------------------
FIELDS = [
    (0, 'dl_DA_device_internal', ''), (1, 'dl_DA_device_external', ''),
    (2, 'ul_AD_device_internal', ''), (3, 'ul_AD_device_external', ''),
    (4, 'sidetone_switch', ''), (5, 'aec_switch', ''),
    (6, 'volume_mode', ''), (7, 'dl_PGA_gain_l', ''), (8, 'dl_PGA_gain_h', ''),
    (9, 'ul_PGA_gain_l', ''), (10, 'ul_PGA_gain_h', ''),
    (11, 'Sample_rate', ''), (12, 'dl_DP_gain', ''), (13, 'dl_DP_attenu', ''),
    (14, 'dl_EQ_bass_alpha', ''), (15, 'dl_EQ_bass_beta', ''),
    (16, 'dl_EQ_bass_gama', ''), (17, 'dl_EQ_bass_gain', ''),
    (18, 'dl_EQ_mid_alpha', ''), (19, 'dl_EQ_mid_beta', ''),
    (20, 'dl_EQ_mid_gama', ''), (21, 'dl_EQ_mid_gain', ''),
    (22, 'dl_EQ_treble_alpha', ''), (23, 'dl_EQ_treble_beta', ''),
    (24, 'dl_EQ_treble_gama', ''), (25, 'dl_EQ_treble_gain', ''),
    (26, 'digital_sidetone_gain', ''), (27, 'ul_DP_gain', ''),
    (28, 'ul_DP_attenu', ''), (29, 'ul_EQ_bass_alpha', ''),
    (30, 'ul_EQ_bass_beta', ''), (31, 'ul_EQ_bass_gama', ''),
    (32, 'ul_EQ_bass_gain', ''), (33, 'ul_EQ_treble_alpha', ''),
    (34, 'ul_EQ_treble_beta', ''), (35, 'ul_EQ_treble_gama', ''),
    (36, 'ul_EQ_treble_gain', ''), (37, 'dl_POP_switch', ''),
    (38, 'dl_AGC_switch', 'bit15: xagc SW; bit8: dl agc lpf on/off'),
    (39, 'dl_EQ_AGC_switch', 'ul iir hpf/dl iir hpf'),
    (40, 'DP_switch', ''), (41, 'dl_EQ_switch', ''),
    (42, 'Dl_agc_Rsv0', ''), (43, 'Dl_agc_Rsv1', ''),
    (44, 'ul_EQ_switch', ''), (45, 'dl_DA_pop_switch', ''),
    (46, 'ul_FIR_HPF_enable', ''), (47, 'aec_enable', ''),
    (48, 'pdelay', ''), (49, 'dl_ref_HPF_enable', ''),
    (50, 'decor_filter_enable', ''), (51, 'fir_taps', ''),
    (52, 'Aec_frozen', ''), (53, 'coeff_frozen', ''),
    (54, 'DT_dect_threshold', ''), (55, 'Dt_noise_floor', ''),
    (56, 'step_size', ''), (57, 'coeff_norm_shift', ''),
    (58, 'SA_ctrl', ''), (59, 'send_attenu_in_dt', ''),
    (60, 'send_attenu_in_rv', ''), (61, 'send_threshold', ''),
    (62, 'r2dt_threshold', ''), (63, 's2dt_threshold', ''),
    (64, 'recv_threshold', ''), (65, 'Bn40', ''),
    (66, 'Sa_AR', ''), (67, 'ng_select', ''), (68, 'alpha_distor', ''),
    (69, 'beta_distor', ''), (70, 'ul_ng_plk_wPyy_a', ''),
    (71, 'ul_ng_plk_wPyy_n', ''), (72, 'ul_ng_plk_holdc', ''),
    (73, 'ul_ng_plk_ATT', ''), (74, 'ul_ng_clk_wPyy_a', ''),
    (75, 'ul_ng_clk_wPyy_n', ''), (76, 'ul_ng_clk_holdc', ''),
    (77, 'ul_ng_clk_ATT', ''), (78, 'dl_ng_plk_wPyy_a', ''),
    (79, 'dl_ng_plk_wPyy_n', ''), (80, 'dl_ng_plk_holdc', ''),
    (81, 'dl_ng_plk_ATT', ''), (82, 'dl_ng_clk_wPyy_a', ''),
    (83, 'dl_ng_clk_wPyy_n', ''), (84, 'dl_ng_clk_holdc', ''),
    (85, 'dl_ng_clk_ATT', ''), (86, 'DA_limit', ''), (87, 'reserved', ''),
    (88, 'extend', 'FIR eq/voice coefficients array [142]'),
    (230, 'arm_volume', 'Arm volume gain Parameter [10]'),
    (240, 'dsp_volume', 'Dsp volume gain Parameter [10]'),
    (250, 'extend2', 'NR/NS/DRC/VAQ parameter array [120]'),
]
# extend2 leaf descriptions (the .nvm ITEM_DESC of the Record-mode leaves)
EXTEND2_DESC = {
    0: 'dl_nr,noise_est_complex_vad,SW', 6: 'ul_no_vad_cnt_thd',
    7: 'ul_min_psne', 8: 'ul_max_temp_uamn', 9: 'ul_vad_thd',
    10: 'ul_active_thd', 11: 'ul_noise_thd', 12: 'ul_max_psne',
    13: 'ul_voice_burst', 14: 'ul_noise_tail', 15: 'ul_rfilter_delay',
    16: 'ul_rfilter_tail', 17: 'ul_rfilter', 18: 'ul_dgain',
    19: 'ul_sim_M', 20: 'ul_sim_fac', 21: 'ul_dac_limit',
    22: 'ul_ns_factor', 23: 'ul_ns_limit', 24: 'ul_ns_up_factor',
    25: 'ul_dis_snr_thd', 26: 'ul_dis_band_1k', 27: 'ul_dis_limit',
    28: 'ul_drc_thd', 29: 'ul_drc_ratio', 30: 'ul_drc_dstep',
    31: 'ul_drc_ustep', 32: 'ul_drc_cnt', 33: 'ul_amb_release',
    34: 'ul_amb_attack', 35: 'ul_amb_ndefault', 36: 'ul_echo_ns_limit',
    37: 'dl_no_vad_cnt_thd', 38: 'dl_min_psne', 39: 'dl_max_temp_uamn',
    40: 'dl_vad_thd', 41: 'dl_active_thd', 42: 'dl_noise_thd',
    43: 'dl_max_psne', 44: 'dl_voice_burst', 45: 'dl_noise_tail',
    46: 'dl_rfilter_delay', 47: 'dl_rfilter_tail', 48: 'dl_rfilter',
    49: 'dl_dgain', 50: 'dl_sim_M', 51: 'dl_sim_fac', 52: 'dl_dac_limit',
    53: 'dl_ns_factor', 54: 'dl_dis_limit', 55: 'dl_ns_up_factor',
    56: 'dl_dis_snr_thd', 57: 'dl_dis_band_1k', 58: 'dl_dis_limit',
    59: 'dl_drc_thd', 60: 'dl_drc_ratio', 61: 'dl_drc_dstep',
    62: 'dl_drc_ustep', 63: 'dl_drc_cnt', 68: 'ul_dgain vol 1',
    69: 'ul_dgain vol 2', 70: 'ul_dgain vol 3', 71: 'ul_dgain vol 4',
    72: 'ul_dgain vol 5', 73: 'ul_dgain vol 6', 74: 'ul_dgain vol 7',
    75: 'ul_dgain vol 8', 76: 'ul_dgain vol 9', 77: 'DL1 PGA vol 1',
    78: 'DL1 PGA vol 2', 79: 'DL1 PGA vol 3', 80: 'DL1 PGA vol 4',
    81: 'DL1 PGA vol 5', 82: 'DL1 PGA vol 6', 83: 'DL1 PGA vol 7',
    84: 'DL1 PGA vol 8', 85: 'DL1 PGA vol 9', 86: 'dl_drc_dgain vol 1',
    87: 'dl_drc_dgain vol 2', 88: 'dl_drc_dgain vol 3',
    89: 'dl_drc_dgain vol 4', 90: 'dl_drc_dgain vol 5',
    91: 'dl_drc_dgain vol 6', 92: 'dl_drc_dgain vol 7',
    93: 'dl_drc_dgain vol 8', 94: 'dl_drc_dgain vol 9',
    95: 'audio_max_sa_limit', 96: 'ul_vaq_vol_buf[1]',
    97: 'ul_vaq_a_bn_set[0]', 98: 'ul_vaq_a_bn_set[1]',
    99: 'ul_vaq_a_tn_set[0]', 100: 'ul_vaq_a_tn_set[1]',
    101: 'ul_vaq_vol_buf[0]', 110: 'compatible SW; bit8: volume stragy',
    115: 'aec_compensate vol 6', 116: 'aec_compensate vol 7',
    117: 'aec_compensate vol 8', 118: 'aec_compensate vol 9',
}


def field_name(i):
    """Field base name + array index for the 370-slot struct."""
    if i < 88:
        return FIELDS[i][1]
    if i < 230:
        return "extend[%d]" % (i - 88)
    if i < 240:
        return "arm_volume[%d]" % (i - 230)
    if i < 250:
        return "dsp_volume[%d]" % (i - 240)
    return "extend2[%d]" % (i - 250)


def field_desc(i):
    if i >= 250:
        return EXTEND2_DESC.get(i - 250, "")
    for lo, hi in ((88, 230), (230, 240), (240, 250)):
        if lo <= i < hi:
            base = "extend" if i < 230 else ("arm_volume" if i < 240 else "dsp_volume")
            for idx, name, desc in FIELDS:
                if name == base:
                    return desc
    return FIELDS[i][2] if i < len(FIELDS) else ""


class BlockNameError(Exception):
    pass


def decode_name(dump, off):
    return bytes(dump[off:off + 16]).split(b"\0", 1)[0].decode("ascii", "replace")


def parse_block(dump, off, expected=None):
    """Name-anchored parse of one ds1 block; returns (name, [370 u16], off)."""
    name = decode_name(dump, off)
    if expected is not None and name != expected:
        raise BlockNameError(
            "block @0x%08X: name mismatch -- expected %r, read %r (16 bytes: %s)"
            % (off, expected, name, bytes(dump[off:off + 16]).hex(" ")))
    if not (name and all(32 <= c < 127 for c in dump[off:off + 16] if c)):
        raise BlockNameError("block @0x%08X: invalid name %r" % (off, name))
    pay = dump[off + 16:off + 16 + PAYLOAD_BYTES]
    if len(pay) < PAYLOAD_BYTES:
        raise BlockNameError("block @0x%08X: payload truncated (%d < %d)"
                             % (off, len(pay), PAYLOAD_BYTES))
    u16s = struct.unpack("<%dH" % PAYLOAD_U16, pay)
    return name, u16s, off


def print_block(dump, off, expected, fmt):
    name, u16s, off = parse_block(dump, off, expected)
    if fmt == "csv":
        print("block,name,offset,field,index,value_hex,desc")
        for i, v in enumerate(u16s):
            print("%s,%s,0x%X,%s,%d,0x%04X,%s" % (
                expected, name, off + 16 + 2 * i, field_name(i), i, v,
                field_desc(i)))
        return
    print("== %s @0x%08X (name-anchored) ==" % (expected, off))
    # summary: the mic-relevant named fields 0..87
    print("-- named fields (0..87) --")
    for i in range(88):
        d = field_desc(i)
        print("  %3d %-28s = 0x%04X%s" % (i, field_name(i), u16s[i],
                                           ("  (%s)" % d) if d else ""))
    print("-- arrays (88..369) --")
    for i in range(88, 370):
        d = field_desc(i)
        if d:
            print("  %3d %-28s = 0x%04X  (%s)" % (i, field_name(i), u16s[i], d))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Decode B310E dsp_codec Record blocks")
    ap.add_argument("--dump", default=DEFAULT_DUMP)
    ap.add_argument("--record", default=None,
                    help="decode one block by name (Record, RecordHeadset, ...)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--all", action="store_true", help="decode all 12 ds1 blocks")
    ap.add_argument("--format", choices=["table", "csv"], default="table")
    args = ap.parse_args(argv)

    try:
        dump = open(args.dump, "rb").read()
    except OSError as e:
        print("FATAL: cannot read dump %s: %s" % (args.dump, e), file=sys.stderr)
        return 2

    h = hashlib.sha256(dump).hexdigest().upper()
    if h != DUMP_SHA256:
        print("FATAL: dump SHA256 %s != expected %s (stale dump)" % (h, DUMP_SHA256),
              file=sys.stderr)
        return 1

    if args.record:
        if args.record not in DS1_NAMES:
            print("FATAL: unknown block %r (have %s)" % (args.record, DS1_NAMES),
                  file=sys.stderr)
            return 2
        off = DS1_BASE + 0x10 + DS1_NAMES.index(args.record) * BLOCK_STRIDE
        try:
            print_block(dump, off, args.record, args.format)
        except BlockNameError as e:
            print("DECODE FAIL: %s" % e, file=sys.stderr)
            return 1
        return 0

    if args.offset is not None:
        # find the block whose range contains the offset (name-anchored)
        for i, nm in enumerate(DS1_NAMES):
            b0 = DS1_BASE + 0x10 + i * BLOCK_STRIDE
            if b0 <= args.offset < b0 + BLOCK_STRIDE:
                try:
                    print_block(dump, b0, nm, args.format)
                except BlockNameError as e:
                    print("DECODE FAIL: %s" % e, file=sys.stderr)
                    return 1
                return 0
        print("DECODE FAIL: offset 0x%X not in any ds1 block range" % args.offset,
              file=sys.stderr)
        return 1

    if args.all:
        rc = 0
        for i, nm in enumerate(DS1_NAMES):
            off = DS1_BASE + 0x10 + i * BLOCK_STRIDE
            try:
                print_block(dump, off, nm, args.format)
            except BlockNameError as e:
                print("DECODE FAIL: %s" % e, file=sys.stderr)
                rc = 1
        return rc

    print("usage: --record NAME | --offset 0x.. | --all", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
