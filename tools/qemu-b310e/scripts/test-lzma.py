#!/usr/bin/env python3
"""Check the accelerator against an independently encoded stream and stock page."""
import argparse
import hashlib
from pathlib import Path
import runpy

machine = runpy.run_path(str(Path(__file__).with_name("test-audio.py")))["machine"]
ZIP = 0x20e00000
SOURCE, DEST, BUFFER = 0x34010000, 0x34020000, 0x34030000
# Our test text, encoded with the public-domain LZMA SDK (plain literals,
# separate length tables, 11-bit probabilities, lc=lp=0, pb=2). Verified
# independently with the SDK decoder; contains matches and repeat matches.
STREAM = bytes.fromhex(
    "5a00100000280200000000000000299371e6aacc63eeff757ff3b7d55fee19cfa19f4"
    "88360a4bf758fe0f86afff3b0a9e130f1f95f104cb97076195b6c85c4e4b647efebc"
    "2eaea7b52b700")
TEXT = b"".join(f"SM-B310E PCM buffer {i % 3:02d}: left=1234 right=-1234\n".encode()
                for i in range(12))


def memory(qt, address, size):
    return bytes.fromhex(qt.command(f"read {address:#x} {size:#x}")[0][2:])


def decode(qt, data, size, start=0, ctl=0, unbounded=False):
    qt.write(ZIP + 8, 0x1f1f)  # W1C errors/finish, enable every source
    qt.memory(SOURCE, data)
    for offset, value in [(0xc, SOURCE), (0x10, DEST),
                          (0x14, 0 if unbounded else len(data)), (0x18, size),
                          (0x1c, BUFFER), (0x20, len(TEXT)), (0x24, start)]:
        qt.write(ZIP + offset, value)
    qt.write(ZIP, ctl | 1)
    return (qt.read(ZIP + 8) >> 24) & 31


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--firmware", type=Path)
    args = parser.parse_args()
    args.qemu, args.output = args.qemu.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with machine(args, "lzma") as (qt, qmp, out):
        qt.write(0x80000008, 1 << 30)
        assert decode(qt, STREAM, len(TEXT)) == 1
        assert memory(qt, DEST, len(TEXT)) == TEXT
        assert qt.read(ZIP + 0x30) == len(TEXT)
        assert qt.read(ZIP + 0x2c) == len(TEXT)
        assert qt.read(0x80000000) & (1 << 30)
        qt.write(ZIP + 8, 0x11f)
        assert qt.read(ZIP + 8) == 31
        assert not qt.read(0x80000000) & (1 << 30)
        assert decode(qt, STREAM, 100, start=150, ctl=12) == 1
        assert memory(qt, DEST, 100) == TEXT[150:250]
        assert memory(qt, BUFFER, len(TEXT)) == TEXT
        qt.write(ZIP + 8, 0x1f1f)
        qt.write(ZIP + 0x20, 10)  # reject an undersized intermediate buffer
        qt.memory(BUFFER, b"\xcc" * len(TEXT))
        qt.write(ZIP, 5)
        assert qt.read(ZIP + 8) >> 24 == 8
        assert memory(qt, BUFFER, len(TEXT)) == b"\xcc" * len(TEXT)
        # SRC_LEN=0 is used by the stock DSP loader; mapping bounds apply.
        assert decode(qt, STREAM, len(TEXT), unbounded=True) == 1
        assert memory(qt, DEST, len(TEXT)) == TEXT
        qt.memory(DEST, b"\xcc" * len(TEXT))
        assert decode(qt, STREAM[:18], len(TEXT)) == 4
        assert memory(qt, DEST, len(TEXT)) == b"\xcc" * len(TEXT)
        assert decode(qt, STREAM, 0) == 8
        assert decode(qt, STREAM, 100, start=len(TEXT)) == 4
        qt.write(ZIP + 0xc, 0x82003000)
        qt.write(ZIP + 0x14, 0)
        qt.write(ZIP, 1)
        assert qt.read(ZIP + 8) & (2 << 24)
        qmp.command("system_reset")
        assert qt.read(ZIP + 8) == 0
        if args.firmware:
            dump = args.firmware.read_bytes()
            assert hashlib.sha256(dump).hexdigest() == (
                "5e44e0858d4eacbd1c0124d3bb8d152cb429c9222a99b42f8f75c86300846c92")
            assert decode(qt, dump[0x1a216c:0x1a216c + 0x9b4], 4096) == 1
            assert hashlib.sha256(memory(qt, DEST, 4096)).hexdigest() == (
                "71082b67a8156ec3bd3cfc02a14325e6747c373ac955ff4e7a84c2c21033a3a7")
            print("PASS stock compressed page: independently verified SHA256")
            assert decode(qt, dump[0xcc874:0xcc874 + 65300], 307200) == 1
            assert hashlib.sha256(memory(qt, DEST, 267484)).hexdigest() == (
                "4bfbfaed86b6c6e66addfa6b4877af42ac5629d866b65720abd733134fc98294")
            print("PASS stock DSP image: 267484 bytes, independently verified SHA256")
    print("PASS LZMA: exact output, slice/buffer, zero source length, IRQ/W1C, errors/reset")


if __name__ == "__main__":
    main()
