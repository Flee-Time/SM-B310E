#!/usr/bin/env python3
"""Make a word list for the OS's word completion (os/ui/words.c) from a plain
word-frequency list.

Input: one word a line, either "word count" (whitespace between; the count
decides the order) or just "word" (most frequent first). Words are put in
small letters as the OS does (Turkish and Azeri: I -> ı, İ -> i); words with
anything but letters the OS folds (Latin, Latin-1, Latin Extended-A/B, Cyrillic
U+0400-045F, Ґ ґ), with fewer than 2 letters or more than 31 bytes are left out.

Output, /b310e-os/words/<lang>.wl on the card (little-endian):

  header, 32 bytes   "B3WL", u16 version 1, u16 key bytes 8, u32 entries,
                     u32 chunks, u32 the chunk keys' offset, u32 the index's
                     offset (512-aligned), u32 words, the language (4 bytes)
  chunk keys         the first key of each index chunk, 8 bytes each: the OS
                     keeps these in RAM (8 bytes per 32 entries)
  index              16-byte entries sorted by key, 32 to a 512-byte chunk:
                     key (a prefix, UTF-8, 0-padded to 8 bytes), u32 the
                     bucket's offset, u16 its length, u8 flags, u8 0
  buckets            words by frequency, each a length byte and the bytes;
                     at most 512 bytes, never across a 512-byte boundary

A prefix with few words (512 bytes or less) gets one bucket with all of them
(flag bit 0) and no longer keys under it; a prefix with more gets a bucket of
its best 8 and a key for each next letter. A query for any prefix so reads
one index chunk and one bucket: two 512-byte reads at most, on the phone's
UI thread.

  python os/tools/mkwords.py --lang tr words-tr.txt -o sdcard/b310e-os/words/tr.wl

The lists are not shipped with the OS (see os/README.md, "Text input").

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
from pathlib import Path
import struct
import sys

KEY, ENTRY, CHUNK, BUCKET, TOP, WORD_MAX = 8, 16, 512, 512, 8, 31
DOTLESS = {'tr', 'az'}


def letter(c):
    o = ord(c)
    return (c.isalpha() and (o < 0x80 or 0xc0 <= o <= 0x24f or 0x400 <= o <= 0x45f or
                             o in (0x490, 0x491)) and o not in (0xd7, 0xf7))


def lower(word, lang):
    if lang in DOTLESS:
        word = word.replace('I', 'ı').replace('İ', 'i')
    return word.replace('İ', 'i').lower()


def read(path, lang, limit):
    counts, order = {}, []
    lines = Path(path).read_text(encoding='utf-8-sig').splitlines()
    for rank, line in enumerate(lines):
        parts = line.split()
        if not parts:
            continue
        w = lower(parts[0], lang)
        n = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else len(lines) - rank
        if len(w) < 2 or len(w.encode()) > WORD_MAX or not all(letter(c) for c in w):
            continue
        if w not in counts:
            order.append(w)
            counts[w] = 0
        counts[w] += n
    order.sort(key=lambda w: -counts[w])        # stable: ties keep the file's order
    return order[:limit]


def build(prefix, words, entries):
    """words: those starting with prefix (and longer), best first"""
    kb = prefix.encode()
    size = sum(1 + len(w.encode()) for w in words)
    children = {}
    for w in words:
        if len(w) > len(prefix):
            children.setdefault(w[len(prefix)], []).append(w)
    split = all(len((prefix + c).encode()) <= KEY for c in children)
    if size <= BUCKET or not split:
        bucket, used = [], 0
        for w in words:
            if used + 1 + len(w.encode()) > BUCKET:
                break
            bucket.append(w)
            used += 1 + len(w.encode())
        entries.append((kb, 1, bucket))
        return
    entries.append((kb, 0, [w for w in words if len(w) > len(prefix)][:TOP]))
    for c in sorted(children, key=lambda c: (prefix + c).encode()):
        build(prefix + c, children[c], entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path)
    parser.add_argument('--lang', required=True, help='the OS\'s code: en, tr, de, fr, es, ru')
    parser.add_argument('--max-words', type=int, default=50000)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()

    words = read(args.input, args.lang, args.max_words)
    firsts = {}
    for w in words:
        firsts.setdefault(w[0], []).append(w)
    entries = []
    for c in sorted(firsts, key=lambda c: c.encode()):
        build(c, firsts[c], entries)
    keys = [k.ljust(KEY, b'\0') for k, _, _ in entries]
    assert keys == sorted(keys) and len(set(keys)) == len(keys), 'keys out of order'

    chunks = (len(entries) + CHUNK // ENTRY - 1) // (CHUNK // ENTRY)
    keys_at = 32
    index_at = (keys_at + chunks * KEY + CHUNK - 1) // CHUNK * CHUNK
    data_at = index_at + chunks * CHUNK
    data, index = bytearray(), bytearray()
    for (kb, flags, bucket), key in zip(entries, keys):
        blob = b''.join(bytes([len(w.encode())]) + w.encode() for w in bucket)
        at = data_at + len(data)
        if at // CHUNK != (at + len(blob) - 1) // CHUNK:    # not across a sector
            data += bytes(CHUNK - at % CHUNK)
            at = data_at + len(data)
        data += blob
        index += key + struct.pack('<IHBB', at, len(blob), flags, 0)
    index += b'\xff' * (chunks * CHUNK - len(index))
    head = b'B3WL' + struct.pack('<HHIIIII', 1, KEY, len(entries), chunks, keys_at, index_at,
                                 len(words)) + args.lang.encode().ljust(4, b'\0')[:4]
    chunk_keys = b''.join(keys[i] for i in range(0, len(keys), CHUNK // ENTRY))
    out = head + chunk_keys
    out += bytes(index_at - len(out)) + index + data
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f'{args.output}: {len(words)} words, {len(entries)} keys in {chunks} chunks '
          f'({chunks * KEY} bytes in RAM), {len(out)} bytes', file=sys.stderr)


if __name__ == '__main__':
    main()
