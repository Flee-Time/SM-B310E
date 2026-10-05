#!/usr/bin/env python3
"""Bounded stock boot capture; never changes the input NOR image."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import re


class QMP:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.stream = self.sock.makefile("rwb")
        self.receive()
        self.command("qmp_capabilities")

    def receive(self):
        line = self.stream.readline()
        if not line:
            raise RuntimeError("QEMU closed QMP")
        return json.loads(line)

    def command(self, name, arguments=None):
        self.stream.write(json.dumps({"execute": name,
                                     "arguments": arguments or {}}).encode() + b"\n")
        self.stream.flush()
        while True:
            msg = self.receive()
            if "error" in msg:
                raise RuntimeError(msg["error"])
            if "return" in msg:
                return msg["return"]

    def hmp(self, text):
        return self.command("human-monitor-command", {"command-line": text})

    def close(self):
        self.stream.close()
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mode", choices=["stock", "warm"], default="stock")
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--audio", action="store_true")
    parser.add_argument("--no-overlays", action="store_true")
    parser.add_argument("--hold-end", action="store_true")
    parser.add_argument("--gpio49-high", action="store_true",
                        help="Experimental external input for the stock LCD sleep/wake wait")
    parser.add_argument("--key", action="append", default=[],
                        help="Wall seconds:key[:hold_ms], e.g. 65:f2 or 68:asterisk:2000")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    machine = f"b310e,boot-mode={args.mode}" + (",boot-overlays=off" if args.no_overlays else "")
    if args.hold_end:
        machine += ",hold-end=on"
    if args.gpio49_high:
        machine += ",gpio49-high=on"
    command = [str(args.qemu.resolve()), "-M", machine,
               "-display", "none", "-serial", "none", "-d", "guest_errors", "-D",
               str(args.output / "trace.log"),
               "--trace", "sc6530_ana_write", "--trace", "sc6530_lcdc_refresh",
               "--trace", "sc6530_vbc_*", "--trace", "sc6530_dma_*",
               "--trace", "sc6530_midi_render",
               "--trace", "sc6530_gpt_write",
               "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
               "-drive", f"file={args.firmware.resolve().as_posix()},format=raw,if=none,id=nor,readonly=on"]
    if args.audio:
        command += ["-audiodev", f"wav,id=audio0,path={(args.output / 'audio.wav').as_posix()}",
                    "-global", "sc6530_adi.audiodev=audio0"]
    (args.output / "command.json").write_text(json.dumps(command, indent=2))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
    qmp = None
    with (args.output / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, stdout=stderr, stderr=stderr, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited; see stderr.log")
                try:
                    qmp = QMP(port)
                    break
                except OSError:
                    time.sleep(0.1)
            if qmp is None:
                raise RuntimeError("QMP startup timed out")
            started = time.monotonic()
            keys = []
            for item in args.key:
                fields = item.split(":")
                if len(fields) not in (2, 3):
                    raise ValueError("--key must be seconds:key[:hold_ms]")
                when, key = float(fields[0]), fields[1]
                hold = int(fields[2]) if len(fields) == 3 else 100
                if (not 0 <= when < args.seconds or not 1 <= hold <= 10000 or
                        not re.fullmatch(r"[a-z0-9_-]+", key)):
                    raise ValueError("--key requires a valid time, key and 1..10000 ms hold")
                keys.append((when, key, hold))
            for index, (when, key, hold) in enumerate(sorted(keys)):
                time.sleep(max(0, when - (time.monotonic() - started)))
                qmp.hmp(f"sendkey {key} {hold}")
                time.sleep(max(1, hold / 1000 + 0.05))
                qmp.command("screendump", {"filename": str(args.output / f"key-{index}-{key}.png"),
                                          "format": "png"})
            time.sleep(max(0, args.seconds - (time.monotonic() - started)))
            qmp.command("stop")
            snapshot = {"status": qmp.command("query-status"),
                        "registers": qmp.hmp("info registers"),
                        "lcdc": qmp.hmp("xp /12wx 0x20d00000"),
                        "vbc": qmp.hmp("xp /24wx 0x82003000"),
                        "codec": qmp.hmp("xp /16wx 0x8a002000"),
                        "midi": qmp.hmp("xp /10wx 0x20b00000"),
                        "dma": qmp.hmp("xp /8wx 0x20100000"),
                        "intc": qmp.hmp("xp /12wx 0x80000000"),
                        "timers": qmp.hmp("xp /20wx 0x81000000"),
                        "alarm": qmp.hmp("xp /4wx 0x81003000"),
                        "rtc": qmp.hmp("xp /16wx 0x82001600"),
                        "dsp_control": qmp.hmp("xp /8wx 0x10000fe0")}
            snapshot["lcdc_interrupts"] = qmp.hmp("xp /4wx 0x20d00110")
            snapshot["keypad"] = qmp.hmp("xp /12wx 0x87000000")
            sp = int(re.search(r"R13=([0-9a-fA-F]+)", snapshot["registers"])[1], 16)
            snapshot["stack"] = qmp.hmp(f"xp /32wx {sp:#x}")
            snapshot["rtos"] = qmp.hmp("xp /32wx 0x0422c640")
            ram_path = (args.output / "psram.bin").as_posix()
            qmp.hmp(f'pmemsave 0x34000000 0x400000 "{ram_path}"')
            (args.output / "state.json").write_text(json.dumps(snapshot, indent=2))
            qmp.command("screendump", {"filename": str(args.output / "screen.png"),
                                      "format": "png"})
            qmp.command("quit")
            process.wait(timeout=10)
            print(f"Captured {args.mode} boot ({args.seconds}s) in {args.output}")
            print(snapshot["registers"])
        finally:
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
