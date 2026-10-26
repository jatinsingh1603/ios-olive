#!/usr/bin/env python3
"""Check the olive shim/stub ramoops lines (olive/shim/plog.h).

  plog-check.py FILE [--text]

FILE is either /sys/fs/pstore/console-ramoops-0 as pulled from Lineage
(plain text) or a raw dump of the console zone (starts with "DBGC" +
start + size, e.g. QEMU pmemsave 0x9ff00000 0x80000). Every line the shim
and stub write ends in " *XX" (low byte of the sum of the bytes before
" *"); the olive DDR flips bits across the warm reset, so each line is
marked:  ok  = checksum matches,  BAD = it does not (do not trust digits
in it),  --- = no checksum (xnu's own output, or a raw dump line).
--text prints the payload text only (for grep).
"""
import re, struct, sys

LINE = re.compile(rb'^(.*) \*([0-9a-f]{2})$')


def load(path):
    data = open(path, 'rb').read()
    if data[:4] == b'DBGC':
        start, size = struct.unpack_from('<II', data, 4)
        body = data[12:12 + min(size, len(data) - 12)]
        return body, f'raw zone: start {start:#x} size {size:#x}'
    return data, 'text'


def main():
    path = next(a for a in sys.argv[1:] if not a.startswith('--'))
    body, kind = load(path)
    text_only = '--text' in sys.argv
    good = bad = plain = 0
    out = []
    for raw in body.split(b'\n'):
        if not raw:
            continue
        m = LINE.match(raw)
        if m:
            ok = (sum(m.group(1)) & 0xff) == int(m.group(2), 16)
            good += ok
            bad += not ok
            tag = 'ok ' if ok else 'BAD'
            txt = m.group(1)
        else:
            plain += 1
            tag, txt = '---', raw
        s = txt.decode('ascii', 'backslashreplace')
        out.append(s if text_only else f'{tag} {s}')
    print('\n'.join(out))
    if not text_only:
        print(f'# {path} ({kind}): {good} ok, {bad} BAD, {plain} without checksum', file=sys.stderr)


if __name__ == '__main__':
    main()
