#!/usr/bin/env python3
"""nv_checksum.py - b310e-audio-eq-tune: compute/verify/patch the NV data
island checksum (dataset-1 header word at bytes 0-1 of nvitem.bin / NOR
0x680000), and emit T8-style sector-RMW commands for validated NV patches.

REVERSED ALGORITHM (verified against dump_firmware.bin, the self-consistent
factory data point):

    field = crc16(0, island[2:])           # Spreadtrum FDL CRC-16
        poly 0x8005 reflected (0xA001), init 0, no xorout  -- the exact
        crc16() from the SDK fdl_bootloader (tf_nv_nor.c:295) and the
        bootloader crc16.c.
    stored as u16 LE at island bytes 0-1.
    bytes 2-3 = constant magic 0x0DB3 (not part of the checksum).

    island  = the full NV data island = nvitem.bin[0 : 0x9ED4]
              (= dump_firmware.bin[0x680000 : 0x680000+0x9ED4]).

VERIFICATION (all 3 data points, this run):
  (1) dump:    crc16(0, dump[0x680002:0x680000+0x9ED4]) = 0x5C61 == stored 0x5C61  [MATCH]
  (2) nvitem:  crc16(0, nvitem[2:0x9ED4])               = 0x9745 != stored 0x2337  [STALE]
       The shipped nvitem.bin's checksum field does NOT match its own content:
       935 tail bytes (0x796C-0x96A7, the NV-store bookkeeping/name table)
       were rewritten AFTER the checksum was computed. The field is a stale
       generation marker, not a live checksum.
  (3) runtime: the phone's runtime recomputed the field to 0x4FC6 after our
       sector-0x680000 patch (read-back check.bin). The exact value cannot be
       reproduced offline because the runtime RE-LAID the store (0x400-byte
       page replication, task-B finding) and the read-back captured only the
       first 0x1000 of the 0x9ED4 island -- but the algorithm family is the
       same crc16 (this tool reproduces the dump's factory value exactly).

PRACTICAL CONSEQUENCE: the runtime RECOMPUTES the checksum field whenever it
writes back the store (proven by data point 3). So a NOR patch "sticks" even
with a stale/zero checksum field -- the runtime fixes it. Using this tool to
emit a self-consistent checksum keeps the image valid for BOTH the runtime and
any offline validation.

Usage:
  python tools\\dsp\\nv_checksum.py --verify stock\\nvitem.bin
  python tools\\dsp\\nv_checksum.py --verify-against-dump       # dump island
  python tools\\dsp\\nv_checksum.py --patch <nvitem.bin> --set 0x42:0x1000,0x4A:0x1000,0x6A:0x1011,0x6C:0x0000 --out patched-nvitem.bin
  python tools\\dsp\\nv_checksum.py --emit <nvitem.bin> --set 0x42:0x1000,0x4A:0x1000,0x6A:0x1011,0x6C:0x0000
                                                               # + sector-RMW commands (NOR 0x680000)
  python tools\\dsp\\nv_checksum_test.py                        # host self-tests

Exit 0: all requested operations succeeded and every verify passed.
Exit 1: verify mismatch / patch validation failure.
Exit 2: usage error.
"""

import argparse
import hashlib
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_DUMP = os.path.join(REPO, "dump_firmware.bin")
DEFAULT_NVITEM = os.path.join(REPO, "stock", "nvitem.bin")
DEFAULT_CHECK = os.path.join(REPO, "tools", "spd_dump", "check.bin")

# ground-truth identities (stale_state guards)
DUMP_SHA256 = "2B62CE1BFBF9C43F80C917BDE4E1130B1A3D24E014DDF0672F138598B6FEF0E6"
NVITEM_SHA256 = "6EC740B7F107FCBAD5C68005F1DCF3602CCA5B654A64EC3E6202EEDC002D4158"

# NV data island geometry (fw-triage + audio-dsp-protocol §NV)
ISLAND_SIZE = 0x9ED4          # nvitem.bin length == dump 0x680000..0x689ED4
DS1_BASE = 0x680000           # NOR base of the NV data island (factory copy)
NOR_FACTORY = 0x680000        # nvitem offset X -> NOR 0x680000 + X
NOR_EFS_HEAD_BLOCK = 0x7F0E6A # user-data EFS Headset block base
EFS_DELTA = 0x7F0E5A          # nvitem offset X -> NOR 0x7F0E5A + X (user copy)
SECTOR = 0x1000
FW_ADDR = 0x30000000          # SC6530C fw_addr (spd_dump.c:1320)
FDL = "nor_fdl1.bin 0x40004000"

# stored field facts (data point ground truth)
DUMP_STORED = 0x5C61          # dump  island field (bytes 0-1 = 61 5C)
NVITEM_STORED = 0x2337        # nvitem island field (bytes 0-1 = 37 23)
RUNTIME_PATCHED = 0x4FC6      # runtime-recomputed field (check.bin bytes 0-1 = C6 4F)


