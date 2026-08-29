#!/usr/bin/env python3
"""eq_patch.py - T8 (b310e-audio-eq-tune): patch generator for the B310E
EQ_* NV records (3.5mm-jack bass boost), emitting the patched record bytes +
the exact spd_dump sector-RMW NOR-write command sequences, host-tested.

DESIGN SOURCE (read at runtime, never hardcoded):
  - .omo/dumpmine/dsp/bass-curve-design.md        section 4.1 before->after
    CSV (the T7 design) + section 2 scalings
    (fo = raw Hz, q = raw/512, boostdB/gaindB = s16 * 0.1 dB).
  - .omo/dumpmine/dsp/bass-curve-active-mode.md   section 2 (THE target-set
    amendment; T8 MUST NOT emit a ROCK-only patch):
    EQ_Headset modes 2..6 + EQ_Tunable modes 2..6, ALL FOUR copies
    (0x6840A8 / 0x6B1D40 / 0x7F4F02 / 0x7FCA64).

AMENDED TARGET SET (the amendment applied to the T7 CSV):
  - EQ_Headset (untunable, item 440) modes 2..6 (mode 1 = OFF untouched):
      band1 boostdB -> 100 (+10.0 dB, the HARD CAP) in every non-OFF mode;
      band2 boostdB -> 60 (+6.0 dB) ONLY where band2 is live
      (band_control bit14 = filter_sw_2; shipped: modes 5-6);
      band1 enable guard (bit15 = filter_sw_1) in any non-OFF mode where
      band1 is dead (shipped: none -> zero bctl writes).
      The ROCK-only extras from design-doc section 4.2 (band3 enable,
      fo/q changes) are DROPPED per the amendment ("become
      EQ_Headset,<mode 2..6>,<band 1..2> rows").
  - EQ_Tunable (item 441) modes 2..6: T7 CSV rows verbatim (band1 -> 80,
    band2 -> 60, written to BOTH boostdB_default and boostdB_current;
    band_control untouched).
  - Rows with after == before are NOT written (T7 CSV convention).

DESIGN FLAG (surfaced, not silently re-engineered): the shipped EQ_Headset
band1 fo is 50 Hz only in ROCK (mode 6); modes 2-5 ship band1 at
12000/4000/600/600 Hz and band2 at 600 Hz (JAZZ). Per the amendment the
boost values are written to the band SLOTS regardless of their shipped fo;
the patch table prints each row's actual fo so the user sees exactly what
the DSP would apply if it reads these curves (Scenario B).

EMISSION (sector-granular RMW; spd_dump.c grammar VERIFIED against the
external spreadtrum_flash clone):
  per copy, sector = (first patched byte) & ~0xFFF, size 0x1000:
    1) read_flash  <0x30000000 + sector> 0 0x1000 <file>   (str_to_size:
       digit-start ONLY - NO fw+ prefix; spd_dump.c:1364)
    2) host patch of the record bytes inside the sector image
       (this tool emits the patched file; the read-back is the live-flash
       ground-truth check against the dump-derived pristine sector)
    3) erase_flash fw+<sector> 0x1000                      (str_to_addr:
       fw+ OK; spd_dump.c:1411)
    4) write_data  fw+<sector> 0 0 <patched file>          (str_to_addr:
       fw+ OK; spd_dump.c:1396; sfc.c:104 = Page Program only, no
       auto-erase -> the erase_flash step is MANDATORY or 0->1 bit
       transitions silently fail)
  There is NO read_data subcommand in spd_dump.c - never emitted.
  fw_addr = 0x30000000 for the SC6530C (spd_dump.c:1320).

Usage:
  python tools\\dsp\\eq_patch.py --dry-run        # patch table + byte diff + commands (default)
  python tools\\dsp\\eq_patch.py --emit <dir>     # also write pristine/patched sector .bin files
  python tools\\dsp\\eq_patch.py --csv <file>     # use a CSV override instead of the design doc (tests)
  python tools\\dsp\\eq_patch_test.py             # the host test suite (tests-after)

Exit 0: emitted (table + commands emitted, all validations passed).
Exit 1: validation failure (cap exceeded, name mismatch, stale dump, ...).
Exit 2: usage error.
"""

