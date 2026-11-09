"""End-to-end tests of the game's updater (installer/updater/update.c, docs/updater.md) on a fake install.

    .venv/Scripts/python tests/updater_test.py [--work DIR] [--shots DIR] [--only NAME[,NAME]] [--slot N]

Windows only; needs MSYS2 UCRT64 clang (C:\\msys64\\ucrt64\\bin), the cryptography and pillow packages and a second
monitor (the windows the tests open are moved there at once, then placed with place-window.ps1 when it exists).

It builds, with a throwaway Ed25519 key (never the release key, so no test manifest could ever pass a real
updater): totk-update.exe for 0.0.9 and 0.1.1, a fake totk.exe linked with the game's own runtime/update_start.c
(tests/updater_fake_game.c) and a fake setup that stands in for a release's python.exe (tests/updater_fake_setup.c).
A fake 0.0.9 install gets update_url pointing at a local HTTP stand-in for GitHub's release API and the release
files. Each scenario starts the fake game as a player would and drives the windows through their buttons:

    offline        no server: the game starts at once, no window
    check_off      update_check = 0: the API is not even asked
    later          the prompt appears (screenshot prompt.png); Later starts the old game, nothing changes
    skip           Skip this version writes update_skip; the next start asks GitHub but shows no prompt
    older          a release older than the install: no prompt; --apply on it is refused
    update         Update: the progress window (progress.png), the patch, the new game starts; settings, saves, mods,
                   the online identity and the shader cache are kept; the next start shows no prompt
    tampered_zip   one byte of the package changed after signing: refused, nothing changed, the old game starts
    tampered_file  a signed manifest whose file entry does not match the package: refused
    bad_signature  a flipped signature byte: refused
    wrong_key      a manifest signed by another key: refused
    fail_halfway   the setup fails after compiling: every file is put back, the old game starts with a message
    killed         the updater is killed while the setup runs: the next start restores the old version
    inputs         the setup asks for the game files: the updater runs it again with the remembered ones

"Nothing changed" is checked on the hashes of every file of the install folder outside update/ and the test's own
start log.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import make_update  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

UCRT = Path(r'C:\msys64\ucrt64\bin')
PLACE = Path(r'C:\Users\Caleb\Desktop\totk-recomp-multiplayer-tools\place-window.ps1')
OLD, NEW = '0.0.9', '0.1.1'
NOTES = ('# TotK Recomp Multiplayer 0.1.1 (TotK Recomp 0.3.13)\n\nThis release fixes the **chat** box and adds a few '
         'things to the player list.\n\n## What is new\n\n- The chat keeps the last 50 lines.\n- Faster connects.\n')

user32 = ctypes.WinDLL('user32', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32.SetProcessDPIAware()
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
BM_CLICK = 0x00F5
SWP_NOZORDER, SWP_NOACTIVATE, SWP_NOSIZE = 0x0004, 0x0010, 0x0001

failures = []


def check(cond, what):
    print(('ok    ' if cond else 'FAIL  ') + what, flush=True)
    if not cond:
        failures.append(what)


# ---- building -----------------------------------------------------------------------------------------------------

def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'{cmd[0]}: exit {r.returncode}\n{r.stdout}\n{r.stderr}')
    return r


def build(work, pub_hex):
    env = dict(os.environ, PATH=str(UCRT) + os.pathsep + os.environ['PATH'])
    out = work / 'bin'
    out.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'installer' / 'updater'
    mono = ROOT / 'online' / 'third_party' / 'monocypher'
    res = out / 'update_rc.o'
    run([UCRT / 'windres.exe', '-I', ROOT / 'runtime', '-I', src, '-O', 'coff', src / 'update.rc', '-o', res], env=env)
    for v in (OLD, NEW):
        run([UCRT / 'clang.exe', '-O2', '-static', '-mwindows', '-municode', '-Wall', f'-DTOTKMP_VERSION="{v}"',
             f'-DUPDATE_PUBLIC_KEY_HEX="{pub_hex}"', '-I', mono, src / 'update.c', src / 'json.c', mono / 'monocypher.c',
             mono / 'monocypher-ed25519.c', res, '-lwinhttp', '-lbcrypt', '-lcomctl32', '-lshell32',
             '-o', out / f'totk-update-{v}.exe'], env=env)
        run([UCRT / 'clang.exe', '-O1', '-static', '-mwindows', f'-DFAKE_VERSION="{v}"', '-I', ROOT / 'runtime',
             ROOT / 'tests' / 'updater_fake_game.c', ROOT / 'runtime' / 'update_start.c', '-o', out / f'totk-{v}.exe'],
            env=env)
    run([UCRT / 'clang.exe', '-O1', '-static', '-municode', ROOT / 'tests' / 'updater_fake_setup.c',
         '-o', out / 'fake-python.exe'], env=env)
    r = subprocess.run([str(out / f'totk-update-{NEW}.exe'), '--selftest'], capture_output=True, text=True)
    check(r.returncode == 0, 'totk-update.exe --selftest')
    if r.returncode:
        print(r.stdout)
    return out


# ---- releases -----------------------------------------------------------------------------------------------------

def make_package(work, bins, version, setup_conf='', name=None):
    """TotKMP-Setup-<version>-windows.zip with the layout of a release (one top folder, TotKMP-Setup)."""
    folder = work / f'pkg-src-{version}'
    shutil.rmtree(folder, ignore_errors=True)
    top = folder / 'TotKMP-Setup'
    for d in ('python', 'bin', 'payload', 'installer/text', 'llvm/bin', 'python/Lib'):
        (top / d).mkdir(parents=True, exist_ok=True)
    shutil.copy2(bins / 'fake-python.exe', top / 'python' / 'python.exe')
    shutil.copy2(bins / f'totk-update-{version if version in (OLD, NEW) else NEW}.exe', top / 'bin' / 'totk-update.exe')
    shutil.copy2(bins / f'totk-{NEW}.exe', top / 'payload' / 'totk.exe')
    shutil.copy2(ROOT / 'installer' / 'text' / 'en.json', top / 'installer' / 'text' / 'en.json')
    (top / 'MP_VERSION').write_text(version + '\n')
    (top / 'VERSION').write_text('0.3.13+abcdef0\n')
    (top / 'fake-setup.txt').write_text(setup_conf)
    (top / 'llvm' / 'bin' / 'clang-stand-in.txt').write_bytes(os.urandom(300000))
    (top / 'python' / 'Lib' / '(odd) name~1.py').write_text('# a file name with brackets and a tilde\n')
    zpath = work / 'release' / (name or f'TotKMP-Setup-{version}-windows.zip')
    zpath.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for p in sorted(top.rglob('*')):
            if p.is_file():
                z.write(p, (Path('TotKMP-Setup') / p.relative_to(top)).as_posix())
    return zpath


def sign(work, zpath, key, edit=None, sig_edit=None):
    """The manifest and signature of zpath into work/release, through tools/make_update.py. edit changes the manifest
    before it is signed; sig_edit changes the signature after."""
    keyfile = work / 'test-key.txt'
    seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    keyfile.write_text(seed.hex())
    out = zpath.parent
    make_update.PUBLIC_KEY_OVERRIDE = key.public_key().public_bytes(serialization.Encoding.Raw,
                                                                    serialization.PublicFormat.Raw).hex()
    make_update.write(zpath, out, keyfile)
    if edit:
        manifest = json.loads((out / make_update.MANIFEST).read_bytes())
        edit(manifest)
        data = (json.dumps(manifest, indent=1, sort_keys=True) + '\n').encode()
        (out / make_update.MANIFEST).write_bytes(data)
        (out / make_update.SIGNATURE).write_text(key.sign(data).hex() + '\n')
    if sig_edit:
        sig = bytearray.fromhex((out / make_update.SIGNATURE).read_text().strip())
        sig_edit(sig)
        (out / make_update.SIGNATURE).write_text(sig.hex() + '\n')


class Server:
    """GitHub's releases/latest and the release files, from work/release."""

    def __init__(self, work):
        self.work, self.tag, self.requests = work, NEW, []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                if self.path == '/api/releases/latest':
                    body = json.dumps(outer.release()).encode()
                    ctype = 'application/json'
                elif self.path.startswith('/files/'):
                    p = outer.work / 'release' / self.path[len('/files/'):]
                    if not p.is_file():
                        self.send_error(404)
                        return
                    body, ctype = p.read_bytes(), 'application/octet-stream'
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except OSError:
                    pass

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def release(self):
        assets = [{'name': p.name, 'size': p.stat().st_size, 'browser_download_url': f'{self.base}/files/{p.name}'}
                  for p in sorted((self.work / 'release').iterdir()) if p.is_file()]
        return {'tag_name': f'v{self.tag}', 'name': f'TotK Recomp Multiplayer {self.tag}', 'draft': False,
                'prerelease': False, 'html_url': f'https://github.com/daurosk/totk-recomp-multiplayer/releases/tag/v{self.tag}',
                'body': NOTES, 'assets': assets}

    @property
    def base(self):
        return f'http://127.0.0.1:{self.port}'

    def api_requests(self):
        return sum(1 for r in self.requests if r.startswith('/api/'))


