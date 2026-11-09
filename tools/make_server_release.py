r"""Packs the multiplayer server for players who host it: no build tools needed on their side.

    python tools/make_server_release.py [--out build/server-release]

Builds the server (tools/build_server.py) and writes, for the system it runs on, one archive into the output folder:

    Windows  TotKMP-Server-0.1.0-windows.zip
    Linux    TotKMP-Server-0.1.0-linux.tar.gz

The version is MP_VERSION. The archive holds one folder, TotKMP-Server-<version>-<system>/:

    totk-server(.exe)     the server
    start-server.bat|sh   starts it with the data folder next to it
    README.md             docs/server.md, with a quick start on top
    server-data/server.toml   the default settings, written by the server itself on its first start
    VERSION.txt           the product, its version and the TotK Recomp release it follows
    LICENSE, THIRD_PARTY_NOTICES.md, licenses/   the licence of the project and of the code in the server

Each system builds its own archive (the CI workflow .github/workflows/release.yml runs this on a Linux and a Windows
runner). The server needs no game files and the archive holds none: the content check (tools/check_content.py) runs
over the folder before it is packed. The server is started once from the folder, on a free port, to read its default
settings; that run's key and data are thrown away. Standard library only.
"""
import argparse
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_server  # noqa: E402
import check_content  # noqa: E402

WIN = sys.platform == 'win32'
SYSTEM = 'windows' if WIN else 'linux'
EXE = '.exe' if WIN else ''

START_BAT = '''@echo off
rem Starts the TotK Recomp Multiplayer server. Its settings are in server-data\\server.toml (see README.md).
cd /d "%~dp0"
totk-server.exe --data server-data %*
pause
'''

START_SH = '''#!/bin/sh
# Starts the TotK Recomp Multiplayer server. Its settings are in server-data/server.toml (see README.md).
cd "$(dirname "$0")" || exit 1
exec ./totk-server --data server-data "$@"
'''

QUICK_START = '''# TotK Recomp Multiplayer server {mp} ({system})

This folder holds the built server. Start it and tell your players the address:

{start}

The first start makes the key and the data in `server-data`, and prints the address, the port (27120 by default) and
the server's key. Open that TCP port in your firewall and router for players outside your network ("Ports and
firewall" below). Settings are in `server-data/server.toml`; stop the server before you edit it.

Players enter `your-address:27120` in the Online tab of the game's settings menu (F1), or in the setup's "Server
address" box. Source code and releases: https://github.com/daurosk/totk-recomp-multiplayer. Questions: the Discord
at https://discord.gg/GwtRKAqng5.

The rest of this file is the server's manual. Its "Build" section is for building from source and is not needed
here.

---

'''


def mp_version():
    return (ROOT / 'MP_VERSION').read_text(encoding='utf-8').strip()


def tool_version():
    return (ROOT / 'VERSION').read_text(encoding='utf-8').strip()


def archive_name():
    return f"TotKMP-Server-{mp_version()}-{SYSTEM}.{'zip' if WIN else 'tar.gz'}"


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def default_settings(exe, work):
    """The server.toml the server writes on its first start, read by starting it once on a free local port."""
    data = work / 'first-run'
    shutil.rmtree(data, ignore_errors=True)
    p = subprocess.Popen([str(exe), '--data', str(data), '--port', str(free_port()), '--bind', '127.0.0.1'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.PIPE)
    toml = data / 'server.toml'
    try:
        for _ in range(100):
            if toml.is_file() and (data / 'server.key').is_file():
                break
            if p.poll() is not None:
                sys.exit(f'the server exited with {p.returncode} before it wrote its settings')
            time.sleep(0.1)
        else:
            sys.exit('the server wrote no server.toml within 10 s')
    finally:
        if p.poll() is None:
            p.kill()
        p.wait()
    return toml.read_bytes().replace(b'\r\n', b'\n')


def assemble(out_dir):
    """The folder to pack, replaced under out_dir."""
    exe = build_server.build(out_dir / 'build')[0]
    folder = out_dir / f'TotKMP-Server-{mp_version()}-{SYSTEM}'
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    shutil.copy2(exe, folder / ('totk-server' + EXE))
    if WIN:
        (folder / 'start-server.bat').write_bytes(START_BAT.replace('\n', '\r\n').encode())
    else:
        (folder / 'start-server.sh').write_text(START_SH, encoding='utf-8', newline='\n')
        (folder / 'start-server.sh').chmod(0o755)
    start = ('    start-server.bat' if WIN else '    ./start-server.sh')
    manual = (ROOT / 'docs' / 'server.md').read_text(encoding='utf-8')
    (folder / 'README.md').write_text(QUICK_START.format(mp=mp_version(), system=SYSTEM, start=start) + manual,
                                      encoding='utf-8', newline='\n')
    (folder / 'server-data').mkdir()
    (folder / 'server-data' / 'server.toml').write_bytes(default_settings(folder / ('totk-server' + EXE), out_dir))
    (folder / 'VERSION.txt').write_text(f'TotK Recomp Multiplayer Server {mp_version()}\n'
                                        f'follows TotK Recomp {tool_version().split("+")[0]}\n', encoding='utf-8')
    shutil.copy2(ROOT / 'LICENSE', folder / 'LICENSE')
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', folder / 'THIRD_PARTY_NOTICES.md')
    (folder / 'licenses').mkdir()
    shutil.copy2(ROOT / 'online' / 'third_party' / 'monocypher' / 'LICENCE.md', folder / 'licenses' / 'Monocypher-LICENCE.md')
    return folder


def check(folder):
    bad = [(p, why) for p in sorted(folder.rglob('*')) if p.is_file() for why in check_content.problems_for(p)]
    if bad:
        sys.exit('content check failed: ' + '; '.join(f'{p.name}: {why}' for p, why in bad))


def members(folder):
    """(archive path, file, is executable) for every file, sorted, so two builds of one tree pack alike."""
    for p in sorted(folder.rglob('*')):
        if p.is_file():
            yield (Path(folder.name) / p.relative_to(folder)).as_posix(), p, p.name in ('start-server.sh', 'totk-server')


def pack(folder, archive):
    archive.parent.mkdir(parents=True, exist_ok=True)
    if WIN:
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for arc, p, _ in members(folder):
                info = zipfile.ZipInfo(arc, (1980, 1, 1, 0, 0, 0))
                info.external_attr = 0o644 << 16
                z.writestr(info, p.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    else:
        with tarfile.open(archive, 'w:gz') as t:
            for arc, p, exec_ in members(folder):
                info = tarfile.TarInfo(arc)
                info.size, info.mode, info.mtime = p.stat().st_size, 0o755 if exec_ else 0o644, 0
                info.uname = info.gname = 'root'
                with open(p, 'rb') as f:
                    t.addfile(info, f)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'server-release',
                    help='folder for the work files and the archive (default build/server-release)')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    folder = assemble(out)
    check(folder)
    archive = out / archive_name()
    pack(folder, archive)
    print(f'{archive}: {archive.stat().st_size // 1024} KiB')


if __name__ == '__main__':
    main()
