#!/usr/bin/env python3
"""Patch the stock B310E NOR to log its DSP calls on a USB port of their own.

  patch.py NOR_IN --output DIR [--cave 0x40c000] [--no-fi] [--prefix arm-none-eabi-]

Writes to DIR:
  stock-usblog.bin              the whole patched NOR (for QEMU)
  sector-<addr>.patched.bin     each changed 4 KiB sector, patched
  sector-<addr>.pristine.bin    the same sectors as they were (to revert)
  flash.txt                     the spd_dump commands for both directions
Nothing here writes to a phone.

What the patch does (README.md has the reasoning and the addresses):
  1. Two changes make the stock start its USB virtual COM service (UCOM) at
     boot and bind the SIO debug port to it, as its own "USB log" does.
  2. Hooks in the layer-1 code call logger.c, placed in free NOR at --cave.
     Each hook replaces 4 bytes with a Thumb BL to a stub in the cave and
     only stores a record in a RAM ring.
  3. The scheduler's idle hook (a veneer literal in the PSRAM load image)
     points at the logger's drain, which sends the records as text lines on
     UCOM port 0 (the "SPRD AT" COM port; AT commands stop working there).

NOR_IN must hold the e52q7a stock kernel; every patched location is compared
with the bytes expected there, and the cave must be erased (0xff).

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SECTOR = 0x1000
CAVE_MAX = 0x2000

# Hooks. kind 'call': the 4 bytes are a BL/BLX; the stub logs, then jumps to
# the original target with lr as the BL left it. kind 'replay': the 4 bytes
# are two 16-bit instructions that use neither pc nor lr; the stub logs,
# runs them and returns. In both, r0-r7 reach the logger untouched.
HOOKS = [
    # name     kind      site     expected    logger       what
    ('cmd',   'call',   0x5f478, '60f0acef', 'log_cmd',   'post_cmd: its first call (IRQ disable)'),
    ('fi',    'replay', 0x5ef7a, '8f26e868', 'log_fi',    'post_fi: movs r6,#143; ldr r0,[r5,#12]'),
    ('tx',    'replay', 0x5f33a, 'c0001500', 'log_tx',    'tx_desc_post: lsls r0,r0,#3; movs r5,r2'),
    ('msg',   'replay', 0x3ab0c, '002489b0', 'log_msg',   'DSP message handler: movs r4,#0; sub sp,#36'),
    ('state', 'replay', 0x2f34c, '00211389', 'log_state', 'SetState: movs r1,#0; ldrh r3,[r2,#8]'),
    ('hello', 'call',   0x436b6, 'ddf7b7f8', 'log_hello', 'usbservice: start of a service (bl 0x20828)'),
    # What the ARM reads back from the DSP (sharedmem_proc.c readers).
    ('rx',    'call',   0x5f6d8, '60f05cef', 'log_rx',    'RX ring read: its memcpy of the 72-byte result'),
    ('resp',  'call',   0x5f774, '01f064fc', 'log_resp',  'RESP ring read: the index write after the copy'),
    ('pm',    'call',   0x5edc8, '61f0e4eb', 'log_pm',    'power-measure read: its memcpy of one result'),
    ('bep',   'call',   0x5ee3a, '61f0aceb', 'log_bep',   'bit-error report read: its memcpy of one report'),
]

# Plain byte changes.
EDITS = [
    # usbservice task (0x43630): after "cmp r0,#4" on the NV-selected USB
    # service, "beq" -> "b": always start service 2 (UCOM) at boot.
    ('usb-start-ucom', 0x43678, '05d0', '05e0'),
    # UCOM bind (0x421d4, ARM): "cmp r0,#1" on the assert-mode byte ->
    # "cmp r0,r0": always move COM_DATA and COM_DEBUG to the two USB ports.
    ('ucom-bind-debug', 0x42208, '010050e3', '000050e1'),
]

# Stock functions logger.c calls: address -> first bytes, as a version check.
CALLED = {
    0x426b0: 'f0412de9',     # UCOM: write up to 64 bytes to a port
    0x209c8: '10b500f0',     # USB service running
    0x111f30: '10b50bf0',    # tick count (the NOR copy of PSRAM 0x04008954)
}
PSRAM_LOAD = (0x1095ec, 0x04000010, 0x154d8)   # NOR source, RAM address, size
DATA_LOAD = (0x11eac4, 0x0422c4e8, 0xf6c0)
BSS = (0x0423bba8, 0xa50b8)

# The idle loop (NOR 0x118178, running at PSRAM 0x0400eb9c) calls an empty
# function through this veneer literal; it becomes the drain's entry.
IDLE_HOOK = (0x11d92c, 0x0001dfe9)
# The layer-1 debug ring the logger takes over: pointer variable, size.
L1_DEBUG_RING = (0x0422c940, 0x23a0)

STUB_HEAD = """\
    .syntax unified
    .arch armv5te
    .section .stubs, "ax", %progbits

    @ The idle loop has a 200-byte stack: run the drain on the logger's own.
    .thumb
    .thumb_func
    .global drain_entry