# ---- the fake install ---------------------------------------------------------------------------------------------

def make_install(work, bins, name, url, extra_ini=''):
    d = work / name
    if d.exists():
        shutil.rmtree(d)
    for sub in ('profile/online', 'mods/a-mod', 'logs', 'shader-cache', 'romfs', 'exefs', 'build-cache', 'romfs-patches'):
        (d / sub).mkdir(parents=True)
    shutil.copy2(bins / f'totk-{OLD}.exe', d / 'totk.exe')
    shutil.copy2(bins / f'totk-update-{OLD}.exe', d / 'totk-update.exe')
    (d / 'totk-install.json').write_text(json.dumps({'format': 1, 'product': 'totk-recomp-multiplayer',
                                                     'release': OLD, 'steps': {}}))
    (d / 'totk.ini').write_text('[display]\nwindow_size = 960x540\nfullscreen = 0\n\n[update]\n'
                                f'update_url = {url}\n{extra_ini}')
    (d / 'pipeline-cache.bin').write_bytes(b'PIPELINE CACHE OF THE OLD VERSION' + bytes(1000))
    (d / 'romfs-index.bin').write_text('romfs index of the old version\n')
    (d / 'SDL3.dll').write_bytes(b'not really SDL')
    (d / 'profile' / 'save.bin').write_bytes(os.urandom(4096))
    (d / 'profile' / 'online' / 'identity.key').write_bytes(os.urandom(64))
    (d / 'mods' / 'a-mod' / 'readme.txt').write_text('a mod\n')
    (d / 'logs' / 'totk.log').write_text('old log\n')
    (d / 'shader-cache' / 'shader-cache.pack').write_bytes(os.urandom(20000))
    (d / 'shader-cache' / 'old.spv').write_bytes(os.urandom(500))
    (d / 'romfs' / 'romfs.bin').write_bytes(os.urandom(100000))
    (d / 'exefs' / 'main').write_bytes(os.urandom(5000))
    (d / 'build-cache' / 'rebuild.txt').write_text('python\n-c\nboot\nC:\\old-setup\n--rebuild\nhere\n')
    (d / 'romfs-patches' / 'manifest.tsv').write_text('patches of the old version\n')
    return d


