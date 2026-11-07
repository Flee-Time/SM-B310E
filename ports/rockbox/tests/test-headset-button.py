#!/usr/bin/env python3
"""Check the headset buttons' plug-in mic check and mapping in QEMU.

When headphones are found, the driver turns on the mic bias and reads the
mic line (ADC channel 1). Only a headset with a mic, whose button line
reads released, enables the buttons: centre = Play (EIC channel 4 and the
line shorted); the side buttons (the line pulled down to part of its idle
level, which only the ADC sees) send Next (+) / Previous (-) on a quick
tap and hold Volume up / down when held past 0.4 s. The QEMU keypad's
"h", "j" and "k" keys hold the centre, + and - buttons. Each case opens
System > Debug > View I/O ports (which ignores these keys), presses the
centre once, taps + and - (200 ms), holds them (1.5 s), saves a dump with
key 9 and checks the HOOK line in .rockbox/torch-b310e.txt.
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


def run(args, name, qemu_args, expect_enabled, expect_presses, expect_taps,
        expect_holds):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "payloads/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg",
                    b"start in screen: files\nstart directory: /\n"
                    b"backlight timeout: on\nidle poweroff: 0\n")
    image = out / "sd.img"
    FAT["write_image"](image, root, 64, True)

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               *qemu_args]
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"D:\Toolchains\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = None

    def key(name, hold=150, pause=.8):
        qmp.hmp(f"sendkey {name} {hold}")
        time.sleep(pause)

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
            # Main menu -> System -> Debug (Keep Out!) -> View I/O ports
            # (same path as test-sim.py).
            key("f1")
            key("up")
            key("up")
            key("ret")
            key("up")
            key("ret")
            key("ret", pause=3)
            key("h", hold=300)              # headset centre
            key("j", hold=200)              # + tap: Next
            key("k", hold=200)              # - tap: Previous
            key("j", hold=1500, pause=1.5)  # + hold: Volume up
            key("k", hold=1500, pause=1.5)  # - hold: Volume down
            key("9", pause=3)               # save dump
            qmp.command("screendump", {"filename": str(out / "screen.png"), "format": "png"})
            key("f2", pause=2)
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    text = image.read_bytes().decode("latin-1")
    hooks = re.findall(r"HOOK enabled=(\d) check_mic_adc=(-?\d+) check_level=(\d) "
                       r"state=(\d) level_now=\d idle=(-?\d+) raw=-?\d+ held=. "
                       r"last_press_raw=(-?\d+) presses=(\d+)/(\d+)/(\d+) "
                       r"taps=(\d+)/(\d+) holds=(\d+)/(\d+)", text)
    assert hooks, (name, "no HOOK line in the dump")
    (enabled, mic_adc, check_level, state, idle, last_raw,
     play, up, down, tap_next, tap_prev, hold_up, hold_down) = map(int, hooks[-1])
    presses = [play, up, down]
    taps, holds = [tap_next, tap_prev], [hold_up, hold_down]
    result = {"case": name, "enabled": enabled, "check_mic_adc": mic_adc,
              "check_level": check_level, "state": state, "idle": idle,
              "last_press_raw": last_raw, "presses": presses,
              "taps": taps, "holds": holds}
    assert enabled == expect_enabled, result
    assert presses == expect_presses, result
    assert taps == expect_taps, result
    assert holds == expect_holds, result
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
    headset = ["-global", "sc6530_aux.headset-present=on"]
    results = [
        run(args, "headset-with-mic", headset + ["-global", "sc6530_adi.headset-mic=on"],
            1, [1, 2, 2], [1, 1], [1, 1]),
        # Side buttons whose contacts short the line for 30 ms on first
        # touch: they must not be taken for the centre button (Play).
        run(args, "side-button-bounce", headset + ["-global", "sc6530_adi.headset-mic=on",
                                                   "-global", "sc6530_adi.headset-bounce-ms=30"],
            1, [1, 2, 2], [1, 1], [1, 1]),
        run(args, "headphones-no-mic", headset, 0, [0, 0, 0], [0, 0], [0, 0]),
        run(args, "nothing-plugged", [], 0, [0, 0, 0], [0, 0], [0, 0]),
    ]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
