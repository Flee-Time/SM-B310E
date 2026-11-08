#!/usr/bin/env python3
"""InfoNES on B310E OS: copy upstream's source (build/infones, unchanged) to
the build and make its changes, each checked:

Bugs of upstream's:
 1. The sound's envelope and sweep counters are unsigned, and only ever
    tested for being below 0: no envelope and no sweep ever advanced. Every
    effect stayed on its first note, at its first volume, until it ended.
    Signed, they would run hundreds of times too fast: they were moved into
    the sample loops with their units left per frame (so they are scaled by
    the samples in a frame). Then the rest that test os/tests/nes-apu-test.c
    holds it to: the envelope's volume and its constant-volume flag the
    right way round, a sweep's division only when it changed (not per
    sample), pulse 2's negative sweep without pulse 1's extra -1, the length
    counter held by its flag, a muted channel still taking later register
    writes, and a timer write restarting the envelope.
 2. A write to the sound's registers past the end of the event queue (a
    game that writes very often) overran it.
 3. APU_Reg[0x4015]: the array has 0x18 bytes, so the frame interrupt's flag
    went into whatever lay 16 KB after it.
 4. An absolute address read its two bytes in one expression, the order
    the compiler's to choose.
 5. Mappers: a register past the end of its array (19, 45), a missing
    break (80, 82), % for & in a bank number (135, 140).

For this phone:
 6. The flag InfoNES sets on the backdrop's colour, for sprites behind the
    background: bit 15 of RGB555 is bit 0x20 here, the lowest of green, so
    the frame is RGB565 as it stands, and needs no turning into it.
 7. The 6502's fetches (nearly all from ROM) take the short way, without a
    call: ROMBANK0-3 become an array, K6502_Fetch inlined.
11. The sound's five channels called a function per sample to see whether
    a register write was due, thousands a frame, with next to none due.
    Now only when there is one.
10. PAL: 312 lines a frame, not 262 (InfoNES_ScanEnd), 50 frames a second, 882
    samples a frame, and pitches for its slower CPU. A PAL game at NTSC's rate
    runs a fifth too fast.
 9. Lines that are cropped (the 8 top and bottom a TV hid) are not drawn:
    InfoNES_LineFirst, InfoNES_LineCount. A frame's emulation takes the same
    time, with a sixteenth less to draw.
 8. No frame: it draws into two line buffers in turn, which stay in the
    cache, and gives each to InfoNES_LineDone, which scales it there and then.
    A frame of 115 KB meant 61440 pixels out to the slow memory and back
    (the ARM926's cache does not fetch a line for a write that misses it),
    the cache emptied by it each time.

  python os/ports/infones/prepare.py <upstream dir> <out dir>

SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import sys

FETCH = '''#include "K6502.h"
#include "InfoNES_System.h"

/* (B310E: see os/ports/infones/prepare.py) */
extern BYTE *ROMBANK[ 4 ];
extern WORD PC;

static inline __attribute__(( always_inline )) BYTE K6502_Fetch( void )
{
  WORD wAddr = PC++;

  return ( wAddr & 0x8000 ) ? ROMBANK[ ( wAddr >> 13 ) & 3 ][ wAddr & 0x1fff ]
                            : K6502_Read( wAddr );
}
'''

CHANGES = {
    'InfoNES_pAPU.cpp': [
        ('BYTE wave_buffers[5][735];', 'BYTE wave_buffers[5][882];'),
        ('  ApuCycleRate       = ApuQual[ ApuQuality ].cycle_rate;\n',
         '  ApuCycleRate       = ApuQual[ ApuQuality ].cycle_rate;\n'
         '  if ( InfoNES_ScanEnd > 262 )    /* PAL: 1.66 MHz, 50 frames a second */\n'
         '  {\n'
         '    ApuPulseMagic      = ( ApuPulseMagic >> 10 ) * 951;\n'
         '    ApuTriangleMagic   = ( ApuTriangleMagic >> 10 ) * 951;\n'
         '    ApuNoiseMagic      = ( ApuNoiseMagic >> 10 ) * 951;\n'
         '    ApuSamplesPerSync  = 882;\n'
         '    ApuCyclesPerSample = 40;\n'
         '  }\n'),
        ('DWORD ApuC1EnvPhase;', 'int ApuC1EnvPhase;'),
        ('DWORD ApuC1SweepPhase;', 'int ApuC1SweepPhase;'),
        ('DWORD ApuC2EnvPhase;', 'int ApuC2EnvPhase;'),
        ('DWORD ApuC2SweepPhase;', 'int ApuC2SweepPhase;'),
        ('DWORD ApuC4EnvPhase;', 'int ApuC4EnvPhase;'),
        ('void ApuWrite##name(WORD addr, BYTE value) \\\n{ \\\n',
         'void ApuWrite##name(WORD addr, BYTE value) \\\n{ \\\n'
         '  if ( cur_event >= APU_EVENT_MAX ) return; \\\n'),
    ],
    'InfoNES.cpp': [
        ('APU_Reg[ 0x4015 ] |= 0x40;', 'APU_Reg[ 0x15 ] |= 0x40;'),
        ('pPoint[ nX ] & 0x8000', 'pPoint[ nX ] & 0x0020'),
        # A line with no sprite (most are) clears no sprite buffer and walks no 256 pixels
        ('''    // Reset sprite buffer
    InfoNES_MemorySet( pSprBuf, 0, sizeof pSprBuf );
''', ''),
        ('''      // Holizontal Sprite Count +1
      ++nSprCnt;
''', '''      // Holizontal Sprite Count +1
      if ( !nSprCnt++ )
        InfoNES_MemorySet( pSprBuf, 0, sizeof pSprBuf );
'''),
        ('''    for ( nX = 0; nX < NES_DISP_WIDTH; ++nX )
    {
      nSprData = pSprBuf[ nX ];
''', '''    for ( nX = nSprCnt ? 0 : NES_DISP_WIDTH; nX < NES_DISP_WIDTH; ++nX )
    {
      nSprData = pSprBuf[ nX ];
'''),
        # Most games' mappers do nothing here: no call for each tile of each line
        ('void (*MapperPPU)( WORD wAddr );',
         'void (*MapperPPU)( WORD wAddr );\nvoid Map0_PPU( WORD wAddr );'),
        ('BYTE *ROMBANK0;\nBYTE *ROMBANK1;\nBYTE *ROMBANK2;\nBYTE *ROMBANK3;\n',
         'BYTE *ROMBANK[ 4 ];\n'),
        ('WORD WorkFrame[ NES_DISP_WIDTH * NES_DISP_HEIGHT ];',
         'static WORD LineBuf[ 2 ][ NES_DISP_WIDTH ] __attribute__(( aligned( 32 ) ));\n'
         'void InfoNES_LineDone( int nLine, WORD *pLine );\n'
         'int InfoNES_LineFirst = 0, InfoNES_LineCount = NES_DISP_HEIGHT;\n'
         'int InfoNES_ScanEnd = 262;'),
        ('BYTE PPU_ScanTable[ 263 ];', 'BYTE PPU_ScanTable[ 313 ];'),
        ('for ( nIdx = 0; nIdx < 263; ++nIdx )', 'for ( nIdx = 0; nIdx < 313; ++nIdx )'),
        ('''  pPoint = &WorkFrame[ PPU_Scanline * NES_DISP_WIDTH ];

  // Clear a scanline if screen is off
''', '''  pPoint = LineBuf[ PPU_Scanline & 1 ];

  // Clear a scanline if screen is off
'''),
        ('''  if ( FrameCnt == 0 &&
       PPU_ScanTable[ PPU_Scanline ] == SCAN_ON_SCREEN )
  {
    InfoNES_DrawLine();
''', '''  if ( FrameCnt == 0 &&
       PPU_ScanTable[ PPU_Scanline ] == SCAN_ON_SCREEN &&
       (unsigned)( PPU_Scanline - InfoNES_LineFirst ) < (unsigned)InfoNES_LineCount )
  {
    InfoNES_DrawLine();
    InfoNES_LineDone( PPU_Scanline, LineBuf[ PPU_Scanline & 1 ] );
'''),
    ],
    'InfoNES.h': [
        ('#define SCAN_VBLANK_END               262',
         'extern int InfoNES_ScanEnd;\n#define SCAN_VBLANK_END               InfoNES_ScanEnd'),
        ('extern BYTE *ROMBANK0;\nextern BYTE *ROMBANK1;\nextern BYTE *ROMBANK2;\nextern BYTE *ROMBANK3;\n',
         'extern BYTE *ROMBANK[ 4 ];\n#define ROMBANK0 ROMBANK[ 0 ]\n#define ROMBANK1 ROMBANK[ 1 ]\n'
         '#define ROMBANK2 ROMBANK[ 2 ]\n#define ROMBANK3 ROMBANK[ 3 ]\n'),
    ],
    'K6502_rw.h': [
        ('byRet = APU_Reg[ 0x4015 ];', 'byRet = APU_Reg[ 0x15 ];'),
        ('APU_Reg[ 0x4015 ] &= ~0x40;', 'APU_Reg[ 0x15 ] &= ~0x40;'),
        ('NesPalette[ byData ] | 0x8000;', 'NesPalette[ byData ] | 0x0020;'),
    ],
    'K6502.cpp': [
        ('#include "K6502.h"\n#include "InfoNES_System.h"\n', FETCH),
        ('#define AA_ABS   ( K6502_Read( PC++ ) | (WORD)K6502_Read( PC++ ) << 8 )',
         '#define AA_ABS   ({ WORD wLo_ = K6502_Fetch(); (WORD)( wLo_ | K6502_Fetch() << 8 ); })'),
        ('#define AA_ABS2  ( K6502_Read( PC++ ) | (WORD)K6502_Read( PC ) << 8 )',
         '#define AA_ABS2  ({ WORD wLo_ = K6502_Fetch(); (WORD)( wLo_ | K6502_Read( PC ) << 8 ); })'),
    ],
    'InfoNES_Mapper_019.cpp': [
        ('  Map19_Regs[ 2 ] = 0x00;\n', ''),
    ],
    'InfoNES_Mapper_045.cpp': [
        ('DWORD Map45_C[4],', 'DWORD Map45_C[8],'),
    ],
    'InfoNES_Mapper_080.cpp': [
        ('''        InfoNES_Mirroring( 0 );
      }

    /* Set ROM Banks */
    case 0x7efa:''', '''        InfoNES_Mirroring( 0 );
      }
      break;

    /* Set ROM Banks */
    case 0x7efa:'''),
    ],
    'InfoNES_Mapper_082.cpp': [
        ('''        InfoNES_Mirroring( 0 );
      }

    /* Set ROM Banks */
    case 0x7efa:''', '''        InfoNES_Mirroring( 0 );
      }
      break;

    /* Set ROM Banks */
    case 0x7efa:'''),
    ],
}

# every one of these, not one (and K6502_Fetch itself goes in above)
def sound_changes():
    out = []
    for c in (1, 2, 4):
        out.append((f'ApuC{c}EnvPhase += ApuC{c}EnvDelay;',
                    f'ApuC{c}EnvPhase += ApuC{c}EnvDelay * ApuSamplesPerSync;'))
    for c in (1, 2):
        out += [
            (f'ApuC{c}SweepPhase += ApuC{c}SweepDelay;',
             f'changed = 1; ApuC{c}SweepPhase += ApuC{c}SweepDelay * ApuSamplesPerSync;'),
            (f'ApuC{c}SweepPhase -= 2;', f'int changed = 0; ApuC{c}SweepPhase -= 2;'),
            (f'/ ApuC{c}Freq;', f'/ ( ApuC{c}Freq + 1 );'),
            (f'if ( ApuC{c}Env )', f'if ( !ApuC{c}Env )'),
            (f'( ApuC{c}Vol + ApuC{c}EnvVol )', f'( 15 - ApuC{c}EnvVol )'),
            (f'if ( ApuC{c}Atl ) {{ ApuC{c}Atl--;  }}',
             f'if ( ApuC{c}Atl && !ApuC{c}Hold ) {{ ApuC{c}Atl--; }}'),
            (f'wave_buffers[{c - 1}][i] = 0;\n      break;',
             f'wave_buffers[{c - 1}][i] = 0;\n      continue;'),
            (f'case 3:\n\t  ApuC{c}d = ApuEventQueue[event].data;',
             f'case 3:\n\t  ApuC{c}d = ApuEventQueue[event].data;\n\t  ApuC{c}EnvVol = 0;\n'
             f'\t  ApuC{c}EnvPhase = ApuC{c}EnvDelay * ApuSamplesPerSync;'),
            (f'      if ( ApuC{c}Freq ) {{\n\tApuC{c}Skip =',
             f'      if ( changed && ApuC{c}Freq ) {{\n\tApuC{c}Skip ='),
        ]
    out.append(('ApuC2Freq -= ~( ApuC2Freq >> ApuC2SweepShifts );',
                'ApuC2Freq -= ( ApuC2Freq >> ApuC2SweepShifts );'))
    return out


REPLACE_ALL = {
    'InfoNES_pAPU.cpp': sound_changes() + [
        (', 0, 735 );', ', 0, 882 );'),
        ('int ApuQuality;', 'int ApuQuality;\nextern int InfoNES_ScanEnd;'),
    ] + [(f'event = ApuWriteWave{n}( cycles, event );',
          f'if ( event < cur_event ) event = ApuWriteWave{n}( cycles, event );') for n in range(1, 6)],
    'InfoNES.cpp': [('pPointTop = &WorkFrame[ PPU_Scanline * NES_DISP_WIDTH ];',
                     'pPointTop = LineBuf[ PPU_Scanline & 1 ];'),
                    ('MapperPPU( PATTBL( pbyChrData ) );',
                     'if ( MapperPPU != Map0_PPU ) MapperPPU( PATTBL( pbyChrData ) );')],
    'K6502.cpp': [('K6502_Read( PC++ )', 'K6502_Fetch()')],
    'InfoNES_Mapper_135.cpp': [('byData%0x07', 'byData&0x07')],
    'InfoNES_Mapper_140.cpp': [('byData%0xF0', 'byData&0xF0')],
}


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    files = [p for p in src.rglob('*') if p.is_file() and p.suffix in ('.cpp', '.h')
             and 'linux' not in p.parts and 'win32' not in p.parts]
    for path in files:
        dest = out / path.relative_to(src)
        dest.parent.mkdir(parents=True, exist_ok=True)
        text = path.read_bytes().decode('latin-1').replace('\r\n', '\n')
        for old, new in CHANGES.get(path.name, []):
            if text.count(old) != 1:
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace(old, new)
        for old, new in REPLACE_ALL.get(path.name, []):
            if old not in text:
                sys.exit(f'prepare.py: {path.name} is not what it was (looked for {old!r})')
            text = text.replace(old, new)
        if not dest.exists() or dest.read_bytes() != text.encode('latin-1'):
            dest.write_bytes(text.encode('latin-1'))
    (out / '.prepared').write_text('ok\n')


if __name__ == '__main__':
    main()
