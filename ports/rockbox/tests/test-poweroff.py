#!/usr/bin/env python3
"""Power Rockbox off in QEMU and check the analog LEDs are off first.

The keypad light, LCD backlight LED and vibrator are driven by the analog
PMU, which keeps its state when power_off() cuts the CPU/RAM supplies. On
the phone a keypad light that was on at shutdown stayed lit (2026-11-06).
The test holds END until Rockbox shuts down (the press also turns the
button light on), waits for QEMU to exit on the LDO power-down write
(ANA 0x82001180), and reads the ANA write trace: the last keypad-light
write (0x82001224) before the power-down must clear the on bit (0x20),
and the backlight (0x82001220) must be switched off: on bit (0x40) clear
and off bit (0x20) set, as the stock on/off function does (NOR 0x1a186).
"""
import argparse
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))

KEYLIGHT, BACKLIGHT, LDO_SET0 = 0x82001224, 0x82001220, 0x82001180


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "runtime", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "output"):
        setattr(args, option, getattr(args, option).resolve())
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg",
                    b"start in screen: files\nstart directory: /\n"
                    b"backlight timeout: on\nidle poweroff: 0\n")
    image = out / "sd.img"
    FAT["write_image"](image, root, 64, True)

    trace = out / "trace.log"
    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(trace),
               "--trace", "sc6530_ana_write",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0"]
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
            time.sleep(15)
            qmp.hmp("sendkey down 150")        # a key press: button light on
            time.sleep(1)
            qmp.hmp("sendkey end 4000")        # hold END: power off
            try:
                process.wait(timeout=40)
            except subprocess.TimeoutExpired:
                raise AssertionError("QEMU did not power off after holding END")
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    writes = [(int(a, 16), int(v, 16)) for a, v in
              re.findall(r"ANA write addr=0x([0-9a-f]+) val=0x([0-9a-f]+)",
                         trace.read_text(errors="replace"))]
    off = max(i for i, (a, v) in enumerate(writes)
              if a == LDO_SET0 and (v & 0x3fff) == 0x3fff)
    before = writes[:off]
    keylight = [v for a, v in before if a == KEYLIGHT]
    backlight = [v for a, v in before if a == BACKLIGHT]
    lit = [v for v in keylight if v & 0x20]
    assert lit, "the button light was never turned on, so the test proves nothing"
    result = {"keylight_last": hex(keylight[-1]), "keylight_was_on": hex(lit[-1]),
              "backlight_last": hex(backlight[-1]) if backlight else None}
    assert not keylight[-1] & 0x20, ("keypad light left on at power-off", result)
    assert backlight and backlight[-1] & 0x60 == 0x20, ("backlight left on at power-off", result)
    print("PASS " + json.dumps(result), flush=True)
    (out / "results.json").write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