import argparse
import hashlib
import os
import struct
import sys

# import the T4 decoder from the same directory (name-anchored parse +
# LAYOUTS + parse_untunable/parse_tunable are the field-map ground truth)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eq_decode import (  # noqa: E402
    LAYOUTS, PRIMARY_BASE, TUNABLE_OFF, decode_name, fmt_s16,
    get_layout, parse_tunable, parse_untunable, walk_records,
)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DUMP = os.path.join(REPO, "dump_firmware.bin")
DEFAULT_DESIGN = os.path.join(REPO, ".omo", "dumpmine", "dsp", "bass-curve-design.md")
DEFAULT_AMENDMENT = os.path.join(REPO, ".omo", "dumpmine", "dsp", "bass-curve-active-mode.md")

# ground-truth dump identity (T1/T4/T7 record; stale_state guard)
DUMP_SHA256 = "2B62CE1BFBF9C43F80C917BDE4E1130B1A3D24E014DDF0672F138598B6FEF0E6"

# copy geometry (T5 V3/V4 + section map): (label, EQ_Headset @, EQ_Tunable @)
COPIES = [
    ("A", 0x6840A8, 0x68608C),   # DSP partition factory default (primary)
    ("B", 0x6B1D40, 0x6B3D28),   # user-data EFS sector copy
    ("C", 0x7F4F02, 0x7F6EE6),   # user-data EFS sector copy (unaligned +2)
    ("D", 0x7FCA64, 0x7FEA4C),   # user-data EFS sector copy (+8 variance)
]

SECTOR_SIZE = 0x1000
FW_ADDR = 0x30000000            # SC6530C fw_addr (spd_dump.c:1320)
FDL = "nor_fdl1.bin 0x40004000"  # repo-sanctioned FDL load (docs/sdboot.md)

# sanctioned write values (T4 inherited wisdom; design integrity check)
ALLOWED_WRITES = {30, 60, 80, 100}   # +3.0 / +6.0 / +8.0 / +10.0 dB
BOOST_CAP = 100                      # +10.0 dB hard cap (scope A3 / T7 QA)

CSV_HEADER = ("record,mode,band,fo_raw,q_raw,boost_raw_before,"
              "gain_raw_before,boost_raw_after,gain_raw_after,"
              "boost_db_after,gain_db_after")

# --------------------------------------------------------------------------
# design CSV parsing (bass-curve-design.md section 4.1)
# --------------------------------------------------------------------------

def extract_csv_text(md_text):
    """Return the section-4.1 CSV block of the design markdown (the fenced
    table after the CSV_HEADER line), or None if not found."""
    lines = md_text.splitlines()
    for i, ln in enumerate(lines):
        if ln.strip() == CSV_HEADER:
            return "\n".join(lines[i:])
    return None


def parse_csv_rows(csv_text):
    """Parse CSV_HEADER-shaped text into a list of dict rows (ints; NA -> None)."""
    rows = []
    for ln in csv_text.splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("```") or ln.startswith("#"):
            continue
        if ln.startswith("record,"):        # any header row (CSV_HEADER or the
            continue                        # section 4.2 extra-fields header)
        if ln.startswith("EQ_"):
            f = [c.strip() for c in ln.split(",")]
            if len(f) != 11:
                break                       # section 4.2 extra-fields rows (7 fields) end the block
        else:
            break                           # non-CSV line (markdown fence end / prose)
        row = {
            "record": f[0], "mode": int(f[1]), "band": int(f[2]),
            "fo_raw": int(f[3]), "q_raw": int(f[4]),
            "boost_before": int(f[5]),
            "gain_before": None if f[6] == "NA" else int(f[6]),
            "boost_after": int(f[7]),
            "gain_after": None if f[8] == "NA" else int(f[8]),
            "boost_db_after": None if f[9] == "NA" else float(f[9]),
            "gain_db_after": None if f[10] == "NA" else float(f[10]),
        }
        rows.append(row)
    return rows


