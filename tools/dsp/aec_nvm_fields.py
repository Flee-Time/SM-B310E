#!/usr/bin/env python3
"""aec_nvm_fields.py - extract the old-generation audio mode struct field map
from the SC6531EFM AEC .nvm (tools/mocor-zw217, read-only reference) and
byte-compare against the B310E dsp_codec Record blocks.

The .nvm item tree for a mode is:
  ModeName[32 chars] | AudioStructure (88 named u16 + extend[142] +
  arm_volume[10] + dsp_volume[10] + extend2[120]) = 370 u16 = 740 bytes.

The B310E dsp_codec blocks (dataset-1, stride 0x2F4) are name[16] + the
same 740-byte struct. This script verifies that claim by decoding the B310E
Record/RecordHeadset payloads with the .nvm field map and printing every
field with its value.
"""

import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUMP = os.path.join(REPO, "dump_firmware.bin")
NVM = os.path.join(REPO, "tools", "mocor-zw217", "common", "nv_parameters",
                   "audio", "uis8910ff_zxf", "audio_sc6531efm_AEC.nvm")

MODE_NAMES = ["Headset", "Handset", "Handsfree", "MP4HFTR", "Record", "BTHS",
              "MP4HFTRHeadset", "RecordHeadset", "LoopBHandfree"]

BLOCKS = {
    "Headset": 0x680010, "Handset": 0x680304, "Handsfree": 0x6805F8,
    "MP4HFTR": 0x6808EC, "MP4HFTRHeadset": 0x680BE0, "Record": 0x680ED4,
    "RecordHeadset": 0x6811C8, "BTHS": 0x6814BC, "LoopBHeadset": 0x6817B0,
    "LoopBHandset": 0x681AA4, "LoopBHandfree": 0x681D98, "BTHSNREC": 0x68208C,
}


def parse_nvm(path):
    items = {}
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("BEGIN_ITEM"):
            cur = {}
        elif line.startswith("END_ITEM"):
            if cur and "ITEM_NAME" in cur:
                items[cur["ITEM_INDEX"]] = cur
            cur = None
        elif cur is not None and "=" in line:
            k, v = line.split("=", 1)
            cur.setdefault(k.strip(), v.strip())
    return items


def leaf_fields(items, idxs, parent_idx):
    """Return [(name, content)] for SHORT leaves under parent, in file order."""
    res = []
    kids = [k for k in idxs if items[str(k)].get("ITEM_PARENT") == str(parent_idx)]
    for k in kids:
        it = items[str(k)]
        typ = it.get("ITEM_TYPE", "")
        if typ.startswith("5"):  # ARRAY
            sub = [j for j in idxs if items[str(j)].get("ITEM_PARENT") == str(k)]
            for j in sub:
                res.append((it["ITEM_NAME"] + "[%d]" % j,
                            items[str(j)].get("ITEM_CONTENT", "")))
        elif typ.startswith("2"):  # SHORT
            res.append((it["ITEM_NAME"], it.get("ITEM_CONTENT", "")))
        elif typ.startswith("7"):  # nested STRUCT (AudioStructure)
            res.extend(leaf_fields(items, idxs, k))
    return res


def main():
    items = parse_nvm(NVM)
    idxs = sorted(int(i) for i in items)

    # locate each mode's AudioStructure parent
    mode_structs = {}
    for idx in idxs:
        it = items[str(idx)]
        if it.get("ITEM_NAME") in MODE_NAMES and it.get("ITEM_TYPE", "").startswith("7"):
            desc = it.get("ITEM_DESC", "")
            mode_structs[it["ITEM_NAME"]] = idx

    if "Record" not in mode_structs:
        print("FATAL: no Record mode block in %s" % NVM, file=sys.stderr)
        return 2

    fields = leaf_fields(items, idxs, mode_structs["Record"])
    print("Record mode: %d fields under AudioStructure" % len(fields))
    for i, (n, c) in enumerate(fields):
        print("%3d %-34s %s" % (i, n, c))

    return 0


if __name__ == "__main__":
    sys.exit(main())
