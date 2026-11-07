#!/usr/bin/env python3
"""Read the emulated SIM card from Rockbox's I/O test screen.

The QEMU machine models the SIM controller (0x85000000) and a GSM test card
(sc6530_sim.c). The test opens System > Debug > View I/O ports, presses #
(read both SIM slots) and checks the SIM lines the screen appends to
.rockbox/torch-b310e.txt: ATR, ICCID and IMSI for slot 0, no card in slot 1,
and "PIN required" when the card's IMSI is PIN-protected.
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

ICCID = "8944110012345678901"
IMSI = "234150123456789"


def run(args, name, pin):
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
               "--trace", "sc6530_sim_*",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               "-global", "sc6530_sim.sim-slots=1",
               "-global", f"sc6530_sim.sim-iccid={ICCID}",
               "-global", f"sc6530_sim.sim-imsi={IMSI}",
               "-global", f"sc6530_sim.sim-pin={'on' if pin else 'off'}"]
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
            # Main menu -> System -> Debug (Keep Out!) -> View I/O ports.
            # System is second-to-last; the menu wraps from Files.
            key("f1")
            key("up")
            key("up")
            key("ret")
            key("up")
            key("ret")
            key("ret", pause=3)
            key("slash", pause=12)          # '#': read both SIM slots
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
    lines = {m.group(1): m.group(0) for m in re.finditer(r"(SIM[01]) [^\r\n]*", text)}
    assert "SIM0" in lines and "SIM1" in lines, (name, "no SIM lines in the dump")
    sim0, sim1 = lines["SIM0"], lines["SIM1"]
    assert f"iccid={ICCID}" in sim0, (name, sim0)
    assert "atr 3b" in sim0, (name, sim0)
    if pin:
        assert "pin=1" in sim0 and "imsi= " in sim0, (name, sim0)
    else:
        assert f"imsi={IMSI}" in sim0 and "pin=0" in sim0, (name, sim0)
    assert "no card" in sim1, (name, sim1)
    result = {"case": name, "sim0": sim0, "sim1": sim1}
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
    results = [run(args, "card", False), run(args, "pin-locked", True)]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
