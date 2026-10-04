#!/usr/bin/env python3
"""Boot e52q7a stock firmware, preview its built-in ringtone and verify WAV output.

Uses the explicitly experimental GPIO49 input and normal keypad events.
Boot/navigation delays are wall time; increase --boot-seconds on slow hosts.
Every key has a screenshot in the capture folder for diagnosing navigation.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import runpy
import sys
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--boot-seconds", type=float, default=90)
    parser.add_argument("--play-seconds", type=float, default=15)
    args = parser.parse_args()
    if args.boot_seconds < 10 or args.play_seconds < 2:
        parser.error("allow at least 10 boot seconds and 2 playback seconds")
    if hashlib.sha256(args.firmware.read_bytes()).hexdigest() != (
            "5e44e0858d4eacbd1c0124d3bb8d152cb429c9222a99b42f8f75c86300846c92"):
        parser.error("this navigation recipe is for the e52q7a.bin dump")
    keys = [(0, "f2", 100), (2, "asterisk", 2000),
            (6, "f1", 100), (8, "f2", 100), (10, "f1", 100)]
    navigation = ["left", "left", "ret", "down", "ret",  # Settings / Profiles
                  "up", "f1", "down", "ret",             # edit Normal
                  "down", "ret", "ret"]                   # call/default ringtones
    keys += [(12 + i * 2, key, 100) for i, key in enumerate(navigation)]
    # capture-stock runs/finishes the process, dumps state and closes the WAV.
    sys.argv = ["capture-stock.py", "--qemu", str(args.qemu), "--firmware",
                str(args.firmware), "--output", str(args.output), "--audio",
                "--hold-end", "--gpio49-high", "--no-overlays", "--seconds",
                str(args.boot_seconds + keys[-1][0] + args.play_seconds)]
    for when, key, hold in keys:
        sys.argv += ["--key", f"{args.boot_seconds + when}:{key}:{hold}"]
    runpy.run_path(str(Path(__file__).with_name("capture-stock.py")), run_name="__main__")
    with wave.open(str(args.output / "audio.wav"), "rb") as wav:
        assert wav.getnchannels() == 2 and wav.getsampwidth() == 2
        pcm = array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
        stats = {"frames": wav.getnframes(), "rate": wav.getframerate(),
                 "nonzero_samples": sum(bool(value) for value in pcm),
                 "peak": max(map(abs, pcm), default=0),
                 "clipped_samples": sum(abs(value) >= 32767 for value in pcm),
                 "rms": math.sqrt(sum(value * value for value in pcm) / max(1, len(pcm)))}
    (args.output / "audio-stats.json").write_text(json.dumps(stats, indent=2))
    if stats["nonzero_samples"] < stats["rate"]:
        raise RuntimeError("No sustained ringtone output; inspect key screenshots and trace.log")
    print("PASS stock ringtone output: " + json.dumps(stats))


if __name__ == "__main__":
    main()
