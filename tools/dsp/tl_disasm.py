#!/usr/bin/env python3
"""
TeakLite-I (TL) disassembler built from the GBATEK "DSi TeakLite II Instruction Set
Encoding" opcode table (http://problemkaputt.de/gbatek-dsi-teaklite-ii-instruction-set-encoding.htm).

Both TL and TL2 use 16-bit opcodes; TL2 adds new opcodes in formerly-unused slots.
This decoder implements the TL-I subset (the "TL" rows) plus the core TL2 additions,
enough to evaluate whether a byte stream is TeakLite-family code.

Usage:
  python tl_disasm.py <file> <start_hex> <count> [--stats]
"""
import sys, struct, collections, re

# (base, mask, mnemonic, fmt, needs_param) — fmt is a python-format-style for the operands.
# Encoded from the GBATEK table. @N = variable field at bit N.
# For branch/relative instructions we compute targets in the disasm line.
TL = [
    # base, mask, mnemonic, paramsize(words), operand-description
    (0x0000, 0xFFFF, "nop", 0, ""),
    (0x0020, 0xFFFF, "trap", 0, ""),
    (0x1000, 0xF800, "callr", 0, "rel7_cond4"),
    (0x5000, 0xF800, "brr", 0, "rel7_cond4"),
    (0x4180, 0xF000, "br", 1, "abs18_cond4"),
    (0x41C0, 0xF000, "call", 1, "abs18_cond4"),
    (0xD480, 0xFFFF, "calla", 0, "axl8"),
    (0xD3C0, 0xFFFF, "break", 0, ""),
    (0x4580, 0xFC00, "ret", 0, "cond"),
    (0xD780, 0xFFFF, "retd", 0, ""),
    (0x45C0, 0xF800, "reti", 0, "cond_ctx"),
    (0x45D0, 0xF800, "reti", 0, "cond_ctx"),
    (0xD7C0, 0xFFFF, "retid", 0, ""),
    (0x0900, 0xFF00, "rets", 0, "imm8"),
    (0x0C00, 0xFF00, "rep", 0, "imm8"),
    (0x0D00, 0xFF00, "rep", 0, "reg0"),
    (0x43C0, 0xFFFF, "dint", 0, ""),
    (0x4380, 0xFFFF, "eint", 0, ""),
    (0x0500, 0xFF00, "mov", 0, "imm8,sv"),
    (0x7D00, 0xFF00, "mov", 0, "sv,memimm8"),
    (0x6D00, 0xFF00, "mov", 0, "memimm8,sv"),
    (0x0400, 0xFF00, "load", 0, "imm8,page"),
    (0x0200, 0xFF00, "load", 0, "imm9,modi"),
    (0x0A00, 0xFF00, "load", 0, "imm9,modj"),
    (0x0080, 0xFFC0, "modr", 0, "memrn"),
    (0x00A0, 0xFFC0, "modr", 0, "memrn,dmod"),
    (0x0100, 0xFF00, "movs", 0, "sv,regp0,ab5"),
    (0x0180, 0xFF00, "movs", 0, "sv,memrn,ab5"),
    (0x6300, 0xFF00, "movs", 0, "sv,memimm8,ab11"),
    (0x2100, 0xFF00, "mov", 0, "imm8u,axl12"),
    (0x2300, 0xFF00, "mov", 0, "imm8s,r0123457"),
    (0x2500, 0xFF00, "mov", 0, "imm8s,axh12"),
    (0x2600, 0xFF00, "mov", 0, "imm8s,axh12"),
    (0x2700, 0xFF00, "mov", 0, "imm8s,axh12"),
    (0x2800, 0xFF00, "mov", 0, "imm8s,axh12"),
    (0x2900, 0xFF00, "mov", 0, "imm8s,ext0"),
    (0x2D00, 0xFF00, "mov", 0, "imm8s,ext1"),
    (0x3900, 0xFF00, "mov", 0, "imm8s,ext2"),
    (0x3D00, 0xFF00, "mov", 0, "imm8s,ext3"),
    (0x1C00, 0xFF00, "mov", 0, "memrn,reg5"),
    (0x1800, 0xFF00, "mov", 0, "reg5,memrn"),
    (0x2000, 0xFF00, "mov", 0, "r0123457,memimm8"),
    (0x3000, 0xFF00, "mov", 0, "ablh9,memimm8"),
    (0x6000, 0xFF00, "mov", 0, "memimm8,r0123457y0"),
    (0x6100, 0xFF00, "mov", 0, "memimm8,ab11"),
    (0x6200, 0xFF00, "mov", 0, "memimm8,ablh10"),
    (0x6500, 0xFF00, "mov", 0, "memimm8,axh12,eu"),
    (0x5E00, 0xFF00, "mov", 1, "imm16,reg0"),
    (0x5E20, 0xFF00, "mov", 1, "imm16,bx8"),
    (0x5E40, 0xFF00, "push", 0, "reg0"),
    (0x5F40, 0xFF00, "push", 1, "imm16"),
    (0x5E60, 0xFF00, "pop", 0, "reg0"),
    (0x5800, 0xFF00, "mov", 0, "regp0,reg5"),
    (0x5EC0, 0xFF00, "mov", 0, "regp0,bx5"),
    (0x47C0, 0xFFFF, "mov", 0, "mixp,reg0"),
    (0x47E0, 0xFFFF, "mov", 0, "memsp,reg0"),
    (0x4F80, 0xFF00, "mov", 0, "imm5,icr"),
    (0x4FC0, 0xFF00, "mov", 0, "reg0,icr"),
    (0x4D80, 0xFF00, "load", 0, "imm2,ps"),
    (0xDB80, 0xFF00, "load", 0, "imm7,stepi"),
    (0xDF80, 0xFF00, "load", 0, "imm7,stepj"),
    (0x49C0, 0xFFF0, "lim", 0, "a0"),
    (0x49D0, 0xFFF0, "lim", 0, "a0a1"),
    (0x49F0, 0xFFF0, "lim", 0, "a1"),
    (0x49E0, 0xFFF0, "lim", 0, "a1a0"),
    (0x4B80, 0xFF00, "banke", 0, "bankflags6"),
    (0x4980, 0xFF00, "swap", 0, "swaptypes4"),
    (0x0080, 0xFFC0, "modr", 0, "memrn"),       # 0x0080
    (0x8060, 0xFFE0, "maxd", 0, "ax8,memr0,ge"),
    (0x8260, 0xFFE0, "maxd", 0, "ax8,memr0,gt"),
    (0x8460, 0xFFE0, "max", 0, "ax8,axnot8,ge"),
    (0x8660, 0xFFE0, "max", 0, "ax8,axnot8,gt"),
    (0x8860, 0xFFE0, "min", 0, "ax8,axnot8,le"),
    (0x8A60, 0xFFE0, "min", 0, "ax8,axnot8,lt"),
    (0x9080, 0xFFC0, "msu", 0, "y0,memrn,ax8"),
    (0x90A0, 0xFFC0, "msu", 0, "y0,reg0,ax8"),
    (0x90C0, 0xFFC0, "msu", 0, "memrn,imm16,ax8"),
    (0xB000, 0xFF00, "msu", 0, "y0,memimm8,ax8"),
    (0x0800, 0xFF00, "mpyi", 0, "p0,y0,imm8s"),
    (0x0E00, 0xFF00, "divs", 0, "memimm8,ax8"),
    (0x0F00, 0xFF00, "divs", 0, "reg,ax8"),
    (0x8000, 0xFFC0, "mpy", 1, "memrn,imm16"),
    (0x8020, 0xFFC0, "mpy", 0, "y0,memrn"),
    (0x8040, 0xFFC0, "mpy", 0, "y0,reg0"),
    (0xE000, 0xFF00, "mpy", 0, "y0,memimm8"),
    (0xE200, 0xFF00, "mac", 0, "y0,memimm8"),
    (0xE400, 0xFF00, "maa", 0, "y0,memimm8"),
    (0xE600, 0xFF00, "macsu", 0, "y0,memimm8"),
    (0x8200, 0xFFC0, "mac", 1, "memrn,imm16"),
    (0x8220, 0xFFC0, "mac", 0, "y0,memrn"),
    (0x8240, 0xFFC0, "mac", 0, "y0,reg0"),
    (0x8400, 0xFFC0, "maa", 1, "memrn,imm16"),
    (0x8420, 0xFFC0, "maa", 0, "y0,memrn"),
    (0x8440, 0xFFC0, "maa", 0, "y0,reg0"),
    (0x8600, 0xFFC0, "macsu", 1, "memrn,imm16"),
    (0x8620, 0xFFC0, "macsu", 0, "y0,memrn"),
    (0x8640, 0xFFC0, "macsu", 0, "y0,reg0"),
    (0xD000, 0xFFC0, "mpy", 0, "memr45,memr0123"),
    (0xD100, 0xFFC0, "mpysu", 0, "memr45,memr0123"),
    (0xD200, 0xFFC0, "mac", 0, "memr45,memr0123"),
    (0xD300, 0xFFC0, "macus", 0, "memr45,memr0123"),
    (0xD400, 0xFFC0, "maa", 0, "memr45,memr0123"),
    (0xD500, 0xFFC0, "macuu", 0, "memr45,memr0123"),
    (0xD600, 0xFFC0, "macsu", 0, "memr45,memr0123"),
    (0xD700, 0xFFC0, "maasu", 0, "memr45,memr0123"),
    (0x8700, 0xFFC0, "maasu", 1, "memrn,imm16"),
    (0x8720, 0xFFC0, "maasu", 0, "y0,memrn"),
    (0x8740, 0xFFC0, "maasu", 0, "y0,reg0"),
    (0xE100, 0xFF00, "set", 1, "imm16,memimm8"),
    (0xE300, 0xFF00, "rst", 1, "imm16,memimm8"),
    (0xE500, 0xFF00, "chng", 1, "imm16,memimm8"),
    (0xE700, 0xFF00, "addv", 1, "imm16,memimm8"),
    (0xED00, 0xFF00, "cmpv", 1, "imm16,memimm8"),
    (0xEF00, 0xFF00, "subv", 1, "imm16,memimm8"),
    (0xE900, 0xFF00, "tst0", 1, "imm16,memimm8"),
    (0xEB00, 0xFF00, "tst1", 1, "imm16,memimm8"),
    (0x80E0, 0xFFC0, "set", 1, "imm16,memrn"),
    (0x81E0, 0xFFC0, "set", 1, "imm16,reg0"),
    (0x82E0, 0xFFC0, "rst", 1, "imm16,memrn"),
    (0x83E0, 0xFFC0, "rst", 1, "imm16,reg0"),
    (0x84E0, 0xFFC0, "chng", 1, "imm16,memrn"),
    (0x85E0, 0xFFC0, "chng", 1, "imm16,reg0"),
    (0x86E0, 0xFFC0, "addv", 1, "imm16,memrn"),
    (0x87E0, 0xFFC0, "addv", 1, "imm16,reg0"),
    (0x8CE0, 0xFFC0, "cmpv", 1, "imm16,memrn"),
    (0x8DE0, 0xFFC0, "cmpv", 1, "imm16,reg0"),
    (0x8EE0, 0xFFC0, "subv", 1, "imm16,memrn"),
    (0x8FE0, 0xFFC0, "subv", 1, "imm16,reg0"),
    (0x88E0, 0xFFC0, "tst0", 1, "imm16,memrn"),
    (0x89E0, 0xFFC0, "tst0", 1, "imm16,reg0"),
    (0x8AE0, 0xFFC0, "tst1", 1, "imm16,memrn"),
    (0x8BE0, 0xFFC0, "tst1", 1, "imm16,reg0"),
    (0xA000, 0xFF00, "or", 0, "memimm8,ax8"),
    (0xA200, 0xFF00, "and", 0, "memimm8,ax8"),
    (0xA400, 0xFF00, "xor", 0, "memimm8,ax8"),
    (0xA600, 0xFF00, "add", 0, "memimm8,ax8"),
    (0xA800, 0xFF00, "tst0", 0, "axl8,memimm8"),
    (0xAA00, 0xFF00, "tst1", 0, "axl8,memimm8"),
    (0xAC00, 0xFF00, "cmp", 0, "memimm8,ax8"),
    (0xAE00, 0xFF00, "sub", 0, "memimm8,ax8"),
    (0xB200, 0xFF00, "addh", 0, "memimm8,ax8"),
    (0xB400, 0xFF00, "addl", 0, "memimm8,ax8"),
    (0xB600, 0xFF00, "subh", 0, "memimm8,ax8"),
    (0xB800, 0xFF00, "subl", 0, "memimm8,ax8"),
    (0xBA00, 0xFF00, "sqr", 0, "memimm8"),
    (0xBC00, 0xFF00, "sqra", 0, "memimm8,ax8"),
    (0xBE00, 0xFF00, "cmpu", 0, "memimm8,ax8"),
    (0xC000, 0xFF00, "or", 0, "imm8u,ax8"),
    (0xC200, 0xFF00, "and", 0, "imm8u,ax8"),
    (0xC400, 0xFF00, "xor", 0, "imm8u,ax8"),
    (0xC600, 0xFF00, "add", 0, "imm8u,ax8"),
    (0xCC00, 0xFF00, "cmp", 0, "imm8u,ax8"),
    (0xCE00, 0xFF00, "sub", 0, "imm8u,ax8"),
    (0x80C0, 0xFFC0, "or", 1, "imm16,ax8"),
    (0x82C0, 0xFFC0, "and", 1, "imm16,ax8"),
    (0x84C0, 0xFFC0, "xor", 1, "imm16,ax8"),
    (0x86C0, 0xFFC0, "add", 1, "imm16,ax8"),
    (0x8CC0, 0xFFC0, "cmp", 1, "imm16,ax8"),
    (0x8EC0, 0xFFC0, "sub", 1, "imm16,ax8"),
    (0xD4F8, 0xFFFF, "or", 1, "memimm16,ax8"),
    (0xD4F9, 0xFFFF, "and", 1, "memimm16,ax8"),
    (0xD4FA, 0xFFFF, "xor", 1, "memimm16,ax8"),
    (0xD4FB, 0xFFFF, "add", 1, "memimm16,ax8"),
    (0xD4FE, 0xFFFF, "cmp", 1, "memimm16,ax8"),
    (0xD4FF, 0xFFFF, "sub", 1, "memimm16,ax8"),
    (0x4000, 0xFF00, "or", 0, "memr7imm7s,ax8"),
    (0x4200, 0xFF00, "and", 0, "memr7imm7s,ax8"),
    (0x4400, 0xFF00, "xor", 0, "memr7imm7s,ax8"),
    (0x4600, 0xFF00, "add", 0, "memr7imm7s,ax8"),
    (0x4C00, 0xFF00, "cmp", 0, "memr7imm7s,ax8"),
    (0x4E00, 0xFF00, "sub", 0, "memr7imm7s,ax8"),
    (0x8080, 0xFFC0, "or", 0, "memrn,ax8"),
    (0x8280, 0xFFC0, "and", 0, "memrn,ax8"),
    (0x8480, 0xFFC0, "xor", 0, "memrn,ax8"),
    (0x8680, 0xFFC0, "add", 0, "memrn,ax8"),
    (0x8880, 0xFFC0, "tst0", 0, "axl8,memrn"),
    (0x8A80, 0xFFC0, "tst1", 0, "axl8,memrn"),
    (0x8C80, 0xFFC0, "cmp", 0, "memrn,ax8"),
    (0x8E80, 0xFFC0, "sub", 0, "memrn,ax8"),
    (0x9280, 0xFFC0, "addh", 0, "memrn,ax8"),
    (0x9480, 0xFFC0, "addl", 0, "memrn,ax8"),
    (0x9680, 0xFFC0, "subh", 0, "memrn,ax8"),
    (0x9880, 0xFFC0, "subl", 0, "memrn,ax8"),
    (0x9A80, 0xFFC0, "sqr", 0, "memrn"),
    (0x9C80, 0xFFC0, "sqra", 0, "memrn,ax8"),
    (0x9E80, 0xFFC0, "cmpu", 0, "memrn,ax8"),
    (0x80A0, 0xFFC0, "or", 0, "regp0,ax8"),
    (0x82A0, 0xFFC0, "and", 0, "regp0,ax8"),
    (0x84A0, 0xFFC0, "xor", 0, "regp0,ax8"),
    (0x86A0, 0xFFC0, "add", 0, "regp0,ax8"),
    (0x88A0, 0xFFC0, "tst0", 0, "axl8,reg0"),
    (0x8AA0, 0xFFC0, "tst1", 0, "axl8,reg0"),
    (0x8CA0, 0xFFC0, "cmp", 0, "regp0,ax8"),
    (0x8EA0, 0xFFC0, "sub", 0, "regp0,ax8"),
    (0x92A0, 0xFFC0, "addh", 0, "reg,ax8"),
    (0x94A0, 0xFFC0, "addl", 0, "reg,ax8"),
    (0x96A0, 0xFFC0, "subh", 0, "reg,ax8"),
    (0x98A0, 0xFFC0, "subl", 0, "reg,ax8"),
    (0x9AA0, 0xFFC0, "sqr", 0, "reg0"),
    (0x9CA0, 0xFFC0, "sqra", 0, "reg0,ax8"),
    (0x9EA0, 0xFFC0, "cmpu", 0, "reg,ax8"),
    (0xD4D8, 0xFFFF, "or", 1, "memr7imm16,ax8"),
    (0xD4D9, 0xFFFF, "and", 1, "memr7imm16,ax8"),
    (0xD4DA, 0xFFFF, "xor", 1, "memr7imm16,ax8"),
    (0xD4DB, 0xFFFF, "add", 1, "memr7imm16,ax8"),
    (0xD4DE, 0xFFFF, "cmp", 1, "memr7imm16,ax8"),
    (0xD4DF, 0xFFFF, "sub", 1, "memr7imm16,ax8"),
    (0xDC80, 0xFF00, "mov", 0, "axl8,memr7imm7s"),
    (0xD880, 0xFF00, "mov", 0, "memr7imm7s,ax8"),
    (0xD49C, 0xFFFF, "mov", 1, "axl8,memr7imm16"),
    (0xD498, 0xFFFF, "mov", 1, "memr7imm16,ax8"),
    (0xD4BC, 0xFFFF, "mov", 1, "axl8,memimm16"),
    (0xD4B8, 0xFFFF, "mov", 1, "memimm16,ax8"),
    (0x98C0, 0xFFC0, "mov", 0, "memrn,bx8"),
    (0x9CE0, 0xFFC0, "movr", 0, "memrn,ax8"),
    (0x9CC0, 0xFFC0, "movr", 0, "regp0,ax8"),
    (0x0040, 0xFF00, "movp", 0, "progmemaxl5,reg0"),
    (0x0600, 0xFF00, "movp", 0, "progmemrn,memr0123"),
    (0x5F80, 0xFFC0, "movd", 0, "memr0123,progmemr45"),
    (0x8864, 0xFFF4, "movr", 0, "memr0425,abh8"),
    (0x9060, 0xFFF8, "exp", 0, "bx0,sv,ax8"),
    (0x9460, 0xFFF8, "exp", 0, "bx0,sv"),
    (0x9040, 0xFFF8, "exp", 0, "regp0,sv,ax8"),
    (0x9440, 0xFFF8, "exp", 0, "regp0,sv"),
    (0x9C40, 0xFFC0, "exp", 0, "memrn,sv"),
    (0x9840, 0xFFC0, "exp", 0, "memrn,sv,ax8"),
    (0x4080, 0xFFC0, "movsi", 0, "imm5s,r0123457,ab5"),
    (0x8C00, 0xFF00, "bkrep", 1, "norev,imm8u,addr16"),
    (0x5C00, 0xFF00, "bkrep", 1, "norev,imm8u,addr16"),
    (0x5D00, 0xFF00, "bkrep", 1, "norev,reg0,addr18"),
    (0x67A0, 0xFFF0, "rnd", 0, "ax12"),
    (0x6780, 0xFFF0, "not", 0, "ax12"),
    (0x6790, 0xFFF0, "neg", 0, "ax12"),
    (0x6760, 0xFFF0, "clr", 0, "ax12"),
    (0x6F60, 0xFFF0, "clr", 0, "bx12"),
    (0x67C0, 0xFFF0, "clrr", 0, "ax12"),
    (0x6F70, 0xFFF0, "clrr", 0, "bx12"),
    (0x67E0, 0xFFF0, "dec", 0, "ax12"),
    (0x67D0, 0xFFF0, "inc", 0, "ax12"),
    (0x67F0, 0xFFF0, "copy", 0, "axnot12,ax12"),
    (0x6700, 0xFFF0, "shr", 0, "ax12"),
    (0x6F00, 0xFFF0, "shr", 0, "bx12"),
    (0x6710, 0xFFF0, "shr4", 0, "ax12"),
    (0x6F10, 0xFFF0, "shr4", 0, "bx12"),
    (0x6720, 0xFFF0, "shl", 0, "ax12"),
    (0x6F20, 0xFFF0, "shl", 0, "bx12"),
    (0x6730, 0xFFF0, "shl4", 0, "ax12"),
    (0x6F30, 0xFFF0, "shl4", 0, "bx12"),
    (0x6740, 0xFFF0, "ror", 0, "ax12"),
    (0x6F40, 0xFFF0, "ror", 0, "bx12"),
    (0x6750, 0xFFF0, "rol", 0, "ax12"),
    (0x6F50, 0xFFF0, "rol", 0, "bx12"),
    (0x67B0, 0xFFF0, "pacr", 0, "ax12"),
    (0xD280, 0xFF00, "shfc", 0, "sv,ab10,ab5"),
    (0x9240, 0xFF00, "shfi", 0, "imm6s,ab10,ab7"),
    (0x9000, 0xFFE0, "tstb", 0, "reg0,imm4bit"),
    (0x9020, 0xFFE0, "tstb", 0, "memrn,imm4bit"),
    (0xF000, 0xFF00, "tstb", 0, "memimm8,imm4bit"),
    (0xD390, 0xFFFF, "cntx", 0, "r"),
    (0xD380, 0xFFFF, "cntx", 0, "s"),
    (0xD290, 0xFF00, "mov", 0, "ab10,ab5"),
    (0xD298, 0xFF00, "mov", 0, "abl10,dvm"),
    (0xD2D8, 0xFF00, "mov", 0, "abl10,x0"),
    (0xD491, 0xFF00, "mov", 0, "dvm,ab5"),
    (0xD492, 0xFF00, "mov", 0, "icr,ab5"),
    (0xD493, 0xFF00, "mov", 0, "x0,ab5"),
    (0xD490, 0xFF00, "mov", 0, "repc,ab5"),
    (0x94C0, 0xFFC0, "norm", 0, "ax8,memrn"),
]

