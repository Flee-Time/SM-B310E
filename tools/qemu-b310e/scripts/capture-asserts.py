#!/usr/bin/env python3
"""Stop at the dump's SCI assertion entry and record its real arguments."""
import argparse
import json
import os
from pathlib import Path
import runpy
import socket
import subprocess
import time

QMP = runpy.run_path(str(Path(__file__).with_name("capture-stock.py")))["QMP"]


class GDB:
    def __init__(self, port, timeout):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)

    def packet(self, data):
        encoded = data.encode()
        self.sock.sendall(b"$" + encoded + b"#" + f"{sum(encoded) & 255:02x}".encode())
        while True:
            byte = self.sock.recv(1)
            if not byte:
                raise RuntimeError("GDB disconnected")
            if byte == b"$":
                break
        response = bytearray()
        while True:
            byte = self.sock.recv(1)
            if not byte:
                raise RuntimeError("GDB disconnected")
            if byte == b"#":
                break
            response += byte
        checksum = bytearray()
        while len(checksum) < 2:
            byte = self.sock.recv(2 - len(checksum))
            if not byte:
                raise RuntimeError("GDB disconnected")
            checksum += byte
        assert int(checksum, 16) == sum(response) & 255
        self.sock.sendall(b"+")
        return response.decode()

    def register(self, number):
        value = self.packet(f"p{number:x}")
        return int.from_bytes(bytes.fromhex(value), "little")

    def string(self, address):
        value = self.packet(f"m{address:x},100")
        if value.startswith("E"):
            return value
        return bytes.fromhex(value).split(b"\0")[0].decode("ascii", errors="replace")


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=["stock", "warm"], default="stock")
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--no-overlays", action="store_true")
    parser.add_argument("--hold-end", action="store_true")
    parser.add_argument("--gpio49-high", action="store_true",
                        help="Experimental external input for the stock LCD sleep/wake wait")
    parser.add_argument("--instructions", action="store_true")
    parser.add_argument("--raw", action="store_true",
                        help="Record registers without interpreting assertion strings")
    parser.add_argument("--break-address", type=lambda value: int(value, 0), action="append",
                        help="Repeat for multiple entries; default covers both SCI assert wrappers")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    debug_port, qmp_port = port(), port()
    machine = f"b310e,boot-mode={args.mode}" + (",boot-overlays=off" if args.no_overlays else "")
    if args.hold_end:
        machine += ",hold-end=on"
    if args.gpio49_high:
        machine += ",gpio49-high=on"
    command = [str(args.qemu.resolve()), "-M", machine,
               "-display", "none", "-serial", "none", "-S", "-gdb",
               f"tcp:127.0.0.1:{debug_port}", "-qmp",
               f"tcp:127.0.0.1:{qmp_port},server=on,wait=off", "-D",
               str(args.output / "trace.log"), "-audiodev", "none,id=audio0",
               "-global", "sc6530_adi.audiodev=audio0", "-drive",
               f"file={args.firmware.resolve().as_posix()},format=raw,if=none,id=nor,readonly=on"]
    command += ["--trace", "sc6530_dma_transfer"]
    env = os.environ.copy()
    if args.instructions:
        command += ["-d", "in_asm,int"]
    if os.name == "nt":
        env["PATH"] = r"C:\msys64\mingw64\bin;" + env.get("PATH", "")
    results = []
    qmp = gdb = None
    with (args.output / "stderr.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited; see stderr.log")
                try:
                    qmp = QMP(qmp_port)
                    break
                except OSError:
                    time.sleep(0.05)
            if qmp is None:
                raise RuntimeError("QMP startup timed out")
            gdb = GDB(debug_port, args.timeout)
            breakpoints = [f"{address:x},2" for address in
                           (args.break_address or [0x11172, 0x11414])]
            for breakpoint in breakpoints:
                assert gdb.packet(f"Z1,{breakpoint}") == "OK"
            for _ in range(args.count):
                try:
                    signal = gdb.packet("c")
                except socket.timeout:
                    qmp.command("stop")
                    snapshot = {"reason": "breakpoint timeout", "hits": len(results),
                                "registers": qmp.hmp("info registers")}
                    (args.output / "timeout.json").write_text(json.dumps(snapshot, indent=2))
                    (args.output / "assertions.json").write_text(json.dumps(results, indent=2))
                    print(json.dumps(snapshot), flush=True)
                    break
                registers = [gdb.register(n) for n in [0, 1, 2, 3, 13, 14, 15]]
                r0, r1, r2, r3, sp, lr, pc = registers
                entry = {"signal": signal,
                         "expression": None if args.raw else gdb.string(r0),
                         "file": None if args.raw else gdb.string(r1), "line": r2,
                         "r0": hex(r0), "r1": hex(r1), "r3": hex(r3),
                         "sp": hex(sp), "lr": hex(lr), "pc": hex(pc),
                         "stack": qmp.hmp(f"xp /32wx {sp:#x}"),
                         "registers": qmp.hmp("info registers")}
                entry["dma"] = qmp.hmp("xp /128wx 0x20101000")
                entry["r6_memory"] = qmp.hmp(f"xp /16bx {gdb.register(6):#x}")
                entry["sfc"] = qmp.hmp("xp /32wx 0x20a00000")
                results.append(entry)
                ram_path = (args.output / f"psram-{len(results)}.bin").as_posix()
                qmp.hmp(f'pmemsave 0x34000000 0x400000 "{ram_path}"')
                (args.output / "assertions.json").write_text(json.dumps(results, indent=2))
                print(json.dumps(entry), flush=True)
                for breakpoint in breakpoints:
                    assert gdb.packet(f"z1,{breakpoint}") == "OK"
                gdb.packet("s")
                for breakpoint in breakpoints:
                    assert gdb.packet(f"Z1,{breakpoint}") == "OK"
            qmp.command("quit")
            process.wait(timeout=10)
        finally:
            if gdb:
                gdb.sock.close()
            if qmp:
                qmp.close()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
