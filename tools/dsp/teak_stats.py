#!/usr/bin/env python3
"""Run teak_disasm over regions of the DSP image and aggregate decode-quality stats.

For each region: instruction count, [ERROR] count, distinct mnemonics,
top-10 mnemonics, and fraction of branch targets landing inside the region.
"""
import os, subprocess, re, collections, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEAK = os.path.join(REPO, "build", "teakra", "build", "teak_disasm.exe")
IMG = os.path.join(REPO, ".omo", "dumpmine", "dsp", "dsp_code_full.bin")
if not os.path.exists(TEAK):
    sys.exit("teak_disasm.exe not found at %s - clone + build teakra into build/teakra (tools/dsp/TOOLING.md)" % TEAK)

def analyze(start_hex, count, base_hex=0, chunk=800):
    done = 0
    pos = int(start_hex, 16)
    total = int(count)
    mnemonics = collections.Counter()
    errors = 0
    instrs = 0
    branch_targets = []
    expansions = 0
    while done < total:
        n = min(chunk, total - done)
        out = subprocess.run([TEAK, IMG, "%X" % pos, str(n), "0"],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if not line or line.startswith("^^"):
                continue
            m = re.match(r"^[0-9A-F]{8}\s+[0-9A-F]{4}\s+(.*)$", line)
            if not m:
                continue
            body = m.group(1)
            instrs += 1
            if "[ERROR]" in body:
                errors += 1
            toks = body.split()
            if toks:
                mnemonics[toks[0]] += 1
            # branch targets: brr/callr/brs with hex target
            bt = re.search(r"\b(?:brr|callr|brs)\s+(?:0x)?([0-9a-f]+)", body)
            if bt:
                branch_targets.append(int(bt.group(1), 16))
        done += n
        pos += n * 2
    print("=== region start 0x%s count %d ===" % (start_hex, total))
    print("instructions: %d, [ERROR]: %d (%.1f%%), expansions consumed: %d" %
          (instrs, errors, 100.0*errors/max(instrs,1), expansions))
    print("distinct mnemonics: %d" % len(mnemonics))
    for m, c in mnemonics.most_common(12):
        print("  %-12s %d" % (m, c))
    if branch_targets:
        lo, hi = min(branch_targets), max(branch_targets)
        print("branch targets: n=%d range 0x%x-0x%x" % (len(branch_targets), lo, hi))

if __name__ == "__main__":
    for arg in sys.argv[1:]:
        parts = arg.split(":")
        if len(parts) == 2:
            analyze(parts[0], parts[1])
        else:
            print("usage: analyze <start_hex>:<count> ...")