COND = ["always", "z", "nz", "eq", "ne", "ge", "lt", "gt", "le", "c", "nc", "o", "no", "p", "n", "?f"]

def decode_word(w):
    for base, mask, mnem, param, fmt in TL:
        if (w & mask) == base:
            return mnem, fmt, param, w & ~mask
    return None

def fmt_operand(fmt, w, extra=None):
    # crude operand formatting; returns string
    if not fmt:
        return ""
    low = w & 0xFF
    if fmt == "imm8,sv" or fmt == "imm8,page" or fmt == "imm8s,sv":
        return "#0x%02x, sv" % low
    if fmt == "imm8u,axl12":
        return "#0x%02x, a%dl" % (low, (w >> 12) & 3)
    if fmt == "imm8s,axh12":
        return "#%d, a%dh" % (low if low < 0x80 else low - 0x100, (w >> 12) & 3)
    if fmt == "imm8s,ext0":
        return "#%d, ext0" % (low if low < 0x80 else low - 0x100)
    if fmt == "imm8s,ext1":
        return "#%d, ext1" % (low if low < 0x80 else low - 0x100)
    if fmt == "imm8s,ext2":
        return "#%d, ext2" % (low if low < 0x80 else low - 0x100)
    if fmt == "imm8s,ext3":
        return "#%d, ext3" % (low if low < 0x80 else low - 0x100)
    if fmt == "imm8s,r0123457":
        return "#%d, r%d" % (low if low < 0x80 else low - 0x100, (w >> 10) & 7)
    if fmt == "memimm8,ax8":
        return "[0x%02x], a%d" % (low, (w >> 8) & 3)
    if fmt == "memimm8,ab11":
        return "[0x%02x], %s" % (low, abname((w >> 11) & 1))
    if fmt == "memimm8,r0123457y0":
        return "[0x%02x], r%d" % (low, (w >> 10) & 7)
    if fmt == "axl8,memimm8":
        return "a%dl, [0x%02x]" % ((w >> 8) & 3, low)
    if fmt == "imm8u,ax8":
        return "#0x%02x, a%d" % (low, (w >> 8) & 3)
    if fmt == "memrn,ax8":
        return "[r%d], a%d" % (low & 7, (w >> 8) & 3)
    if fmt == "regp0,ax8":
        return "r%d, a%d" % ((w >> 12) & 3, (w >> 8) & 3)
    if fmt == "memrn,reg5":
        return "[r%d], r%d" % (low & 7, (w >> 8) & 7)
    if fmt == "reg5,memrn":
        return "r%d, [r%d]" % ((w >> 8) & 7, low & 7)
    if fmt == "memr7imm7s,ax8":
        return "[r7+#%d], a%d" % (low if low < 0x80 else low - 0x100, (w >> 8) & 3)
    if fmt == "rel7_cond4":
        return "cond=%s" % COND[w & 0xF]
    if fmt == "cond":
        return COND[w & 0xF]
    if fmt == "cond_ctx":
        return COND[(w >> 4) & 0xF]
    if fmt == "imm8":
        return "#0x%02x" % low
    if fmt == "reg0":
        return "r%d" % ((w >> 4) & 0xF)
    if fmt == "imm16,reg0" or fmt == "imm16,bx8":
        return "#0x%04x, r%d" % (extra, (w >> 8) & 7) if extra is not None else "#imm16"
    if fmt == "imm16,memimm8":
        return "#0x%04x, [0x%02x]" % (extra, low)
    if fmt == "memimm16,ax8":
        return "[0x%04x], a%d" % (extra, (w >> 8) & 3)
    if fmt == "memrn,imm16" or fmt == "memrn,imm16,ax8":
        return "[r%d], #0x%04x, a%d" % (low & 7, extra, (w >> 8) & 3) if extra is not None else "[r%d], #imm16" % (low & 7)
    if fmt == "y0,memrn":
        return "y0, [r%d]" % (low & 7)
    if fmt == "y0,memimm8":
        return "y0, [0x%02x]" % low
    if fmt == "y0,reg0":
        return "y0, r%d" % ((w >> 12) & 3)
    if fmt == "imm16,memrn":
        return "#0x%04x, [r%d]" % (extra, low & 7)
    if fmt == "imm16,reg0":
        return "#0x%04x, r%d" % (extra, (w >> 12) & 3)
    if fmt == "memr45,memr0123":
        return "[r%d],[r%d]" % (4 + ((w >> 5) & 3), (w & 3))
    if fmt == "regp0,reg5":
        return "r%d, r%d" % ((w >> 12) & 3, (w >> 8) & 7)
    if fmt == "regp0,bx5":
        return "r%d, b%d" % ((w >> 12) & 3, (w >> 8) & 1)
    if fmt == "ab10,ab5":
        return "%s, %s" % (abname((w >> 10) & 1), abname((w >> 5) & 1))
    if fmt == "ax12":
        return "a%d" % ((w >> 12) & 3)
    if fmt == "bx12":
        return "b%d" % ((w >> 12) & 3)
    if fmt == "memimm8,imm4bit":
        return "[0x%02x], bit%d" % (low, (w >> 8) & 0xF)
    if fmt == "reg0,imm4bit":
        return "r%d, bit%d" % ((w >> 4) & 0xF, (w >> 8) & 0xF)
    if fmt == "memrn,imm4bit":
        return "[r%d], bit%d" % (low & 7, (w >> 8) & 0xF)
    if fmt == "sv,memimm8":
        return "sv, [0x%02x]" % low
    if fmt == "memimm8,sv":
        return "[0x%02x], sv" % low
    if fmt == "memimm8,axh12,eu":
        return "[0x%02x], a%dh, eu" % (low, (w >> 12) & 3)
    if fmt == "memimm8,ablh10":
        return "[0x%02x], %s" % (low, abname((w >> 10) & 1))
    if fmt == "ablh9,memimm8":
        return "%s, [0x%02x]" % (abname((w >> 9) & 1), low)
    if fmt == "sv,regp0,ab5":
        return "sv, r%d, %s" % ((w >> 12) & 3, abname((w >> 5) & 1))
    if fmt == "sv,memrn,ab5":
        return "sv, [r%d], %s" % (low & 7, abname((w >> 5) & 1))
    if fmt == "sv,memimm8,ab11":
        return "sv, [0x%02x], %s" % (low, abname((w >> 11) & 1))
    if fmt == "y0,memrn,ax8":
        return "y0, [r%d], a%d" % (low & 7, (w >> 8) & 3)
    if fmt == "y0,reg0,ax8":
        return "y0, r%d, a%d" % ((w >> 12) & 3, (w >> 8) & 3)
    if fmt == "reg,ax8":
        return "r%d, a%d" % ((w >> 12) & 3, (w >> 8) & 3)
    if fmt == "axl8,memr7imm7s":
        return "a%dl, [r7+#%d]" % ((w >> 8) & 3, low if low < 0x80 else low - 0x100)
    if fmt == "memr7imm7s,ax8":
        return "[r7+#%d], a%d" % (low if low < 0x80 else low - 0x100, (w >> 8) & 3)
    if fmt == "memrn,bx8":
        return "[r%d], b%d" % (low & 7, (w >> 8) & 1)
    if fmt == "imm2,ps":
        return "#%d, ps" % (w & 3)
    if fmt == "imm7,stepi":
        return "#%d, stepi" % ((w & 0x7F))
    if fmt == "imm7,stepj":
        return "#%d, stepj" % ((w & 0x7F))
    if fmt == "imm5,icr":
        return "#0x%02x, icr" % (w & 0x1F)
    if fmt == "reg0,icr":
        return "r%d, icr" % ((w >> 4) & 0xF)
    if fmt == "norev,imm8u,addr16":
        return "imm8=#0x%02x" % low
    if fmt == "norev,reg0,addr18":
        return "reg=r%d" % ((w >> 4) & 0xF)
    if fmt == "progmemaxl5,reg0":
        return "pm[a%dl], r%d" % ((w >> 5) & 3, (w >> 4) & 0xF)
    if fmt == "p0,y0,imm8s":
        return "p0, y0, #%d" % (low if low < 0x80 else low - 0x100)
    if fmt == "ax8,axnot8,ge":
        return "a%d, a%d, ge" % ((w >> 8) & 3, ((w >> 8) & 3) ^ 1)
    if fmt == "ax8,axnot8,gt":
        return "a%d, a%d, gt" % ((w >> 8) & 3, ((w >> 8) & 3) ^ 1)
    if fmt == "ax8,axnot8,le":
        return "a%d, a%d, le" % ((w >> 8) & 3, ((w >> 8) & 3) ^ 1)
    if fmt == "ax8,axnot8,lt":
        return "a%d, a%d, lt" % ((w >> 8) & 3, ((w >> 8) & 3) ^ 1)
    if fmt == "ax8,memr0,ge":
        return "a%d, [r0], ge" % ((w >> 8) & 3)
    if fmt == "ax8,memr0,gt":
        return "a%d, [r0], gt" % ((w >> 8) & 3)
    if fmt == "memr0123,progmemr45":
        return "[r%d], pm[r%d]" % (w & 3, 4 + ((w >> 5) & 3))
    if fmt == "progmemrn,memr0123":
        return "pm[r%d], [r%d]" % (low & 7, (w >> 5) & 7)
    if fmt == "memr0425,abh8":
        return "[r%d], %s" % ((w >> 2) & 3, abname((w >> 8) & 1))
    if fmt == "ax8,memrn":
        return "a%d, [r%d]" % ((w >> 8) & 3, low & 7)
    if fmt == "bx0,sv":
        return "b0, sv"
    if fmt == "bx0,sv,ax8":
        return "b0, sv, a%d" % ((w >> 8) & 3)
    if fmt == "regp0,sv":
        return "r%d, sv" % ((w >> 12) & 3)
    if fmt == "regp0,sv,ax8":
        return "r%d, sv, a%d" % ((w >> 12) & 3, (w >> 8) & 3)
    if fmt == "memrn,sv":
        return "[r%d], sv" % (low & 7)
    if fmt == "memrn,sv,ax8":
        return "[r%d], sv, a%d" % (low & 7, (w >> 8) & 3)
    if fmt == "imm6s,ab10,ab7":
        return "#%d, %s, %s" % ((w & 0x3F), abname((w >> 10) & 1), abname((w >> 7) & 1))
    if fmt == "sv,ab10,ab5":
        return "sv, %s, %s" % (abname((w >> 10) & 1), abname((w >> 5) & 1))
    if fmt == "memimm8,ab11":
        return "[0x%02x], %s" % (low, abname((w >> 11) & 1))
    if fmt == "memrn,ax8":
        return "[r%d], a%d" % (low & 7, (w >> 8) & 3)
    if fmt == "abl10,dvm":
        return "a%dl, dvm" % ((w >> 10) & 1)
    if fmt == "abl10,x0":
        return "a%dl, x0" % ((w >> 10) & 1)
    if fmt == "dvm,ab5":
        return "dvm, %s" % abname((w >> 5) & 1)
    if fmt == "icr,ab5":
        return "icr, %s" % abname((w >> 5) & 1)
    if fmt == "x0,ab5":
        return "x0, %s" % abname((w >> 5) & 1)
    if fmt == "repc,ab5":
        return "repc, %s" % abname((w >> 5) & 1)
    if fmt == "imm4bit":
        return "bit%d" % ((w >> 8) & 0xF)
    if fmt == "swaptypes4":
        return "swap#%d" % (w & 0xF)
    if fmt == "bankflags6":
        return "bank#%d" % (w & 0x3F)
    if fmt == "axl8":
        return "a%dl" % ((w >> 8) & 3)
    return fmt + " (w=%04x)" % w

