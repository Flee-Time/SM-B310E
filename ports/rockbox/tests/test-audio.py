#!/usr/bin/env python3
"""Compile the real Rockbox playback stack for ARM and run it in B310E QEMU.

Run the full port build first to prepare the pinned upstream headers/config.
No firmware dump, SD image, vendor source, Python packages or driver stubs
are required. The small harness replaces only application/scheduler hooks.
"""
import argparse
from array import array
import json
import os
from pathlib import Path
import subprocess
import sys
import runpy
import socket
import threading
import time
import wave

PORT = Path(__file__).resolve().parents[1]
REPO = PORT.parents[1]
FRAMES = 4093


def hotplug_run(command, env, port):
    QTest = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))["QTest"]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    timeout = threading.Timer(20, process.kill)
    timeout.start()
    qt = None
    output = []
    try:
        for _ in range(100):
            try:
                qt = QTest(port)
                break
            except OSError:
                if process.poll() is not None:
                    break
                time.sleep(0.01)
        assert qt is not None, "hotplug QTest connection failed"
        for line in process.stderr:
            output.append(line)
            if line.strip() in ("HOTPLUG INSERT", "HOTPLUG REMOVE"):
                level = int(line.strip() == "HOTPLUG REMOVE")
                qt.command(f"set_irq_in /machine/peripheral/sc6530-aux eic-input 0 {level}")
        process.wait(timeout=5)
        return subprocess.CompletedProcess(command, process.returncode, process.stdout.read(), "".join(output))
    finally:
        timeout.cancel()
        if qt:
            qt.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)


