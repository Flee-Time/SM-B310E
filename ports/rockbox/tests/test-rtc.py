#!/usr/bin/env python3
"""Rockbox's date and time against the SC6530 RTC, in QEMU.

The RTC keeps a 16-bit day counter (day 0 = 1980-01-01) beside seconds,
minutes and hours; new values go to +0x10..+0x1c and the RTC acknowledges
them in raw status bits 8-11 (rtc-sc6530c.c, docs/rockbox-power.md). The
test presets the RTC model to 2026-03-15 10:20:00 through those registers,
boots Rockbox into its Settings menu, opens Time & Date -> Set Time/Date,
raises the hour by one (Up) and sets it (Select). The RTC must then hold
day 16875 (2026-03-15, read by Rockbox and written back unchanged) at
11:20 (11:21 if the minute turned meanwhile), with the acknowledgements
cleared. The old driver read the day counter as packed day/month/year
fields and wrote them back that way, and the keymap gave the Set
Time/Date screen no keys to change a field.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import runpy
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))
RTC = 0x82001600
PRESET = datetime.date(2026, 3, 15)
DAY = (PRESET - datetime.date(1980, 1, 1)).days


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=REPO / "build/qemu/build/qemu-system-arm.exe")
    parser.add_argument("--rockbox", type=Path, default=REPO / "sdcard/payloads/rockbox.bin")
    parser.add_argument("--runtime", type=Path, default=REPO / "sdcard/.rockbox")
    parser.add_argument("--output", type=Path, default=REPO / "build/rockbox-tests/rtc")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "payloads/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg",
                    b"start in screen: menu\nbacklight timeout: on\nidle poweroff: 0\n")
    image = out / "sd.img"
    FAT["write_image"](image, root, 64, True)

    qmp_port, qtest_port = API["free_port"](), API["free_port"]()
    command = [str(args.qemu.resolve()), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none",
               "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
               "-qtest", f"tcp:127.0.0.1:{qtest_port},server=on,wait=off",
               "-drive", f"file={args.rockbox.resolve().as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0"]
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"D:\Toolchains\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = qt = None
    result = {}
    with (out / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError(f"QEMU exited: {out / 'stderr.log'}")
                try:
                    qmp = API["QMP"](qmp_port)
                    qt = API["QTest"](qtest_port)
                    break
                except OSError:
                    time.sleep(.1)
            assert qmp and qt, "QEMU did not start"

            def rtc():
                return [qt.read(RTC + 4 * i) & 0xffff for i in range(4)]

            # preset: new values, which the model takes and acknowledges
            for i, v in enumerate((0, 20, 10, DAY)):
                qt.write(RTC + 0x10 + 4 * i, v)
            qt.write(RTC + 0x38, 0xf00)
            result["preset"] = rtc()
            assert result["preset"][3] == DAY, result

            def key(name, pause=1.2):
                qmp.hmp(f"sendkey {name} 150")
                time.sleep(pause)

            def shot(name):
                qmp.command("screendump", {"filename": str(out / f"{name}.png"), "format": "png"})

            time.sleep(15)                      # boot into Settings
            for _ in range(5):
                key("down")                     # Time & Date
            key("ret")
            shot("time-date")
            key("ret")                          # Set Time/Date
            shot("set-before")
            key("up")                           # hour + 1
            shot("set-edited")
            key("ret", 2)                       # set
            shot("set-after")
            r = rtc()
            raw = qt.read(RTC + 0x34) & 0xffff
            result.update({"after": r, "raw": hex(raw)})
        finally:
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    sec, minute, hour, day = result["after"]
    assert day == DAY, ("the date did not survive Rockbox's read and write", result)
    assert hour == 11 and minute in (20, 21), ("the time was not set", result)
    assert not int(result["raw"], 16) & 0xf00, ("acknowledgements left raised", result)
    result["day"] = DAY
    (out / "results.json").write_text(json.dumps(result, indent=2))
    print("PASS " + json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
