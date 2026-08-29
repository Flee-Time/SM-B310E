#!/usr/bin/env python3
"""verify_extract.py - T1 (b310e-audio-eq-tune): byte-verify the
.omo/dumpmine/dsp/ extraction artifacts against dump_firmware.bin.

Each artifact is checked against its corresponding dump slice:
  - "raw" artifacts  : byte-compared against dump[offset:offset+len(artifact)]
  - "drps" artifacts : CAPN-decompressed outputs, re-derived by re-running
                       fphelper.exe drps_dec <dump> 0x12e524 <n> <out> and
                       byte-compared against the re-derived output.

Exit 0: every artifact matches. Exit 1: any mismatch (message printed).
Python 3 stdlib only (no external deps). Windows/PowerShell friendly.

Usage:
  python tools\\dsp\\verify_extract.py
  python tools\\dsp\\verify_extract.py --dump <path> --dsp-dir <path> --fphelper <path>
"""

import argparse
import os
import subprocess
import sys
import tempfile

# repo root = <repo>/tools/dsp/verify_extract.py -> 3 levels up
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEFAULT_DUMP = os.path.join(REPO, "dump_firmware.bin")
DEFAULT_DSP_DIR = os.path.join(REPO, ".omo", "dumpmine", "dsp")
DEFAULT_FPHELPER = os.path.join(REPO, "tools", "spd_dump", "fphelper.exe")

# (artifact, kind, slice_offset, slice_length, note)
# slice_length for "raw" = the declared extraction length (ground truth from the
# 2026-08-26 probe); a truncated/extended artifact FAILS the length check even if
# its bytes are a prefix/superset of the dump slice.
# slice_offset for "drps" = the DRPS container offset in the dump (source of the
# CAPN blobs); the artifact itself is the decompressed output, not a raw slice.
ARTIFACTS = [
    ("dsp_code_full.bin",    "raw",  0x570000, 0x11C000, "DSP_CODE partition (0x570000-0x68C000, 0x11C000 B)"),
    ("dsp_code_660000.bin",  "raw",  0x660000, 0x20000,  "DSP code tail (0x660000-0x680000)"),
    ("dsp_after_strings.bin", "raw", 0x74C00,  0x500,    "downloader-area region (0x74C00-0x75100)"),
    ("dsp_codec_full.bin",   "raw",  0x87760,  0x1000,   "ARM audio_nv_dsp.c module region (0x87760-0x88760)"),
    ("dsp_codec_arm.bin",    "raw",  0x87760,  0x400,    "ARM dsp_codec walker region (0x87760-0x87B60, sub-slice)"),
    ("dsp_dl_code.bin",      "raw",  0x73E00,  0xA00,    "DSP downloader code (0x73E00-0x74800)"),
    ("dsp_downloader.bin",   "raw",  0x74800,  0x600,    "layer1_dsp_download fn (0x74800-0x74E00)"),
    ("dsp_loader_fn.bin",    "raw",  0x74600,  0x224,    "loader fn, inside downloader region (0x74600-0x74824)"),
    ("dsp_parse_fn.bin",     "raw",  0x74400,  0x40C,    "parse fn, inside downloader region (0x74400-0x7480C)"),
    ("drps_12E524_0.bin",    "drps", 0x12E524, 0,        "CAPN blob 0 of DRPS container @0x12E524 (fphelper drps_dec 0x12e524 0)"),
    ("drps_12E524_1.bin",    "drps", 0x12E524, 0,        "CAPN blob 1 of DRPS container @0x12E524 (fphelper drps_dec 0x12e524 1)"),
]


def read_ok(path):
    try:
        with open(path, "rb") as f:
            return f.read(), None
    except OSError as e:
        return None, str(e)


