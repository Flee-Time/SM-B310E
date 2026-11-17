#!/usr/bin/env python3
"""Read the B310E OS data store (/b310e-os/data, os/fs/db.c) on the PC.

  python os/tools/b310e-db.py data            every key and table, as the shell's
                                              "db export" writes them
  python os/tools/b310e-db.py data --json     the same as JSON

As a module (the tests): parse(bytes) -> Store, with .keys {key: value},
.tables {name: Table(fields, version, index, records {id: [values]})}, .end
(where the good records end), .error (why they end there, or None), .dead.

Standard library only. SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
import struct
import sys
import zlib

MAGIC, HDR, REC_HDR = 0x42443342, 16, 8
KV_SET, KV_DEL, T_DEF, T_DROP, R_PUT, R_DEL = range(1, 7)
INT, BOOL, STR, BLOB = range(1, 5)
TYPES = {INT: 'int', BOOL: 'bool', STR: 'str', BLOB: 'blob'}


class Table:
    def __init__(self, name):
        self.name, self.fields, self.version, self.index = name, [], 0, -1
        self.next_id, self.records = 1, {}


class Store:
    def __init__(self):
        self.keys, self.tables, self.end, self.error, self.dead = {}, {}, 0, None, 0
        self.stamp, self.last = None, None


def _value(b, p, t):
    if t == INT:
        return struct.unpack_from('<i', b, p)[0], p + 4
    if t == BOOL:
        return bool(b[p]), p + 1
    n = struct.unpack_from('<H', b, p)[0]
    raw = bytes(b[p + 2:p + 2 + n])
    if len(raw) != n:
        raise ValueError('value past the record')
    return (raw.decode('utf-8') if t == STR else raw), p + 2 + n


def parse(data):
    s = Store()
    if len(data) < HDR or struct.unpack_from('<I', data)[0] != MAGIC or \
            zlib.crc32(data[:12]) != struct.unpack_from('<I', data, 12)[0]:
        s.error = 'no valid header'
        return s
    s.stamp = struct.unpack_from('<I', data, 8)[0]
    by_num = {}
    p = HDR
    while p < len(data):
        if len(data) - p < REC_HDR:
            s.error = 'a torn record'
            break
        kind, num, length, rid = struct.unpack_from('<BBHI', data, p)
        total = REC_HDR + length + 4
        if len(data) - p < total:
            s.error = 'a torn record'
            break
        if zlib.crc32(data[p:p + total - 4]) != struct.unpack_from('<I', data, p + total - 4)[0]:
            s.error = 'a record with a bad CRC'
            break
        b = data[p + REC_HDR:p + REC_HDR + length]
        if kind in (KV_SET, KV_DEL):
            key = b[1:1 + b[0]].decode('utf-8')
            if kind == KV_SET:
                s.keys[key] = _value(b, 2 + b[0], b[1 + b[0]])[0]
            else:
                s.keys.pop(key, None)
        elif kind == T_DEF:
            version, nf, index, next_id, nl = struct.unpack_from('<HBbIB', b)
            name = b[9:9 + nl].decode('utf-8')
            t = s.tables.get(name) or Table(name)
            q = 9 + nl
            t.fields = []
            for _ in range(nf):
                t.fields.append((b[q + 2:q + 2 + b[q + 1]].decode('utf-8'), b[q]))
                q += 2 + b[q + 1]
            t.version, t.index, t.next_id = version, index, max(t.next_id, next_id)
            s.tables[name] = t
            by_num[num] = t
        elif kind == T_DROP:
            t = by_num.pop(num, None)
            if t:
                s.tables.pop(t.name, None)
        elif kind == R_PUT and num in by_num:
            t = by_num[num]
            nf, q, vals = b[0], 1, []
            for _ in range(nf):
                v, q = _value(b, q + 1, b[q])
                vals.append(v)
            t.records[rid] = vals
        elif kind == R_DEL and num in by_num:
            by_num[num].records.pop(rid, None)
        s.last = p
        p += total
    s.end = p
    return s


def last_record(data):
    """Where the last good record starts"""
    return parse(data).last


def _q(text):
    out = ''
    for c in text:
        out += {'"': '\\"', '\\': '\\\\', '\n': '\\n', '\r': '\\r', '\t': '\\t'}.get(
            c, f'\\x{ord(c):02x}' if ord(c) < 0x20 or ord(c) == 0x7f else c)
    return f'"{out}"'


def _v(v):
    if isinstance(v, bool):
        return str(int(v))
    if isinstance(v, bytes):
        return 'x' + v.hex()
    if isinstance(v, str):
        return _q(v)
    return str(v)


def export_text(s):
    """The store as "db export" writes it (os/fs/db_text.c)."""
    lines = ['# B310E OS data store (os/fs/db_text.c)']
    for k in sorted(s.keys, key=lambda k: k.encode()):
        v = s.keys[k]
        t = 'bool' if isinstance(v, bool) else 'int' if isinstance(v, int) else \
            'blob' if isinstance(v, bytes) else 'str'
        lines.append(f'key {_q(k)} {t} {_v(v)}')
    for t in s.tables.values():
        lines.append(f'table {_q(t.name)} {t.version} {t.index} ' +
                     ' '.join(f'{_q(n)} {TYPES[ty]}' for n, ty in t.fields))
        for rid in sorted(t.records):
            lines.append(f'rec {_q(t.name)} {rid} ' + ' '.join(_v(v) for v in t.records[rid]))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    s = parse(open(a.file, 'rb').read())
    if a.json:
        print(json.dumps({'keys': {k: v.hex() if isinstance(v, bytes) else v for k, v in s.keys.items()},
                          'tables': {t.name: {'version': t.version, 'fields': t.fields,
                                              'records': {i: [x.hex() if isinstance(x, bytes) else x
                                                              for x in r] for i, r in t.records.items()}}
                                     for t in s.tables.values()},
                          'end': s.end, 'error': s.error}, indent=2, ensure_ascii=False))
    else:
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stdout.write(export_text(s))
    if s.error:
        print(f'# stops at byte {s.end}: {s.error}', file=sys.stderr)


if __name__ == '__main__':
    main()
