#!/usr/bin/env python3
"""Headset buttons while recording, in QEMU.

Any recording turns the headset buttons off: from the headset mic a press
would short the mic being recorded, and in every recording the centre
button (Play) would start a new file in the recording screen (on the phone
the buttons also misbehaved during FM recording). Each case starts a
recording, presses the centre button ("h") in the middle, stops, and
counts the new WAV files on the card: one file means the press was
ignored.
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))
REC = runpy.run_path(str(Path(__file__).with_name("test-recording.py")))


def run(args, name, config, qemu_args, expect_files):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "payloads/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "start in screen: recording\nrec format: wave\nrec frequency: 16\n" + config +
        "backlight timeout: on\nidle poweroff: 0\n").encode())
    image_path = out / "sd.img"
    FAT["write_image"](image_path, root, 64, True)
    before = set(REC["wav_headers"](image_path.read_bytes()))

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image_path.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               "-global", "sc6530_aux.headset-present=on",
               "-global", "sc6530_adi.headset-mic=on",
               "-global", "sc6530_adi.mic-tone-hz=1000", *qemu_args]
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
            time.sleep(10)
            qmp.hmp("sendkey ret 180")              # start recording
            time.sleep(4)
            qmp.hmp("sendkey h 300")                # headset centre (Play)
            time.sleep(4)
            qmp.command("screendump", {"filename": str(out / "recording.png"), "format": "png"})
            qmp.hmp("sendkey f2 180")               # stop
            time.sleep(5)
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    new = sorted(set(REC["wav_headers"](image_path.read_bytes())) - before)
    result = {"case": name, "new_wav_files": len(new), "expected": expect_files}
    assert len(new) == expect_files, result
    print("PASS " + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "runtime", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "output"):
        setattr(args, option, getattr(args, option).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    results = [
        run(args, "headset-mic-recording",
            "rec source: mic\nrec channels: mono\nrec mic gain: 8\n", [], 1),
        run(args, "fm-recording",
            "rec source: fmradio\nrec channels: mono\nfm_region: eu\n", [], 1),
    ]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