def run(command, log):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
    log.write(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True, help="directory containing arm-none-eabi-gcc")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rates", default="8000,11025,12000,16000,22050,24000,32000,44100,48000,96000")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    upstream = REPO / "build/rockbox"
    fw = upstream / "firmware"
    config = upstream / "build-b310e"
    target = PORT / "firmware/target/arm/sc6530c"
    if not (config / "autoconf.h").exists():
        parser.error("run the root build.ps1 rockbox (or build.sh rockbox) first")
    exe = ".exe" if os.name == "nt" else ""
    cc = args.toolchain / ("arm-none-eabi-gcc" + exe)
    objcopy = args.toolchain / ("arm-none-eabi-objcopy" + exe)
    flags = ["-mcpu=arm926ej-s", "-marm", "-Os", "-std=gnu11", "-DB310E", "-DMEMORYSIZE=4",
             "-ffreestanding", "-fno-delete-null-pointer-checks", "-ffunction-sections",
             "-fdata-sections", "-fno-common", "-Werror", "-Wno-pointer-sign"]
    for path in [config, target, fw / "target/arm", fw / "export", fw,
                 fw / "drivers", fw / "include", fw / "libc/include", fw / "kernel/include", upstream / "lib/fixedpoint",
                 upstream / "lib/rbcodec", upstream / "apps", upstream / "apps/gui",
                 upstream / "apps/recorder", upstream / "lib/rbcodec/metadata",
                 upstream / "lib/rbcodec/dsp", upstream / "lib/skin_parser"]:
        flags += ["-I" + str(path)]
    sources = [target / "crt0.S", target / "system-sc6530c.c", target / "pcm-sc6530c.c",
               target / "audiohw-sc6530c.c", target / "button-sc6530c.c", fw / "target/arm/mmu-arm.S",
               fw / "pcm.c", fw / "pcm_sw_volume.c", fw / "pcm_sampr.c", fw / "general.c",
               upstream / "apps/audio_path.c",
               upstream / "lib/fixedpoint/fixedpoint.c"]
    objects = []
    with (args.output / "build.log").open("w") as log:
        for src in sources:
            obj = args.output / (src.stem + ".o")
            run([cc, *flags, "-c", src, "-o", obj], log)
            objects.append(obj)
        cases = [(int(rate), 0, 0, FRAMES, "rate-" + rate) for rate in args.rates.split(",")]
        cases += [(48000, -60, 0, FRAMES, "volume-minus6db"), (48000, -2147483648, 0, FRAMES, "software-mute"),
                  (48000, 0, 1, FRAMES, "stop-restart"), (48000, 0, 2, FRAMES, "dma-error"),
                  (48000, 0, 3, FRAMES, "nested-lock")]
        cases += [(44100, 0, 0, FRAMES, "headset-playback"),
                  (48000, 0, 4, FRAMES, "speaker-route-switch"),
                  (48000, 0, 4, FRAMES, "headset-route-switch"),
                  (48000, 0, 5, FRAMES, "codec-reinit")]
        cases += [(48000, 0, 0, n, f"short-{n}") for n in [1, 13, 159, 160]]
        cases += [(48000, db * 10, 0, FRAMES, f"headset-gain-plus{db}db")
                  for db in [1, 2, 3, 4, 6, 9, 12, 15, 18, 21, 24]]
        cases += [(48000, 240, 4, FRAMES, "headset-gain-route-switch"),
                  (48000, 240, 5, FRAMES, "headset-gain-reinit"),
                  (48000, 240, 0, FRAMES, "speaker-gain-cap"),
                  (48000, 1000, 0, FRAMES, "headset-gain-clamp"),
                  (48000, 240, 6, FRAMES, "jack-hotplug")]
        results = []
        for rate, volume, case, frames, name in cases:
            headset = name.startswith("headset-")
            out = args.output / name
            out.mkdir(exist_ok=True)
            obj = out / "main.o"
            run([cc, *flags, f"-DTEST_RATE={rate}", f"-DTEST_VOLUME={volume}", f"-DTEST_CASE={case}", f"-DTEST_FRAMES={frames}",
                 f"-DTEST_HEADSET={int(headset)}", "-c", PORT / "tests/audio-main.c", "-o", obj], log)
            elf, binary = out / "audio.elf", out / "audio.bin"
            run([cc, "-mcpu=arm926ej-s", "-nostdlib", "-Wl,--gc-sections",
                 "-Wl,--no-warn-rwx-segments", "-T", PORT / "tests/audio.lds", *objects,
                 obj, "-lgcc", "-lc", "-o", elf], log)
            run([objcopy, "-O", "binary", elf, binary], log)
            wav = out / "audio.wav"
            command = [str(args.qemu.resolve()), "-M", "b310e,boot-mode=rockbox", "-display", "none",
                       "-icount", "shift=3,sleep=off",
                       "-serial", "none", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                       "-drive", f"file={binary.as_posix()},format=raw,if=none,id=os,readonly=on",
                       "-audiodev", f"wav,id=audio0,path={wav.as_posix()},out.frequency={rate},out.channels=2,out.format=s16",
                       "-global", "sc6530_adi.audiodev=audio0", "-D", str(out / "trace.log"),
                       "--trace", "sc6530_dma_*", "--trace", "sc6530_vbc_*", "--trace", "sc6530_ana_write"]
            if headset:
                command += ["-global", "sc6530_aux.headset-present=on"]
            if case == 6:
                with socket.socket() as sock:
                    sock.bind(("127.0.0.1", 0))
                    test_port = sock.getsockname()[1]
                command += ["-qtest", f"tcp:127.0.0.1:{test_port},server=on,wait=off",
                            "-qtest-log", str(out / "qtest.log")]
            (out / "command.json").write_text(json.dumps(command, indent=2))
            env = os.environ.copy()
            if os.name == "nt":
                env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
            result = (hotplug_run(command, env, test_port) if case == 6 else
                      subprocess.run(command, capture_output=True, text=True, env=env, timeout=20))
            (out / "stderr.log").write_text(result.stdout + result.stderr)
            assert result.returncode == 0 and "PASS Rockbox ARM audio" in result.stderr, (name, result.stderr)
            with wave.open(str(wav), "rb") as w:
                assert w.getframerate() == rate and w.getnchannels() == 2 and w.getsampwidth() == 2
                pcm = array("h", w.readframes(w.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
            stereo = list(zip(pcm[::2], pcm[1::2]))
            audible = [(l, r) for l, r in stereo if l or r]
            expected = [(1200 + i % 200, -2500 - i % 200) for i in range(frames)]
            if case in (1, 5):
                second = [(l, r) for l, r in audible if l >= 20000]
                assert second == [(20000 + i % 200, -21000 - i % 200) for i in range(FRAMES)], (name, len(second))
            elif case == 2:
                assert len(audible) < FRAMES
            elif case in (4, 6):
                if case == 4:
                    assert len(audible) < FRAMES  # intentional mute while switching
                else:
                    assert len(audible) >= FRAMES
                assert all(l > 0 and r < 0 and r == -l - 1300 for l, r in audible)
            elif volume == -2147483648:
                assert not audible
            elif volume == -60:
                assert len(audible) == FRAMES, (name, len(audible))
                ratios = [l / expected[i][0] for i, (l, _) in enumerate(audible)]
                assert all(0.49 < ratio < 0.51 for ratio in ratios)
            elif volume > 0 and headset:
                volume = min(volume, 240)
                residual = volume - ((volume + 29) // 30) * 30
                factor = 10 ** (residual / 200)
                assert len(audible) == frames, (name, len(audible))
                assert all(abs(l - expected[i][0] * factor) <= 2 and
                           abs(r - expected[i][1] * factor) <= 2
                           for i, (l, r) in enumerate(audible)), (name, residual, audible[:12])
            else:
                assert audible == expected, (name, len(audible), audible[:12])
            if case == 0 and volume != -2147483648:
                positions = [i for i, pair in enumerate(stereo) if any(pair)]
                assert positions[-1] - positions[0] + 1 == frames, (name, "gap in stream")
            stats = {"case": name, "rate": rate, "frames": len(stereo), "audible_frames": len(audible)}
            results.append(stats)
            print("PASS " + json.dumps(stats), flush=True)
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
