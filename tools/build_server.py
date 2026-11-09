r"""Builds the multiplayer server and the test bot.

    python tools/build_server.py [--out build/server] [--debug] [--werror]

Writes totk-server and net_bot (with .exe on Windows) into the output folder. Uses $CC when set, otherwise MSYS2
UCRT64 Clang on Windows (C:\msys64\ucrt64\bin) and cc on other systems. Standard library only. docs/server.md
describes building by hand.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROTO = ROOT / 'online' / 'proto'
MONO = ROOT / 'online' / 'third_party' / 'monocypher'
SERVER = ROOT / 'server'
WIN = sys.platform == 'win32'


def compiler():
    cc = os.environ.get('CC')
    if cc:
        return cc.split()
    if WIN:
        ucrt = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        return [str(ucrt)] if ucrt.exists() else ['clang']
    return [shutil.which('cc') or shutil.which('clang') or 'gcc']


def env():
    e = dict(os.environ)
    if WIN:
        e['PATH'] = r'C:\msys64\ucrt64\bin;' + e.get('PATH', '')
    return e


def build(out_dir, debug=False, werror=False):
    """Compiles both programs into out_dir and returns (server, bot) paths. werror turns our warnings into errors:
    the tests ask for it, while a plain build only shows them, since a host's compiler may warn where ours does not."""
    out_dir = Path(out_dir)
    obj_dir = out_dir / 'obj'
    obj_dir.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11', '-Wall', '-Wextra', '-I', str(PROTO)]
    flags += ['-O1', '-g', '-fsanitize=undefined', '-fsanitize-trap=undefined'] if debug else ['-O2']
    if not WIN:
        flags += ['-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-pthread']
    libs = ['-lws2_32', '-lbcrypt'] if WIN else ['-pthread']
    shared = sorted(PROTO.glob('*.c')) + sorted(MONO.glob('*.c')) + [SERVER / 'sv_os.c']
    programs = {
        'totk-server': [SERVER / n for n in ('sv_main.c', 'sv_room.c', 'sv_store.c', 'sv_econ.c')],
        'net_bot': [ROOT / 'tools' / 'net_bot.c'],
    }
    objs = {}
    for src in shared + [s for v in programs.values() for s in v]:
        obj = obj_dir / f'{src.parent.name}-{src.stem}.o'
        # Monocypher is vendored unmodified: its warnings are not ours to fix, so only our code gets -Werror.
        f = flags + ['-Werror'] if werror and src.parent != MONO else flags
        p = subprocess.run(compiler() + f + ['-c', str(src), '-o', str(obj)], capture_output=True, text=True, env=env())
        if p.returncode:
            sys.exit(f'compile {src.relative_to(ROOT)} failed:\n{p.stderr}')
        if p.stderr.strip() and src.parent != MONO:
            print(p.stderr, file=sys.stderr)
        objs[src] = str(obj)
    exe = '.exe' if WIN else ''
    paths = []
    for name, srcs in programs.items():
        out = out_dir / (name + exe)
        cmd = compiler() + flags + [objs[s] for s in shared + srcs] + ['-o', str(out)] + libs
        p = subprocess.run(cmd, capture_output=True, text=True, env=env())
        if p.returncode:
            sys.exit(f'link {name} failed:\n{p.stderr}')
        paths.append(out)
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', default=str(ROOT / 'build' / 'server'))
    ap.add_argument('--debug', action='store_true', help='undefined-behaviour checks that trap, and symbols')
    ap.add_argument('--werror', action='store_true', help='fail on any warning in our code')
    a = ap.parse_args()
    for p in build(a.out, a.debug, a.werror):
        print(p)


if __name__ == '__main__':
    main()