def snapshot(d):
    out = {}
    for p in sorted(d.rglob('*')):
        rel = p.relative_to(d).as_posix()
        if rel.split('/')[0] in ('update',) or rel in ('game-starts.txt', 'game-log.txt'):
            continue
        if p.is_file():
            out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
        elif p.is_dir():
            out[rel + '/'] = 'dir'
    return out


def starts(d):
    p = d / 'game-starts.txt'
    return p.read_text().split('\n')[:-1] if p.is_file() else []


# ---- windows --------------------------------------------------------------------------------------------------------

def process_path(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return ''
    buf = ctypes.create_unicode_buffer(4096)
    n = wt.DWORD(4096)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value if ok else ''


def windows_under(folder):
    """[(hwnd, pid, title, class)] of the visible top-level windows whose process runs from below folder."""
    found = []
    folder = str(folder).lower()

    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if process_path(pid.value).lower().startswith(folder):
                t = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(hwnd, t, 512)
                c = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, c, 256)
                found.append((hwnd, pid.value, t.value, c.value))
        return True
    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found


class MONITORINFO(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('rcMonitor', wt.RECT), ('rcWork', wt.RECT), ('dwFlags', wt.DWORD)]


def second_monitor():
    rects = []
    MONITORENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)

    def cb(hmon, hdc, rect, _):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(mi)
        user32.GetMonitorInfoW(hmon, ctypes.byref(mi))
        if not mi.dwFlags & 1:
            rects.append(mi.rcWork)
        return True
    user32.EnumDisplayMonitors(0, 0, MONITORENUMPROC(cb), 0)
    return rects[0] if rects else None


