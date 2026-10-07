#!/usr/bin/env python3
"""Play a stereo WAV through the real player and check saved sound settings."""
import argparse
from array import array
import json
import math
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time
import wave

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))


def amplitude(samples, frequency, rate):
    # Measure an integer number of periods after decoder/volume startup.
    values = samples[rate:rate * 2]
    phase = 2 * math.pi * frequency / rate
    real = sum(v * math.cos(phase * i) for i, v in enumerate(values))
    imag = sum(v * math.sin(phase * i) for i, v in enumerate(values))
    return 2 * math.hypot(real, imag) / len(values)


def check(args, name, settings, expected):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime), path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, "test.wav", FAT["test_audio"](10))
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "volume: 0\nspeaker mode: off\nstart in screen: files\n"
        "start directory: /\nbacklight timeout: on\nidle poweroff: 0\n" + settings).encode())
    FAT["write_image"](out / "sd.img", root, 64, True)
    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-d", "guest_errors", "-D",
               str(out / "trace.log"), "-qmp",
               f"tcp:127.0.0.1:{port},server=on,wait=off", "-drive",
               f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={(out / 'sd.img').as_posix()},format=raw,if=none,id=sdcard",
               "-global", "sc6530_aux.headset-present=on", "-audiodev",
               f"wav,id=audio0,path={(out / 'audio.wav').as_posix()},out.frequency=44100",
               "-global", "sc6530_adi.audiodev=audio0"]
    if not args.realtime:
        command += ["-icount", "shift=3,sleep=off"]
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
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
            time.sleep(4)
            qmp.hmp("sendkey down 180")
            time.sleep(.6)
            qmp.hmp("sendkey ret 180")
            time.sleep(6)
            qmp.command("screendump", {"filename": str(out / "screen.png"), "format": "png"})
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    with wave.open(str(out / "audio.wav"), "rb") as wav:
        assert wav.getnchannels() == 2 and wav.getsampwidth() == 2
        rate = wav.getframerate()
        pcm = array("h", wav.readframes(wav.getnframes()))
    if sys.byteorder != "little":
        pcm.byteswap()
    frames = list(zip(pcm[::2], pcm[1::2]))
    first = next(i for i, pair in enumerate(frames) if any(pair))
    frames = frames[first:]
    assert len(frames) >= rate * 3, (name, "playback too short")
    actual = [amplitude([pair[c] for pair in frames], f, rate)
              for c in range(2) for f in (440, 660)]
    # Amplitudes are normalized to the stereo fixture's 4096 full scale.
    for measured, fraction in zip(actual, expected):
        assert abs(measured / 4096 - fraction) < .015, (name, actual, expected)
    if name in ("mono", "width-zero"):
        assert all(abs(l-r) <= 1 for l, r in frames[rate:rate*2]), name
    if name == "karaoke":
        assert all(abs(l+r) <= 1 for l, r in frames[rate:rate*2]), name
    result = {"case": name, "amplitudes": actual}
    print("PASS " + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "runtime", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--cases", help="Optional comma-separated case names")
    parser.add_argument("--realtime", action="store_true", help="Also exercise host-clock playback timing")
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "output"):
        setattr(args, option, getattr(args, option).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [
        ("stereo", "channels: stereo\n", [1, 0, 0, 1]),
        ("mono", "channels: mono\n", [.5, .5, .5, .5]),
        ("mono-left", "channels: mono left\n", [1, 0, 1, 0]),
        ("mono-right", "channels: mono right\n", [0, 1, 0, 1]),
        ("swap", "channels: swap\n", [0, 1, 1, 0]),
        ("karaoke", "channels: karaoke\n", [.5, .5, .5, .5]),
        ("width-zero", "channels: custom\nstereo_width: 0\n", [.5, .5, .5, .5]),
        ("balance-left", "balance: -100\n", [1, 0, 0, 0]),
        ("balance-right", "balance: 100\n", [0, 0, 0, 1]),
        ("balance-half", "balance: 50\n", [10 ** (-50 / 20), 0, 0, 1]),
        ("gain-balance", "volume: 9\nbalance: -100\n", [1, 0, 0, 0]),
    ]
    if args.cases:
        requested = args.cases.split(",")
        assert set(requested) <= {case[0] for case in cases}, requested
        cases = [case for case in cases if case[0] in requested]
    # Virtual instruction timing lets the guest service each VBC bank before
    # the next deadline. Disable idle poweroff for accelerated idle time.
    # Run one player at a time to avoid starving the host WAV backend.
    results = [check(args, *case) for case in cases]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
