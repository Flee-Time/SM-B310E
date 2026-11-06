#!/usr/bin/env python3
"""Record from the emulated microphone through Rockbox and check the WAV file.

The QEMU VBC model feeds a synthetic tone into VBC AD0 only while the codec's
ADC, mic path and mic bias are powered (see emulator sc6530_adi.c). Rockbox
starts in the recording screen, Center starts a recording, Back stops it, and
the test reads the new WAV back out of the private SD image.
"""
import argparse
from array import array
import json
import math
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))


def tone(samples, frequency, rate):
    phase = 2 * math.pi * frequency / rate
    real = sum(v * math.cos(phase * i) for i, v in enumerate(samples))
    imag = sum(v * math.sin(phase * i) for i, v in enumerate(samples))
    return 2 * math.hypot(real, imag) / len(samples)


def wav_headers(image):
    found, start = {}, 0
    while (pos := image.find(b"RIFF", start)) >= 0:
        start = pos + 4
        if image[pos + 8:pos + 16] == b"WAVEfmt ":
            found[pos] = image[pos:pos + 44]
    return found


def read_wav(image, pos):
    channels, rate = struct.unpack_from("<HI", image, pos + 22)
    bits = struct.unpack_from("<H", image, pos + 34)[0]
    chunk = pos + 12
    while image[chunk:chunk + 4] != b"data":
        chunk += 8 + struct.unpack_from("<I", image, chunk + 4)[0]
    size = struct.unpack_from("<I", image, chunk + 4)[0]
    pcm = array("h", image[chunk + 8:chunk + 8 + size])
    if sys.byteorder != "little":
        pcm.byteswap()
    return channels, rate, bits, pcm


def run(args, name, tone_hz, gain, frequency):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "start in screen: recording\nrec source: mic\nrec format: wave\n"
        f"rec frequency: {frequency // 1000}\nrec channels: mono\n"
        f"rec mic gain: {gain}\nbacklight timeout: on\nidle poweroff: 0\n").encode())
    image_path = out / "sd.img"
    FAT["write_image"](image_path, root, 64, True)
    before = set(wav_headers(image_path.read_bytes()))

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "--trace", "sc6530_vbc_ad",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image_path.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               "-global", f"sc6530_adi.mic-tone-hz={tone_hz}",
               "-icount", "shift=3,sleep=off"]
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"D:\Toolchains\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = None
    with (out / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited: {out / 'stderr.log'}")
                try:
                    qmp = API["QMP"](port)
                    break
                except OSError:
                    time.sleep(.1)
            assert qmp is not None, "QMP startup timeout"
            time.sleep(8)
            qmp.command("screendump", {"filename": str(out / "ready.png"), "format": "png"})
            qmp.hmp("sendkey ret 180")              # start recording
            time.sleep(args.seconds)
            qmp.command("screendump", {"filename": str(out / "recording.png"), "format": "png"})
            qmp.hmp("sendkey f2 180")               # stop
            time.sleep(4)
            qmp.command("screendump", {"filename": str(out / "stopped.png"), "format": "png"})
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    image = image_path.read_bytes()
    new = sorted(set(wav_headers(image)) - before)
    assert new, (name, "no new WAV file on the card")
    channels, rate, bits, pcm = read_wav(image, new[-1])
    assert (channels, rate, bits) == (1, frequency, 16), (name, channels, rate, bits)
    assert len(pcm) >= rate, (name, "recording shorter than one second", len(pcm))
    window = list(pcm[len(pcm) // 2 - rate // 4:len(pcm) // 2 + rate // 4])
    level = tone(window, tone_hz, rate)
    off = tone(window, tone_hz * 1.37, rate)
    expected = 8192 * (gain + 1) / 16
    assert abs(level - expected) < expected * .05, (name, level, expected)
    assert off < expected * .02, (name, "unexpected energy off the tone", off)
    result = {"case": name, "seconds": len(pcm) / rate, "rate": rate,
              "tone": tone_hz, "level": round(level), "expected": round(expected)}
    print("PASS " + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "runtime", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=6)
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "output"):
        setattr(args, option, getattr(args, option).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [("mic-16k", 1000, 8, 16000), ("mic-8k-gain15", 440, 15, 8000)]
    results = [run(args, *case) for case in cases]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