placed = set()


def place(hwnd, pid, slot):
    """Moves a new window onto the second monitor at once, then lets place-window.ps1 put it in its slot."""
    if hwnd in placed:
        return
    placed.add(hwnd)
    mon = second_monitor()
    if mon:
        user32.SetWindowPos(hwnd, 0, mon.left + 20, mon.top + 20, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE)
    if PLACE.is_file():
        subprocess.run(['powershell', '-NoProfile', '-File', str(PLACE), '-ProcessId', str(pid), '-Slot', str(slot)],
                       capture_output=True, timeout=90)


def wait_window(folder, slot, want=None, timeout=60):
    """The first visible window of a process under folder for which want(title, class) is true; moved at once."""
    end = time.time() + timeout
    while time.time() < end:
        for hwnd, pid, title, cls in windows_under(folder):
            if want is None or want(title, cls):
                place(hwnd, pid, slot)
                return hwnd, pid, title, cls
        time.sleep(0.05)
    return None


def children(hwnd):
    out = []

    def cb(h, _):
        t = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(h, t, 512)
        c = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, c, 256)
        out.append((h, t.value, c.value))
        return True
    user32.EnumChildWindows(hwnd, WNDENUMPROC(cb), 0)
    return out


def text_of(hwnd):
    return '\n'.join(t for _, t, _ in children(hwnd) if t)


def click(hwnd, label):
    for h, t, c in children(hwnd):
        if c.lower() == 'button' and t.replace('&', '') == label:
            user32.PostMessageW(h, BM_CLICK, 0, 0)
            return True
    return False


def shot(hwnd, path):
    if not path:
        return
    # PrintWindow draws the window itself into a bitmap: another window over it on that monitor does not matter
    from PIL import Image
    gdi32 = ctypes.WinDLL('gdi32')
    gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
    gdi32.CreateCompatibleDC.restype = wt.HDC
    gdi32.SelectObject.argtypes = (wt.HDC, wt.HGDIOBJ)
    gdi32.GetBitmapBits.argtypes = (wt.HBITMAP, ctypes.c_long, ctypes.c_void_p)
    gdi32.CreateCompatibleDC.argtypes = (wt.HDC,)
    gdi32.CreateCompatibleBitmap.argtypes = (wt.HDC, ctypes.c_int, ctypes.c_int)
    gdi32.DeleteObject.argtypes = (wt.HGDIOBJ,)
    gdi32.DeleteDC.argtypes = (wt.HDC,)
    user32.ReleaseDC.argtypes = (wt.HWND, wt.HDC)
    user32.GetWindowDC.restype = wt.HDC
    user32.PrintWindow.argtypes = (wt.HWND, wt.HDC, wt.UINT)
    time.sleep(0.3)
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    wdc = user32.GetWindowDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)   # PW_RENDERFULLCONTENT
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetBitmapBits(bmp, w * h * 4, buf)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, wdc)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.frombuffer('RGBA', (w, h), buf.raw, 'raw', 'BGRA', 0, 1).convert('RGB').save(path)
    print(f'      screenshot {path}')


def start_game(d, env=None):
    return subprocess.Popen([str(d / 'totk.exe')], cwd=d, env=env)


def wait_exit(proc, timeout=60):
    try:
        proc.wait(timeout)
        return True
    except subprocess.TimeoutExpired:
        return False


def wait_until(cond, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.1)
    return False


def updater_running(d):
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq totk-update.exe', '/FO', 'CSV', '/NH'], capture_output=True,
                         text=True).stdout
    return any(process_path(int(line.split('","')[1])).lower().startswith(str(d).lower())
               for line in out.splitlines() if line.startswith('"'))


def no_updater_left(d, timeout=60):
    return wait_until(lambda: not updater_running(d), timeout)


# ---- scenarios ------------------------------------------------------------------------------------------------------

def prompt_window(d, slot):
    return wait_window(d, slot, lambda t, c: c == '#32770' and t == 'TotK Recomp Multiplayer', 30)