def crc16(crc, buf):
    """The exact SDK FDL crc16() (tf_nv_nor.c:295 / crc16.c): poly 0x8005
    reflected (0xA001), init passed in, no xorout."""
    for b in buf:
        crc2 = 0
        c = (crc ^ b) & 0xFF
        for _ in range(8):
            if (crc2 ^ c) & 1:
                crc2 = (crc2 >> 1) ^ 0xA001
            else:
                crc2 >>= 1
            c >>= 1
        crc = (crc >> 8) ^ crc2
    return crc


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def nvitem_checksum(island):
    """Compute the dataset-1 checksum for an NV island image (>= 4 bytes)."""
    if len(island) < 4:
        raise ValueError("island too short (%d < 4)" % len(island))
    if len(island) != ISLAND_SIZE:
        raise ValueError(
            "island size %d != %d (0x9ED4): the checksum covers the FULL "
            "data island, not a sub-slice" % (len(island), ISLAND_SIZE))
    return crc16(0, island[2:])


def verify_island(island, label):
    """Return (ok, computed, stored) for an island image."""
    computed = nvitem_checksum(island)
    stored = struct.unpack_from("<H", island, 0)[0]
    return computed == stored, computed, stored


def apply_writes(island, writes):
    """Apply [(offset, value_u16)] to a bytearray copy; validate range."""
    out = bytearray(island)
    for off, val in writes:
        if off < 0 or off + 2 > len(out):
            raise ValueError("write @0x%X out of island range" % off)
        out[off:off + 2] = struct.pack("<H", val & 0xFFFF)
    return bytes(out)


def parse_sets(spec):
    """Parse '0x42:0x1000,0x4A:0x1000,...' -> [(off, val)]. LE u16 writes."""
    writes = []
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if ":" not in tok:
            raise ValueError("set token %r: want offset:value" % tok)
        off_s, val_s = tok.split(":", 1)
        off = int(off_s, 0)
        val = int(val_s, 0)
        if not (0 <= val <= 0xFFFF):
            raise ValueError("set token %r: value 0x%X > 0xFFFF" % (tok, val))
        writes.append((off, val))
    return writes


# --------------------------------------------------------------------------
# spd_dump.c command grammar mirrors (eq_patch.py provenance)
# --------------------------------------------------------------------------
def str_to_size_ok(s):
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
    if s.startswith("fw"):
        rest = s[2:]
        if rest == "":
            return True
        if not rest.startswith("+"):
            return False
        return str_to_size_ok(rest[1:])
    return str_to_size_ok(s)


def validate_command(tokens):
    """Mirror eq_patch.validate_command (spd_dump.c grammar)."""
    i = 1
    while i < len(tokens):
        sub = tokens[i]
        if sub == "fdl":
            if i + 3 > len(tokens) or not str_to_size_ok(tokens[i + 2]):
                return "fdl: want 2 args (file addr)"
            i += 3
        elif sub == "write_data":
            if (i + 5 > len(tokens) or not str_to_addr_ok(tokens[i + 1])
                    or not str_to_size_ok(tokens[i + 2])
                    or not str_to_size_ok(tokens[i + 3])):
                return "write_data: want 4 args (addr offset size file)"
            i += 5
        elif sub == "erase_flash":
            if i + 3 > len(tokens) or not str_to_addr_ok(tokens[i + 1]) \
                    or not str_to_size_ok(tokens[i + 2]):
                return "erase_flash: want 2 args (addr size)"
            i += 3
        else:
            return "unknown subcommand %r" % sub
    return None


