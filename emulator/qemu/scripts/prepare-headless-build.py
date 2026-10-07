#!/usr/bin/env python3
"""Allow a local Windows QEMU build without creating an install bundle."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu_source", type=Path)
    args = parser.parse_args()
    path = args.qemu_source / "scripts/symlink-install-tree.py"
    source = path.read_text(encoding="utf-8")
    if "getattr(e, 'winerror', None) == 1314" in source:
        print("Windows install-bundle workaround already present")
        return
    anchor = "    except OSError as e:\n"
    if source.count(anchor) != 1:
        raise RuntimeError("Unexpected QEMU symlink helper; use pinned v11.1.0")
    source = source.replace(anchor, anchor +
        "        if os.name == 'nt' and getattr(e, 'winerror', None) == 1314:\n"
        "            # B310E local headless build: no install bundle is needed.\n"
        "            continue\n")
    path.write_text(source, encoding="utf-8", newline="\n")
    print("Prepared Windows headless build; make install bundle is unavailable")


if __name__ == "__main__":
    main()