def answer_prompt(d, label, slot, shot_path=None):
    w = prompt_window(d, slot)
    check(w is not None, 'the update prompt appears')
    if not w:
        return False
    # the task dialog draws its text itself (no child windows hold it): the updater logs what it showed
    log = (d / 'update' / 'update.log').read_text(encoding='utf-8', errors='replace')
    text = log[log.rfind('check: prompt:'):]
    check(f'TotK Recomp Multiplayer {NEW} is out. Update now?' in text, 'the prompt names the new version')
    check('fixes the chat box' in text, "the prompt shows the release notes' first lines")
    shot(w[0], shot_path)
    check(click(w[0], label), f'the prompt has a {label} button')
    return True


def ok_message(d, slot, expect_words, shot_path=None):
    w = wait_window(d, slot, lambda t, c: c == '#32770' and t == 'TotK Recomp Multiplayer', 120)
    check(w is not None, 'a message box appears')
    if not w:
        return ''
    text = text_of(w[0])
    for word in expect_words:
        check(word in text, f'the message says "{word}"')
    shot(w[0], shot_path)
    click(w[0], 'OK')
    return text


def s_offline(work, bins, server, key, a):
    d = make_install(work, bins, 'offline', 'http://127.0.0.1:9/api/releases/latest')
    t0 = time.time()
    p = start_game(d)
    check(wait_exit(p, 30) and wait_until(lambda: starts(d) == [f'started {OLD}'], 10), 'offline: the game starts')
    check(time.time() - t0 < 8, f'offline: within the timeout ({time.time() - t0:.1f} s)')
    check(not windows_under(d), 'offline: no window')


def s_check_off(work, bins, server, key, a):
    d = make_install(work, bins, 'check_off', f'{server.base}/api/releases/latest', 'update_check = 0\n')
    n = server.api_requests()
    p = start_game(d)
    check(wait_exit(p, 30) and starts(d) == [f'started {OLD}'], 'check_off: the game starts')
    check(server.api_requests() == n, 'check_off: GitHub is not asked')


def s_later(work, bins, server, key, a):
    d = make_install(work, bins, 'later', f'{server.base}/api/releases/latest')
    before = snapshot(d)
    p = start_game(d)
    if answer_prompt(d, 'Later', a.slot, a.shots / 'prompt.png' if a.shots else None):
        check(wait_exit(p, 30) and wait_until(lambda: starts(d) == [f'started {OLD}'], 10), 'later: the old game starts')
    check(snapshot(d) == before, 'later: nothing in the install folder changed')


def s_skip(work, bins, server, key, a):
    d = make_install(work, bins, 'skip', f'{server.base}/api/releases/latest')
    p = start_game(d)
    if answer_prompt(d, 'Skip this version', a.slot):
        check(wait_exit(p, 30) and wait_until(lambda: starts(d) == [f'started {OLD}'], 10), 'skip: the old game starts')
    ini = (d / 'totk.ini').read_text()
    check(f'update_skip = {NEW}' in ini and 'window_size = 960x540' in ini, 'skip: update_skip is written, the rest kept')
    n = server.api_requests()
    p = start_game(d)
    check(wait_exit(p, 30) and starts(d)[-1:] == [f'started {OLD}'] and server.api_requests() == n + 1,
          'skip: the next start asks GitHub and shows no prompt')


def s_older(work, bins, server, key, a):
    shutil.rmtree(work / 'release')
    sign(work, make_package(work, bins, '0.0.8'), key)
    server.tag = '0.0.8'
    d = make_install(work, bins, 'older', f'{server.base}/api/releases/latest')
    before = snapshot(d)
    p = start_game(d)
    check(wait_exit(p, 30) and starts(d) == [f'started {OLD}'], 'older: no prompt for an older release')
    up = subprocess.Popen([str(d / 'totk-update.exe'), '--apply', str(d), '--pid', '0'])
    ok_message(d, a.slot, ['not newer than the installed version'])
    wait_exit(up, 30)
    check(snapshot(d) == before, 'older: --apply refused it, nothing changed')
    server.tag = NEW


def release_new(work, bins, key, conf='', **kw):
    shutil.rmtree(work / 'release', ignore_errors=True)
    z = make_package(work, bins, NEW, conf)
    sign(work, z, key, **kw)
    return z


def update_flow(d, a, shot_progress=None, env=None):
    p = start_game(d, env)
    if not answer_prompt(d, 'Update', a.slot):
        return None
    check(wait_exit(p, 30), 'the old game exits when Update is chosen')
    w = wait_window(d, a.slot, lambda t, c: c == 'TotKMPUpdate', 30)
    check(w is not None, 'the progress window appears')
    if w and shot_progress:
        wait_until(lambda: 'Step 5' in text_of(w[0]) or not user32.IsWindow(w[0]), 30)
        shot(w[0], shot_progress)
    return w