drain_entry:
    push {{r4, lr}}
    mov  r4, sp
    ldr  r0, ={stack_top:#x}
    mov  sp, r0
    bl   drain
    mov  sp, r4
    pop  {{r4, pc}}
    .ltorg

    .balign 4
    .arm
    .global irq_save
    .type irq_save, %function
irq_save:
    mrs  r0, cpsr
    orr  r1, r0, #0x80
    msr  cpsr_c, r1
    bx   lr
    .global irq_restore
    .type irq_restore, %function
irq_restore:
    msr  cpsr_c, r0
    bx   lr

    .thumb
"""
STUB_CALL = """\
    .thumb_func
    .global stub_{name}
stub_{name}:
    push {{r0-r7, lr}}
    mov  r0, sp
    bl   {logger}
    ldr  r0, [sp, #32]
    mov  lr, r0
    ldr  r0, ={target:#x}
    mov  ip, r0
    pop  {{r0-r7}}
    add  sp, #4
    bx   ip
    .ltorg
"""
STUB_REPLAY = """\
    .thumb_func
    .global stub_{name}
stub_{name}:
    push {{r0-r7, lr}}
    mov  r0, sp
    bl   {logger}
    ldr  r0, [sp, #32]
    mov  ip, r0
    pop  {{r0-r7}}
    add  sp, #4
    .inst.n {a:#06x}
    .inst.n {b:#06x}
    bx   ip
"""
LINK = """\
SECTIONS {{
    . = {cave:#x};
    .text : {{ *(.stubs) *(.text*) *(.rodata*) }}
    .data : {{ *(.data*) }}
    .bss  : {{ *(.bss*) *(COMMON) }}
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "the logger runs from NOR: no writable data")
    /DISCARD/ : {{ *(.ARM.*) *(.comment) *(.note*) }}
}}
"""


def bl_decode(site, raw):
    """Target of the Thumb BL/BLX pair at site; bit 0 set for a Thumb target."""
    h1, h2 = struct.unpack('<HH', raw)
    if h1 & 0xf800 != 0xf000 or h2 & 0xe800 != 0xe800:
        sys.exit(f'{site:#x}: {raw.hex()} is not a BL/BLX pair')
    off = (h1 & 0x7ff) << 12 | (h2 & 0x7ff) << 1
    if off & 0x400000:
        off -= 0x800000
    target = site + 4 + off
    return target | 1 if h2 & 0x1000 else target & ~3


def bl_encode(site, target):
    off = target - (site + 4)
    if not -0x400000 <= off < 0x400000 or off & 1:
        sys.exit(f'{site:#x}: BL cannot reach {target:#x}')
    return struct.pack('<HH', 0xf000 | off >> 12 & 0x7ff, 0xf800 | off >> 1 & 0x7ff)


def replayable(insn):
    """A 16-bit Thumb instruction the stub may run in the cave unchanged."""
    top = insn >> 11
    if top in (0x09, 0x14, 0x1c, 0x1d, 0x1e, 0x1f):   # ldr pc-rel, adr, b, bl
        return False
    if insn >> 12 == 0xd or insn >> 8 in (0x47, 0xbd, 0xb5):   # bcc, bx, pop pc, push lr
        return False
    if insn >> 10 == 0x11 and ((insn & 0x87) == 0x87 or (insn >> 3 & 0xf) == 0xf):   # hi-reg op with pc
        return False
    return True


def run(*command):
    subprocess.run([str(c) for c in command], check=True)


def build(cave, hooks, nor, prefix, work, defines, ring_base):
    stubs = [STUB_HEAD.format(stack_top=ring_base + L1_DEBUG_RING[1])]
    defines = defines + [f'RING_BASE={ring_base:#x}u']
    for name, kind, site, _, logger, _ in hooks:
        raw = nor[site:site + 4]
        if kind == 'call':
            stubs.append(STUB_CALL.format(name=name, logger=logger, target=bl_decode(site, raw)))
        else:
            a, b = struct.unpack('<HH', raw)
            if not (replayable(a) and replayable(b)):
                sys.exit(f'{name}: {raw.hex()} at {site:#x} cannot be replayed')
            stubs.append(STUB_REPLAY.format(name=name, logger=logger, a=a, b=b))
    (work / 'stubs.S').write_text(''.join(stubs), newline='\n')
    (work / 'logger.ld').write_text(LINK.format(cave=cave), newline='\n')
    flags = ['-march=armv5te', '-mtune=arm926ej-s', '-mthumb', '-mthumb-interwork', '-Os',
             '-ffreestanding', '-fno-builtin', '-fno-common', '-Wall', '-Wextra', '-Werror']
    flags += ['-D' + d for d in defines]
    elf = work / 'logger.elf'
    run(prefix + 'gcc', *flags, '-nostdlib', '-Wl,--build-id=none', '-Wl,-T', work / 'logger.ld',
        '-Wl,-Map', work / 'logger.map', work / 'stubs.S', HERE / 'logger.c', '-o', elf)
    run(prefix + 'objcopy', '-O', 'binary', '-j', '.text', elf, work / 'logger.bin')
    symbols = {}
    out = subprocess.run([prefix + 'nm', str(elf)], check=True, capture_output=True, text=True).stdout
    for row in out.splitlines():
        parts = row.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
        elif len(parts) == 2 and parts[0] == 'U':
            sys.exit(f'logger.c needs {parts[1]}: a library call crept in')
    return (work / 'logger.bin').read_bytes(), symbols


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nor_in', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--cave', type=lambda x: int(x, 0), default=0x40c000,
                    help='free NOR for the logger (default: just past the boot loader\'s 64 KiB)')
    ap.add_argument('--no-fi', action='store_true', help='leave post_fi alone (no per-frame lines)')
    ap.add_argument('--no-pm', action='store_true',
                    help='leave the power-measure read alone (thousands of lines in a band sweep)')
    ap.add_argument('--prefix', default='arm-none-eabi-')
    args = ap.parse_args()

    pristine = args.nor_in.read_bytes()
    if len(pristine) != 0x800000:
        sys.exit(f'{args.nor_in}: {len(pristine):#x} bytes, expected the 8 MiB NOR')
    nor = bytearray(pristine)
    hooks = [h for h in HOOKS if not (args.no_fi and h[0] == 'fi') and not (args.no_pm and h[0] == 'pm')]

    for name, _, site, expect, _, _ in hooks:
        if nor[site:site + 4].hex() != expect:
            sys.exit(f'{name}: {nor[site:site + 4].hex()} at {site:#x}, expected {expect}: not this firmware')
    for name, site, old, _ in EDITS:
        if nor[site:site + len(old) // 2].hex() != old:
            sys.exit(f'{name}: {nor[site:site + len(old) // 2].hex()} at {site:#x}, expected {old}')
    for addr, expect in CALLED.items():
        if nor[addr:addr + 4].hex() != expect:
            sys.exit(f'stock function at {addr:#x} starts {nor[addr:addr + 4].hex()}, expected {expect}')
    if args.cave % SECTOR or nor[args.cave:args.cave + CAVE_MAX] != b'\xff' * CAVE_MAX:
        sys.exit(f'cave {args.cave:#x}: not sector-aligned erased flash')
    source, ram, size = PSRAM_LOAD
    table = struct.pack('<III', source, ram, size)
    if table not in pristine[0x109000:0x109800]:
        sys.exit('the PSRAM load table is not where logger.c\'s TICK_ADDR assumes')

    hook_at, hook_old = IDLE_HOOK
    if struct.unpack_from('<I', nor, hook_at)[0] != hook_old:
        sys.exit(f'idle hook literal at {hook_at:#x} is not {hook_old:#x}')
    ring_var, ring_size = L1_DEBUG_RING
    ring_base, = struct.unpack_from('<I', nor, DATA_LOAD[0] + ring_var - DATA_LOAD[1])
    if not BSS[0] <= ring_base <= BSS[0] + BSS[1] - ring_size or ring_base % 8:
        sys.exit(f'layer-1 debug ring at {ring_base:#x}: not in the BSS')

    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        code, symbols = build(args.cave, hooks, nor, args.prefix, Path(tmp), [], ring_base)
        (args.output / 'logger.map').write_bytes((Path(tmp) / 'logger.map').read_bytes())
    if len(code) > CAVE_MAX:
        sys.exit(f'logger is {len(code)} bytes, more than {CAVE_MAX:#x}')
    nor[args.cave:args.cave + len(code)] = code
    for name, _, site, _, _, _ in hooks:
        nor[site:site + 4] = bl_encode(site, symbols['stub_' + name])
    for _, site, _, new in EDITS:
        nor[site:site + len(new) // 2] = bytes.fromhex(new)
    struct.pack_into('<I', nor, hook_at, symbols['drain_entry'] | 1)

    (args.output / 'stock-usblog.bin').write_bytes(nor)
    sectors = [s for s in range(0, len(nor), SECTOR) if nor[s:s + SECTOR] != pristine[s:s + SECTOR]]
    patch, revert = [], []
    for s in sectors:
        for kind, data in (('patched', nor), ('pristine', pristine)):
            (args.output / f'sector-{s:06x}.{kind}.bin').write_bytes(data[s:s + SECTOR])
        patch.append(f'  erase_flash fw+{s:#x} {SECTOR:#x} write_data fw+{s:#x} 0 0 sector-{s:06x}.patched.bin')
        revert.append(f'  erase_flash fw+{s:#x} {SECTOR:#x} write_data fw+{s:#x} 0 0 sector-{s:06x}.pristine.bin')
    head = '.\\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `\n'
    (args.output / 'flash.txt').write_text(
        '# Run from tools/spd_dump with the sector files copied there; the phone in\n'
        '# bootrom mode. Each erase_flash asks for a typed "yes". See README.md.\n\n'
        '# PATCH\n' + head + ' `\n'.join(patch) + '\n\n# REVERT\n' + head + ' `\n'.join(revert) + '\n',
        newline='\n')

    print(f'logger: {len(code)} bytes at {args.cave:#x}')
    for name, kind, site, _, _, what in hooks:
        print(f'  hook {name:6} {site:#08x} -> {symbols["stub_" + name]:#x}  {what}')
    for name, site, old, new in EDITS:
        print(f'  edit {name:16} {site:#08x} {old} -> {new}')
    print(f'  idle hook {hook_at:#08x} {hook_old:#x} -> {symbols["drain_entry"] | 1:#x}; '
          f'ring and drain stack at {ring_base:#x}')
    print(f'{len(sectors)} sectors changed: ' + ' '.join(f'{s:#x}' for s in sectors))
    print(f'wrote {args.output / "stock-usblog.bin"}')


if __name__ == '__main__':
    main()
