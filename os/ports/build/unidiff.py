"""Unified diffs (git's) for the ports' prepare.py: read one, apply some of its hunks.
Each hunk's old lines must be in the file once, or at the line it names if more than
once: nothing is applied by guesswork.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys


def read(text, strip=1):
    """{path: [hunk]}, a hunk being (old line, [old lines], [new lines]); a hunk ends
    where its header's counts say"""
    files, path = {}, None
    lines = text.replace('\r\n', '\n').split('\n')
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if line.startswith('+++ '):
            name = line[4:].split('\t')[0]
            path = name.split('/', strip)[strip] if name != '/dev/null' else None
            files.setdefault(path, [])
        elif line.startswith('@@') and path:
            old_range, new_range = line.split()[1:3]
            start, _, n_old = old_range[1:].partition(',')
            n_new = new_range[1:].partition(',')[2]
            n_old = int(n_old) if n_old else 1
            n_new = int(n_new) if n_new else 1
            old, new = [], []
            while (len(old) < n_old or len(new) < n_new) and i < len(lines):
                body = lines[i]
                i += 1
                if body.startswith('\\'):     # "\ No newline at end of file"
                    continue
                kind, body = (body[:1] or ' '), body[1:]
                if kind == ' ':
                    old.append(body)
                    new.append(body)
                elif kind == '-':
                    old.append(body)
                elif kind == '+':
                    new.append(body)
            files[path].append((int(start), old, new))
    return files


def apply(text, hunks, name='?'):
    """text with the hunks applied (its line ends as they were)"""
    crlf = '\r\n' in text
    lines = text.replace('\r\n', '\n').split('\n')
    shift = 0
    for start, old, new in hunks:
        found = [i for i in range(len(lines) - len(old) + 1) if lines[i:i + len(old)] == old]
        if not found:
            sys.exit(f'prepare.py: {name}: a hunk at line {start} does not apply')
        if len(found) > 1:
            want = start - 1 + shift
            found = [i for i in found if i == want]
            if len(found) != 1:
                sys.exit(f'prepare.py: {name}: a hunk at line {start} applies in more than one place')
        i = found[0]
        lines[i:i + len(old)] = new
        shift += len(new) - len(old)
    out = '\n'.join(lines)
    return out.replace('\n', '\r\n') if crlf else out