def s_update(work, bins, server, key, a):
    release_new(work, bins, key, 'delay_ms 400\n')
    d = make_install(work, bins, 'update', f'{server.base}/api/releases/latest')
    before = snapshot(d)
    update_flow(d, a, a.shots / 'progress.png' if a.shots else None)
    check(wait_until(lambda: starts(d)[-1:] == [f'started {NEW}'], 120), 'update: the new game starts by itself')
    no_updater_left(d)
    after = snapshot(d)
    for kept in ('profile/save.bin', 'profile/online/identity.key', 'mods/a-mod/readme.txt', 'totk.ini',
                 'shader-cache/shader-cache.pack', 'shader-cache/old.spv', 'romfs/romfs.bin', 'exefs/main', 'logs/totk.log'):
        check(after.get(kept) == before.get(kept), f'update: {kept} kept')
    check(after.get('totk.exe') != before.get('totk.exe'), 'update: totk.exe replaced')
    check(json.loads((d / 'totk-install.json').read_text())['release'] == NEW, 'update: totk-install.json says 0.1.1')
    check(not (d / 'update' / 'backup').exists() and not (d / 'update' / 'journal.txt').exists(),
          'update: the backup and the journal are gone')
    setup = d / 'update' / f'setup-{NEW}'
    check((setup / 'python' / 'python.exe').is_file(), 'update: the new setup is kept for the mods\' rebuilds')
    check(str(setup) in (d / 'build-cache' / 'rebuild.txt').read_text(), 'update: rebuild.txt names the new setup')
    attrs = kernel32.GetFileAttributesW(str(setup / 'temp'))
    check(attrs != 0xFFFFFFFF and attrs & 0x400, 'update: the setup\'s temp folder is a junction to update/temp')
    args = (setup / 'fake-setup-args.txt').read_text()
    check('--dest' in args and '--base' not in args, 'update: the setup ran with the install folder only')
    n = server.api_requests()
    p = start_game(d)
    check(wait_exit(p, 30) and starts(d)[-1:] == [f'started {NEW}'] and server.api_requests() == n + 1,
          'update: the next start finds nothing newer')


def refused(name, work, bins, server, key, a, why, **kw):
    release_new(work, bins, key, **{k: v for k, v in kw.items() if k in ('edit', 'sig_edit')})
    if 'zip_edit' in kw:
        kw['zip_edit'](work / 'release' / f'TotKMP-Setup-{NEW}-windows.zip')
    d = make_install(work, bins, name, f'{server.base}/api/releases/latest')
    before = snapshot(d)
    p = start_game(d)
    if answer_prompt(d, 'Update', a.slot):
        wait_exit(p, 30)
        ok_message(d, a.slot, why + ['Nothing was changed'], kw.get('shot'))
    check(wait_until(lambda: starts(d)[-1:] == [f'started {OLD}'], 60), f'{name}: the old game starts')
    no_updater_left(d)
    check(snapshot(d) == before, f'{name}: nothing in the install folder changed')
    check(not list((d / 'update').glob('setup-*')), f'{name}: no setup was kept')


