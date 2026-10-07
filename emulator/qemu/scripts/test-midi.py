#!/usr/bin/env python3
"""Exercise wavetable DMA with our own samples and 96-byte voice descriptors."""
import argparse
from pathlib import Path
import runpy
import struct

audio = runpy.run_path(str(Path(__file__).with_name("test-audio.py")))
lzma = runpy.run_path(str(Path(__file__).with_name("test-lzma.py")))
MIDI, INTC = 0x20b00000, 0x80000000
VOICE, WAVE, OUTPUT = 0x34010000, 0x34020000, 0x34030000


def memory(qt, addr, size):
    return bytes.fromhex(qt.command(f"read {addr:#x} {size:#x}")[0][2:])


def descriptor(sample16=False, next_voice=0xffffffff, looping=True):
    voice = bytearray(96)
    struct.pack_into("<Ihh", voice, 4, next_voice, 32767, 0)
    voice[0xc:0x10] = bytes([0, 127, 8, 2])
    # Sustain, full channel/pan gain, exactly one sample per output frame.
    target = (((((127 * 256) ** 2 >> 15) * 32767 >> 15) * 32767 >> 15) * 32767 >> 15)
    struct.pack_into("<hh", voice, 0x10, target, 32767)
    voice[0x1c:0x1e] = b"\x05\x05"
    struct.pack_into("<hh", voice, 0x20, 32767, 0)
    struct.pack_into("<hh", voice, 0x24, 32767, 0)
    end = WAVE + 3 * (2 if sample16 else 1)
    struct.pack_into("<IIII", voice, 0x28, WAVE if looping else end, end, WAVE, 0)
    struct.pack_into("<hh", voice, 0x38, 32767, 32767)
    return voice, target


