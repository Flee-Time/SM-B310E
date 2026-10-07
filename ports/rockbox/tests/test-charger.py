#!/usr/bin/env python3
"""Check the full player's external-power, charger-status and battery reporting.

Change only external inputs/registers representing the attached charger.
Observe the linked player's state; never patch guest code or RAM.
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


def check(args, plugged):
    out = args.output / ("powered-boot" if plugged else "hotplug")
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime), path.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", b"backlight timeout: on\nidle poweroff: 1\n")
    FAT["write_image"](out / "sd.img", root, 64, True)
    exe = ".exe" if os.name == "nt" else ""
    nm = subprocess.check_output([args.toolchain / ("arm-none-eabi-nm" + exe), args.elf], text=True)
    symbols = {name: int(re.search(r"^([0-9a-f]+) [A-Za-z] " + name + "$", nm, re.M)[1], 16)
               for name in ("voltage_now", "percent_now", "charge_state", "power_thread_inputs", "current_tick")}
    qmp_port, test_port = API["free_port"](), API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={(out / 'sd.img').as_posix()},format=raw,if=none,id=sdcard",
               "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
               "-qtest", f"tcp:127.0.0.1:{test_port},server=on,wait=off",
               "-qtest-log", str(out / "qtest.log"), "-global",
               "sc6530_adi.charger-present=" + ("on" if plugged else "off")]
    (out / "command.json").write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = qt = None
    states = []
    with (out / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                assert process.poll() is None, "QEMU exited; see stderr.log"
                try:
                    qmp = API["QMP"](qmp_port)
                    break
                except OSError:
                    time.sleep(.1)
            assert qmp is not None, "QMP startup timeout"
            qt = API["QTest"](test_port)

            def state():
                data = {name: qt.read(addr) for name, addr in symbols.items()}
                for name in ("voltage_now", "percent_now"):
                    if data[name] & 0x80000000:
                        data[name] -= 1 << 32
                return data

            def wait_for(predicate, name, timeout=6):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    assert process.poll() is None, "unexpected poweroff"
                    data = state()
                    if predicate(data):
                        states.append({"name": name, **data})
                        print("PASS " + name + " " + json.dumps(data), flush=True)
                        return data
                    time.sleep(.2)
                raise AssertionError((name, state()))

            def input_level(pin, level):
                qt.command(f"set_irq_in /machine/peripheral/sc6530-aux gpio-input {pin} {level}")

            def cable(level):
                qt.command(f"set_irq_in /machine/peripheral/sc6530-adi charger-input 0 {level}")

            time.sleep(3)
            # The target must not commandeer the existing charger enable pin.
            assert not (qt.read(0x8a000004) & qt.read(0x8a000008) & (1 << 6))
            assert qt.read(0x82001904) & 12 == 12, "charger and END channels preserved"
            if plugged:
                wait_for(lambda s: s["power_thread_inputs"] == 17 and s["charge_state"] == 3
                         and s["voltage_now"] == s["percent_now"] == -1, "powered boot unknown battery")
                # Paused idle poweroff must be inhibited for over its 60s timeout.
                wait_for(lambda s: s["current_tick"] >= 6500 and s["power_thread_inputs"] == 17,
                         "external power inhibits idle shutdown", timeout=120)
                cable(0)
                wait_for(lambda s: s["power_thread_inputs"] == 0 and 3900 < s["voltage_now"] < 4000
                         and 0 <= s["percent_now"] < 100, "first unplugged battery sample")
            else:
                initial = wait_for(lambda s: s["power_thread_inputs"] == 0 and 3900 < s["voltage_now"] < 4000,
                                   "unplugged battery")
                cable(1)
                data = wait_for(lambda s: s["power_thread_inputs"] == 17 and s["charge_state"] == 3,
                                "plugged charging")
                assert abs(data["voltage_now"] - initial["voltage_now"]) <= 20
                assert data["percent_now"] < 100
                qmp.command("screendump", {"filename": str(out / "charging.png"), "format": "png"})
                input_level(9, 1)
                wait_for(lambda s: s["charge_state"] == 0 and s["power_thread_inputs"] == 17, "charge complete")
                input_level(8, 1)
                input_level(9, 0)
                fault_tick = state()["current_tick"]
                wait_for(lambda s: s["charge_state"] == 0 and s["current_tick"] >= fault_tick + 100,
                         "charger input fault")
                input_level(8, 0)
                wait_for(lambda s: s["charge_state"] == 3, "charging resumes")
                # Represent a loader that explicitly disabled the charger.
                qt.write(0x8a000004, qt.read(0x8a000004) | 64)
                qt.write(0x8a000008, qt.read(0x8a000008) | 64)
                qt.write(0x8a000000, qt.read(0x8a000000) | 64)
                wait_for(lambda s: s["charge_state"] == 0, "charger disabled by loader")
                cable(0)
                wait_for(lambda s: s["power_thread_inputs"] == 0 and s["charge_state"] == 0, "cable removed")
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
    (out / "results.json").write_text(json.dumps(states, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "elf", "runtime", "toolchain", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "elf", "runtime", "toolchain", "output"):
        setattr(args, option, getattr(args, option).resolve())
    check(args, False)
    check(args, True)


if __name__ == "__main__":
    main()
