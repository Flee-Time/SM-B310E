#!/usr/bin/env python3
"""Regression checks for the actual common strncat used by save filenames."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main(cc):
    out = ROOT / 'build/validation/game-strings'
    out.mkdir(parents=True, exist_ok=True)
    common = (ROOT / 'build/game-ports/fpdoom/common.c').read_text()
    start = common.index('char *strncat(')
    end = common.index('\nint strcmp(', start)
    # Compile the prepared implementation, not the host C library's version.
    source = out / 'strings.c'
    source.write_text('#include <string.h>\n#include <assert.h>\n#include <stdio.h>\n' +
                      common[start:end] + '''
int main(void) {
    char name[32] = "savegam0.";
    assert(strncat(name, "wl6", 3) == name);
    assert(!strcmp(name, "savegam0.wl6"));
    char bounded[8] = {'a', 0, 'X', 'X', 'X', 'X', 'X', 'X'};
    strncat(bounded, "bcdef", 2);
    assert(!strcmp(bounded, "abc") && bounded[4] == 'X');
    strncat(bounded, "ignored", 0);
    assert(!strcmp(bounded, "abc") && bounded[4] == 'X');
    char short_source[8] = {'a', 0, 'X', 'X', 'X', 'X', 'X', 'X'};
    strncat(short_source, "b", 5);
    assert(!strcmp(short_source, "ab") && short_source[3] == 'X');
    puts("PASS: save suffix, bounded append, zero count and short source");
    return 0;
}
''')
    exe = out / 'strings.exe'
    subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-fno-builtin',
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    main(parser.parse_args().cc)
