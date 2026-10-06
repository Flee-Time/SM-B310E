#!/usr/bin/env python3
"""Exercise the full player's CPU idle, LCD timeout and pause/resume wake paths.

Uses normal keypad events and observes MMIO; never patches guest RAM.
QEMU checks behavior, not electrical power consumption.
"""
import argparse
from array import array
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import struct
import sys
import time
import wave

REPO = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(REPO / "tools/qemu-b310e/scripts/test-audio.py"))
FAT = runpy.run_path(str(Path(__file__).with_name("make-sd-image.py")))


def saved_files(image):
    """Read the resulting FAT32 directory chains, including grown directories."""
    data = image.read_bytes()
    start = struct.unpack_from("<I", data, 454)[0]
    boot = start * 512
    spc = data[boot + 13]
    reserved = struct.unpack_from("<H", data, boot + 14)[0]
    fat_sectors = struct.unpack_from("<I", data, boot + 36)[0]
    first_fat = (start + reserved) * 512
    fat = data[first_fat:first_fat + fat_sectors * 512]
    assert fat == data[first_fat + fat_sectors*512:first_fat + fat_sectors*1024], "FAT copies differ"
    first_data = start + reserved + 2 * fat_sectors

    def chain(cluster):
        seen, result = set(), bytearray()
        while 2 <= cluster < 0x0ffffff8:
            assert cluster not in seen, "cyclic FAT chain"
            seen.add(cluster)
            offset = (first_data + (cluster - 2) * spc) * 512
            result += data[offset:offset + spc * 512]
            cluster = struct.unpack_from("<I", fat, cluster * 4)[0] & 0x0fffffff
        return bytes(result)

    result = {}

    def walk(cluster, path):
        directory = chain(cluster)
        longname = {}
        for offset in range(0, len(directory), 32):
            entry = directory[offset:offset + 32]
            if entry[0] == 0:
                break
            if entry[0] == 0xe5:
                longname.clear()
                continue
            if entry[11] == 0x0f:
                longname[entry[0] & 0x1f] = entry[1:11] + entry[14:26] + entry[28:32]
                continue
            if longname:
                units = b"".join(longname[k] for k in sorted(longname))
                name = units.decode("utf-16le").split("\0", 1)[0].rstrip("\uffff")
            else:
                name = entry[:8].decode().rstrip() + ("." + entry[8:11].decode().rstrip() if entry[8:11].strip() else "")
            longname.clear()
            if name in (".", "..") or entry[11] & 8:
                continue
            first = struct.unpack_from("<H", entry, 26)[0] | struct.unpack_from("<H", entry, 20)[0] << 16
            full = path + "/" + name
            if entry[11] & 0x10:
                walk(first, full)
            else:
                size = struct.unpack_from("<I", entry, 28)[0]
                result[full] = chain(first)[:size]

    walk(struct.unpack_from("<I", data, boot + 44)[0], "")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--rockbox", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--headset", action="store_true")
    parser.add_argument("--controls", action="store_true",
                        help="Also exercise volume, track skip/seek, Menu and Back")
    parser.add_argument("--shutdown", choices=["manual", "idle"],
                        help="Power off a paused player by holding END or idle timeout")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = ".exe" if os.name == "nt" else ""
    nm = subprocess.check_output([args.toolchain / ("arm-none-eabi-nm" + exe),
                                  args.elf], text=True)
    tick = int(re.search(r"^([0-9a-f]+) [A-Za-z] current_tick$", nm, re.M)[1], 16)
    keypad_count = int(re.search(r"^([0-9a-f]+) [A-Za-z] s_keypad_irq_count$", nm, re.M)[1], 16)
    stray_count = int(re.search(r"^([0-9a-f]+) [A-Za-z] s_stray_irq_count$", nm, re.M)[1], 16)
    volume_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] global_status$", nm, re.M)[1], 16)
    wps_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] wps_state$", nm, re.M)[1], 16)
    # action.c's action_last_t begins action, tick, button, context;
    # all are 32-bit on the pinned ARM target.
    context_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] action_last$", nm, re.M)[1], 16) + 12
    held_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] held_mask$", nm, re.M)[1], 16)
    # Obtain inspection offsets from the pinned target's actual C headers.
    # These constants never become guest code and no guest RAM is patched.
    upstream = REPO / "build/rockbox"
    layout = out / "layout.c"
    layout.write_text('#include <stddef.h>\n#include "settings.h"\n'
                      '#include "metadata.h"\n#include "wps.h"\n'
                      'const unsigned int layout[] = {offsetof(struct system_status, volume), '
                      'offsetof(struct wps_state, id3), offsetof(struct mp3entry, elapsed), '
                      'CONTEXT_WPS, ALLOW_SOFTLOCK};\n')
    includes = [args.elf.parent, upstream / "firmware/export", upstream / "firmware/include",
                upstream / "firmware/libc/include", upstream / "firmware/kernel/include",
                upstream / "firmware/target/arm/sc6530c", upstream / "firmware/target/arm",
                upstream / "firmware", upstream / "apps", upstream / "apps/gui",
                upstream / "apps/recorder", upstream / "lib/skin_parser",
                upstream / "lib/rbcodec", upstream / "lib/rbcodec/metadata",
                upstream / "lib/rbcodec/dsp", upstream / "lib/fixedpoint"]
    # action.h supplies CONTEXT_WPS without depending on enum values here.
    layout.write_text('#include "action.h"\n' + layout.read_text())
    subprocess.run([str(args.toolchain / ("arm-none-eabi-gcc" + exe)),
                    "-mcpu=arm926ej-s", "-DB310E", "-DMEMORYSIZE=4", "-c", str(layout),
                    "-o", str(out / "layout.o"), *["-I" + str(p) for p in includes]], check=True)
    subprocess.run([str(args.toolchain / ("arm-none-eabi-objcopy" + exe)),
                    "-O", "binary", "--only-section=.rodata", str(out / "layout.o"),
                    str(out / "layout.bin")], check=True)
    volume_offset, id3_offset, elapsed_offset, wps_context, softlock_flag = struct.unpack("<5I", (out / "layout.bin").read_bytes())
    volume_addr += volume_offset
    wps_addr += id3_offset
    disassembly = subprocess.check_output([
        args.toolchain / ("arm-none-eabi-objdump" + exe), "-d",
        "--disassemble=core_idle", args.elf], text=True)
    assert re.search(r"mcr\s+15, 0, r\d+, cr7, cr0, \{4\}", disassembly), disassembly
    (out / "core-idle.txt").write_text(disassembly)

    root = FAT["directory"]()
    for path in sorted(args.runtime.rglob("*")):
        if path.is_file():
            FAT["add_file"](root, Path(".rockbox") / path.relative_to(args.runtime), path.read_bytes())
    FAT["add_file"](root, "progs/rockbox.bin", args.rockbox.read_bytes())
    FAT["add_file"](root, "test.wav", FAT["test_audio"](60))
    if args.controls:
        FAT["add_file"](root, "z-second.wav", FAT["test_audio"](60))
    FAT["add_file"](root, ".rockbox/config.cfg", (
        "volume: 0\nspeaker mode: auto\nstart in screen: files\n"
        "start directory: /\nbacklight timeout: 2\n"
        "backlight filters first keypress: off\n" +
        ("idle poweroff: 1\n" if args.shutdown == "idle" else "")).encode())
    FAT["write_image"](out / "sd.img", root, 64, True)
    qmp_port, test_port = API["free_port"](), API["free_port"]()
    command = [str(args.qemu.resolve()), "-M", "b310e,boot-mode=rockbox",
               "-display", "none", "-serial", "none", "-monitor", "none",
               "-drive", f"file={args.rockbox.resolve().as_posix()},format=raw,if=none,id=os,readonly=on",
               "-drive", f"file={(out / 'sd.img').as_posix()},format=raw,if=none,id=sdcard",
               "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
               "-qtest", f"tcp:127.0.0.1:{test_port},server=on,wait=off",
               "-qtest-log", str(out / "qtest.log"),
               "-audiodev", f"wav,id=audio0,path={(out / 'audio.wav').as_posix()},out.frequency=44100",
               "-global", "sc6530_adi.audiodev=audio0",
               "-D", str(out / "trace.log")]
    for pattern in ["sc6530_lcm_command", "sc6530_lcdc_refresh", "sc6530_aux_write",
                    "sc6530_dma_*", "sc6530_vbc_*"]:
        command += ["--trace", pattern]
    if args.headset:
        command += ["-global", "sc6530_aux.headset-present=on"]
    (out / "command.json").write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = "C:/msys64/mingw64/bin;" + env.get("PATH", "")
    states, qmp, qt = [], None, None
    with (out / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited; see stderr.log")
                try:
                    qmp = API["QMP"](qmp_port)
                    break
                except OSError:
                    time.sleep(.05)
            if qmp is None:
                raise RuntimeError("QMP startup timeout")
            qt = API["QTest"](test_port)

            def state():
                return {"tick": qt.read(tick), "keypad_irqs": qt.read(keypad_count),
                        "stray_irqs": qt.read(stray_count), "irq_mask": qt.read(0x80000008),
                        "lcdc": qt.read(0x20d00000),
                        "lcm_mode": qt.read(0x20800010), "backlight": qt.read(0x82001220),
                        "keylight": qt.read(0x82001224), "vbc": qt.read(0x82003018),
                        "sd_clock": qt.read(0x2070002c)}

            def snapshot(name):
                s = state()
                s.update(held=qt.read(held_addr), action=qt.read(context_addr - 12),
                         last_button=qt.read(context_addr - 4), context=qt.read(context_addr))
                states.append({"name": name, **s})
                qmp.command("screendump", {"filename": str(out / (name + ".ppm")), "format": "ppm"})
                qmp.command("screendump", {"filename": str(out / (name + ".png")), "format": "png"})
                if name.startswith("failure-"):
                    (out / "failure-registers.txt").write_text(qmp.hmp("info registers"))
                    qmp.hmp(f'pmemsave 0x04000000 0x400000 "{(out / "stalled-psram.bin").as_posix()}"')
                print("PASS " + name + " " + json.dumps(s), flush=True)
                return s

            def asleep():
                s = state()
                return not (s["lcdc"] & 1 or s["backlight"] & 0x40) and s["lcm_mode"] == 1

            def wait_for(condition, name):
                deadline = time.monotonic() + 8
                while not condition():
                    if time.monotonic() > deadline:
                        snapshot("failure-" + name)
                        raise AssertionError(name)
                    time.sleep(.1)

            def key(name, hold_ms=180):
                before = qt.read(keypad_count)
                qmp.hmp(f"sendkey {name} {hold_ms}")
                time.sleep(hold_ms / 1000 + .4)
                # A coincident button tick can acknowledge the edge before
                # IRQ8 dispatch. Check delivery across the full run instead.
                assert qt.read(keypad_count) >= before
                assert qt.read(stray_count) == 0, "unhandled wake interrupt"

            def volume():
                v = qt.read(volume_addr)
                return v if v < 0x80000000 else v - 0x100000000

            def context():
                return qt.read(context_addr) & ~softlock_flag

            def track():
                entry = qt.read(wps_addr)
                data = bytes.fromhex(qt.command(f"read {entry:#x} 260")[0].removeprefix("0x"))
                return data.split(b"\0", 1)[0].decode(), qt.read(entry + elapsed_offset)

            def awake():
                s = state()
                return s["lcdc"] & 1 and s["backlight"] & 0x40 and s["lcm_mode"] == 0x28

            def idle(name, playing):
                wait_for(asleep, name)
                time.sleep(.3)  # panel settling must also finish
                before = snapshot(name)
                assert bool(before["vbc"] & 0xe000) == playing, before
                assert not before["keylight"] & 0x20, before
                assert not before["sd_clock"] & 4, "card clock should already be gated between commands"
                count = (out / "trace.log").read_text().count("sc6530_lcdc_refresh")
                time.sleep(1)
                after = state()
                assert after["tick"] > before["tick"], "IRQ-masked CPU sleep lost timer wakeup"
                assert (out / "trace.log").read_text().count("sc6530_lcdc_refresh") == count, "DMA refresh while LCD asleep"
                picture = (out / (name + ".ppm")).read_bytes()[-128*160*3:]
                assert not any(picture), "sleeping panel must be blank"

            time.sleep(4)
            if args.shutdown == "idle":
                timeout_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] poweroff_timeout$", nm, re.M)[1], 16)
                assert qt.read(timeout_addr) == 1, "private config did not set one-minute idle timeout"
            idle("idle-screen-off", False)
            key("down")  # progs/ -> test.wav; config allows this wake key's action
            wait_for(awake, "keypad wake")
            snapshot("keypad-wake")
            key("ret")
            wait_for(lambda: state()["vbc"] & 0xe000, "playback start")
            idle("playing-screen-off", True)
            key("down")  # volume down wakes the WPS while playback continues
            wait_for(awake, "playback wake")
            snapshot("playing-wake")
            key("ret")  # Center: pause/resume
            wait_for(lambda: not state()["vbc"] & 0xe000, "pause")
            idle("paused-screen-off", False)
            key("ret")
            wait_for(lambda: state()["vbc"] & 0xe000, "resume")
            wait_for(awake, "resume wake")
            snapshot("resumed")
            idle("resumed-screen-off", True)
            key("down", 900)  # held matrix key: repeats must not wedge wakeup
            wait_for(awake, "held key wake")
            snapshot("held-key-wake")
            idle("held-key-screen-off", True)
            if args.controls:
                before = volume()
                key("up")
                assert volume() > before, "Up did not raise volume"
                key("down")
                assert volume() == before, "Down did not lower volume"
                key("right")
                wait_for(lambda: track()[0].endswith("/z-second.wav"), "Right skip next")
                key("left")
                wait_for(lambda: track()[0].endswith("/test.wav"), "Left skip previous")
                time.sleep(1.1)  # Rockbox's immediate skip-then-hold shortcut changes directory
                before_seek = track()[1]
                key("right", 1500)
                assert track()[0].endswith("/test.wav"), "seek release skipped track"
                assert track()[1] > before_seek + 2500, "Right hold did not seek"
                time.sleep(2)  # let the seek refill finish before the next key
                before_seek = track()[1]
                key("left", 1500)
                assert track()[0].endswith("/test.wav"), "backward seek release skipped track"
                assert track()[1] < before_seek, ("Left hold did not seek back", before_seek, track())
                snapshot("seek-controls")
                time.sleep(2)  # the backward seek also refills the audio buffer
                key("f2")
                time.sleep(1)
                before = volume()
                key("up")
                assert volume() == before, "Back did not leave playback for the file browser"
                snapshot("back-to-files")
                key("down")
                assert context() != wps_context, "Back stayed in WPS"
                key("kp_enter")  # DIAL returns to WPS from the file browser
                before = volume()
                key("up")  # last context updates on a button, not an idle poll
                assert volume() > before, "DIAL did not return to playback"
                wait_for(lambda: context() == wps_context, "return to playback")
                key("f1")
                before = volume()
                key("down")
                assert volume() == before, "Menu did not leave playback"
                assert context() != wps_context, "Menu stayed in WPS"
                snapshot("main-menu")
                key("f2")
                time.sleep(1)
                before = volume()
                key("up")
                snapshot("back-from-menu")
                key("down")
                before = volume()
                key("up")
                assert volume() > before, "Back did not return from Menu to playback"
                wait_for(lambda: context() == wps_context, "return from menu")
                key("ret", 900)
                time.sleep(2)  # context menu can wait for filesystem metadata
                before = volume()
                key("down")
                assert volume() == before, "Center hold did not open context menu"
                key("up")
                wait_for(lambda: context() != wps_context, "Center hold context menu")
                key("f2")
                key("up")
                wait_for(lambda: context() == wps_context, "return from context menu")
                assert state()["vbc"] & 0xe000, "Center hold/release paused playback"
                print("PASS primary volume/skip/seek/Menu/Back controls", flush=True)
            if args.shutdown:
                qmp.hmp("trace-event sc6530_ana_write on")
                key("ret")
                wait_for(lambda: not state()["vbc"] & 0xe000, "pause before shutdown")
                snapshot("paused-before-shutdown")
                if args.shutdown == "manual":
                    time.sleep(15)  # include the delayed resume/settings flush
                    qmp.hmp("sendkey esc 4000")
                deadline = time.monotonic() + (90 if args.shutdown == "idle" else 12)
                power_symbols = {name: int(re.search(r"^([0-9a-f]+) [A-Za-z] " + name + "$", nm, re.M)[1], 16)
                                 for name in ["poweroff_timeout", "last_event_tick", "sd_activity", "shutdown_timeout"]}
                previous_tick = None
                while process.poll() is None and time.monotonic() < deadline:
                    try:
                        debug = {name: qt.read(addr) for name, addr in power_symbols.items()}
                        debug["tick"] = qt.read(tick)
                        print("SHUTDOWN " + json.dumps(debug), flush=True)
                        if previous_tick == debug["tick"]:
                            (out / "stalled-registers.txt").write_text(qmp.hmp("info registers"))
                            panic_addr = int(re.search(r"^([0-9a-f]+) [A-Za-z] panic_buf$", nm, re.M)[1], 16)
                            panic = bytes.fromhex(qt.command(f"read {panic_addr:#x} 128")[0].removeprefix("0x"))
                            (out / "panic.txt").write_text(panic.split(b"\0", 1)[0].decode(errors="replace"))
                            qmp.hmp(f'pmemsave 0x04000000 0x400000 "{(out / "stalled-psram.bin").as_posix()}"')
                            snapshot("stalled-shutdown")
                            raise AssertionError("kernel tick stopped during shutdown; see stalled-registers.txt")
                        previous_tick = debug["tick"]
                    except (OSError, RuntimeError):
                        break
                    time.sleep(5)
                process.wait(timeout=5)
                assert process.returncode == 0, "shutdown did not exit cleanly"
                print("PASS paused " + args.shutdown + " power-off", flush=True)
            else:
                qmp.command("quit")
                process.wait(timeout=10)
        finally:
            (out / "state.json").write_text(json.dumps(states, indent=2))
            if qt:
                qt.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    trace = (out / "trace.log").read_text()
    commands = [int(v, 16) for v in re.findall(r"sc6530_lcm_command LCM offset=0x0 val=0x([0-9a-f]+)", trace)]
    assert commands.count(0x10) >= 5 and commands.count(0x11) >= 5, commands
    assert len(re.findall(r"addr=0x20500070 val=0x00001000", trace)) >= 5, "missing LCDC gate-off"
    assert len(re.findall(r"addr=0x20500060 val=0x00001000", trace)) >= 5, "missing LCDC gate restore"
    if args.shutdown:
        set1 = trace.rfind("ANA write addr=0x82001184 val=0x1f")
        set0 = trace.rfind("ANA write addr=0x82001180 val=0x3fff")
        assert 0 <= set1 < set0, "missing ordered stock LDO power-off request"
        assert not re.search(r"ANA write addr=0x82001480 val=0x4000", trace), "power-off used watchdog reboot"
        files = saved_files(out / "sd.img")
        assert files["/test.wav"] == FAT["test_audio"](60), "SD writes corrupted test audio"
        assert files["/.rockbox/.playlist_control"].startswith(b"P:"), "playlist state was lost"
        # Native Rockbox deliberately promotes .new to .cfg on the next
        # boot, so a clean shutdown leaves the durable temporary file.
        resume = files["/.rockbox/.resume.cfg.new"]
        assert all(field in resume for field in (b"IDX:", b"ELA:", b"OFF:", b"PVS:")), "resume settings were lost"
        print("PASS saved playlist/resume files, FAT mirrors and intact test WAV", flush=True)
    assert states[-1]["keypad_irqs"] >= 3, "matrix IRQ8 was not serviced"
    with wave.open(str(out / "audio.wav"), "rb") as wav:
        samples = array("h", wav.readframes(wav.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        assert wav.getframerate() == 44100 and wav.getnchannels() == 2
        assert sum(bool(v) for v in samples) > 44100 * 4, "no sustained playback across sleep/resume"
        assert max(map(abs, samples)) <= 4096, "unexpected PCM corruption"
    print("PASS full Rockbox idle, LCD sleep, keypad wake and audio pause/resume", flush=True)


if __name__ == "__main__":
    main()