def start(qt, voices=1, stereo=True, sample16=False):
    qt.write(MIDI + 4, ((voices - 1) << 16) | int(stereo) | int(sample16) * 2)
    qt.write(MIDI + 0x10, VOICE)
    qt.write(MIDI + 0x14, OUTPUT)
    qt.write(MIDI, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.qemu, args.output = args.qemu.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    nor = args.output / "samples.bin"
    nor.write_bytes(bytes(4096) + bytes([192, 0, 64, 127]))
    with audio["machine"](args, "midi-nor", nor=nor) as (qt, qmp, out):
        voice, target = descriptor()
        struct.pack_into("<III", voice, 0x28, 4096, 4099, 4096)
        qt.memory(VOICE, voice)
        start(qt)
        first = struct.unpack("<i", memory(qt, OUTPUT, 4))[0]
        assert first == (((-4096 * target) >> 14) * 32767 >> 4)
    with audio["machine"](args, "midi") as (qt, qmp, out):
        qt.write(INTC + 8, 1 << 30)
        qt.write(MIDI + 8, 1)
        for sample16, stereo in [(False, True), (True, True), (False, False)]:
            values = [-16384, 0, 16384, 32512]
            qt.memory(WAVE, struct.pack("<4h", *values) if sample16 else bytes([192, 0, 64, 127]))
            voice, target = descriptor(sample16)
            qt.memory(VOICE, voice)
            start(qt, sample16=sample16, stereo=stereo)
            channels = 2 if stereo else 1
            pcm = struct.unpack(f"<{32 * channels}i", memory(qt, OUTPUT, 32 * channels * 4))
            expected = [(((v >> 2) * target >> 14) * 32767 >> 4) if stereo
                        else ((v >> 2) * target >> 3) for v in values]
            assert pcm == tuple(value for i in range(32) for value in [expected[i % 4]] * channels)
            updated = memory(qt, VOICE, 96)
            assert updated[0xe] == 0 and updated[0] == 0
            assert struct.unpack_from("<II", updated, 0x30) == (WAVE, 0)
            assert qt.read(MIDI) & 1 == 0
            assert qt.read(MIDI + 0xc) == 1
            assert qt.read(MIDI + 8) == 0x1010001
            assert qt.read(INTC) & (1 << 30)
            qt.write(MIDI + 8, 0x101)
            assert qt.read(MIDI + 8) == 1 and not qt.read(INTC) & (1 << 30)
        # Two linked voices add on the 32-bit bus, then terminate at -1.
        voice, _ = descriptor(next_voice=VOICE + 96)
        second, _ = descriptor()
        qt.memory(VOICE, voice + second)
        start(qt, voices=2)
        pcm = struct.unpack("<64i", memory(qt, OUTPUT, 256))
        assert pcm[0] == (((values[0] >> 2) * target >> 14) * 32767 >> 4) * 2
        assert qt.read(MIDI + 0xc) == 2
        qt.write(MIDI + 8, 0x101)
        # An octave down advances half a sample and interpolates between points.
        voice, target = descriptor()
        struct.pack_into("<h", voice, 0xa, -1200)
        qt.memory(VOICE, voice)
        start(qt)
        pcm = struct.unpack("<64i", memory(qt, OUTPUT, 256))
        linear = [v >> 2 for v in values]
        half = [linear[i // 2] if i % 2 == 0 else
                (linear[i // 2] + linear[(i // 2 + 1) % 4]) // 2 for i in range(8)]
        expected_half = [((v * target >> 14) * 32767 >> 4) for v in half]
        assert pcm == tuple(expected_half[i % 8] for i in range(32) for _ in range(2))
        assert struct.unpack_from("<II", memory(qt, VOICE, 96), 0x30) == (WAVE, 0)
        qt.write(MIDI + 8, 0x101)
        # Attack reaches peak and changes to decay; release eventually ends.
        voice, _ = descriptor()
        voice[0x1c] = 2
        struct.pack_into("<hh", voice, 0x20, 0, 32767)
        struct.pack_into("<hh", voice, 0x50, 16384, 0)
        qt.memory(VOICE, voice)
        start(qt)
        updated = memory(qt, VOICE, 96)
        assert updated[0x1c] == 4
        assert struct.unpack_from("<hh", updated, 0x20) == (32767, 16384)
        updated = bytearray(updated)
        updated[0x1c] = 6
        struct.pack_into("<h", updated, 0x22, 0)
        qt.memory(VOICE, updated)
        start(qt)
        updated = memory(qt, VOICE, 96)
        assert updated[0] == 1 and updated[0x1c] == 8
        qt.write(MIDI + 8, 0x101)
        # Last sample ends an unlooped voice and returns done to the guest.
        voice, _ = descriptor(looping=False)
        qt.memory(VOICE, voice)
        start(qt)
        assert memory(qt, VOICE, 1) == b"\x01"
        assert memory(qt, OUTPUT + 32, 224) == bytes(224)
        qt.write(MIDI + 8, 0x101)
        # Both accelerators share IRQ30: clearing either must preserve the other.
        qt.memory(VOICE, second)
        start(qt)
        assert lzma["decode"](qt, lzma["STREAM"], len(lzma["TEXT"])) == 1
        qt.write(MIDI + 8, 0x101)
        assert qt.read(INTC) & (1 << 30)
        qt.memory(VOICE, second)
        start(qt)
        qt.write(lzma["ZIP"] + 8, 0x11f)
        assert qt.read(INTC) & (1 << 30)
        qt.write(MIDI + 8, 0x101)
        assert not qt.read(INTC) & (1 << 30)
        # A cycle/malformed guest pointer completes boundedly without MMIO DMA.
        voice, _ = descriptor(next_voice=VOICE)
        qt.memory(VOICE, voice)
        start(qt, voices=64)
        assert qt.read(MIDI) == 0
        qt.write(MIDI + 0x10, MIDI)
        qt.write(MIDI, 1)
        assert qt.read(MIDI) == 0
        qmp.command("system_reset")
        assert qt.read(MIDI + 8) == qt.read(MIDI + 0xc) == 0
        assert not qt.read(INTC) & (1 << 30)
    print("PASS MIDI: exact 8/16-bit stereo/mono PCM, linked voices, end/state, IRQ/W1C/shared IRQ/reset/bounds")


if __name__ == "__main__":
    main()