def rederive_drps(dump_path, fphelper_path, container_off, blob_idx, artifact_path, tmp_dir):
    """Re-run fphelper drps_dec and return (bytes, err)."""
    out = os.path.join(tmp_dir, "drps_redo_%d.bin" % blob_idx)
    if not os.path.isfile(fphelper_path):
        return None, "fphelper.exe not found: %s" % fphelper_path
    cmd = [fphelper_path, dump_path, "drps_dec", "0x%x" % container_off,
           str(blob_idx), out]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        return None, "fphelper drps_dec failed (rc=%d): %s" % (r.returncode, r.stderr.strip() or r.stdout.strip())
    if not os.path.isfile(out):
        return None, "fphelper drps_dec produced no output file"
    with open(out, "rb") as f:
        return f.read(), None


def verify_artifact(dump, dump_path, dsp_dir, fphelper, artifact, tmp_dir):
    name, kind, off, decl_len, note = artifact
    art, err = read_ok(os.path.join(dsp_dir, name))
    if err:
        return name, "ERROR", "artifact unreadable: %s" % err, note
    if kind == "raw":
        if len(art) != decl_len:
            return name, "MISMATCH", (
                "length %#x != declared slice length %#x (truncated/extended artifact)" % (len(art), decl_len)), note
        if off < 0 or off + decl_len > len(dump):
            return name, "ERROR", "slice 0x%08X+0x%X out of dump range" % (off, decl_len), note
        sl = dump[off:off + decl_len]
        if art == sl:
            return name, "OK", "dump[0x%08X:+0x%X] byte-identical" % (off, decl_len), note
        d = next((i for i in range(decl_len) if art[i] != sl[i]), decl_len)
        return name, "MISMATCH", (
            "dump[0x%08X:+0x%X] differs at +0x%X "
            "(artifact=%#04x dump=%#04x)" % (off, decl_len, d, art[d], sl[d])), note
    else:  # drps: derived data - re-derive and compare
        redo, err2 = rederive_drps(dump_path, fphelper, off, int(name.rsplit("_", 1)[1].split(".")[0]),
                                   os.path.join(dsp_dir, name), tmp_dir)
        if err2:
            return name, "ERROR", err2, note
        if art == redo:
            return name, "OK", "re-derived fphelper drps_dec output byte-identical (src DRPS @0x%08X)" % off, note
        d = next((i for i in range(min(len(art), len(redo))) if art[i] != redo[i]), min(len(art), len(redo)))
        return name, "MISMATCH", (
            "fphelper re-derivation differs at +0x%X "
            "(artifact=%#04x redo=%#04x)" % (d, art[d], redo[d])), note


def main(argv=None):
    ap = argparse.ArgumentParser(description="Verify .omo/dumpmine/dsp artifacts against dump_firmware.bin")
    ap.add_argument("--dump", default=DEFAULT_DUMP, help="dump_firmware.bin path")
    ap.add_argument("--dsp-dir", default=DEFAULT_DSP_DIR, help="artifacts directory")
    ap.add_argument("--fphelper", default=DEFAULT_FPHELPER, help="fphelper.exe path (drps artifacts)")
    args = ap.parse_args(argv)

    dump, err = read_ok(args.dump)
    if err:
        print("FATAL: cannot read dump: %s" % err, file=sys.stderr)
        return 2
    if not os.path.isdir(args.dsp_dir):
        print("FATAL: artifacts dir not found: %s" % args.dsp_dir, file=sys.stderr)
        return 2

    results = []
    with tempfile.TemporaryDirectory(prefix="verify_extract_") as tmp:
        for artifact in ARTIFACTS:
            results.append(verify_artifact(dump, args.dump, args.dsp_dir, args.fphelper, artifact, tmp))

    fails = 0
    for name, status, detail, note in results:
        print("%-22s %-9s %s" % (name, status, detail))
        if status != "OK":
            fails += 1

    n = len(results)
    if fails:
        print("\nFAIL: %d of %d artifacts MISMATCH/ERROR - extraction is stale or the slice table is wrong" % (fails, n))
        return 1
    print("\nall %d artifacts match their dump slices" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
