#!/usr/bin/env python3
"""Start Rockbox's FM radio against the emulated RTC6211 and check the driver.

The QEMU machine models I2C controller 1 (0x86001000) and an RTC6211S tuner
with stations at 92.3 and 100.0 MHz (sc6530_i2c.c). Rockbox starts in the
radio screen with no presets, accepts the first-run auto-scan prompt, and
the test reads the trace for:
  - tuner detection and the six stock init tables streamed from NOR,
  - scan tunes that land on both stations with the model's on-station RSSI,
  - the codec's analog FM route (line-in mux and PGA -> headphone mixer),
  - the reset line (GPIO31) and the 32 kHz pad (0x8c00017c = 0x111).
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
    for option in ("qemu", "rockbox", "runtime", "nor", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=40)
    parser.add_argument("--chip", choices=("n", "s"), default="n",
                        help="tuner variant to emulate (the tested phone has the N)")
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "nor", "output"):
        setattr(args, option, getattr(args, option).resolve())
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "payloads/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "start in screen: radio\nfm_region: eu\nbacklight timeout: on\n"
        "idle poweroff: 0\nvolume: -10\n").encode())
    image = out / "sd.img"
    FAT["write_image"](image, root, 64, True)

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "--trace", "sc6530_fm_*", "--trace", "sc6530_i2c_cmd",
               "--trace", "sc6530_ana_write", "--trace", "sc6530_aux_write",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.nor.as_posix()},format=raw,if=none,id=nor,readonly=on",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image.as_posix()},format=raw,if=none,id=sdcard",
               "-global", "sc6530_aux.headset-present=on",
               "-global", f"sc6530_i2c.fm-chip={args.chip}",
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
            time.sleep(12)
            qmp.command("screendump", {"filename": str(out / "prompt.png"), "format": "png"})
            qmp.hmp("sendkey ret 180")           # yes: auto-scan presets
            time.sleep(args.seconds)
            qmp.command("screendump", {"filename": str(out / "radio.png"), "format": "png"})
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    trace = (out / "trace.log").read_text(errors="replace").splitlines()
    tunes = [tuple(map(int, m.groups())) for m in
             (re.search(r"RTC6211 tuned (\d+) x10kHz rssi=(\d+)", l) for l in trace) if m]
    hits = sorted({f for f, rssi in tunes if rssi >= 50})
    writes = [l for l in trace if "I2C1 cmd=" in l]
    # Length of every write transaction to the tuner (0xC8), in bytes.
    lengths, current = [], None
    for l in writes:
        cmd = int(re.search(r"cmd=0x([0-9a-f]+)", l).group(1), 16)
        if cmd & 0x20:
            if current is not None:
                lengths.append(current)
            current = 0 if cmd >> 8 == 0xc8 else None
        elif cmd & 0x04 and current is not None:
            current += 1
    if current is not None:
        lengths.append(current)
    nor_tables = lengths.count(46)
    id_reads = sum(1 for l in trace if re.search(r"I2C1 cmd=0xc924 ", l))
    table_bytes = sum(1 for l in writes if re.search(r"cmd=0x[0-9a-f]{2}0[45] ", l))
    ana = [l for l in trace if "ANA write" in l]
    route_lin = any("addr=0x82001a6c val=0xac" in l for l in ana)
    route_hp = any("addr=0x82001a78 val=0x84" in l for l in ana)
    pad = any("addr=0x8c00017c val=0x00000111" in l for l in trace)
    rst = any("addr=0x8a000080" in l and "val=0x00008000" in l for l in trace)
    result = {"chip": args.chip, "tunes": len(tunes), "stations_found": hits, "reads_started": id_reads,
              "bytes_written": table_bytes, "nor_tables": nor_tables, "linein_route": route_lin,
              "hp_route": route_hp, "pad_32k": pad, "reset_released": rst}
    (out / "results.json").write_text(json.dumps(result, indent=2))
    assert id_reads, ("tuner never addressed", result)
    tables = 5 if args.chip == "n" else 6
    assert nor_tables >= tables, ("NOR init tables not streamed", result)
    assert hits == [9230, 10000], ("scan missed a station", result)
    assert route_lin and route_hp, ("analog FM route not set", result)
    assert pad and rst, ("tuner clock/reset not driven", result)
    print("PASS " + json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