def flip_zip_byte(z):
    data = bytearray(z.read_bytes())
    data[len(data) // 2] ^= 0x40
    z.write_bytes(data)


def bad_entry(m):
    m['files'][0]['sha256'] = '0' * 64


def s_tampered_zip(work, bins, server, key, a):
    refused('tampered_zip', work, bins, server, key, a, ['does not match the signed manifest'], zip_edit=flip_zip_byte,
            shot=a.shots / 'refused.png' if a.shots else None)


def s_tampered_file(work, bins, server, key, a):
    refused('tampered_file', work, bins, server, key, a, ['does not match the manifest'], edit=bad_entry)


def s_bad_signature(work, bins, server, key, a):
    refused('bad_signature', work, bins, server, key, a, ['signature is not valid'],
            sig_edit=lambda s: s.__setitem__(5, s[5] ^ 1))


def s_wrong_key(work, bins, server, key, a):
    refused('wrong_key', work, bins, server, Ed25519PrivateKey.generate(), a, ['signature is not valid'])


def s_fail_halfway(work, bins, server, key, a):
    release_new(work, bins, key, 'delay_ms 100\nfail_after prepare\n')
    d = make_install(work, bins, 'fail_halfway', f'{server.base}/api/releases/latest')
    before = snapshot(d)
    if update_flow(d, a):
        ok_message(d, a.slot, ['failed', 'Compiling totk.exe failed (exit code 3)', 'previous version was put back'],
                   a.shots / 'rolled-back.png' if a.shots else None)
    check(wait_until(lambda: starts(d)[-1:] == [f'started {OLD}'], 60), 'fail_halfway: the old game starts')
    no_updater_left(d)
    after = snapshot(d)
    check(after == before, 'fail_halfway: every file is as before the update' +
          ('' if after == before else f' (differ: {sorted(set(after.items()) ^ set(before.items()))[:6]})'))


def s_killed(work, bins, server, key, a):
    release_new(work, bins, key, 'delay_ms 100\nhang_after prepare\n')
    d = make_install(work, bins, 'killed', f'{server.base}/api/releases/latest')
    before = snapshot(d)
    w = update_flow(d, a)
    if not w:
        return
    hung = wait_until(lambda: (d / 'pipeline-cache.bin').read_bytes().startswith(b'PIPELINE CACHE OF THE NEW'), 60)
    check(hung, 'killed: the setup changed the install folder before it hung')
    subprocess.run(['taskkill', '/F', '/PID', str(w[1])], capture_output=True)
    check(wait_until(lambda: not updater_running(d), 30), 'killed: the updater is gone')
    time.sleep(1)
    check((d / 'update' / 'journal.txt').read_text().strip() == 'patching', 'killed: the journal says patching')
    p = start_game(d)
    ok_message(d, a.slot, ['did not finish', 'previous version was put back'])
    if answer_prompt(d, 'Later', a.slot):   # 0.1.1 is still newer than what is installed again
        check(wait_exit(p, 30) and wait_until(lambda: starts(d)[-1:] == [f'started {OLD}'], 10),
              'killed: the old game starts')
    after = snapshot(d)
    check(after == before, 'killed: every file is as before the update' +
          ('' if after == before else f' (differ: {sorted(set(after.items()) ^ set(before.items()))[:6]})'))


def s_inputs(work, bins, server, key, a):
    release_new(work, bins, key, 'delay_ms 50\nneed_inputs 1\n')
    d = make_install(work, bins, 'inputs', f'{server.base}/api/releases/latest')
    files = work / 'game-files'
    files.mkdir(exist_ok=True)
    for n in ('base.xci', 'update.nsp', 'prod.keys'):
        (files / n).write_text('stand-in')
    recent = work / 'setup.json'
    recent.write_text(json.dumps({'format': 1, 'dest': str(d), 'base': str(files / 'base.xci'),
                                  'update': str(files / 'update.nsp'), 'keys': str(files / 'prod.keys')}))
    update_flow(d, a, env=dict(os.environ, TOTK_SETUP_RECENT=str(recent)))
    check(wait_until(lambda: starts(d)[-1:] == [f'started {NEW}'], 120), 'inputs: the new game starts')
    no_updater_left(d)
    args = (d / 'update' / f'setup-{NEW}' / 'fake-setup-args.txt').read_text().splitlines()
    check(len(args) == 2 and '--base' not in args[0] and str(files / 'base.xci') in args[1] and '--keys' in args[1],
          'inputs: the second run got the remembered game files')


SCENARIOS = ['offline', 'check_off', 'later', 'skip', 'older', 'update', 'tampered_zip', 'tampered_file',
             'bad_signature', 'wrong_key', 'fail_halfway', 'killed', 'inputs']


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--work', type=Path, default=ROOT / 'build' / 'updater-test')
    ap.add_argument('--shots', type=Path)
    ap.add_argument('--only')
    ap.add_argument('--slot', type=int, default=0)
    a = ap.parse_args()
    work = a.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    key = Ed25519PrivateKey.generate()
    pub = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()
    bins = build(work, pub)
    server = Server(work)
    release_new(work, bins, key)
    for name in (a.only.split(',') if a.only else SCENARIOS):
        print(f'-- {name}', flush=True)
        if name != 'older':
            release_new(work, bins, key)
        globals()[f's_{name}'](work, bins, server, key, a)
    print(f'{len(failures)} failed' if failures else 'all passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
