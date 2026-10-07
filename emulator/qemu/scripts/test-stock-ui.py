#!/usr/bin/env python3
"""Boot unmodified e52q7a, unlock its menu and repeat LCD sleep/key wake."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import time

API = runpy.run_path(str(Path(__file__).with_name("test-audio.py")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "firmware", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    for option in ("qemu", "firmware", "output"):
        setattr(args, option, getattr(args, option).resolve())
    assert hashlib.sha256(args.firmware.read_bytes()).hexdigest() == (
        "5e44e0858d4eacbd1c0124d3bb8d152cb429c9222a99b42f8f75c86300846c92"), (
        "The UI navigation and icon checks are specific to the local e52q7a dump")
    args.output.mkdir(parents=True, exist_ok=True)
    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e", "-display", "none", "-serial", "none",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off", "-drive",
               f"if=none,id=nor,file={args.firmware.as_posix()},format=raw,readonly=on"]
    (args.output / "command.json").write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = None
    start = time.monotonic()
    with (args.output / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited; see stderr.log")
                try:
                    qmp = API["QMP"](port)
                    break
                except OSError:
                    time.sleep(.1)
            assert qmp is not None

            def screen(name):
                path = args.output / (name + ".ppm")
                qmp.command("screendump", {"filename": str(path), "format": "ppm"})
                data = path.read_bytes()
                assert data.startswith(b"P6\n128 160\n255\n")
                return data[-128*160*3:]

            def key(value, hold=180):
                qmp.hmp(f"sendkey {value} {hold}")
                time.sleep(hold / 1000 + .5)

            def wait_for(predicate, timeout, label):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if predicate():
                        return
                    time.sleep(.3)
                raise AssertionError(label)

            # Full reset must initialize clocks/kernel objects without RAM
            # overlays. Save startup frames and bound the model-number splash.
            startup = []
            while time.monotonic() - start < 5:
                data = screen(f"boot-{len(startup):02}")
                startup.append({"seconds": time.monotonic() - start,
                                "sha256": hashlib.sha256(data).hexdigest()})
                time.sleep(.25)
            assert len({s["sha256"] for s in startup}) >= 3, "stalled startup splash"
            # The stock home screen auto-locks and turns the LCD off. One key
            # wakes it; Right soft key + long Star unlocks the keyguard.
            wait_for(lambda: not any(screen("first-sleep")), 35, "stock did not sleep")
            for cycle in range(3):
                key("f2")
                wait_for(lambda: any(screen(f"wake-{cycle}")), 3, "keypress did not wake LCD")
                key("f2")
                key("asterisk", 2000)
                key("f1")  # open main menu
                # A No-SIM dialog can be the first left-soft-key operation.
                key("f2")
                key("f1")
                data = screen(f"menu-{cycle}")
                # The 3x3 menu has colored icon interiors in every cell.
                # Missing RGB888 layers used to leave only the blue background.
                counts = []
                for y in (32, 69, 106):
                    for x in (7, 50, 93):
                        colors = {tuple(data[(yy*128+xx)*3:(yy*128+xx)*3+3])
                                  for yy in range(y, y+18) for xx in range(x, x+18)}
                        counts.append(len(colors))
                assert min(counts) > 12, ("missing menu icons", cycle, counts)
                qmp.command("screendump", {"filename": str(args.output / f"menu-{cycle}.png"), "format": "png"})
                key("right")
                key("left")
                key("f2")
                wait_for(lambda: not any(screen(f"sleep-{cycle}")), 35, "stock did not return to sleep")
            (args.output / "results.json").write_text(json.dumps({"startup": startup, "sleep_wake_cycles": 3}, indent=2))
            qmp.command("quit")
            process.wait(timeout=10)
            print("PASS default stock boot, nine icons and three sleep/key wake cycles")
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
