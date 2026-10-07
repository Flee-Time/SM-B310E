#!/usr/bin/env python3
"""Integration checks of SC6530 MMIO, DMA, ping-pong playback and WAV output."""
import argparse
from array import array
from contextlib import contextmanager
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import wave
import runpy

QMP = runpy.run_path(str(Path(__file__).with_name("capture-stock.py")))["QMP"]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class QTest:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.stream = self.sock.makefile("rwb")

    def command(self, command):
        self.stream.write(command.encode() + b"\n")
        self.stream.flush()
        while True:
            result = self.stream.readline().decode().strip()
            if result.startswith("IRQ "):
                continue
            if not result.startswith("OK"):
                raise RuntimeError(result)
            return result.split()[1:]

    def write(self, addr, val):
        self.command(f"writel {addr:#x} {val:#x}")

    def read(self, addr):
        return int(self.command(f"readl {addr:#x}")[0], 0)

    def memory(self, addr, data):
        self.command(f"write {addr:#x} {len(data):#x} 0x{data.hex()}")

    def advance(self, ns):
        self.command(f"clock_step {ns}")

    def close(self):
        self.stream.close()
        self.sock.close()


@contextmanager
def machine(args, name, rate=8000, nor=None):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    qmp_port, test_port = free_port(), free_port()
    command = [str(args.qemu), "-M", "b310e,boot-mode=ours", "-display", "none",
               "-serial", "none", "-accel", "qtest", "-qmp",
               f"tcp:127.0.0.1:{qmp_port},server=on,wait=off", "-qtest",
               f"tcp:127.0.0.1:{test_port},server=on,wait=off", "-qtest-log",
               str(out / "qtest.log"), "-D", str(out / "trace.log"),
               "--trace", "sc6530_vbc_*", "--trace", "sc6530_dma_*",
               "-audiodev", f"wav,id=audio0,path={(out / 'audio.wav').as_posix()},out.frequency={rate},out.channels=2,out.format=s16",
               "-global", "sc6530_adi.audiodev=audio0"]
    if nor:
        command += ["-drive", f"file={nor.as_posix()},format=raw,if=none,id=nor,readonly=on"]
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = qt = None
    with (out / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited; see {out / 'stderr.log'}")
                try:
                    qmp = QMP(qmp_port)
                    break
                except OSError:
                    time.sleep(0.05)
            if qmp is None:
                raise RuntimeError("QMP startup timed out")
            qt = QTest(test_port)
            yield qt, qmp, out
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def configure(qt, mode=10, owned=True):
    qt.write(0x82001440, 4)
    qt.write(0x82001440, 1)
    qt.write(0x82001440, 2)  # SET aliases retain the audio clock
    qt.write(0x8b0001c4, 4 if owned else 0)
    qt.write(0x8a002000, 5)
    qt.write(0x8a00200c, mode | 0x8000)  # mute controller enabled, mute request clear
    qt.write(0x82003010, 63 << 8)


def samples(path):
    with wave.open(str(path), "rb") as wav:
        assert wav.getsampwidth() == 2 and wav.getnchannels() == 2
        rate = wav.getframerate()
        data = array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            data.byteswap()
        return rate, list(zip(data[::2], data[1::2]))


def test_cpu(args, mode=10, rate=8000, muted=False, owned=True):
    state = "dsp-owned-silent" if not owned else "muted-arm" if muted else "audible-arm"
    name = f"cpu-{rate}-{state}"
    with machine(args, name, rate) as (qt, qmp, out):
        configure(qt, mode, owned)
        if muted:
            qt.write(0x8a00200c, mode | 0xc000)
        for bank in range(2):
            qt.write(0x82003018, 0x400 | (bank << 9))
            for i in range(64):
                sample = 6000 if i % 16 < 8 else -6000
                qt.write(0x82003000, sample & 0xffff)
                qt.write(0x82003004, (-sample) & 0xffff)
        qt.write(0x82003018, 0x8000)
        qmp.command("cont")
        for _ in range(100):
            qt.advance(1_000_000)
        # A running bank indicator must have advanced with the sample clock.
        assert (qt.read(0x82003018) >> 9) & 1 == (rate // 10 // 64) & 1
        byte = int(qt.command("readb 0x82003019")[0], 16)
        assert byte == qt.read(0x82003018) >> 8
        qt.write(0x82003018, 0)
        qt.advance(20_000_000)
    actual_rate, pcm = samples(out / "audio.wav")
    assert actual_rate == rate and len(pcm) >= rate // 20, (actual_rate, len(pcm))
    if muted or not owned:
        assert all(l == r == 0 for l, r in pcm)
    else:
        nonzero = [(l, r) for l, r in pcm if l or r]
        assert len(nonzero) >= rate // 20
        assert all(l == -r and abs(l) == 6000 for l, r in nonzero)
        assert {l for l, _ in nonzero} == {-6000, 6000}
    print(f"PASS {name}: {len(pcm)} stereo frames")


def test_dma(args, standard=False, clocked=True):
    channels = [2, 3] if standard else [24, 25]
    name = "dma-no-analog-clock" if not clocked else "dma-standard" if standard else "dma-full"
    with machine(args, name) as (qt, qmp, out):
        configure(qt)
        if not clocked:
            qt.write(0x82001444, 4)
            qt.write(0x82001440, 3)  # enabling DAC paths cannot re-enable clock
        qt.write(0x80000008, 1 << 20)
        qt.write(0x20102038, channels[0] + 1)
        qt.write(0x2010203c, channels[1] + 1)
        for ch, source, destination, sign in [(channels[0], 0x34010000, 0x82003000, 1),
                                               (channels[1], 0x34011000, 0x82003004, -1)]:
            qt.memory(source, struct.pack("<512h", *[sign * (1000 + i) for i in range(512)]))
            base = 0x20101000 + ch * 0x40
            for offset, value in [(0x10, source), (0x14, destination),
                                  (0x18, 0x50300080 if standard else 0x05000080),
                                  (0x1c, 1024 if standard else 128),
                                  (0x20, 1024), (0x24, 2), (0x0c, 6),
                                  (0x08, 0x3001)]:
                qt.write(base + offset, value)
        qt.write(0x82003018, 0xe000)
        qmp.command("cont")
        if standard:
            qt.advance(1_000_000)
            assert qt.read(0x20101000 + channels[0] * 0x40 + 0x1c) == 896
            assert qt.read(0x20101000 + channels[0] * 0x40 + 0x10) == 0x34010080
        for _ in range(80):
            qt.advance(1_000_000)
        assert qt.read(0x20100010) == sum(1 << ch for ch in channels)
        assert qt.read(0x80000004) & (1 << 20)
        assert qt.read(0x20101000 + channels[0] * 0x40 + 0x10) == 0x34010400
        assert not (qt.read(0x20101000 + channels[0] * 0x40 + 8) & 1)
        for ch in channels:
            base = 0x20101000 + ch * 0x40
            qt.write(base + 0x0c, 0x1f000006)
            assert qt.read(base + 0x0c) == 6
        assert qt.read(0x20100010) == 0
        assert not (qt.read(0x80000004) & (1 << 20))
        qt.write(0x82003018, 0)
        qt.advance(20_000_000)
    _, pcm = samples(out / "audio.wav")
    nonzero = [(l, r) for l, r in pcm if l or r]
    assert nonzero == ([(1000 + i, -1000 - i) for i in range(512)] if clocked else []), nonzero[:12]
    print(f"PASS {name}: source progress, completion, IRQ clear, {'512 exact frames' if clocked else 'silent output'}")


def test_copy(args):
    with machine(args, "copy") as (qt, qmp, out):
        data = bytes(range(48))
        qt.memory(0x34010000, data)
        base = 0x20101600
        for off, val in [(0x10, 0x34010000), (0x14, 0x34011000),
                         (0x18, 0x0a000030), (0x1c, 48), (0x20, 48),
                         (0x24, 0x00040004), (0x0c, 6), (0x08, 1), (0x04, 1)]:
            qt.write(base + off, val)
        for i in range(0, 48, 4):
            assert qt.read(0x34011000 + i) == int.from_bytes(data[i:i + 4], "little")
        assert qt.read(base + 0x10) == 0x34010030
        assert qt.read(base + 0x14) == 0x34011030
        assert qt.read(0x20100010) == (1 << 24)
        qmp.command("system_reset")
        assert qt.read(0x20100010) == qt.read(0x82003018) == 0
    print("PASS software DMA: memory copy, completion, reset")


def test_request_route(args):
    with machine(args, "request-routing") as (qt, qmp, out):
        configure(qt)
        base = 0x201010c0  # one-based channel4
        qt.memory(0x34010000, struct.pack('<64h', *range(1, 65)))
        for off, val in [(0x10, 0x34010000), (0x14, 0x82003000),
                         (0x18, 0x50300080), (0x1c, 128), (0x0c, 2), (8, 0x3001)]:
            qt.write(base + off, val)
        qt.write(0x82003018, 0xa000)
        qmp.command('cont')
        qt.advance(10_000_000)
        assert qt.read(base + 0x1c) == 128  # destination match is insufficient
        qt.write(0x20102038, 3)  # wrong channel cannot service DA0
        qt.advance(10_000_000)
        assert qt.read(base + 0x1c) == 128
        qt.write(0x20102038, 4)
        qt.advance(10_000_000)
        assert qt.read(base + 0x1c) == 0
        assert qt.read(0x20100010) == 8
        # Reserved gaps must not index beyond the 32-channel state array.
        qt.write(0x20101ffc, 0xffffffff)
        assert qt.read(0x20101ffc) == 0
        qmp.command('system_reset')
        assert qt.read(0x20102038) == 0
    print('PASS VBC request routing: unmapped/wrong channel stalls, correct UID progresses, reset')


def test_standard_fill(args):
    # Actual ringtone initialization encoding observed in stock channel3.
    # A wrong width/fixed-address interpretation writes across VBC controls.
    with machine(args, "standard-fill") as (qt, qmp, out):
        configure(qt)
        qt.write(0x82003010, 159 << 8)
        qt.write(0x82003018, 0x400)
        values = list(range(1000, 1160))
        qt.memory(0x34010000, struct.pack("<160h", *values))
        base = 0x20101080
        for off, val in [(0x10, 0x34010000), (0x14, 0x82003000),
                         (0x18, 0x50300140), (0x1c, 320), (0x0c, 2),
                         (0x08, 0x3001), (0x04, 1)]:
            qt.write(base + off, val)
        assert qt.read(base + 0x10) == 0x34010140
        assert qt.read(base + 0x14) == 0x82003000
        assert qt.read(base + 0x1c) == 0
        assert qt.read(0x82003010) == 159 << 8
        assert qt.read(0x82003018) == 0x400
        assert qt.read(0x2010000c) == qt.read(0x20100010) == 4
        assert qt.read(0x20100018) == 0
        qt.write(base + 0x0c, 0x0f000002)
        assert qt.read(0x20100010) == 0
        qt.write(0x82003018, 0x8000)
        qmp.command("cont")
        for _ in range(30):
            qt.advance(1_000_000)
        qt.write(0x82003018, 0)
        qt.advance(20_000_000)
    _, pcm = samples(out / "audio.wav")
    assert [(l, r) for l, r in pcm if l or r] == [(v, 0) for v in values]
    print("PASS standard DMA: stock halfword FIFO fill, controls preserved, IRQ status")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.qemu = args.qemu.resolve()
    args.output = args.output.resolve()
    test_copy(args)
    test_request_route(args)
    test_standard_fill(args)
    test_cpu(args)
    test_cpu(args, mode=1, rate=48000)
    test_cpu(args, muted=True)
    test_cpu(args, owned=False)
    test_dma(args)
    test_dma(args, standard=True)
    test_dma(args, standard=True, clocked=False)


if __name__ == "__main__":
    main()
