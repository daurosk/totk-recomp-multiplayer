"""Offline lab for actor sync: builds tests/net_actors_lab.c with runtime/net/net_actors.c and the wire codec, and
tests/net_actors_link_lab.c with the link to the network thread (runtime/net/net_actors_link.c) too, and runs both.

    python tests/net_actors_check.py

The lab plays two invented games and a stub server in one process (docs/online-actors.md, "The lab"). It is built
twice, optimised and with the undefined-behaviour checks trapping. Each case prints ok or FAIL; the exit code is the
number of failures.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = [ROOT / 'runtime' / 'net' / 'net_actors.c', ROOT / 'online' / 'proto' / 'ow_codec.c',
        ROOT / 'online' / 'proto' / 'ow_classes.c']
LABS = (('core', [ROOT / 'tests' / 'net_actors_lab.c'] + CORE),
        ('link', [ROOT / 'tests' / 'net_actors_link_lab.c', ROOT / 'runtime' / 'net' / 'net_actors_link.c'] + CORE))


def compiler():
    cc = os.environ.get('CC')
    if cc:
        return cc.split()
    ucrt = Path(r'C:\msys64\ucrt64\bin\clang.exe')
    return [str(ucrt)] if ucrt.exists() else ['clang']


def main():
    env = dict(os.environ)
    if sys.platform == 'win32':
        env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for (lab, sources), (name, extra) in ((l, b) for l in LABS for b in (
                ('O2', ['-O2']), ('checked', ['-O1', '-g', '-fsanitize=undefined', '-fsanitize-trap=undefined']))):
            name = f'{lab}-{name}'
            exe = Path(tmp) / f'net_actors_lab-{name}.exe'
            cmd = compiler() + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'online' / 'proto'),
                                '-I', str(ROOT / 'runtime')] + extra
            p = subprocess.run(cmd + [str(s) for s in sources] + ['-o', str(exe), '-lm'], capture_output=True, text=True,
                               env=env)
            if p.returncode:
                sys.exit(f'build ({name}) failed:\n{p.stderr}')
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=120)
            print(f'-- {name}')
            print(r.stdout, end='')
            if r.returncode < 0 or r.returncode > 200:
                print(f'FAIL the lab stopped ({r.returncode}): {r.stderr}')
                failures += 1
            else:
                failures += r.returncode
    return failures


if __name__ == '__main__':
    sys.exit(main())