def load_design_csv(design_path, csv_override=None):
    """Read the machine-parseable design: CSV_HEADER rows from the override
    file, else from the design markdown's section 4.1 fenced block."""
    if csv_override:
        with open(csv_override, "r", encoding="utf-8") as f:
            text = f.read()
        if CSV_HEADER not in text:
            # plain CSV without the header -> prepend it
            text = CSV_HEADER + "\n" + text
        return parse_csv_rows(text)
    with open(design_path, "r", encoding="utf-8") as f:
        text = f.read()
    block = extract_csv_text(text)
    if block is None:
        raise ValueError(
            "design CSV (header %r) not found in %s" % (CSV_HEADER, design_path))
    return parse_csv_rows(block)


def amend_rows(csv_rows):
    """Apply the bass-curve-active-mode.md section-2 amendment to the T7 CSV:

    - 'EQ_Headset-Rock' rows (T7's ROCK-only target, mode 6) become the
      EQ_Headset all-active-modes target: modes 2..6, bands 1..2, with the
      ROCK design's values (band1 -> 100, band2 -> 60). Bands 3-8 rows are
      dropped (amendment: "<band 1..2> rows").
    - 'EQ_Tunable' rows are kept VERBATIM (T7 CSV modes 2-6).

    Returns a list of {'record','mode','band','boost_after'} target rows.
    """
    headset_val = {"b1": None, "b2": None}
    tunable_rows = []
    for r in csv_rows:
        if r["record"] == "EQ_Headset-Rock":
            if r["band"] == 1 and headset_val["b1"] is None:
                headset_val["b1"] = r["boost_after"]
            elif r["band"] == 2 and headset_val["b2"] is None:
                headset_val["b2"] = r["boost_after"]
        elif r["record"] == "EQ_Tunable":
            tunable_rows.append(r)
    if headset_val["b1"] is None or headset_val["b2"] is None:
        raise ValueError("design CSV lacks EQ_Headset-Rock band1/band2 rows "
                         "(band1=%r band2=%r)" % (headset_val["b1"], headset_val["b2"]))

    out = []
    for m in range(2, 7):           # modes 2..6 (1-indexed, non-OFF)
        for b, after in ((1, headset_val["b1"]), (2, headset_val["b2"])):
            out.append({"record": "EQ_Headset", "mode": m, "band": b,
                        "boost_after": after})
    for r in tunable_rows:
        out.append({"record": r["record"], "mode": r["mode"], "band": r["band"],
                    "boost_after": r["boost_after"]})
    return out


# --------------------------------------------------------------------------
# write-site computation (name-anchored against the live dump)
# --------------------------------------------------------------------------

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


class PatchError(Exception):
    pass


