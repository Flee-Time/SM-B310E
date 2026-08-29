#!/usr/bin/env python3
"""eq_decode.py - T4 (b310e-audio-eq-tune): decode the B310E's 16 EQ_* NV
records into the 8-band EQ layout.

The records live in dump_firmware.bin's DSP partition NV-data island
(section map: .omo/dumpmine/dsp/dsp-section-map.md). Layouts recovered from
the leaked W217 SDK (aud_enha_exp.h, cited file:line in the task-4 evidence;
files fetched hash-pinned into the GITIGNORED tools/dsp/reference/ by
tools/dsp/fetch-reference.ps1). The dump is the ground truth -- every record
is name-anchored (16-byte ASCII name at the record start) and the parse
FAILS if a name does not match.

AUDIO_ENHA_EQ_STRUCT_T      (untunable EQ, "PACKED 272 words" = 0x220 B):
  para_name[16] | eq_control u16 | eq_modes[6] x ( agc_in_gain u16,
  band_control u16, eq_band[8] x {fo u16, q u16, boostdB s16, gaindB s16} )
  | externdArray[59] u16
  -> record = 16 + 2 + 6*(2+2+64) + 118 = 544 = 0x220
  band_control bit15..bit8 = filter_sw_1..8 (which bands are live),
  bit1 = high-shelf, bit0 = low-shelf. eq_control bit15 = 8-band switch.

AUDIO_ENHA_TUNABLE_EQ_STRUCT_T  (tunable EQ, "PACKED 188 words" = 0x178 B):
  para_name[16] | eq_control u16 | fo_array[8] | q_array[8] | level_n u16 |
  eq_modes[6] x ( agc_in_gain u16, band_control u16,
                   boostdB_default[8], boostdB_current[8] ) | externdArray[54]
  -> record = 16 + 376 = 392 = 0x188 (mini header 0x178 = struct size)

CONFIRMED unit scalings (empirical, evidence in task-4 file -- T7 must cite
THIS, not the SDK hints):
  fo       = raw Hz            (scale 1.0)   .nvm ITEM_DESC "central
             frequency" + literal 62/250/1000/4000/16000; EQ_Tunable
             fo_array byte-identical; EQ_Headset bands 12000/4000/600/50/100
  q        = raw / 512                       .nvm ITEM_DESC "for band width;
             scaled by 512"; EQ_Tunable q_array 273/453 byte-identical
  boostdB  = raw * 0.1 dB (s16)  0xFF4C = -180 = -18.0 dB byte-identical to
             .nvm EQ_Headset eq_mode_2 eq_band_1; only sane value range
  gaindB   = raw * 0.1 dB (s16)  0x001E = +30 = +3.0 dB byte-identical

Usage:
  python tools\\dsp\\eq_decode.py                          # primary table, all 16 records
  python tools\\dsp\\eq_decode.py --offset 0x6840A8 --count 16
  python tools\\dsp\\eq_decode.py --record EQ_Headset      # one record, table
  python tools\\dsp\\eq_decode.py --record EQ_Headset --format csv
  python tools\\dsp\\eq_decode.py --record EQ_Tunable
  python tools\\dsp\\eq_decode.py --region B               # copy B region
  python tools\\dsp\\eq_decode.py --offset 0x6840B0        # must exit 1 (name mismatch)

Exit 0: all requested records decoded, every name matched.
Exit 1: name-mismatch / read error / unknown record.
Exit 2: usage error.

Layout is data (adjustable): --layout selects a built-in layout by name
(untunable/tunable/auto) and --layout-json <file> loads a full override
({name, record_size, stride, ...}) so a different-generation struct can be
tuned without editing code.
"""

import argparse
import json
import os
import struct
import sys

# --------------------------------------------------------------------------
# repo / dump path resolution
# --------------------------------------------------------------------------
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DUMP = os.path.join(REPO, "dump_firmware.bin")

