#!/usr/bin/env python3
"""Check stock LCD layers, source pitch and retained partial panel updates."""
import argparse
from pathlib import Path
import runpy
import struct

machine = runpy.run_path(str(Path(__file__).with_name("test-audio.py")))["machine"]
LCD = 0x20d00000


def window(qt, x, y, w, h):
    for cmd, start, end in [(0x2a, x, x+w-1), (0x2b, y, y+h-1)]:
        qt.write(0x60000000, cmd)
        for byte in [start >> 8, start & 255, end >> 8, end & 255]:
            qt.write(0x60020000, byte)
    qt.write(0x60000000, 0x2c)


def pixels(qmp, path):
    qmp.command("screendump", {"filename": str(path), "format": "ppm"})
    data = path.read_bytes()
    assert data.startswith(b"P6\n128 160\n255\n")
    return data[-128*160*3:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.qemu = args.qemu.resolve()
    args.output = args.output.resolve()
    with machine(args, "partial-layers") as (qt, qmp, out):
        image = out / "panel.ppm"
        qt.memory(0x34040000, b"\x00\xf8" * (128*160))
        for offset, value in [(4,128|(160<<16)), (8,0), (12,128|(160<<16)),
                              (0x20,0x251), (0x24,0x34040000>>2),
                              (0x2c,128|(160<<16)), (0x30,128)]:
            qt.write(LCD+offset, value)
        window(qt, 0, 0, 128, 160)
        qt.write(LCD, 8)
        original = pixels(qmp, image)
        assert original == b"\xf8\0\0" * (128*160)
        # The old image allocation has been freed. Only OSD2 is enabled.
        qt.memory(0x34040000, b"\0" * (128*160*2))
        qt.write(LCD+0x20, 0x250)
        qt.write(LCD+0x24, 0xffffffff)
        qt.memory(0x34050000, struct.pack("<8H", 31,0x7e0,0,0,0xffff,0xf800,0,0))
        for offset, value in [(4,2|(2<<16)), (12,2|(2<<16)), (0x80,0x255),
                              (0x84,0x34050000>>2), (0x8c,2|(2<<16)),
                              (0x90,4), (0x94,0), (0x98,255)]:
            qt.write(LCD+offset,value)
        window(qt,10,20,2,2)
        qt.write(LCD,8)
        actual = pixels(qmp,image)
        expected = bytearray(original)
        for x,y,rgb in [(10,20,b"\0\0\xf8"), (11,20,b"\0\xfc\0"),
                         (10,21,b"\xf8\xfc\xf8"), (11,21,b"\xf8\0\0")]:
            i = (y*128+x)*3
            expected[i:i+3] = rgb
        assert actual == expected, "OSD2, stride or partial panel preservation"
        # Source layer position and LCM crop use display coordinates;
        # CASET/RASET selects a separate destination in panel GRAM.
        qt.write(LCD+4,4|(4<<16))
        qt.write(LCD+8,1|(1<<16))
        qt.write(LCD+0x94,1|(1<<16))
        window(qt,30,40,2,2)
        qt.write(LCD,8)
        actual = pixels(qmp,image)
        for y in range(2):
            src = ((20+y)*128+10)*3
            dst = ((40+y)*128+30)*3
            expected[dst:dst+6] = expected[src:src+6]
        assert actual == expected, "layer position / source crop"
        # Color key reveals the background, and block alpha blends once.
        qt.write(LCD+8,0)
        qt.write(LCD+0x94,0)
        qt.write(LCD+4,1|(1<<16))
        qt.write(LCD+12,1|(1<<16))
        qt.write(LCD+0x10,0xf8)
        qt.memory(0x34050000,b"\0\xf8")
        qt.write(LCD+0x80,0x257)
        qt.write(LCD+0xa0,0xf80000)
        window(qt,50,50,1,1)
        qt.write(LCD,8)
        actual = pixels(qmp,image)
        assert actual[(50*128+50)*3:(50*128+50)*3+3] == b"\0\0\xf8"
        qt.write(LCD+0x80,0x255)
        qt.write(LCD+0x98,128)
        window(qt,51,50,1,1)
        qt.write(LCD,8)
        actual = pixels(qmp,image)
        assert actual[(50*128+51)*3:(50*128+51)*3+3] == b"\x78\0\x78"
        # Direct 8-bit DBI RGB565 writes share the same retained panel.
        window(qt,60,60,1,1)
        qt.write(0x60020000,0x07)
        qt.write(0x60020000,0xe0)
        actual = pixels(qmp,image)
        assert actual[(60*128+60)*3:(60*128+60)*3+3] == b"\0\xfc\0"
        # Stock composes into RAM, then displays that buffer as OSD2.
        # A capture must write memory and leave the visible panel alone.
        before_capture = actual
        qt.write(LCD+0x80, 0x254)
        qt.write(LCD+0x20, 0x251)
        qt.write(LCD+0x24, 0x34060000 >> 2)
        qt.write(LCD+0x2c, 2 | (2 << 16))
        qt.write(LCD+0x30, 4)
        qt.memory(0x34060000, struct.pack("<8H", 31,0x7e0,0,0,0xffff,0xf800,0,0))
        for offset,value in [(4,2|(2<<16)), (0xe0,0x55), (0xe4,0x34070000>>2),
                             (0xe8,0), (0xec,2|(2<<16)), (0xf0,4)]:
            qt.write(LCD+offset,value)
        qt.write(LCD,8)
        assert qt.read(0x34070000) == 0x07e0001f
        assert qt.read(0x34070008) == 0xf800ffff
        assert pixels(qmp,image) == before_capture
        qt.write(LCD+0xe0,0x54)
        qt.write(LCD+0x20,0x250)
        qt.write(LCD+0x80,0x255)
        qt.write(LCD+0x84,0x34070000>>2)
        qt.write(LCD+0x98,255)
        qt.write(LCD+12,2|(2<<16))
        window(qt,70,80,2,2)
        qt.write(LCD,8)
        actual = pixels(qmp,image)
        assert actual[(80*128+70)*3:(80*128+70)*3+6] == b"\0\0\xf8\0\xfc\0"
        qmp.command("system_reset")
        assert pixels(qmp,image) == b"\0" * (128*160*3)
    print("PASS LCD disabled image, OSD2, pitch, crop, panel window, color key, alpha, RAM capture, DBI and reset")


if __name__ == "__main__":
    main()