def compute_copy_writes(dump, copy_label, headset_off, tunable_off, targets):
    """Compute the exact u16 write sites for one copy. Every site is
    name-anchored (the record start must decode to the expected name) and
    validated against the dump's current bytes + the shipped liveness.

    targets: amended row list from amend_rows().
    Returns list of write dicts:
      {copy, record, mode, band, field, abs_off, before, after, fo_hz, note}
    plus the list of bctl writes (band1 enable guard).
    """
    writes, bctl_writes = [], []

    hs_off = headset_off
    if decode_name(dump, hs_off) != "EQ_Headset":
        raise PatchError("copy %s: name mismatch at EQ_Headset 0x%08X -> %r"
                         % (copy_label, hs_off, decode_name(dump, hs_off)))
    tu_off = tunable_off
    if decode_name(dump, tu_off) != "EQ_Tunable":
        raise PatchError("copy %s: name mismatch at EQ_Tunable 0x%08X -> %r"
                         % (copy_label, tu_off, decode_name(dump, tu_off)))

    hlay = get_layout("untunable")
    tlay = get_layout("tunable")

    for t in targets:
        if t["record"] == "EQ_Headset":
            m, b = t["mode"], t["band"]
            mo = hlay["mode_base"] + (m - 1) * hlay["mode_stride"]
            bctl = struct.unpack_from("<H", dump, hs_off + mo + 2)[0]
            band_off = mo + hlay["band_base_delta"] + (b - 1) * hlay["band_stride"]
            fo = struct.unpack_from("<H", dump, hs_off + band_off)[0]
            live = bool(bctl & (0x8000 >> (b - 1)))
            boost_off = hs_off + band_off + 4
            before = fmt_s16(struct.unpack_from("<H", dump, boost_off)[0])
            after = t["boost_after"]
            if b == 2 and not live:
                continue                    # band2 only boosted where live
            if b == 1 and not live:
                # enable guard: every non-OFF mode carries band1
                bctl_writes.append({
                    "copy": copy_label, "record": "EQ_Headset", "mode": m,
                    "field": "band_control", "abs_off": hs_off + mo + 2,
                    "before": bctl, "after": bctl | 0x8000, "note": "band1 enable",
                })
            if before == after:
                continue                    # CSV convention: no-op rows not written
            writes.append({
                "copy": copy_label, "record": "EQ_Headset", "mode": m, "band": b,
                "field": "boostdB", "abs_off": boost_off,
                "before": before, "after": after, "fo_hz": fo,
                "note": "live" if live else "",
            })
        else:  # EQ_Tunable -- T7 CSV verbatim: both default and current
            m, b = t["mode"], t["band"]
            mo = tlay["mode_base"] + (m - 1) * tlay["mode_stride"]
            for field, d in (("boostdB_default", 4), ("boostdB_current", 20)):
                off = tu_off + mo + d + (b - 1) * 2
                before = fmt_s16(struct.unpack_from("<H", dump, off)[0])
                after = t["boost_after"]
                if before == after:
                    continue
                writes.append({
                    "copy": copy_label, "record": "EQ_Tunable", "mode": m,
                    "band": b, "field": field, "abs_off": off,
                    "before": before, "after": after, "fo_hz": 0,
                    "note": "def+cur invariant",
                })

    # hard cap + allowed-value checks (T7 QA: any boost > +10 dB exits 1)
    for w in writes:
        if w["after"] > BOOST_CAP:
            raise PatchError(
                "copy %s %s mode %d band %d: boost_raw_after %d exceeds the "
                "+10.0 dB hard cap (%d) -- design/CSV out of range"
                % (w["copy"], w["record"], w["mode"], w["band"], w["after"], BOOST_CAP))
        if w["after"] not in ALLOWED_WRITES:
            raise PatchError(
                "copy %s %s mode %d band %d: boost_raw_after %d not in the "
                "sanctioned write set %s (T4 scalings)"
                % (w["copy"], w["record"], w["mode"], w["band"],
                   w["after"], sorted(ALLOWED_WRITES)))
    return writes, bctl_writes


# --------------------------------------------------------------------------
# sector assembly + diff
# --------------------------------------------------------------------------

def apply_writes(pristine, writes):
    """Apply the write list to a pristine sector image; return (patched, diff)
    where diff is {in_sector_offset: (before_bytes, after_bytes)} (u16 LE)."""
    patched = bytearray(pristine)
    diff = {}
    for w in writes:
        rel = w["abs_off"] % len(pristine)
        bb = struct.pack("<H", w["before"] & 0xFFFF)
        ab = struct.pack("<H", w["after"] & 0xFFFF)
        if bytes(patched[rel:rel + 2]) != bb:
            raise PatchError("write @0x%X: sector byte %s != before %s -- "
                             "sector image is not the pristine ground truth"
                             % (w["abs_off"], bytes(patched[rel:rel + 2]).hex(),
                                bb.hex()))
        if ab != bb:
            diff[rel] = (bb, ab)
        patched[rel:rel + 2] = ab
    return bytes(patched), diff


def group_sectors(writes, bctl_writes):
    """Group writes by sector; return {sector: [writes...]} (writes only)."""
    groups = {}
    for w in writes + bctl_writes:
        groups.setdefault(w["abs_off"] & ~0xFFF, []).append(w)
    return groups


# --------------------------------------------------------------------------
# spd_dump.c command grammar (verified against the external clone)
# --------------------------------------------------------------------------

