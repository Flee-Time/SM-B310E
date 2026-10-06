#!/usr/bin/env python3
"""Exercise the real C JSON parser, ROM discovery and launch arguments."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='fpmain-config-') as tmp:
        root = Path(tmp)
        exe = root / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(PORT), str(PORT / 'tests/config-driver.c'), '-o', str(exe)], check=True)
        manifest = root / 'files.txt'
        manifest.write_text('games/snes|My Game.SFC\ngames/snes|Another.smc\n'
                            'games/snes|readme.txt\ngames/snes|.sfc\n'
                            'games/nes|Spaced name.nes\n')
        count = 0

        def run(source, valid=True):
            nonlocal count
            file, out = root / 'config.json', root / 'output.bin'
            file.write_bytes(source.encode('utf-8') if isinstance(source, str) else source)
            result = subprocess.run([str(exe), str(file), str(manifest), str(out)], cwd=root, timeout=3)
            assert result.returncode == (0 if valid else 1), (source[:300], result.returncode)
            count += 1
            if not valid:
                return None
            data = out.read_bytes()
            items, pos = [], 0
            while struct.unpack_from('<I', data, pos)[0]:
                pos += struct.unpack_from('<I', data, pos)[0]
                assert pos % 4 == 0 and pos < len(data)
                end = data.index(0, pos + 4)
                name = data[pos + 4:end].decode('utf-8')
                argc = struct.unpack_from('<H', data, end + 1)[0]
                args, cursor = [], end + 3
                for _ in range(argc):
                    stop = data.index(0, cursor)
                    args.append(data[cursor:stop].decode('utf-8'))
                    cursor = stop + 1
                items.append((name, args))
            return items

        assert run('{}') == []
        assert run('{"categories":[],"emulators":[],"system":[]}') == []
        assert run(b'\xef\xbb\xbf{}\r\n') == []
        ports = {'categories': [{'items': [{'args': ['doom', 'file with spaces.wad'],
                                            'bin': 'fpbin/doom.bin', 'name': 'Port'}], 'name': 'Ports'}],
                 'system': ['--bright', '50']}
        items = run(json.dumps(ports))
        assert items[1] == ('Port', ['fpbin/doom.bin', '--bright', '50', 'doom', 'file with spaces.wad'])
        assert (root / 'boot-args.tmp').read_bytes() == struct.pack('<i', 2) + b'--bright\x0050\x00'
        ports['categories'][0]['items'][0]['name'] = 'Caf\u00e9 \U0001f3ae'
        assert run(json.dumps(ports))[1][0] == 'Caf\u00e9 \U0001f3ae'
        spec = {'name': 'SNES', 'bin': 'fpbin/snes.bin', 'directory': 'games/snes',
                'extensions': ['.sfc', 'SMC'], 'args': ['--dir', 'games/snes', 'snes9x']}
        items = run(json.dumps({'emulators': [spec], 'system': ['--bright', '50']}))
        assert [x[0] for x in items] == ['=== SNES ===', 'My Game.SFC', 'Another.smc']
        assert items[1][1] == ['fpbin/snes.bin', '--bright', '50', '--dir', 'games/snes', 'snes9x', 'My Game.SFC']
        spec['directory'] = 'missing'
        assert len(run(json.dumps({'emulators': [spec]}))) == 1
        run(PORT.joinpath('config.json').read_text())
        # Every truncated prefix of a valid config fails; no partial list escapes.
        text = json.dumps(ports)
        for length in range(len(text)):
            run(text[:length], False)
        for bad in ['', '[]', '{', '{"categories":[]', '{}junk', '{"x":true,}',
                    '{"x":01}', '{"x":1.}', '{"x":1e}', '{"x":NaN}',
                    '{"x":"unterminated}', '{"x":"\\q"}', '{"x":"\\u0000"}',
                    '{"x":"\\ud800"}', '{"x":"\\udc00"}', '{"x":1,"x":2}',
                    '{"system":1}', '{"categories":[{}]}', '{"emulators":[{}]}',
                    '{"categories":[{"name":"P","items":[{}]}]}']:
            run(bad, False)
        run('{"x":' + '[' * 18 + '0' + ']' * 18 + '}', False)
        run(json.dumps({'system': ['a'] * 49}), False)
        run(json.dumps({'x': 'a' * 256}), False)
        run(json.dumps({'x': [0] * 2048}), False)
        run(b' ' * 65537, False)
        spec['directory'] = 'games/snes'
        manifest.write_text(''.join(f'games/snes|ROM {i}.sfc\n' for i in range(100)))
        items = run(json.dumps({'emulators': [spec]}))
        assert len(items) == 65 and items[-1][1][-1] == 'ROM 63.sfc'
        print(f'PASS {count} configurations: empty/BOM/property order/Unicode, ROM filtering/limits, argv, malformed/truncated input')


if __name__ == '__main__':
    main()
