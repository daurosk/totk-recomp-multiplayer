"""Builds and runs tests/sv_save_check.c: the server's saves and world state (server/sv_save.c) offline.

    python tests/sv_save_check.py

Uses the compiler and flags tools/build_server.py uses. Exit status 0 when every check passes."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_server  # noqa: E402


def main():
    srcs = [ROOT / 'tests' / 'sv_save_check.c', ROOT / 'server' / 'sv_save.c', ROOT / 'server' / 'sv_os.c']
    srcs += sorted(build_server.PROTO.glob('*.c')) + sorted(build_server.MONO.glob('*.c'))
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-I', str(build_server.PROTO),
             '-fsanitize=undefined', '-fsanitize-trap=undefined']
    if not build_server.WIN:
        flags += ['-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-pthread']
    libs = ['-lws2_32', '-lbcrypt'] if build_server.WIN else ['-pthread']
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ('sv_save_check' + ('.exe' if build_server.WIN else ''))
        p = subprocess.run(build_server.compiler() + flags + [str(s) for s in srcs] + ['-o', str(exe)] + libs,
                           capture_output=True, text=True)
        if p.returncode:
            print(p.stdout + p.stderr)
            return 1
        data = Path(tmp) / 'data'
        data.mkdir()
        return subprocess.run([str(exe), str(data)]).returncode


if __name__ == '__main__':
    sys.exit(main())