def emit_commands(sector, sector_file):
    """erase + write per sector (no read_flash -- the FDL read is broken)."""
    return [
        ".\\spd_dump.exe fdl %s erase_flash fw+0x%X 0x%X" % (FDL, sector, SECTOR),
        ".\\spd_dump.exe fdl %s write_data fw+0x%X 0 0 %s" % (FDL, sector, sector_file),
    ]


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="B310E NV data-island checksum tool")
    ap.add_argument("--dump", default=DEFAULT_DUMP)
    ap.add_argument("--island", default=None,
                    help="an NV island image file (default: stock/nvitem.bin)")
    ap.add_argument("--verify", action="store_true",
                    help="verify the island file's checksum field vs its content")
    ap.add_argument("--verify-against-dump", action="store_true",
                    help="verify the dump's factory island (must match 0x5C61)")
    ap.add_argument("--patch", action="store_true",
                    help="apply --set writes + recompute the checksum; write --out")
    ap.add_argument("--emit", action="store_true",
                    help="+ print T8-style sector-RMW NOR commands for the patch")
    ap.add_argument("--set", default=None,
                    help="'0x42:0x1000,0x4A:0x1000,...' u16 LE writes (nvitem offsets)")
    ap.add_argument("--out", default=None, help="patched island output path")
    args = ap.parse_args(argv)

    if args.verify_against_dump:
        dump = open(args.dump, "rb").read()
        if hashlib.sha256(dump).hexdigest().upper() != DUMP_SHA256:
            print("FATAL: stale dump (SHA256 mismatch)", file=sys.stderr)
            return 1
        island = dump[DS1_BASE:DS1_BASE + ISLAND_SIZE]
        ok, comp, stored = verify_island(island, "dump")
        print("dump island [0x%08X:0x%08X): computed=0x%04X stored=0x%04X %s"
              % (DS1_BASE, DS1_BASE + ISLAND_SIZE, comp, stored,
                 "MATCH" if ok else "MISMATCH"))
        print("  expected stored == 0x%04X (the reversed algorithm's ground truth)"
              % DUMP_STORED)
        return 0 if ok else 1

    island_path = args.island or DEFAULT_NVITEM
    island = open(island_path, "rb").read()
    if len(island) != ISLAND_SIZE:
        print("FATAL: %s is %d bytes, expected 0x%X (the NV data island)"
              % (island_path, len(island), ISLAND_SIZE), file=sys.stderr)
        return 2

    writes = parse_sets(args.set) if args.set else []

    if args.verify:
        ok, comp, stored = verify_island(island, island_path)
        print("%s: computed=0x%04X stored=0x%04X %s" % (
            island_path, comp, stored, "VALID" if ok else "STALE (computed != stored)"))
        print("  note: the shipped stock/nvitem.bin is EXPECTED stale (0x2337 "
              "stored vs 0x9745 computed) -- its tail bookkeeping was rewritten "
              "after checksum generation. The dump's factory island validates "
              "(0x5C61).")
        return 0 if ok else 1

    if args.patch or args.emit:
        if not writes:
            print("FATAL: --patch/--emit need --set", file=sys.stderr)
            return 2
        patched = apply_writes(island, writes)
        new_ck = nvitem_checksum(patched)
        old_ck = struct.unpack_from("<H", island, 0)[0]
        patched = patched[:0] + struct.pack("<H", new_ck & 0xFFFF) + patched[2:]
        print("island %s (len 0x%X)" % (island_path, len(island)))
        print("  writes:")
        for off, val in writes:
            before = struct.unpack_from("<H", island, off)[0]
            print("    +0x%04X: 0x%04X -> 0x%04X" % (off, before, val))
        print("  checksum field: 0x%04X -> 0x%04X" % (old_ck, new_ck))
        ok, comp, stored = verify_island(patched, "patched")
        print("  patched island self-consistent: %s (computed 0x%04X stored 0x%04X)"
              % ("YES" if ok else "NO", comp, stored))
        if not ok:
            print("FATAL: patched island does not self-validate -- internal bug",
                  file=sys.stderr)
            return 1

        if args.out:
            with open(args.out, "wb") as f:
                f.write(patched)
            print("  wrote %s" % args.out)
        if args.emit:
            # factory copy @0x680000 (sector), user-data EFS copy @0x7F0E5A+X
            # (only the Headset-region pages are emitted here; full copy 2 is
            # the 0x7F0000 sector set)
            groups = {}
            for off, _v in writes:
                s = (NOR_FACTORY + off) & ~0xFFF
                groups.setdefault(s, []).append(off)
            for s in sorted(groups):
                rels = sorted(groups[s])
                base = "nv-factory-sect-0x%05X" % s
                # build the full 0x1000 sector image for this sector
                sec = bytearray(open(args.dump, "rb").read()[s:s + SECTOR])
                for off, val in writes:
                    if s <= NOR_FACTORY + off < s + SECTOR:
                        sec[NOR_FACTORY + off - s:NOR_FACTORY + off - s + 2] = \
                            struct.pack("<H", val & 0xFFFF)
                # recompute the island checksum for the patched island
                patched_island = apply_writes(island, writes)
                new_ck = nvitem_checksum(patched_island)
                sec[0:2] = struct.pack("<H", new_ck & 0xFFFF)
                pfile = os.path.join(os.getcwd(), base + ".bin")
                with open(pfile, "wb") as f:
                    f.write(bytes(sec))
                print("  sector image %s (factory, %d patched bytes)" % (base, len(sec)))
                for cmd in emit_commands(s, base + ".bin"):
                    print("    " + cmd)
                err = validate_command(cmd.split())
                if err:
                    print("  FATAL: emitted command fails grammar: %s" % err,
                          file=sys.stderr)
                    return 1
            print("  [USER] run from tools\\spd_dump\\; keep the USB cable seated "
                  "between erase_flash and write_data.")
            print("  WARNING: the runtime RE-LAYS sector 0x680000 into 0x400-byte "
                  "page copies beyond page0 (task-B finding) -- only offsets inside "
                  "the first 0x400 bytes of sector 0x680000 (header + Headset + "
                  "Handset-start) survive; block patches at 0x400+ are clobbered.")
        return 0

    print("usage: --verify | --verify-against-dump | (--patch|--emit) --set ... "
          "--out ...", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