# --------------------------------------------------------------------------
# record geometry (T1-verified, name-anchored)
# --------------------------------------------------------------------------
PRIMARY_BASE = 0x6840A8
STRIDE = 0x220  # 15 untunable records, exact
TUNABLE_OFF = 0x68608C  # 4 bytes past the stride-perfect position 0x686088

# canonical 16-name table (the primary region order)
NAMES = [
    "EQ_Headset", "EQ_Headfree", "EQ_Handset", "EQ_Handsfree", "EQ_BTHS",
    "EQ_Headset_1", "EQ_Headfree_1", "EQ_Handset_1", "EQ_Handsfree_1", "EQ_BTHS_1",
    "EQ_Headset_2", "EQ_Headfree_2", "EQ_Handset_2", "EQ_Handsfree_2", "EQ_BTHS_2",
    "EQ_Tunable",
]

# AUD_ENHA_EQPARA_SET_E mapping (SDK aud_enha_exp.h) -- B310E record names
# match the headset/headfree/handset/handsfree/BTHS set + the *_1/*_2
# generations (untunableEQ sets EQ_Headset..EQ_Handsfree_1 per audio_eq.nvm).

# per-mode preset name inference: AUD_ENHA_EQMODE_SEL_E { OFF=0, REGULAR=1,
# CLASSIC=2, ODEUM=3, JAZZ=4, ROCK=5, SOFTROCK=6 } -- eq_modes[] indexed by
# enum value; eq_mode_1 = OFF (all-zero + band_control 0), eq_mode_6 = ROCK
# (the B310E Music Player preset names are Rock/Classic/Jazz per string
# mining). This is a documentation aid, not a decode output.
MODE_GUESS = {0: "OFF", 1: "REGULAR", 2: "CLASSIC", 3: "ODEUM", 4: "JAZZ", 5: "ROCK"}

# --------------------------------------------------------------------------
# layouts (adjustable constant tables; --layout-json overrides)
# --------------------------------------------------------------------------
LAYOUTS = {
    "untunable": {
        "name": "untunable",
        "record_size": 0x220,
        "stride": 0x220,
        "eq_control": 0x10,          # u16
        "mode_base": 0x12,           # first eq_mode agc_in_gain
        "mode_stride": 68,           # 2 + 2 + 8*8
        "band_base_delta": 4,        # band 0 offset inside a mode
        "band_stride": 8,
        "externd_offset": 0x1AA,
        "externd_count": 59,
        "kind": "bands",             # 4-field band struct
    },
    "tunable": {
        "name": "tunable",
        "record_size": 0x188,        # 16 name + 0x178 struct
        "stride": None,
        "eq_control": 0x10,          # u16 (bit15 8-band sw; bit9-0 level_step)
        "fo_array": 0x12,            # 8 x u16
        "q_array": 0x22,             # 8 x u16
        "level_n": 0x32,             # u16
        "mode_base": 0x34,
        "mode_stride": 36,           # 2 + 2 + 16 + 16
        "externd_offset": 0x10C,
        "externd_count": 54,
        "kind": "tunable",
    },
}


def get_layout(name, overrides=None):
    """Return the layout dict for a name, overlaying --layout-json values."""
    if name not in LAYOUTS:
        raise ValueError("unknown layout %r (have %s)" % (name, sorted(LAYOUTS)))
    lay = dict(LAYOUTS[name])
    if overrides:
        lay.update(overrides)
    return lay


# --------------------------------------------------------------------------
# decode primitives
# --------------------------------------------------------------------------
def decode_name(dump, off):
    return bytes(dump[off:off + 16]).split(b"\0", 1)[0].decode("ascii", "replace")


def band_of(mode_off, b, lay):
    return mode_off + lay["band_base_delta"] + b * lay["band_stride"]


def fmt_s16(v):
    return v - 0x10000 if v & 0x8000 else v


