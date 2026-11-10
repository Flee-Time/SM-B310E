#!/usr/bin/env python3
"""The SNES for the phone: libretro's snes9x2002 (build/snes9x2002, python
os/ports/sources.py snes9x2002), its core and its libretro glue, copied to the build
with these changes (each checked). Snes9x's licence is not the GPL: nothing of it is
in the repository, and a built snes9x.app is for the user's own phone.

 5. The tile caches as big as what is written in them: 64 bytes a tile (8 x 8),
    not 128 (448 KB, room for 14 more ROM pages).
 4. The screens are as big as a frame (239 lines of 640 bytes), the sub screen
    0x28000 after the main one, not 2 MB (tile16_t.h's offset with it); the depth
    buffers 320 x 256. The core asked for 8 MB, did not look whether it got it and
    drew through NULL, into the flash's addresses.
 3. A ROM bigger than the memory left is in pages (32 KB, 64 KB for HiROM) read from
    the card as the game reaches them, the least used giving way (snes_kit.c): Map's
    blocks of it not in are MAP_PAGED, which the accessors (getset.h) page in. Its
    start is read whole; it is not summed or CRC'd (that would read it all).
 2. Room for 8192 raster operations a frame, not 65536 (512 KB); no cheat search's
    copies of the RAM (300 KB; cheats.c is not built).
 1. The ROM's buffer is as big as the ROM (the frontend says, s9x_rom_alloc), not
    6 MB; the frontend reads the ROM into it itself, so there is one copy.

  python os/ports/snes9x/prepare.py build/snes9x2002 <out>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

CHANGES = {
    'src/getset.h': [
        ('#include "sa1.h"\n',
         '#include "sa1.h"\n\nuint8 *S9xPageIn(uint32 Address);   /* (B310E) snes_kit.c: its block\'s Map */\n'),
        ('      return (*(GetAddress + (Address & 0xffff)));\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n',
         '      return (*(GetAddress + (Address & 0xffff)));\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n   case MAP_PAGED:\n      return (*(S9xPageIn(Address) + (Address & 0xffff)));\n'),
        ('#endif\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n',
         '#endif\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n   case MAP_PAGED:\n      S9xPageIn(Address);\n      return (S9xGetWord(Address));\n'),
        ('   if (GetAddress >= (uint8*) MAP_LAST)\n      return (GetAddress);\n\n   switch ((intptr_t) GetAddress)\n   {\n',
         '   if (GetAddress >= (uint8*) MAP_LAST)\n      return (GetAddress);\n\n   switch ((intptr_t) GetAddress)\n   {\n   case MAP_PAGED:\n      return (S9xPageIn(Address));\n'),
        ('   if (GetAddress >= (uint8*) MAP_LAST)\n      return (GetAddress + (Address & 0xffff));\n\n   switch ((intptr_t) GetAddress)\n   {\n',
         '   if (GetAddress >= (uint8*) MAP_LAST)\n      return (GetAddress + (Address & 0xffff));\n\n   switch ((intptr_t) GetAddress)\n   {\n   case MAP_PAGED:\n      return (S9xPageIn(Address) + (Address & 0xffff));\n'),
        ('      CPU.PC = GetAddress + (Address & 0xffff);\n      return;\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n',
         '      CPU.PC = GetAddress + (Address & 0xffff);\n      return;\n   }\n\n   switch ((intptr_t) GetAddress)\n   {\n   case MAP_PAGED:\n      S9xPageIn(Address);\n      S9xSetPCBase(Address);\n      return;\n'),
    ],
    'src/memmap.h': [
        ('   MAP_BWRAM_BITMAP2, MAP_SA1RAM, MAP_LAST',
         '   MAP_BWRAM_BITMAP2, MAP_SA1RAM, MAP_PAGED /* (B310E) a ROM page not in yet */, MAP_LAST'),
    ],
    'src/rops.h': [
        ('#define MAX_ROPS  0x10000', '#define MAX_ROPS  0x2000  /* (B310E) a frame\'s: 512 KB was room for 65536 */'),
    ],
    'src/cheats.h': [                           # (cheat search, cheats.c, is not built)
        ('   uint8       CWRAM [0x20000];\n   uint8       CSRAM [0x10000];\n   uint8       CIRAM [0x2000];\n',
         '   uint8       CWRAM [4];  /* (B310E) for the search, not built */\n   uint8       CSRAM [4];\n   uint8       CIRAM [4];\n'),
        ('   uint32      WRAM_BITS [0x20000 >> 3];\n   uint32      SRAM_BITS [0x10000 >> 3];\n   uint32      IRAM_BITS [0x2000 >> 3];\n',
         '   uint32      WRAM_BITS [1];\n   uint32      SRAM_BITS [1];\n   uint32      IRAM_BITS [1];\n'),
    ],
    'libretro/libretro.c': [                    # (see 4)
        ('   GFX.Screen_buffer = (uint8 *) calloc(1, 2048 * 512 * 2 * 2 + safety);\n',
         '   GFX.Screen_buffer = (uint8 *) calloc(1, 2 * 0x28000 + safety);  /* (B310E) 2 x 239 lines of 640 */\n'),
        ('   GFX.SubScreen = GFX.Screen + 2048 * 512 * 2;\n',
         '   GFX.SubScreen = GFX.Screen + 0x28000;  /* (B310E) tile16_t.h\'s offset */\n'),
        ('calloc(1, GFX.Pitch * 512 * sizeof(uint16) + safety);\n',
         'calloc(1, 320 * 256 + safety);  /* (B310E) GFX_ZPITCH x the lines */\n', 2),
        ('   GFX.Delta = 1048576; //(GFX.SubScreen - GFX.Screen) >> 1;\n',
         '   GFX.Delta = 0x28000 >> 1;  /* (B310E) */\n'
         '   if (!GFX.Screen_buffer || !GFX.ZBuffer_buffer || !GFX.SubZBuffer_buffer)\n'
         '      exit(1);  /* (B310E) not drawn through NULL: that is the flash */\n'),
    ],
    'src/tile16_t.h': [                         # (see 4)
        ('\tmov\tr8, #0x200000\t', '\tmov\tr8, #0x28000\t', 2),
    ],
    'src/memmap.c': [
        ('    IPPU.TileCache [TILE_2BIT] = (uint8 *) malloc (MAX_2BIT_TILES * 128);\n'
         '    IPPU.TileCache [TILE_4BIT] = (uint8 *) malloc (MAX_4BIT_TILES * 128);\n'
         '    IPPU.TileCache [TILE_8BIT] = (uint8 *) malloc (MAX_8BIT_TILES * 128);\n',
         '    /* (B310E) 64 bytes a tile: tile16.c writes 8 x 8, tile16.h reads at << 6 */\n'
         '    IPPU.TileCache [TILE_2BIT] = (uint8 *) malloc (MAX_2BIT_TILES * 64);\n'
         '    IPPU.TileCache [TILE_4BIT] = (uint8 *) malloc (MAX_4BIT_TILES * 64);\n'
         '    IPPU.TileCache [TILE_8BIT] = (uint8 *) malloc (MAX_8BIT_TILES * 64);\n'),
        ('      FileSize = READ_STREAM (ptr, MAX_ROM_SIZE + 0x200 - (ptr - Memory.ROM), ROMFile);\n      CLOSE_STREAM (ROMFile);\n',
         '      FileSize = READ_STREAM (ptr, MAX_ROM_SIZE + 0x200 - (ptr - Memory.ROM), ROMFile);\n      CLOSE_STREAM (ROMFile);\n      {  /* (B310E) a ROM in pages: only its start is here; its size is all of it */\n         extern unsigned s9x_rom_paged;\n         if (s9x_rom_paged)\n            FileSize = s9x_rom_paged;\n      }\n'),
        ('   for (i = 0; i < size; i++)\n      sum1 += Memory.ROM [i];\n\n   for (i = 0; i < (int) remainder; i++)\n      sum2 += Memory.ROM [size + i];\n',
         '   {  /* (B310E) a ROM in pages is not all here to be summed */\n      extern unsigned s9x_rom_paged;\n      if (s9x_rom_paged)\n         size = remainder = 0, sum1 = Memory.ROMChecksum;\n   }\n   for (i = 0; i < size; i++)\n      sum1 += Memory.ROM [i];\n\n   for (i = 0; i < (int) remainder; i++)\n      sum2 += Memory.ROM [size + i];\n'),
        ('   Memory.CalculatedChecksum = caCRC32(&Memory.ROM[0], Memory.CalculatedSize);\n',
         '   {\n      extern unsigned s9x_rom_paged;\n      Memory.CalculatedChecksum = s9x_rom_paged ? 0 : caCRC32(&Memory.ROM[0], Memory.CalculatedSize);\n   }\n'),
        ('    Memory.ROM     = (uint8 *) malloc (MAX_ROM_SIZE + 0x200 + 0x8000);\n',
         '    {   /* (B310E) as big as the ROM, its header and the core\'s margin */\n'
         '        extern unsigned s9x_rom_alloc;\n'
         '        Memory.ROM = (uint8 *) malloc (s9x_rom_alloc ? s9x_rom_alloc : MAX_ROM_SIZE + 0x200 + 0x8000);\n'
         '    }\n'),
        ('memset (Memory.ROM + Memory.CalculatedSize, 0, MAX_ROM_SIZE - Memory.CalculatedSize);',
         '{ extern unsigned s9x_rom_alloc; unsigned end_ = s9x_rom_alloc ? s9x_rom_alloc - 0x200 - 0x8000 : MAX_ROM_SIZE; '
         'if (end_ > Memory.CalculatedSize) memset (Memory.ROM + Memory.CalculatedSize, 0, end_ - Memory.CalculatedSize); }'
         '  /* (B310E) to its buffer\'s end */', 2),
    ],
}

KINDS = ('.c', '.h', '.S', '.s')


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    stamp = out / '.prepared'
    if stamp.exists():
        stamp.unlink()
    trees = [src / 'src', src / 'libretro', src / 'libretro/libretro-common']
    for tree in trees:
        for path in sorted(tree.rglob('*') if tree.name == 'libretro-common' else tree.iterdir()):
            if not path.is_file() or path.suffix not in KINDS:
                continue
            rel = path.relative_to(src).as_posix()
            data = path.read_bytes()
            if rel in CHANGES:
                text = data.decode('latin-1')
                crlf = '\r\n' in text
                text = text.replace('\r\n', '\n')
                for old, new, *times in CHANGES[rel]:
                    if text.count(old) != (times[0] if times else 1):
                        sys.exit(f'prepare.py: {rel} is not what it was (looked for {old!r})')
                    text = text.replace(old, new)
                if crlf:
                    text = text.replace('\n', '\r\n')
                data = text.encode('latin-1')
            dest = out / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            if not dest.exists() or dest.read_bytes() != data:
                dest.write_bytes(data)
    stamp.write_text('ok\n')


if __name__ == '__main__':
    main()