def str_to_size_ok(s):
    """Mirror spd_dump.c str_to_size (line 1143): digit-start ONLY, optional
    K/M/G suffix; no fw+/ram+ prefix."""
    if not s or not s[0].isdigit():
        return False
    body = s
    for suf in ("K", "M", "G"):
        if body.endswith(suf):
            body = body[:-1]
            break
    try:
        int(body, 0)
        return True
    except ValueError:
        return False


def str_to_addr_ok(s):
    """Mirror spd_dump.c str_to_addr (line 1163): a bare literal (base 0) or
    'fw'+offset (base = fw_addr). 'fw' alone = base itself."""
    if s.startswith("fw"):
        rest = s[2:]
        if rest == "":
            return True
        if not rest.startswith("+"):
            return False
        return str_to_size_ok(rest[1:])
    return str_to_size_ok(s)


def validate_command(tokens):
    """Validate one spd_dump command LINE (exe + command groups) against the
    real spd_dump.c grammar (argc guards are effectively per-group since the
    consumed-count equals the group length). Returns None if valid, else an
    error string. Group shapes (verified against spd_dump.c):
      fdl <file> <addr>                       (2 args; addr bare literal)
      read_flash <addr> <offset> <size> <file> (addr via str_to_size: digit-
        start ONLY, NO fw+ prefix -- spd_dump.c:1364)
      write_word <addr> <data> / write_data <addr> <offset> <size> <file> /
      erase_flash <addr> <size>               (addr via str_to_addr: bare or
        fw+ -- spd_dump.c:1379/1396/1411)
      NO read_data subcommand exists in spd_dump.c -- rejected.
    """
    i = 1
    while i < len(tokens):
        sub = tokens[i]
        if sub == "fdl":
            if i + 3 > len(tokens):
                return "fdl: want 2 args (file addr)"
            if not str_to_size_ok(tokens[i + 2]):
                return "fdl addr %r: must be a bare literal (ram unknown pre-load)" % tokens[i + 2]
            i += 3
        elif sub == "read_flash":
            if i + 5 > len(tokens):
                return "read_flash: want 4 args (addr offset size file)"
            if not str_to_size_ok(tokens[i + 1]):
                return "read_flash addr %r: str_to_size (digit-start, NO fw+)" % tokens[i + 1]
            if not str_to_size_ok(tokens[i + 2]):
                return "read_flash offset %r: str_to_size (digit-start)" % tokens[i + 2]
            if tokens[i + 3] != "auto" and not str_to_size_ok(tokens[i + 3]):
                return "read_flash size %r: str_to_size (digit-start)" % tokens[i + 3]
            i += 5
        elif sub == "write_word":
            if i + 3 > len(tokens):
                return "write_word: want 2 args (addr data)"
            if not str_to_addr_ok(tokens[i + 1]):
                return "write_word addr %r: str_to_addr (bare or fw+)" % tokens[i + 1]
            i += 3
        elif sub == "write_data":
            if i + 5 > len(tokens):
                return "write_data: want 4 args (addr offset size file)"
            if not str_to_addr_ok(tokens[i + 1]):
                return "write_data addr %r: str_to_addr (bare or fw+)" % tokens[i + 1]
            if not str_to_size_ok(tokens[i + 2]) or not str_to_size_ok(tokens[i + 3]):
                return "write_data offset/size: str_to_size (digit-start)"
            i += 5
        elif sub == "erase_flash":
            if i + 3 > len(tokens):
                return "erase_flash: want 2 args (addr size)"
            if not str_to_addr_ok(tokens[i + 1]):
                return "erase_flash addr %r: str_to_addr (bare or fw+)" % tokens[i + 1]
            if not str_to_size_ok(tokens[i + 2]):
                return "erase_flash size %r: str_to_size (digit-start)" % tokens[i + 2]
            i += 3
        else:
            return "unknown subcommand %r" % sub
    return None


