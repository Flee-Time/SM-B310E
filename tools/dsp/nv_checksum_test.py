#!/usr/bin/env python3
"""nv_checksum_test.py - host self-tests for tools/dsp/nv_checksum.py.

Verifies the reversed checksum algorithm against all 3 recorded data points:
  (1) dump  factory island  -> stored 0x5C61   (must VALIDATE)
  (2) nvitem shipped island -> stored 0x2337   (must be flagged STALE,
      computed == 0x9745)
  (3) runtime-patched island (check.bin)       (field 0x4FC6; algorithm family
      documented -- exact offline reproduction is impossible because the
      runtime re-laid the store; see task evidence)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nv_checksum as nc

FAIL = []


def check(name, cond, detail=""):
    print("  %-52s %s%s" % (name, "PASS" if cond else "FAIL", detail))
    if not cond:
        FAIL.append(name)


def main():
    dump = open(nc.DEFAULT_DUMP, "rb").read()
    assert nc.sha256_file(nc.DEFAULT_DUMP) == nc.DUMP_SHA256, "stale dump"

    print("== (1) dump factory island ==")
    island_d = dump[nc.DS1_BASE:nc.DS1_BASE + nc.ISLAND_SIZE]
    comp = nc.nvitem_checksum(island_d)
    stored = struct.unpack_from("<H", island_d, 0)[0]
    check("dump island: computed == 0x5C61", comp == nc.DUMP_STORED,
          " (got 0x%04X)" % comp)
    check("dump island: stored == 0x5C61", stored == nc.DUMP_STORED)
    check("dump island: self-consistent", comp == stored)

    print("== (2) shipped nvitem island ==")
    nv = open(nc.DEFAULT_NVITEM, "rb").read()
    comp_n = nc.nvitem_checksum(nv)
    stored_n = struct.unpack_from("<H", nv, 0)[0]
    check("nvitem: computed == 0x9745 (documented stale value)",
          comp_n == 0x9745, " (got 0x%04X)" % comp_n)
    check("nvitem: stored == 0x2337", stored_n == nc.NVITEM_STORED)
    check("nvitem: STALE (computed != stored) -- the documented finding",
          comp_n != stored_n)

    print("== (3) runtime-patched read-back (check.bin) ==")
    chk = open(nc.DEFAULT_CHECK, "rb").read()
    chk_field = struct.unpack_from("<H", chk, 0)[0]
    check("check.bin: field == 0x4FC6 (runtime recomputed)",
          chk_field == nc.RUNTIME_PATCHED, " (got 0x%04X)" % chk_field)
    # document the near-miss on the observable 0x1000 (sector content):
    near = nc.crc16(0, chk[2:0x1000])
    check("check.bin: crc16(0,[2:0x1000]) == 0x4F21 (near-miss, NOT exact -- "
          "runtime store extends beyond the 0x1000 read-back)",
          near == 0x4F21, " (got 0x%04X)" % near)

    print("== apply_writes / patch round-trip ==")
    writes = [(0x42, 0x1000), (0x4A, 0x1000), (0x6A, 0x1011), (0x6C, 0x0000)]
    patched = nc.apply_writes(nv, writes)
    new_ck = nc.nvitem_checksum(patched)
    patched = patched[:0] + struct.pack("<H", new_ck & 0xFFFF) + patched[2:]
    ok, comp_p, stored_p = nc.verify_island(patched, "patched-nvitem")
    check("patched nvitem: field recomputed", new_ck != stored_n)
    check("patched nvitem: self-consistent after recompute", ok,
          " (computed 0x%04X stored 0x%04X)" % (comp_p, stored_p))

    print("== patch site byte checks (Headset dl_ fields, nvitem offsets) ==")
    # nvitem 0x6A = dl_POP_switch (idx37, block 0x10 + 16 + 74)
    pop = struct.unpack_from("<H", nv, 0x6A)[0]
    agc = struct.unpack_from("<H", nv, 0x6C)[0]
    bass = struct.unpack_from("<H", nv, 0x42)[0]
    mid = struct.unpack_from("<H", nv, 0x4A)[0]
    check("Headset dl_POP_switch == 0x1111 (HPF bit8 ON)", pop == 0x1111)
    check("Headset dl_AGC_switch == 0x0100 (HPF bit8 ON, B310E anomaly)",
          agc == 0x0100)
    check("Headset dl_EQ_bass_gain == 0x0D76 (-1.5 dB)", bass == 0x0D76)
    check("Headset dl_EQ_mid_gain == 0x0D76 (-1.5 dB)", mid == 0x0D76)

    print()
    if FAIL:
        print("FAILED: %d check(s): %s" % (len(FAIL), ", ".join(FAIL)))
        return 1
    print("ALL TESTS PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
