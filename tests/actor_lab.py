"""The replay lab of the actors add-on: every case in tests/actor_cases/ against the server's library.

    python tests/actor_lab.py [CASE.txt ...]

Builds tests/actor_lab.c with server/sv_actors.c, server/sv_owner.c and online/proto, with undefined-behaviour checks
that trap, and runs the cases on a virtual clock (tests/actor_lab.c describes the case files). Each case replays a
situation the old port's owner rules were settled on and checks the owner changes and messages it must give. Uses $CC
when set, otherwise MSYS2 UCRT64 Clang on Windows and cc elsewhere. Standard library only.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIN = sys.platform == 'win32'


def compiler():
    cc = os.environ.get('CC')
    if cc:
        return cc.split()
    if WIN:
        ucrt = Path(r'C:\msys64\ucrt64\bin\clang.exe')
        return [str(ucrt)] if ucrt.exists() else ['clang']
    return [shutil.which('cc') or shutil.which('clang') or 'gcc']


def main():
    cases = [Path(a) for a in sys.argv[1:]] or sorted((ROOT / 'tests' / 'actor_cases').glob('*.txt'))
    env = dict(os.environ)
    if WIN:
        env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
    srcs = [ROOT / 'tests' / 'actor_lab.c', ROOT / 'server' / 'sv_actors.c', ROOT / 'server' / 'sv_owner.c',
            ROOT / 'online' / 'proto' / 'ow_classes.c', ROOT / 'online' / 'proto' / 'ow_codec.c']
    with tempfile.TemporaryDirectory(prefix='actor_lab_') as tmp:
        exe = Path(tmp) / ('actor_lab' + ('.exe' if WIN else ''))
        flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=undefined',
                 '-fsanitize-trap=undefined', '-I', str(ROOT / 'online' / 'proto')]
        p = subprocess.run(compiler() + flags + [str(s) for s in srcs] + ['-o', str(exe), '-lm'], capture_output=True,
                           text=True, env=env)
        if p.returncode:
            sys.exit(f'actor_lab: build failed:\n{p.stderr}')
        p = subprocess.run([str(exe)] + [str(c) for c in cases], text=True, env=env, timeout=1800)
    sys.exit(p.returncode)


if __name__ == '__main__':
    main()