def parse_untunable(dump, off, lay):
    """Parse one AUDIO_ENHA_EQ_STRUCT_T record. Returns a dict."""
    rec = dump[off:off + lay["record_size"]]
    r = {
        "name": decode_name(dump, off),
        "eq_control": struct.unpack_from("<H", rec, lay["eq_control"])[0],
        "modes": [],
    }
    for m in range(6):
        mo = lay["mode_base"] + m * lay["mode_stride"]
        agc = struct.unpack_from("<H", rec, mo)[0]
        bctl = struct.unpack_from("<H", rec, mo + 2)[0]
        bands = []
        for b in range(8):
            bo = band_of(mo, b, lay)
            fo, q, boost, gain = struct.unpack_from("<4H", rec, bo)
            bands.append({
                "fo": fo, "q": q,
                "boostdB": fmt_s16(boost), "gaindB": fmt_s16(gain),
            })
        r["modes"].append({"agc_in_gain": agc, "band_control": bctl, "bands": bands})
    ext_off = lay["externd_offset"]
    r["externd"] = list(struct.unpack_from("<%dH" % lay["externd_count"], rec, ext_off))
    return r


def parse_tunable(dump, off, lay):
    rec = dump[off:off + lay["record_size"]]
    r = {
        "name": decode_name(dump, off),
        "eq_control": struct.unpack_from("<H", rec, lay["eq_control"])[0],
        "fo_array": list(struct.unpack_from("<8H", rec, lay["fo_array"])),
        "q_array": list(struct.unpack_from("<8H", rec, lay["q_array"])),
        "level_n": struct.unpack_from("<H", rec, lay["level_n"])[0],
        "modes": [],
    }
    for m in range(6):
        mo = lay["mode_base"] + m * lay["mode_stride"]
        agc = struct.unpack_from("<H", rec, mo)[0]
        bctl = struct.unpack_from("<H", rec, mo + 2)[0]
        bdef = [fmt_s16(v) for v in struct.unpack_from("<8H", rec, mo + 4)]
        bcur = [fmt_s16(v) for v in struct.unpack_from("<8H", rec, mo + 20)]
        r["modes"].append({
            "agc_in_gain": agc, "band_control": bctl,
            "boostdB_default": bdef, "boostdB_current": bcur,
        })
    r["externd"] = list(struct.unpack_from("<%dH" % lay["externd_count"], rec, lay["externd_offset"]))
    return r


def find_tunable(dump, region_base):
    """Name-anchored EQ_Tunable search within +-0x100 of the stride-perfect
    position (the primary is +4; copies vary). Returns offset or None."""
    stride_pos = region_base + 15 * STRIDE
    lo, hi = stride_pos - 0x100, min(stride_pos + 0x100, len(dump) - 16)
    for off in range(lo, hi):
        if dump[off:off + 10] == b"EQ_Tunable":
            return off
    return None


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------
def band_table_row(rec_name, m, b, band, enabled):
    return {
        "record": rec_name, "mode": m + 1, "mode_guess": MODE_GUESS.get(m, "?"),
        "band": b + 1, "enabled": 1 if enabled else 0,
        "fo_raw": band["fo"], "q_raw": band["q"],
        "boost_raw": band["boostdB"], "gain_raw": band["gaindB"],
        "fo_hz": band["fo"], "q_scaled": round(band["q"] / 512.0, 4),
        "boost_db": round(band["boostdB"] * 0.1, 2),
        "gain_db": round(band["gaindB"] * 0.1, 2),
    }


def csv_lines(rows, cols):
    yield ",".join(cols)
    for row in rows:
        yield ",".join(str(row[c]) for c in cols)


def print_table(rows, cols):
    # cols: (key, header, width, align)
    hdr = " ".join(h.ljust(w) for _, h, w, _ in cols)
    print(hdr)
    print("-" * len(hdr))
    for row in rows:
        cells = []
        for key, _h, w, align in cols:
            s = str(row[key])
            cells.append(s.rjust(w) if align == ">" else s.ljust(w))
        print(" ".join(cells))


def print_record(parsed, format_):
    if parsed["name"] == "EQ_Tunable":
        print_record_tunable(parsed, format_)
    else:
        print_record_untunable(parsed, format_)


