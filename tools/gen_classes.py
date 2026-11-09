"""Writes online/proto/ow_classes_data.h from online/proto/classes.txt.

    python tools/gen_classes.py [--check]

The class table is compiled into the game and the server as text (online/proto/ow_classes.c reads it), so neither
build needs a step of its own and both decide from the same rows. --check writes nothing and fails when the header
differs from the text; tests/net_proto_check.py runs it.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT = ROOT / 'online' / 'proto' / 'classes.txt'
HEADER = ROOT / 'online' / 'proto' / 'ow_classes_data.h'


def c_string(line):
    out = []
    for ch in line:
        if ch in '\\"':
            out.append('\\' + ch)
        elif ' ' <= ch <= '~':
            out.append(ch)
        else:
            raise SystemExit(f'classes.txt: only printable ASCII is allowed, found {ch!r}')
    return '"' + ''.join(out) + '\\n"'


def render():
    lines = TEXT.read_text(encoding='utf-8').splitlines()
    body = '\n'.join('    ' + c_string(x) for x in lines)
    return ('/* The class table as text, written by tools/gen_classes.py from online/proto/classes.txt: edit that file\n'
            ' * and run the tool, never this file. */\n'
            '#ifndef OW_CLASSES_DATA_H\n#define OW_CLASSES_DATA_H\n\n'
            '#define OW_CLASSES_TEXT \\\n' + ' \\\n'.join(body.split('\n')) + '\n\n#endif\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true', help='fail when the header differs from classes.txt')
    a = ap.parse_args()
    want = render()
    if a.check:
        have = HEADER.read_text(encoding='utf-8') if HEADER.exists() else ''
        if have != want:
            sys.exit('online/proto/ow_classes_data.h differs from classes.txt: run python tools/gen_classes.py')
        print('classes: ow_classes_data.h agrees with classes.txt')
        return
    HEADER.write_text(want, encoding='utf-8', newline='\n')
    print(HEADER)


if __name__ == '__main__':
    main()
