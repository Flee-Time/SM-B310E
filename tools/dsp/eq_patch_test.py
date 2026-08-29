#!/usr/bin/env python3
"""eq_patch_test.py - T8 (b310e-audio-eq-tune): host test suite for
tools/dsp/eq_patch.py (tests-after; every assertion is a REAL byte
comparison with printed hex, never a mock).

Tests:
  (a) byte round-trip: decode -> patch -> decode yields the designed values
  (b) golden / neighbor-preservation: pristine sector patched -> ALL
      non-target bytes byte-identical AND target bytes == designed changes
  (c) diff-size: the changed-byte set == the target-byte set exactly
  (d) offset-window: every emitted target lies in the sanctioned regions
      (0x6840A8-0x6862AC / 0x6B1D40 / 0x7F4F02 / 0x7FCA64) -- exit 1 otherwise
  (e) command-grammar: every emitted command validates against the real
      spd_dump.c grammar (digit-start read_flash, fw+ only for
      write/erase, no read_data) + RMW completeness (every write_data is
      preceded by a read_flash + erase_flash of the same sector)
  (f) failure mode: boost_raw_after > 100 (CSV override) -> eq_patch.py
      exits 1 with a clear message
  (g) negative: a bare write_data (no sector-RMW triple) is NEVER emitted
  (h) stale_state: dump SHA256 == the T1/T4/T7/T8 record

Exit 0: all pass. Exit 1: any failure (message printed). Python 3 stdlib only.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eq_patch  # noqa: E402
from eq_decode import decode_name, fmt_s16, get_layout  # noqa: E402

REPO = eq_patch.REPO
DUMP = eq_patch.DEFAULT_DUMP
DESIGN = eq_patch.DEFAULT_DESIGN
AMEND = eq_patch.DEFAULT_AMENDMENT
EXPECTED_SHA = eq_patch.DUMP_SHA256

PASS = 0
FAILS = []


def check(name, ok, detail):
    global PASS, FAILS
    if ok:
        PASS += 1
        print("  PASS  %-46s %s" % (name, detail))
    else:
        FAILS.append(name)
        print("  FAIL  %-46s %s" % (name, detail))


def verify_dump(dump):
    import hashlib
    h = hashlib.sha256(dump).hexdigest().upper()
    check("(h) stale_state: dump SHA256 == record", h == EXPECTED_SHA,
          h[:16] + "... (want " + EXPECTED_SHA[:16] + "...)")


def expected_write_set(dump, copy_label, hs_off, tu_off, targets):
    """Independent re-derivation of the target byte set (abs_off -> after s16)
    for the golden/diff-size tests. Mirrors eq_patch.compute_copy_writes but
    is computed straight from the layout constants + dump bytes."""
    out = {}
    hlay = get_layout("untunable")
    tlay = get_layout("tunable")
    for t in targets:
        if t["record"] == "EQ_Headset":
            m, b = t["mode"], t["band"]
            mo = hlay["mode_base"] + (m - 1) * hlay["mode_stride"]
            bctl = struct.unpack_from("<H", dump, hs_off + mo + 2)[0]
            live = bool(bctl & (0x8000 >> (b - 1)))
            if b == 2 and not live:
                continue
            off = hs_off + mo + hlay["band_base_delta"] + (b - 1) * hlay["band_stride"] + 4
            before = fmt_s16(struct.unpack_from("<H", dump, off)[0])
            if before != t["boost_after"]:
                out[off] = t["boost_after"]
        else:
            m, b = t["mode"], t["band"]
            mo = tlay["mode_base"] + (m - 1) * tlay["mode_stride"]
            for d in (4, 20):
                off = tu_off + mo + d + (b - 1) * 2
                before = fmt_s16(struct.unpack_from("<H", dump, off)[0])
                if before != t["boost_after"]:
                    out[off] = t["boost_after"]
    return out


def main():
    print("=" * 78)
    print("EQ PATCH HOST TESTS (T8 tests-after)")
    print("=" * 78)

    dump = open(DUMP, "rb").read()
    verify_dump(dump)

    csv_rows = eq_patch.load_design_csv(DESIGN)
    targets = eq_patch.amend_rows(csv_rows)
    n_headset = sum(1 for t in targets if t["record"] == "EQ_Headset")
    n_tunable = sum(1 for t in targets if t["record"] == "EQ_Tunable")
    print("\namended target rows: %d EQ_Headset (modes 2-6) + %d EQ_Tunable "
          "(modes 2-6, T7 verbatim)" % (n_headset, n_tunable))
    check("amendment: NOT ROCK-only", n_headset == 10 and n_tunable == 40,
          "%d + %d rows" % (n_headset, n_tunable))
    check("amendment: every EQ_Headset mode in 2..6",
          all(2 <= t["mode"] <= 6 for t in targets if t["record"] == "EQ_Headset"),
          "modes %s" % sorted({t["mode"] for t in targets if t["record"] == "EQ_Headset"}))
    check("amendment: every EQ_Tunable mode in 2..6",
          all(2 <= t["mode"] <= 6 for t in targets if t["record"] == "EQ_Tunable"),
          "modes %s" % sorted({t["mode"] for t in targets if t["record"] == "EQ_Tunable"}))

    # (d) offset-window: sanctioned regions per copy (section map + F4 window)
    windows = {
        "A": (0x6840A8, 0x6862AC),
        "B": (0x6B1D40, 0x6B3D28 + 0x164),
        "C": (0x7F4F02, 0x7F6EE6 + 0x164),
        "D": (0x7FCA64, 0x7FEA4C + 0x164),
    }
    print("\n--- (d) offset-window + per-copy write computation ---")
    per_copy = {}
    for label, hs_off, tu_off in eq_patch.COPIES:
        writes, bctl = eq_patch.compute_copy_writes(dump, label, hs_off, tu_off, targets)
        per_copy[label] = (writes, bctl)
        lo, hi = windows[label]
        bad = [w for w in writes + bctl if not (lo <= w["abs_off"] < hi)]
        check("offset-window %s: all %d writes in 0x%X..0x%X"
              % (label, len(writes) + len(bctl), lo, hi), not bad,
              "bad=%s" % [hex(x["abs_off"]) for x in bad] if bad else "ok")
        check("name-anchored %s: records at 0x%X/0x%X"
              % (label, hs_off, tu_off),
              decode_name(dump, hs_off) == "EQ_Headset"
              and decode_name(dump, tu_off) == "EQ_Tunable",
              "%s / %s" % (decode_name(dump, hs_off), decode_name(dump, tu_off)))

    # (b) golden / neighbor-preservation + (c) diff-size, per sector
    print("\n--- (b) golden + (c) diff-size (per sector, real bytes) ---")
    total_sectors = 0
    for label, hs_off, tu_off in eq_patch.COPIES:
        writes, bctl = per_copy[label]
        exp = expected_write_set(dump, label, hs_off, tu_off, targets)
        # expected set from the generator == independent re-derivation
        gen = {w["abs_off"]: w["after"] for w in writes + bctl}
        check("write-set %s: generator == independent re-derivation" % label,
              gen == exp,
              "%d == %d sites" % (len(gen), len(exp)))
        sectors = eq_patch.group_sectors(writes, bctl)
        for sector in sorted(sectors):
            total_sectors += 1
            sec_writes = sectors[sector]
            pristine = dump[sector:sector + eq_patch.SECTOR_SIZE]
            patched, diff = eq_patch.apply_writes(pristine, sec_writes)
            target_rels = {}
            for w in sec_writes:
                target_rels[w["abs_off"] - sector] = w["after"]
            # (b) neighbor preservation: every non-target byte identical
            non_target_bad = []
            for rel in range(0, len(pristine), 2):
                if rel in target_rels:
                    continue
                if patched[rel:rel + 2] != pristine[rel:rel + 2]:
                    non_target_bad.append(rel)
            check("golden %s sect 0x%X: non-target bytes identical"
                  % (label, sector), not non_target_bad,
                  "bad=%s" % [hex(x) for x in non_target_bad[:5]] if non_target_bad else
                  "%d u16 non-target words preserved" % (len(pristine) // 2 - len(target_rels)))
            # (b) target bytes == designed changes exactly
            tgt_bad = [rel for rel, after in target_rels.items()
                       if struct.unpack_from("<H", patched, rel)[0] != after]
            check("golden %s sect 0x%X: target bytes == designed changes"
                  % (label, sector), not tgt_bad,
                  "bad=%s" % [hex(x) for x in tgt_bad] if tgt_bad else
                  "%d target u16s correct" % len(target_rels))
            # (c) diff-size: changed set == target set exactly (2 bytes per u16)
            changed = {rel for rel in range(0, len(pristine), 2)
                       if patched[rel:rel + 2] != pristine[rel:rel + 2]}
            check("diff-size %s sect 0x%X: changed == target set exactly"
                  % (label, sector), changed == set(target_rels),
                  "extra=%s missing=%s" % (
                      [hex(x) for x in sorted(changed - set(target_rels))],
                      [hex(x) for x in sorted(set(target_rels) - changed)])
                  if changed != set(target_rels) else "%d u16 changed" % len(changed))
    check("sectors covered", total_sectors == 9,
          "%d sectors (A/B/D: 2 each; C: 3 -- EQ_Headset @0x7F4F02 straddles "
          "0x7F4000/0x7F5000)" % total_sectors)

    # (a) byte round-trip: patch pristine -> re-decode -> designed values
    print("\n--- (a) byte round-trip (patch -> decode -> designed values) ---")
    round_ok = True
    for label, hs_off, tu_off in eq_patch.COPIES:
        writes, bctl = per_copy[label]
        sectors = eq_patch.group_sectors(writes, bctl)
        patched_dump = bytearray(dump)
        for sector in sorted(sectors):
            pristine = dump[sector:sector + eq_patch.SECTOR_SIZE]
            patched, _ = eq_patch.apply_writes(pristine, sectors[sector])
            patched_dump[sector:sector + eq_patch.SECTOR_SIZE] = patched
        # re-decode both records from the patched image
        hset = [t for t in targets if t["record"] == "EQ_Headset"]
        tun = [t for t in targets if t["record"] == "EQ_Tunable"]
        hlay = get_layout("untunable")
        tlay = get_layout("tunable")
        bad = []
        for t in hset:
            m, b = t["mode"], t["band"]
            mo = hlay["mode_base"] + (m - 1) * hlay["mode_stride"]
            bctl_live = struct.unpack_from("<H", patched_dump, hs_off + mo + 2)[0]
            live = bool(bctl_live & (0x8000 >> (b - 1)))
            if b == 2 and not live:
                continue
            off = hs_off + mo + hlay["band_base_delta"] + (b - 1) * hlay["band_stride"] + 4
            got = fmt_s16(struct.unpack_from("<H", patched_dump, off)[0])
            if got != t["boost_after"]:
                bad.append("HS %s m%d b%d @0x%X: %d != %d" % (label, m, b, off, got, t["boost_after"]))
        for t in tun:
            m, b = t["mode"], t["band"]
            mo = tlay["mode_base"] + (m - 1) * tlay["mode_stride"]
            for d, field in ((4, "def"), (20, "cur")):
                off = tu_off + mo + d + (b - 1) * 2
                got = fmt_s16(struct.unpack_from("<H", patched_dump, off)[0])
                if got != t["boost_after"]:
                    bad.append("TUN %s m%d b%d %s @0x%X: %d != %d"
                               % (label, m, b, field, off, got, t["boost_after"]))
        check("round-trip %s: patched records decode to designed values" % label,
              not bad, "; ".join(bad[:3]) if bad else "%d sites correct" % (len(hset) + len(tun) * 2))
        round_ok = round_ok and not bad

    # (e) command-grammar + RMW completeness + (g) no bare write_data
    print("\n--- (e) command-grammar + RMW completeness ---")
    commands = []
    for label, hs_off, tu_off in eq_patch.COPIES:
        writes, bctl = per_copy[label]
        for sector in sorted(eq_patch.group_sectors(writes, bctl)):
            base = "eq-%s-sect-0x%05X" % (label, sector)
            for cmd in eq_patch.emit_commands(label, sector, base + ".bin"):
                commands.append((label, sector, cmd))
    grammar_bad = []
    for label, sector, cmd in commands:
        err = eq_patch.validate_command(cmd.split())
        if err:
            grammar_bad.append("%s %s: %s" % (label, cmd, err))
    check("command-grammar: all %d commands valid" % len(commands), not grammar_bad,
          "; ".join(grammar_bad[:2]) if grammar_bad else "ok")
    check("no read_data subcommand emitted",
          not any("read_data" in c for _, _, c in commands),
          "%d commands scanned" % len(commands))
    # RMW completeness: for every write_data, a read_flash + erase_flash of the
    # same resolved address precedes it; erase always precedes write.
    seq = [(label, sector, cmd) for label, sector, cmd in commands]
    rmw_bad = []
    for i, (label, sector, cmd) in enumerate(seq):
        toks = cmd.split()
        sub, addr = eq_patch.resolve_addr(toks)
        if sub != "write_data":
            continue
        earlier = [c.split() for _, _, c in seq[:i]]
        has_read = any(eq_patch.resolve_addr(t)[0] == "read_flash"
                       and eq_patch.resolve_addr(t)[1] == addr for t in earlier)
        has_erase = any(eq_patch.resolve_addr(t)[1] == addr
                        and eq_patch.resolve_addr(t)[0] == "erase_flash" for t in earlier)
        if not (has_read and has_erase):
            rmw_bad.append("%s write_data 0x%X read=%s erase=%s"
                           % (label, addr, has_read, has_erase))
    check("RMW completeness: every write_data has prior read+erase (g)",
          not rmw_bad, "; ".join(rmw_bad[:3]) if rmw_bad else "all %d write_data covered" %
          sum(1 for _, _, c in seq if eq_patch.resolve_addr(c.split())[0] == "write_data"))

    # (f) failure mode: out-of-range boost (CSV override) -> exit 1 + message
    print("\n--- (f) failure mode: boost_raw_after > +10 dB -> exit 1 ---")
    with tempfile.TemporaryDirectory(prefix="eq_patch_fail_") as tmp:
        base_csv = os.path.join(tmp, "bad.csv")
        rows = eq_patch.load_design_csv(DESIGN)
        with open(base_csv, "w", encoding="utf-8") as f:
            f.write(eq_patch.CSV_HEADER + "\n")
            for r in rows:
                row = "%s,%d,%d,%d,%d,%d,%s,%d,%s,%s,%s" % (
                    r["record"], r["mode"], r["band"], r["fo_raw"], r["q_raw"],
                    r["boost_before"], r["gain_before"] if r["gain_before"] is not None else "NA",
                    110 if (r["record"] == "EQ_Headset-Rock" and r["band"] == 1) else r["boost_after"],
                    r["gain_after"] if r["gain_after"] is not None else "NA",
                    r["boost_db_after"] if r["boost_db_after"] is not None else "NA",
                    r["gain_db_after"] if r["gain_db_after"] is not None else "NA")
                f.write(row + "\n")
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "dsp", "eq_patch.py"),
                            "--dump", DUMP, "--csv", base_csv, "--dry-run"],
                           capture_output=True, text=True)
        ok_fail = r.returncode == 1 and "exceeds the +10.0 dB hard cap" in r.stderr
        check("cap: out-of-range CSV -> exit 1 + clear message", ok_fail,
              "rc=%d stderr=%r" % (r.returncode, r.stderr.strip()[:120]))

    print("\n" + "=" * 78)
    if FAILS:
        print("FAIL: %d test(s) failed: %s" % (len(FAILS), ", ".join(FAILS)))
        return 1
    print("ALL TESTS PASS (%d checks)" % PASS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
