#!/usr/bin/env python3
"""Patch the stock B310E NOR to log the ARM->DSP command ring into a RAM buffer.

A trampoline on post_cmd (FUN_0005f474) copies each 5-word DSP command into a
capture buffer in RAM; read the buffer back afterward (QEMU pmemsave, or spd_dump
on hardware) and decode it. Commands only (low volume); add post_fi later for FIs.

The hook asm (hook.s) is assembled and linked at CAVE with the buffer base baked in.
Nothing here flashes anything; it only writes a patched image file.

Usage:
  patch.py <nor-in> <nor-out> [--buf 0x10010000] [--cave 0x121390]
           [--target 0x5f474] [--as arm-none-eabi-as] [--ld ...] [--objcopy ...]

HARDWARE NOTE: --buf must be FREE RAM on the target. In QEMU the upper DSP-share
window (0x10010000) is free (128 KiB). On the real phone the share window is only
4 KiB, so pick a free PSRAM address (0x34000000..0x343fffff) that the stock does
not use -- determine it before deploying. The cave (--cave) must be unused,
executable NOR in the XIP kernel region (0x10000..0x12e524); 0x121390 is a verified
zero-filled gap. post_cmd's first 4 bytes must be f8 b5 04 00 (checked).
"""
import argparse, struct, subprocess, tempfile, os, sys

PROLOGUE = bytes.fromhex("f8b50400")   # push {r3-r7,lr}; movs r4,r0


def asm_hook(buf, cave, target, tools):
    a_as, a_ld, a_oc = tools
    src = f"""
    .syntax unified
    .thumb
    .thumb_func
    .global hook
hook:
    push {{r4-r6}}
    ldr  r4, ={buf:#x}
    ldr  r5, [r4]
    cmp  r5, #4
    bhs  1f
    movs r5, #4
1:  cmp  r5, #0x10000
    bhi  2f
    adds r6, r4, r5
    ldrh r1, [r0, #0]
    strh r1, [r6, #0]
    ldrh r1, [r0, #2]
    strh r1, [r6, #2]
    ldrh r1, [r0, #4]
    strh r1, [r6, #4]
    ldrh r1, [r0, #6]
    strh r1, [r6, #6]
    ldrh r1, [r0, #8]
    strh r1, [r6, #8]
    adds r5, r5, #10
    str  r5, [r4]
2:  pop  {{r4-r6}}
    push {{r3-r7, lr}}
    movs r4, r0
    b.w  {target + 4:#x}
    .ltorg
"""
    d = tempfile.mkdtemp()
    s, o, e, b = (os.path.join(d, x) for x in ("h.s", "h.o", "h.elf", "h.bin"))
    open(s, "w").write(src)
    subprocess.run([a_as, "-mcpu=cortex-r4", "-mthumb", s, "-o", o], check=True)
    subprocess.run([a_ld, f"-Ttext={cave:#x}", f"--entry={cave:#x}", o, "-o", e],
                   check=True, stderr=subprocess.DEVNULL)
    subprocess.run([a_oc, "-O", "binary", e, b], check=True)
    hook = open(b, "rb").read()
    # entry branch b.w cave at target
    es, eo, ee, eb = (os.path.join(d, x) for x in ("e.s", "e.o", "e.elf", "e.bin"))
    open(es, "w").write(f".syntax unified\n.thumb\n.thumb_func\n.global e\ne:\n b.w {cave:#x}\n")
    subprocess.run([a_as, "-mcpu=cortex-r4", "-mthumb", es, "-o", eo], check=True)
    subprocess.run([a_ld, f"-Ttext={target:#x}", f"--entry={target:#x}", eo, "-o", ee],
                   check=True, stderr=subprocess.DEVNULL)
    subprocess.run([a_oc, "-O", "binary", ee, eb], check=True)
    return hook, open(eb, "rb").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nor_in"); ap.add_argument("nor_out")
    ap.add_argument("--buf", type=lambda x: int(x, 0), default=0x10010000)
    ap.add_argument("--cave", type=lambda x: int(x, 0), default=0x121390)
    ap.add_argument("--target", type=lambda x: int(x, 0), default=0x5f474)
    ap.add_argument("--as", dest="a_as", default="arm-none-eabi-as")
    ap.add_argument("--ld", default="arm-none-eabi-ld")
    ap.add_argument("--objcopy", default="arm-none-eabi-objcopy")
    args = ap.parse_args()

    nor = bytearray(open(args.nor_in, "rb").read())
    if nor[args.target:args.target + 4] != PROLOGUE:
        sys.exit(f"target {args.target:#x} prologue {nor[args.target:args.target+4].hex()} "
                 f"!= {PROLOGUE.hex()} -- wrong image or address")
    hook, entry = asm_hook(args.buf, args.cave, args.target, (args.a_as, args.ld, args.objcopy))
    if nor[args.cave:args.cave + len(hook)] != b"\x00" * len(hook):
        sys.exit(f"cave {args.cave:#x} is not {len(hook)} zero bytes -- pick another")
    nor[args.cave:args.cave + len(hook)] = hook
    nor[args.target:args.target + 4] = entry
    open(args.nor_out, "wb").write(nor)
    print(f"patched: hook {len(hook)}B @ {args.cave:#x}, buffer {args.buf:#x}, "
          f"target post_cmd {args.target:#x} -> {args.nor_out}")


if __name__ == "__main__":
    main()
