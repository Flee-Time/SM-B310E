#!/usr/bin/env python3
"""Run the ADC R bit sweep from Rockbox's I/O test screen in QEMU.

The sweep (key 0 in System > Debug > View I/O ports) powers stereo capture
on the line-in route, flips one codec/VBC bit at a time, measures both
channels and writes .rockbox/adcr-sweep.txt. In the QEMU model ADC R works
(the line-in tones are 600 Hz left, 900 Hz right), so the baseline must
show both channels live, and the bits the model treats as fatal must be
caught: codec +0x5c bit 6 pins ADC R at the rail, and VBC +0x80 bit 5
sends AD0 to the DSP (the ARM sees silence on the left).
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

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "--trace", "sc6530_vbc_ad",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               "-global", "sc6530_aux.headset-present=on"]
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
            key("0", pause=60)              # ADC R bit sweep
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
    # The format string lives in rockbox.bin on the same image; the
    # written file has a digit after "variant=".
    found = list(re.finditer(r"ADC R sweep variant=\d", text))
    assert found, "no adcr-sweep.txt content in the image"
    start = found[-1].start()
    end = text.find("\n", text.find("END entries=", start))
    log = text[start:end]
    (out / "adcr-sweep.txt").write_text(log)
    lines = log.splitlines()
    entries = int(re.search(r"END entries=(\d+)", log).group(1))
    assert entries >= 120, ("too few sweep entries", entries)

    def find(addr, mask):
        for line in lines:
            if line.startswith(f"BIT {addr:08x} m={mask:04x} "):
                return line
        raise AssertionError(("missing sweep entry", hex(addr), hex(mask)))

    base = next(l for l in lines if l.startswith("BASE "))
    assert "R-LIVE" in base and "L-DEAD" not in base, ("baseline", base)
    bit6 = find(0x82001a5c, 0x40)
    assert "R-LIVE" not in bit6, ("+0x5c bit 6 should rail ADC R", bit6)
    dsp = find(0x82003080, 0x20)
    assert "L-DEAD" in dsp, ("VBC +0x80 bit 5 should silence the left", dsp)
    result = {"entries": entries, "base": base, "codec_5c_bit6": bit6,
              "vbc_80_bit5": dsp}
    print("PASS " + json.dumps(result), flush=True)
    (out / "results.json").write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