def resolve_addr(tokens):
    """Resolve a command LINE's flash address for the LAST group (read_flash:
    literal; write/erase: fw+). Returns (sub, addr)."""
    # walk groups like validate_command to find the flash command
    i = 1
    last = None
    while i < len(tokens):
        sub = tokens[i]
        if sub in ("read_flash", "write_data", "erase_flash", "write_word"):
            last = (sub, tokens[i + 1])
            if sub == "read_flash":
                i += 5
            elif sub == "write_data":
                i += 5
            else:
                i += 3
        elif sub == "fdl":
            i += 3
        else:
            break
    if last is None:
        return None, None
    sub, a = last
    if sub == "read_flash":
        return sub, int(a, 0)
    if a.startswith("fw+"):
        return sub, FW_ADDR + int(a[3:], 0)
    if a == "fw":
        return sub, FW_ADDR
    return sub, int(a, 0)


# --------------------------------------------------------------------------
# emission
# --------------------------------------------------------------------------

def emit_commands(copy_label, sector, sector_file):
    """The 3 separate spd_dump invocations per sector (docs/sdboot.md + the
    T8 contract: read -> host patch -> erase -> write; NO read_data)."""
    lit = FW_ADDR + sector
    return [
        ".\\spd_dump.exe fdl %s read_flash 0x%X 0 0x%X %s" % (FDL, lit, SECTOR_SIZE, sector_file),
        ".\\spd_dump.exe fdl %s erase_flash fw+0x%X 0x%X" % (FDL, sector, SECTOR_SIZE),
        ".\\spd_dump.exe fdl %s write_data fw+0x%X 0 0 %s" % (FDL, sector, sector_file),
    ]


def format_diff(copy_label, sector, pristine, diff):
    lines = ["  diff (%d u16 changes):" % len(diff)]
    for rel in sorted(diff):
        bb, ab = diff[rel]
        lines.append("    sector+0x%04X  %s -> %s" % (rel, bb.hex(" "), ab.hex(" ")))
    return lines