def print_record_untunable(r, format_):
    name = r["name"]
    print("== %s  eq_control=0x%04X (bit15=8-band sw) ==" % (name, r["eq_control"]))
    if format_ == "csv":
        cols = ["record", "mode", "mode_guess", "band", "enabled", "fo_raw", "q_raw",
                "boost_raw", "gain_raw", "fo_hz", "q_scaled", "boost_db", "gain_db"]
        rows = []
        for m, mod in enumerate(r["modes"]):
            bctl = mod["band_control"]
            for b, band in enumerate(mod["bands"]):
                enabled = bool(bctl & (0x8000 >> b))
                rows.append(band_table_row(name, m, b, band, enabled))
        for line in csv_lines(rows, cols):
            print(line)
        return
    cols = [
        ("mode", "mode", 4, ">"), ("guess", "preset", 7, "<"),
        ("band", "band", 4, ">"), ("en", "en", 2, ">"),
        ("fo_hz", "fo Hz", 7, ">"), ("q_scaled", "q(=/512)", 9, ">"),
        ("boost_db", "boostdB", 8, ">"), ("gain_db", "gaindB", 7, ">"),
        ("fo_raw", "fo raw", 6, ">"), ("q_raw", "q raw", 6, ">"),
        ("boost_raw", "boost raw", 9, ">"), ("gain_raw", "gain raw", 8, ">"),
    ]

    def tabrow(m, b, band, enabled):
        r = band_table_row(name, m, b, band, enabled)
        return {
            "mode": r["mode"], "guess": r["mode_guess"], "band": r["band"],
            "en": r["enabled"], "fo_hz": r["fo_hz"], "q_scaled": r["q_scaled"],
            "boost_db": r["boost_db"], "gain_db": r["gain_db"],
            "fo_raw": r["fo_raw"], "q_raw": r["q_raw"],
            "boost_raw": r["boost_raw"], "gain_raw": r["gain_raw"],
        }

    rows = []
    for m, mod in enumerate(r["modes"]):
        bctl = mod["band_control"]
        if format_ == "compact" and bctl == 0 and not any(
                band["fo"] or band["q"] or band["boostdB"] or band["gaindB"]
                for band in mod["bands"]):
            rows.append({"mode": m + 1, "guess": MODE_GUESS.get(m, "?"),
                         "band": "-", "en": 0, "fo_hz": 0, "q_scaled": 0,
                         "boost_db": 0, "gain_db": 0, "fo_raw": 0, "q_raw": 0,
                         "boost_raw": 0, "gain_raw": 0})
            continue
        for b, band in enumerate(mod["bands"]):
            enabled = bool(bctl & (0x8000 >> b))
            rows.append(tabrow(m, b, band, enabled))
    print_table(rows, cols)
    print("  mode agc_in_gain/band_control:", ", ".join(
        "m%d=0x%04X/0x%04X" % (i + 1, mod["agc_in_gain"], mod["band_control"])
        for i, mod in enumerate(r["modes"])))


def print_record_tunable(r, format_):
    print("== EQ_Tunable  eq_control=0x%04X (bit15=8-band sw, bit9-0=level_step) ==" % r["eq_control"])
    print("  fo_array (Hz):   ", r["fo_array"])
    print("  q_array  (raw/512):", [round(x / 512.0, 3) for x in r["q_array"]], " raw:", r["q_array"])
    print("  level_n = %d  (level_step from eq_control = %d)" % (r["level_n"], r["eq_control"] & 0x3FF))
    if format_ == "csv":
        cols = ["record", "mode", "band", "boost_def_raw", "boost_cur_raw", "boost_def_db", "boost_cur_db"]
        rows = []
        for m, mod in enumerate(r["modes"]):
            for b in range(8):
                rows.append({
                    "record": "EQ_Tunable", "mode": m + 1, "band": b + 1,
                    "boost_def_raw": mod["boostdB_default"][b],
                    "boost_cur_raw": mod["boostdB_current"][b],
                    "boost_def_db": round(mod["boostdB_default"][b] * 0.1, 2),
                    "boost_cur_db": round(mod["boostdB_current"][b] * 0.1, 2),
                })
        for line in csv_lines(rows, cols):
            print(line)
        return
    for m, mod in enumerate(r["modes"]):
        print("  mode %d: agc=0x%04X band_ctl=0x%04X" % (m + 1, mod["agc_in_gain"], mod["band_control"]))
        print("    boostdB_default:", [round(x * 0.1, 1) for x in mod["boostdB_default"]],
              " raw:", mod["boostdB_default"])
        print("    boostdB_current:", [round(x * 0.1, 1) for x in mod["boostdB_current"]],
              " raw:", mod["boostdB_current"])


