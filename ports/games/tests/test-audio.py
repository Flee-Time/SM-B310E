#!/usr/bin/env python3
"""Exercise the actual integer game mixer and sequencer with bounded fixtures."""
import argparse
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]
def main(cc):
    out=ROOT/'build/validation/game-audio';out.mkdir(parents=True,exist_ok=True)
    source=ROOT/'build/game-ports/fpdoom';exe=out/'audio-test.exe'
    subprocess.run([cc,'-std=c99','-Wall','-Wextra','-O2','-I',str(source),
                    str(Path(__file__).with_name('audio-test.c')),str(source/'b310e-mixer.c'),
                    str(source/'b310e-music.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--cc',default='gcc')
    main(parser.parse_args().cc)