def build_report(dump, targets, emit_dir=None):
    """Full report: per-copy patch table, sector diffs, command sequences.
    Returns (report_lines, all_commands, all_writes)."""
    lines = []
    lines.append("=" * 78)
    lines.append("EQ PATCH REPORT - all-modes bass boost (T8)")
    lines.append("dump SHA256: %s" % DUMP_SHA256)
    lines.append("targets: EQ_Headset modes 2-6 + EQ_Tunable modes 2-6, "
                 "ALL FOUR copies (bass-curve-active-mode.md section 2)")
    lines.append("=" * 78)

    all_commands = []
    all_writes = []
    for label, hs_off, tu_off in COPIES:
        writes, bctl = compute_copy_writes(dump, label, hs_off, tu_off, targets)
        all_writes += writes + bctl
        lines.append("")
        lines.append("copy %s  EQ_Headset @0x%08X  EQ_Tunable @0x%08X"
                     % (label, hs_off, tu_off))
        if bctl:
            for w in bctl:
                lines.append("  bctl: mode %d 0x%04X -> 0x%04X @0x%08X (%s)"
                             % (w["mode"], w["before"], w["after"], w["abs_off"], w["note"]))
        else:
            lines.append("  bctl: no writes (band1 live in every non-OFF mode)")
        if not writes:
            lines.append("  (no writes)")
        for w in sorted(writes, key=lambda x: x["abs_off"]):
            lines.append(
                "  %-13s m%d b%d %-16s @0x%08X  %5d -> %5d  (+%.1f dB)  fo=%sHz%s"
                % (w["record"], w["mode"], w["band"], w["field"], w["abs_off"],
                   w["before"], w["after"], w["after"] * 0.1,
                   ("%d" % w["fo_hz"]) if w["fo_hz"] else "-",
                   (" [%s]" % w["note"]) if w["note"] else ""))

        sectors = group_sectors(writes, bctl)
        for sector in sorted(sectors):
            sec_writes = sectors[sector]
            pristine = dump[sector:sector + SECTOR_SIZE]
            patched, diff = apply_writes(pristine, sec_writes)
            lines.append("")
            lines.append("  sector 0x%05X (bytes 0x%05X-0x%05X): %d u16 writes"
                         % (sector, sector, sector + SECTOR_SIZE - 1, len(sec_writes)))
            lines += format_diff(label, sector, pristine, diff)
            base = "eq-%s-sect-0x%05X" % (label, sector)
            if emit_dir:
                os.makedirs(emit_dir, exist_ok=True)
                with open(os.path.join(emit_dir, base + ".pristine.bin"), "wb") as f:
                    f.write(pristine)
                with open(os.path.join(emit_dir, base + ".patched.bin"), "wb") as f:
                    f.write(patched)
                lines.append("  wrote %s.pristine.bin / %s.patched.bin"
                             % (os.path.join(emit_dir, base), os.path.join(emit_dir, base)))
            for cmd in emit_commands(label, sector, base + ".bin"):
                lines.append("    " + cmd)
                all_commands.append((label, sector, cmd))
    return lines, all_commands, all_writes


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="EQ patch generator (T8)")
    ap.add_argument("--dump", default=DEFAULT_DUMP, help="dump_firmware.bin path")
    ap.add_argument("--design", default=DEFAULT_DESIGN, help="bass-curve-design.md path")
    ap.add_argument("--amendment", default=DEFAULT_AMENDMENT, help="bass-curve-active-mode.md path (documented, rules are code-cited)")
    ap.add_argument("--csv", default=None, help="CSV override (replaces the design-doc section 4.1 block; tests)")
    ap.add_argument("--emit", default=None, metavar="DIR", help="write pristine/patched sector files to DIR")
    ap.add_argument("--dry-run", action="store_true", help="print only (default behaviour; kept for the QA contract)")
    args = ap.parse_args(argv)

    if args.dry_run and args.emit:
        print("FATAL: --dry-run and --emit are mutually exclusive", file=sys.stderr)
        return 2

    if not os.path.isfile(args.dump):
        print("FATAL: dump not found: %s" % args.dump, file=sys.stderr)
        return 2
    got = sha256_file(args.dump)
    if got != DUMP_SHA256:
        print("FATAL: stale dump: SHA256 %s != recorded %s (T1/T4/T7/T8). "
              "Ground truth changed -- regenerate the design before patching."
              % (got, DUMP_SHA256), file=sys.stderr)
        return 1

    try:
        csv_rows = load_design_csv(args.design, args.csv)
        targets = amend_rows(csv_rows)
        dump = open(args.dump, "rb").read()
        lines, commands, writes = build_report(dump, targets, emit_dir=args.emit)
    except (PatchError, ValueError, struct.error) as e:
        print("EQ-PATCH FAIL: %s" % e, file=sys.stderr)
        return 1

    print("\n".join(lines))

    # design flag: band-slot fo ground truth for the headset rows
    print()
    print("DESIGN FLAG (surface, do not re-engineer): shipped EQ_Headset band1 fo "
          "is 50 Hz only in ROCK (mode 6);")
    print("modes 2-5 ship band1 at 12000/4000/600/600 Hz and JAZZ band2 at 600 Hz "
          "(read from this dump, see fo= above).")
    print("The boost is written to the band SLOTS per bass-curve-active-mode.md section 2; "
          "if the DSP applies a")
    print("mode whose bands sit above the 60-250 Hz window, the boost lands at that "
          "band's frequency (Scenario-B empirical test).")

    # command grammar self-check (fail loudly if we ever emit bad commands)
    for label, sector, cmd in commands:
        err = validate_command(cmd.split())
        if err:
            print("EQ-PATCH FAIL: emitted command fails the spd_dump.c grammar "
                  "check: %s | %s" % (err, cmd), file=sys.stderr)
            return 1
    print()
    print("grammar check: all %d emitted commands validate against spd_dump.c "
          "(read_flash digit-start, fw+ only for write/erase, no read_data)" % len(commands))
    print("user instructions (per sector): run the read_flash command; verify the "
          "read-back == the .pristine.bin file")
    print("(fc /b); replace the file with the .patched.bin; then run the erase_flash "
          "and write_data commands.")
    print("WARNING: keep the USB cable seated between erase_flash and write_data -- a "
          "mid-sequence drop leaves the")
    print("sector erased until the write completes. Recovery: re-run the sequence, or "
          "full restore per docs/sdboot.md")
    print("(full-backup.bin). Do NOT run any of these commands yourself if you are an "
          "agent -- this output is for [USER].")
    return 0


if __name__ == "__main__":
    sys.exit(main())