def print_scalings():
    print("""CONFIRMED UNIT SCALINGS (empirical; evidence = task-4 file, byte-match proofs below)
  fo       raw Hz                  (scale 1.0)      .nvm fo_array desc 'central frequency' + literal 62/250/1000/4000/16000;
                                                    EQ_Tunable fo_array byte-identical; EQ_Headset bands 12000/4000/600/50/100 Hz
  q        raw / 512                               .nvm q_array desc 'for band width; scaled by 512'; 0x200=512 -> 1.0, 0x100=256 -> 0.5
  boostdB  raw * 0.1 dB (signed s16)               0xFF4C = -180 = -18.0 dB (byte-identical to .nvm EQ_Headset eq_mode_2 band_1)
  gaindB   raw * 0.1 dB (signed s16)               0x001E = +30 = +3.0 dB (byte-identical to .nvm EQ_Headset eq_mode_2 band_1)""")


# --------------------------------------------------------------------------
# record iteration (name-anchored)
# --------------------------------------------------------------------------
def walk_records(dump, start, count, overrides, expected_names=None):
    """Yield (index, offset, layout, parsed) for count records from start.
    Name-anchored: the 16 bytes at each record start must decode to the
    expected canonical name; any mismatch raises RecordNameError.
    """
    names = expected_names if expected_names is not None else NAMES
    if expected_names is None:
        # align the canonical sequence to the actual start record so a
        # mid-table offset (e.g. --offset 0x6842C8 = EQ_Headfree) still passes.
        got = decode_name(dump, start)
        try:
            k = names.index(got)
        except ValueError:
            k = 0
        names = names[k:]
    off = start
    for i in range(count):
        name = decode_name(dump, off)
        if i < len(names):
            expected = names[i]
            if name != expected and expected == "EQ_Tunable":
                # tunable name sits +4..+8 past the stride position (mini
                # header BA 01 78 01 precedes it) in every region -- scan.
                found = None
                for scan in range(off, min(off + 0x100, len(dump) - 16)):
                    if bytes(dump[scan:scan + 16]).split(b"\0", 1)[0] == b"EQ_Tunable":
                        found = scan
                        break
                if found is not None:
                    off = found
                    name = "EQ_Tunable"
            if name != expected:
                raise RecordNameError(
                    "record %d @0x%08X: name mismatch -- expected %r, "
                    "read %r (16 bytes: %s). Wrong offset or generation drift."
                    % (i, off, expected, name, bytes(dump[off:off + 16]).hex(" ")))
        else:
            if not (name.startswith("EQ_") and len(name) > 3 and all(
                    32 <= c < 127 for c in dump[off:off + 16] if c)):
                raise RecordNameError(
                    "record %d @0x%08X: not a valid EQ_* name -- read %r"
                    % (i, off, name))
        if name == "EQ_Tunable":
            lay = get_layout("tunable", overrides)
            parsed = parse_tunable(dump, off, lay)
            nxt = off + lay["record_size"]
        else:
            lay = get_layout("untunable", overrides)
            parsed = parse_untunable(dump, off, lay)
            nxt = off + lay["stride"]
        yield i, off, lay, parsed
        off = nxt


