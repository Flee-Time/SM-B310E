#!/usr/bin/env python3
"""Quiet display during headset-mic recording, in QEMU.

The headset mic picks up the LCD bus, so while recording from the headset
mic the panel is refreshed at most twice a second, at full rate for 0.5 s
after a key, and not at all (backlight LED off) after 5 s without keys.
Phone-mic and FM recordings are left alone.

Each case starts a recording, waits 9 s, presses a key, waits, stops,
and reads the LCDC refresh trace (sc6530_lcdc_refresh, timestamped with
-msg timestamp=on) and the LED writes (sc6530_ana_write to 0x82001220):
- headset mic: <= 2.5 refreshes/s in seconds 1.5-4.5 of the recording,
  none in the quiet window (6 s until the key), one within 0.5 s of the
  key, and the LED switched off (off bit 0x20 set, on bit 0x40 clear) in
  between;
- phone mic and FM: more than 3 refreshes/s in both windows.
"""
import argparse
from datetime import datetime, timezone
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


def run(args, name, config, qemu_args, quiet):
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "start in screen: recording\nrec format: wave\nrec frequency: 16\n" + config +
        "backlight timeout: on\nidle poweroff: 0\n").encode())
    image_path = out / "sd.img"
    FAT["write_image"](image_path, root, 64, True)

    trace = out / "trace.log"
    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(trace),
               "-msg", "timestamp=on",
               "--trace", "sc6530_lcdc_refresh", "--trace", "sc6530_ana_write",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image_path.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
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
            t0 = time.time()
            qmp.hmp("sendkey ret 100")              # start recording
            time.sleep(9)
            t1 = time.time()
            qmp.hmp("sendkey down 100")             # any key
            time.sleep(2)
            qmp.hmp("sendkey f2 180")               # stop
            time.sleep(3)
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    # -msg timestamp=on prefixes each line with an ISO-8601 UTC time.
    stamp = r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)Z "

    def epoch(s):
        return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%f").replace(
            tzinfo=timezone.utc).timestamp()

    text = trace.read_text(errors="replace")
    kicks = [epoch(t) for t in re.findall(stamp + r"sc6530_lcdc_refresh", text)]
    led = [(epoch(t), int(v, 16)) for t, v in re.findall(
        stamp + r"sc6530_ana_write ANA write addr=0x82001220 val=0x([0-9a-f]+)", text)]
    assert kicks, (name, "no timestamped refreshes in the trace")

    def count(a, b):
        return sum(a <= k < b for k in kicks)

    early = count(t0 + 1.5, t0 + 4.5) / 3.0
    quiet_window = count(t0 + 6, t1)
    after_key = count(t1, t1 + 0.5)
    led_off = any(t0 + 4 <= t < t1 and v & 0x60 == 0x20 for t, v in led)
    result = {"case": name, "early_per_s": round(early, 2),
              "quiet_window_refreshes": quiet_window,
              "after_key_refreshes": after_key, "led_off_in_quiet": led_off}
    if quiet:
        assert 0 < early <= 2.5, result
        assert quiet_window == 0, result
        assert after_key >= 1, result
        assert led_off, result
    else:
        assert early > 3, result
        assert quiet_window / (t1 - t0 - 6) > 3, result
        assert not led_off, result
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
    mic = "rec source: mic\nrec channels: mono\nrec mic gain: 8\n"
    headset = ["-global", "sc6530_aux.headset-present=on",
               "-global", "sc6530_adi.headset-mic=on"]
    results = [
        run(args, "headset-mic", mic, headset, True),
        run(args, "phone-mic", mic, [], False),
        run(args, "fm", "rec source: fmradio\nrec channels: mono\nfm_region: eu\n",
            ["-global", "sc6530_aux.headset-present=on"], False),
    ]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
