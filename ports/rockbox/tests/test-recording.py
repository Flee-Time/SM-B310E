#!/usr/bin/env python3
"""Record from the emulated microphone through Rockbox and check the WAV file.

The QEMU VBC model feeds a synthetic tone into VBC AD0 only while the codec's
ADC, mic path and mic bias are powered (see emulator sc6530_adi.c). Rockbox
starts in the recording screen, Center starts a recording, Back stops it, and
the test reads the new WAV back out of the private SD image.
"""
import argparse
from array import array
import json
import math
import os
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "emulator/qemu/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))


def tone(samples, frequency, rate):
    phase = 2 * math.pi * frequency / rate
    real = sum(v * math.cos(phase * i) for i, v in enumerate(samples))
    imag = sum(v * math.sin(phase * i) for i, v in enumerate(samples))
    return 2 * math.hypot(real, imag) / len(samples)


def phase_jumps(samples, frequency, rate, window):
    """Count steps in the tone's phase between consecutive windows.

    The QEMU tone is phase-continuous, so a step means samples were lost
    (PCM buffer overflow drops a whole chunk). Windows hold whole cycles;
    a steady drift from a slightly different clock is subtracted."""
    w = 2 * math.pi * frequency / rate
    phases = []
    for start in range(0, len(samples) - window, window):
        re = sum(samples[start + i] * math.cos(w * (start + i)) for i in range(window))
        im = sum(samples[start + i] * math.sin(w * (start + i)) for i in range(window))
        phases.append(math.atan2(im, re))
    steps = [(b - a + math.pi) % (2 * math.pi) - math.pi
             for a, b in zip(phases, phases[1:])]
    drift = sorted(steps)[len(steps) // 2]
    return sum(abs(s - drift) > math.radians(20) for s in steps)


def wav_headers(image):
    found, start = {}, 0
    while (pos := image.find(b"RIFF", start)) >= 0:
        start = pos + 4
        if image[pos + 8:pos + 16] == b"WAVEfmt ":
            found[pos] = image[pos:pos + 44]
    return found


def read_wav(image, pos):
    channels, rate = struct.unpack_from("<HI", image, pos + 22)
    bits = struct.unpack_from("<H", image, pos + 34)[0]
    chunk = pos + 12
    while image[chunk:chunk + 4] != b"data":
        chunk += 8 + struct.unpack_from("<I", image, chunk + 4)[0]
    size = struct.unpack_from("<I", image, chunk + 4)[0]
    pcm = array("h", image[chunk + 8:chunk + 8 + size])
    if sys.byteorder != "little":
        pcm.byteswap()
    return channels, rate, bits, pcm


def run(args, name, frequency, config, qemu_args, expect):
    """expect: one (tone_hz, amplitude) per WAV channel."""
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime),
                            path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "start in screen: recording\nrec format: wave\n"
        f"rec frequency: {frequency // 1000}\n" + config +
        "backlight timeout: on\nidle poweroff: 0\n").encode())
    image_path = out / "sd.img"
    FAT["write_image"](image_path, root, 64, True)
    before = set(wav_headers(image_path.read_bytes()))

    port = API["free_port"]()
    command = [str(args.qemu), "-M", "b310e,boot-mode=rockbox", "-display", "none",
               "-serial", "none", "-monitor", "none", "-D", str(out / "trace.log"),
               "--trace", "sc6530_vbc_ad", "--trace", "sdhci_send_command",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.rockbox.as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={image_path.as_posix()},format=raw,if=none,id=sdcard",
               "-audiodev", "none,id=audio0", "-global", "sc6530_adi.audiodev=audio0",
               *qemu_args,
               "-icount", "shift=3,sleep=off"]
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
            time.sleep(8)
            qmp.command("screendump", {"filename": str(out / "ready.png"), "format": "png"})
            qmp.hmp("sendkey ret 180")              # start recording
            time.sleep(args.seconds)
            qmp.command("screendump", {"filename": str(out / "recording.png"), "format": "png"})
            # Slow-card cases stop with a short tap: the keypad scan must
            # keep running while the card programs (missed presses during
            # 48 kHz stereo FM recording, 2026-11-06).
            slow = any("write-busy-every" in a for a in qemu_args)
            qmp.hmp("sendkey f2 40" if slow else "sendkey f2 180")  # stop
            # A slow card makes QEMU spend real time on the status polls;
            # give the guest longer to flush and close the WAV.
            time.sleep(40 if slow else 4)
            qmp.command("screendump", {"filename": str(out / "stopped.png"), "format": "png"})
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    image = image_path.read_bytes()
    new = sorted(set(wav_headers(image)) - before)
    assert new, (name, "no new WAV file on the card")
    channels, rate, bits, pcm = read_wav(image, new[-1])
    assert (channels, rate, bits) == (len(expect), frequency, 16), (name, channels, rate, bits)
    frames = len(pcm) // channels
    assert frames >= rate, (name, "recording shorter than one second", frames)
    levels = []
    for c, tones in enumerate(expect):
        # One (hz, amplitude) per channel, or a list when a channel carries
        # several tones (the FM mono mix).
        tones = tones if isinstance(tones, list) else [tones]
        samples = pcm[c::channels]
        window = list(samples[frames // 2 - rate // 4:frames // 2 + rate // 4])
        for tone_hz, expected in tones:
            level = tone(window, tone_hz, rate)
            off = tone(window, tone_hz * 1.37, rate)
            assert abs(level - expected) < expected * .05, (name, c, tone_hz, level, expected)
            assert off < expected * .02, (name, c, "unexpected energy off the tone", off)
            levels.append(round(level))
    result = {"case": name, "seconds": frames / rate, "rate": rate,
              "channels": channels, "levels": levels,
              "expected": [round(e) for t in expect
                           for _, e in (t if isinstance(t, list) else [t])]}
    # The WAV goes out as multi-block writes (CMD25, up to 32 blocks each);
    # single-block CMD24 remains only for lone blocks such as FAT updates.
    log = (out / "trace.log").read_text(errors="replace")
    result["cmd25"] = log.count("sdhci_send_command CMD25 ")
    result["cmd24"] = log.count("sdhci_send_command CMD24 ")
    assert result["cmd25"] > 0, (name, "no multi-block writes", result)
    if any("fail-write-every" in a for a in qemu_args):
        log = (out / "trace.log").read_text(errors="replace")
        result["injected_write_faults"] = log.count("injected write fault")
        assert result["injected_write_faults"] > 0, (name, "no faults injected")
    if any("write-busy-every" in a for a in qemu_args):
        log = (out / "trace.log").read_text(errors="replace")
        result["injected_busy_writes"] = log.count("injected write busy")
        assert result["injected_busy_writes"] > 0, (name, "no slow writes injected")
        tone_hz = (expect[0] if isinstance(expect[0], tuple) else expect[0][0])[0]
        body = pcm[0::channels][rate // 5:frames - rate // 5]
        result["phase_jumps"] = phase_jumps(body, tone_hz, rate, rate // 100)
        assert result["phase_jumps"] == 0, (name, "audio dropped during slow writes",
                                            result["phase_jumps"])
    print("PASS " + json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("qemu", "rockbox", "runtime", "output"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--cases", help="comma-separated case names")
    args = parser.parse_args()
    for option in ("qemu", "rockbox", "runtime", "output"):
        setattr(args, option, getattr(args, option).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    def level(gain):
        return 8192 * (gain + 1) / 16
    cases = [
        ("mic-16k", 16000, "rec source: mic\nrec channels: mono\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000"], [(1000, level(8))]),
        ("mic-8k-gain15", 8000, "rec source: mic\nrec channels: mono\nrec mic gain: 15\n",
         ["-global", "sc6530_adi.mic-tone-hz=440"], [(440, level(15))]),
        # Headset with a mic: detected on ADC channel 1, its mic is used
        # at 2/3 of the mic gain setting (stock's lower headset table).
        ("headset-mic", 16000, "rec source: mic\nrec channels: mono\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000",
          "-global", "sc6530_aux.headset-present=on",
          "-global", "sc6530_adi.headset-mic=on"],
         [(1000, level((8 * 2 + 1) // 3))]),     # headset runs at 2/3 gain
        # Headphones without a mic: falls back to the phone's own mic.
        ("headphones-no-mic", 16000, "rec source: mic\nrec channels: mono\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000",
          "-global", "sc6530_aux.headset-present=on"], [(1000, level(8))]),
        # FM line-in: ADC R does not work on the SC6530C, so FM is recorded
        # as stock does, both channels (600 Hz left, 900 Hz right) mixed into
        # ADC L, and written to both WAV channels.
        ("fm-mono-mix", 16000, "rec source: fmradio\nrec channels: stereo\n"
         "rec left gain: 8\nrec right gain: 8\nfm_region: eu\n",
         ["-global", "sc6530_aux.headset-present=on"],
         [[(600, level(8)), (900, level(8))], [(600, level(8)), (900, level(8))]]),
        # Every 7th SD write command fails (lost on the bus): the driver has
        # to recover and retry, or the FAT writeback panics (2026-11-06).
        ("sd-write-faults", 16000, "rec source: mic\nrec channels: mono\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000",
          "-global", "sc6530_sdio.fail-write-every=7", "-d", "unimp"],
         [(1000, level(8))]),
        # Slow card: every 30th write command stays busy for 1 s (the phone
        # showed ~1 s stalls). At 48 kHz the PCM buffer holds 1.2 s; the
        # recording must not lose audio (warning 0x1 on the phone). With
        # 16 KB multi-block writes there are ~6 write commands a second.
        ("sd-slow-card", 48000, "rec source: mic\nrec channels: mono\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000",
          "-global", "sc6530_sdio.write-busy-every=30",
          "-global", "sc6530_sdio.write-busy-ms=1000", "-d", "unimp"],
         [(1000, level(8))]),
        # 48 kHz stereo WAV (192 KB/s) on a card that takes 1 ms longer on
        # every write command: Rockbox flushes hundreds of blocks at once,
        # and spinning through them starved the encoder (warning 0x1 on the
        # phone with WAV at 48 kHz, not with MP3).
        ("sd-48k-stereo-wav", 48000, "rec source: mic\nrec channels: stereo\nrec mic gain: 8\n",
         ["-global", "sc6530_adi.mic-tone-hz=1000",
          "-global", "sc6530_sdio.write-busy-every=1",
          "-global", "sc6530_sdio.write-busy-ms=1", "-d", "unimp"],
         [(1000, level(8)), (1000, level(8))]),
    ]
    if args.cases:
        cases = [c for c in cases if c[0] in args.cases.split(",")]
    results = [run(args, *case) for case in cases]
    (args.output / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
