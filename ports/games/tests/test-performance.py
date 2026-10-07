#!/usr/bin/env python3
"""Compare optimized audio with the pinned engines, using their real code."""
import argparse
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]
TEST=Path(__file__).resolve().parent
def main(cc,cxx):
    out=ROOT/'build/validation/game-performance';out.mkdir(parents=True,exist_ok=True)
    wolf=ROOT/'build/game-ports/wolf3d'
    # Restore just the calibration loop in a separate namespace for comparison.
    source=(wolf/'b310e-dbopl.cpp').read_text()
    original=(wolf/'Wolf4SDL/dosbox/dbopl.cpp').read_text()
    first=original.index('\t//Generate the best matching attack rate')
    last=original.index('\tfor ( Bit8u i = 62; i < 76; i++ )',first)
    a=source.index('\t/* Fixed output rate:');b=source.index('\tfor ( Bit8u i = 62; i < 76; i++ )',a)
    source=source[:a]+original[first:last]+source[b:]
    source=source.replace('"b310e-dbopl.h"','"reference-dbopl.h"').replace('DBOPL','DBOPLReference')
    (out/'reference-dbopl.cpp').write_text(source)
    (out/'reference-dbopl.h').write_text((wolf/'b310e-dbopl.h').read_text().replace('DBOPL','DBOPLReference'))
    exe=out/'opl-test.exe'
    subprocess.run([cxx,'-O2','-I',str(wolf),'-I',str(out),str(TEST/'opl-test.cpp'),
        str(wolf/'b310e-dbopl.cpp'),str(out/'reference-dbopl.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
    exe=out/'cache-test.exe';sys=ROOT/'build/game-ports/fpdoom'
    subprocess.run([cc,'-std=c99','-O2','-I',str(sys),str(TEST/'cache-test.c'),
        str(sys/'b310e-mixer.c'),str(sys/'b310e-music.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
    exe=out/'nes-test.exe';nes=ROOT/'build/game-ports/infones/InfoNES/src'
    subprocess.run([cc,'-std=c99','-O2','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-I',str(nes),str(TEST/'nes-apu-test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--cc',default='gcc');p.add_argument('--cxx',default='g++')
    a=p.parse_args();main(a.cc,a.cxx)