class RecordNameError(Exception):
    pass


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Decode B310E EQ_* NV records (8-band EQ)")
    ap.add_argument("--dump", default=DEFAULT_DUMP, help="dump_firmware.bin path")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=None,
                    help="record start offset (0x..); default = primary table 0x6840A8")
    ap.add_argument("--count", type=int, default=None, help="number of records (default 16 for primary, 1 otherwise)")
    ap.add_argument("--record", default=None, help="decode a single record by name (EQ_Headset, ...)")
    ap.add_argument("--region", choices=["primary", "B", "C", "D"], default=None,
                    help="decode the full 16-record table of a region")
    ap.add_argument("--format", choices=["table", "compact", "csv"], default="compact",
                    help="output format (default compact)")
    ap.add_argument("--layout", choices=sorted(LAYOUTS), default=None,
                    help="force a layout (default auto by record name)")
    ap.add_argument("--layout-json", default=None, help="JSON file overriding layout constants")
    ap.add_argument("--scalings", action="store_true", help="print the confirmed unit scalings and exit")
    args = ap.parse_args(argv)

    overrides = None
    if args.layout_json:
        with open(args.layout_json, "r", encoding="utf-8") as f:
            overrides = json.load(f)

    if args.scalings:
        print_scalings()
        return 0

    try:
        dump = open(args.dump, "rb").read()
    except OSError as e:
        print("FATAL: cannot read dump %s: %s" % (args.dump, e), file=sys.stderr)
        return 2

    # resolve start offset
    if args.record:
        if args.record not in NAMES:
            print("FATAL: unknown record name %r (have %s)" % (args.record, NAMES), file=sys.stderr)
            return 2
        idx = NAMES.index(args.record)
        if args.record == "EQ_Tunable":
            start = TUNABLE_OFF
        else:
            start = PRIMARY_BASE + idx * STRIDE
        count = 1
        records = list(walk_records(dump, start, count, overrides, expected_names=[args.record]))
        for i, off, lay, parsed in records:
            print("# %d  @0x%08X  layout=%s" % (i, off, lay["name"]))
            print_record(parsed, args.format)
        return 0
    elif args.region and args.region != "primary":
        base = {"B": 0x6B1D40, "C": 0x7F4F02, "D": 0x7FCA64}[args.region]
        start = base
        count = 15
        tun = find_tunable(dump, base)
        # walk 15 untunable, then the tunable if found
        items = list(walk_records(dump, start, 15, overrides))
        if tun is None:
            print("WARN: no EQ_Tunable found in region %s (stride pos 0x%X)" % (args.region, base + 15 * STRIDE), file=sys.stderr)
        else:
            items.append((15, tun, get_layout("tunable", overrides), parse_tunable(dump, tun, get_layout("tunable", overrides))))
        for i, off, lay, parsed in items:
            print_record(parsed, args.format)
        return 0
    else:
        start = args.offset if args.offset is not None else PRIMARY_BASE
        count = args.count if args.count is not None else (16 if start == PRIMARY_BASE else 1)

    if args.layout == "tunable":
        # force: parse as tunable (used with --offset for a tunable record)
        for i in range(count):
            off = start + i * 0x188
            lay = get_layout("tunable", overrides)
            parsed = parse_tunable(dump, off, lay)
            print_record(parsed, args.format)
        return 0

    try:
        records = list(walk_records(dump, start, count, overrides))
    except RecordNameError as e:
        print("EQ-DECODE FAIL: %s" % e, file=sys.stderr)
        return 1
    except struct.error as e:
        print("EQ-DECODE FAIL: struct read error (record runs past end of dump?): %s" % e, file=sys.stderr)
        return 1

    for i, off, lay, parsed in records:
        print("# %d  @0x%08X  layout=%s" % (i, off, lay["name"]))
        print_record(parsed, args.format)

    # verify the walked offsets against the canonical geometry when primary
    if start == PRIMARY_BASE and count == 16:
        canon = [PRIMARY_BASE + i * STRIDE for i in range(15)] + [TUNABLE_OFF]
        actual = [off for _, off, _, _ in records]
        if actual != canon:
            print("WARN: walked offsets differ from canonical table: %s" %
                  ["0x%X!=0x%X" % (a, c) for a, c in zip(actual, canon) if a != c], file=sys.stderr)
            return 1
        print("OK: all 16 records name-anchored at the canonical offsets (0x6840A8..0x68608C)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