def abname(bit):
    return "a%dh" % bit if bit & 1 == 0 else "a%dh" % bit

def rel_target(addr, w, is_callr):
    # callr/brr: RelAddr7@4, target = pc + signed(RelAddr7)*2? (GBATEK: 7-bit relative, word units)
    disp = (w >> 4) & 0x7F
    if disp & 0x40:
        disp -= 0x80
    return addr + 1 + disp  # word-address; page wrapping ignored

def main():
    path = sys.argv[1]
    start = int(sys.argv[2], 16)
    count = int(sys.argv[3])
    stats = "--stats" in sys.argv[4:]
    data = open(path, "rb").read()
    mnemonics = collections.Counter()
    unknown = 0
    branch_ok = 0
    branch_total = 0
    out_lines = []
    pos = start
    addr = 0
    while pos + 1 < len(data) and addr < count:
        w = data[pos] | (data[pos+1] << 8)
        dec = decode_word(w)
        if dec is None:
            out_lines.append("%08X  %04X          ???" % (addr, w))
            unknown += 1
            mnemonics["???"] += 1
            pos += 2
            addr += 1
            continue
        mnem, fmt, param, rest = dec
        extra = None
        nparam = param
        if nparam:
            if pos + 3 >= len(data):
                break
            extra = data[pos+2] | (data[pos+3] << 8)
        line = "%08X  %04X          %-7s %s" % (addr, w, mnem, fmt_operand(fmt, w, extra))
        if mnem in ("brr", "callr"):
            tgt = rel_target(addr, w, mnem == "callr")
            cond = COND[w & 0xF]
            line = "%08X  %04X          %-7s %s  ; -> 0x%04X" % (addr, w, mnem, cond, tgt & 0xFFFF)
            branch_total += 1
            # self-consistency: target within the disassembled region ± 256
            if 0 <= tgt - start // 2 < count + 256:
                branch_ok += 1
        out_lines.append(line)
        mnemonics[mnem] += 1
        pos += 2 + nparam * 2
        addr += 1 + nparam

    if stats:
        print("region 0x%06X..0x%06X  count=%d" % (start, start + (pos - start), addr))
        print("unknown words: %d" % unknown)
        print("distinct mnemonics: %d" % len(mnemonics))
        for m, c in mnemonics.most_common(14):
            print("  %-10s %d" % (m, c))
        print("branches: %d, in-region targets: %d (%.1f%%)" % (branch_total, branch_ok,
              100.0*branch_ok/max(branch_total,1)))
    else:
        for l in out_lines:
            print(l)

if __name__ == "__main__":
    main()
