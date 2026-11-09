"""The signed update manifest of a release (docs/updater.md): what the game's updater checks before it touches anything.

    python tools/make_update.py --zip build/release/TotKMP-Setup-0.1.1-windows.zip --out build/release
    python tools/make_update.py --verify build/release --zip build/release/TotKMP-Setup-0.1.1-windows.zip
    python tools/make_update.py --new-key C:/somewhere/outside/the/repo

--zip writes TotKMP-Update-windows.json and TotKMP-Update-windows.json.sig into --out. The manifest names the setup zip
(the update package: the updater downloads the same file a new player does) with its size and SHA-256, the release's
version (MP_VERSION inside the zip) and every file in the zip with its size and SHA-256. The .sig is the Ed25519
signature of the manifest's exact bytes, 128 hex digits. The updater (installer/updater/update.c) holds the public key
(installer/updater/key.h) and refuses a manifest whose signature, version, package or files do not match.

The signing key is 64 hex digits (the Ed25519 seed). It comes from --key FILE or, without it, from the environment
variable TOTKMP_UPDATE_KEY (the GitHub Actions secret of the same name, .github/workflows/sign-update.yml). It is never
printed, logged or written anywhere but by --new-key. Keep it outside the repository.

--new-key DIR writes DIR/update-signing-key.txt (the private seed; refuses to overwrite one) and
DIR/update-signing-key.pub.txt, and prints the public key, which goes into installer/updater/key.h.

--public-key HEX replaces key.h's key for a test build (tests/updater_test.py).

--verify DIR checks DIR's manifest and signature against key.h and, with --zip, the zip against the manifest: the same
checks the updater makes, so a release can be tried before it is published.

Needs the cryptography package (tools/requirements.txt, tools/requirements-py314.txt).
"""
import argparse
import hashlib
import json
import os
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

ROOT = Path(__file__).resolve().parent.parent
KEY_HEADER = ROOT / 'installer' / 'updater' / 'key.h'
MANIFEST = 'TotKMP-Update-windows.json'
SIGNATURE = MANIFEST + '.sig'
PRODUCT = 'totk-recomp-multiplayer'
KEY_ENV = 'TOTKMP_UPDATE_KEY'
VERSION = re.compile(r'^\d{1,6}(\.\d{1,6}){0,3}$')
# A path the updater accepts (update.c safe_rel_path): parts joined by /, none empty, "." or "..", none starting with a
# space or ending in a space or a dot, no control character and none of \ : * ? " < > |, no Windows device name
BAD = r'\x00-\x1f\x7f\\:*?"<>|/'
PART = re.compile(rf'^[^{BAD} ](?:[^{BAD}]*[^{BAD}. ])?$')
DEVICES = {'con', 'prn', 'aux', 'nul', *(f'com{i}' for i in range(10)), *(f'lpt{i}' for i in range(10))}
MAX_FILES = 200000
PUBLIC_KEY_OVERRIDE = None   # --public-key: a test build's key (tests/updater_test.py)


def safe_rel_path(path):
    parts = path.split('/')
    return all(PART.match(p) and p not in ('.', '..') and p.split('.')[0].rstrip(' ').lower() not in DEVICES
               for p in parts) and len(path.encode('utf-8')) < 400


def read_seed(key_file):
    text = Path(key_file).read_text(encoding='ascii') if key_file else os.environ.get(KEY_ENV, '')
    text = text.strip()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', text):
        sys.exit('make_update: the signing key must be 64 hex digits (--key FILE or the environment variable '
                 f'{KEY_ENV}); none was given or it has another form')
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(text))


def public_hex(private):
    return private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


def built_in_public_key():
    if PUBLIC_KEY_OVERRIDE:
        return PUBLIC_KEY_OVERRIDE
    m = re.search(r'UPDATE_PUBLIC_KEY_HEX\s+"([0-9a-f]{64})"', KEY_HEADER.read_text(encoding='utf-8'))
    if not m:
        sys.exit(f'make_update: no UPDATE_PUBLIC_KEY_HEX in {KEY_HEADER}')
    return m.group(1)


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def zip_files(zip_path):
    """[(path below the zip's one top folder, size, sha256)], the top folder; stops on anything the updater refuses."""
    files, root = [], None
    with zipfile.ZipFile(zip_path) as z:
        names = set()
        for info in z.infolist():
            name = info.filename
            if info.is_dir():
                continue
            top, _, rest = name.partition('/')
            if root is None:
                root = top
            if top != root or not rest or not safe_rel_path(name):
                sys.exit(f'make_update: {zip_path}: the entry {name!r} is outside one top folder or has a name the '
                         'updater refuses')
            key = rest.lower()
            if key in names:
                sys.exit(f'make_update: {zip_path}: {name!r} twice (names differ only in case)')
            names.add(key)
            h = hashlib.sha256()
            size = 0
            with z.open(info) as f:
                while chunk := f.read(1 << 20):
                    h.update(chunk)
                    size += len(chunk)
            files.append((rest, size, h.hexdigest()))
    if not files or len(files) > MAX_FILES:
        sys.exit(f'make_update: {zip_path}: {len(files)} files')
    return sorted(files), root


def manifest_for(zip_path):
    files, root = zip_files(zip_path)
    by_path = {p: (s, h) for p, s, h in files}

    def text(name):
        if name not in by_path:
            sys.exit(f'make_update: {zip_path}: no {root}/{name}')
        with zipfile.ZipFile(zip_path) as z:
            return z.read(f'{root}/{name}').decode('utf-8').strip()

    version = text('MP_VERSION')
    if not VERSION.match(version):
        sys.exit(f'make_update: MP_VERSION {version!r} is not numbers joined by dots')
    if 'bin/totk-update.exe' not in by_path:
        sys.exit(f'make_update: {zip_path}: no bin/totk-update.exe; the release could not update itself again')
    return {
        'format': 1,
        'product': PRODUCT,
        'platform': 'windows',
        'version': version,
        'base_version': text('VERSION').split('+')[0],
        'package': {'name': Path(zip_path).name, 'size': Path(zip_path).stat().st_size,
                    'sha256': file_sha256(zip_path)},
        'root': root,
        'files': [{'path': p, 'size': s, 'sha256': h} for p, s, h in files],
    }


def write(zip_path, out, key_file):
    private = read_seed(key_file)
    manifest = manifest_for(zip_path)
    data = (json.dumps(manifest, indent=1, sort_keys=True) + '\n').encode('utf-8')
    if public_hex(private) != built_in_public_key():
        sys.exit('make_update: the signing key does not belong to the public key in installer/updater/key.h; the '
                 'updater would refuse this manifest')
    out.mkdir(parents=True, exist_ok=True)
    (out / MANIFEST).write_bytes(data)
    (out / SIGNATURE).write_text(private.sign(data).hex() + '\n', encoding='ascii', newline='\n')
    print(f'{out / MANIFEST}: version {manifest["version"]}, {len(manifest["files"])} files, package '
          f'{manifest["package"]["name"]} ({manifest["package"]["size"]} bytes)')
    print(f'{out / SIGNATURE}: signed')


def verify(folder, zip_path):
    data = (folder / MANIFEST).read_bytes()
    sig = bytes.fromhex((folder / SIGNATURE).read_text(encoding='ascii').strip())
    Ed25519PublicKey.from_public_bytes(bytes.fromhex(built_in_public_key())).verify(sig, data)   # raises if bad
    manifest = json.loads(data)
    print(f'{folder / MANIFEST}: signature good, version {manifest["version"]}')
    if zip_path:
        package = manifest['package']
        if (zip_path.name != package['name'] or zip_path.stat().st_size != package['size']
                or file_sha256(zip_path) != package['sha256']):
            sys.exit(f'make_update: {zip_path} is not the package the manifest names')
        files, root = zip_files(zip_path)
        if root != manifest['root'] or [[f['path'], f['size'], f['sha256']] for f in manifest['files']] != \
                [list(f) for f in files]:
            sys.exit(f'make_update: the files in {zip_path} differ from the manifest')
        print(f'{zip_path}: matches the manifest')


def new_key(folder):
    folder.mkdir(parents=True, exist_ok=True)
    private_file = folder / 'update-signing-key.txt'
    if private_file.exists():
        sys.exit(f'make_update: {private_file} exists; it is kept')
    if ROOT in folder.resolve().parents or folder.resolve() == ROOT:
        sys.exit('make_update: the signing key never goes inside the repository')
    private = Ed25519PrivateKey.generate()
    seed = private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                 serialization.NoEncryption())
    fd = os.open(private_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='ascii') as f:
        f.write(seed.hex() + '\n')
    (folder / 'update-signing-key.pub.txt').write_text(public_hex(private) + '\n', encoding='ascii')
    print(f'private key: {private_file} (keep it secret, outside the repository)')
    print(f'public key:  {public_hex(private)}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--zip', type=Path, help='the setup zip (TotKMP-Setup-<version>-windows.zip)')
    ap.add_argument('--out', type=Path, help='folder for the manifest and its signature')
    ap.add_argument('--key', type=Path, help=f'the signing key file (default: the environment variable {KEY_ENV})')
    ap.add_argument('--verify', type=Path, metavar='DIR', help='check the manifest in DIR (and --zip against it)')
    ap.add_argument('--new-key', type=Path, metavar='DIR', help='make a new signing key in DIR')
    ap.add_argument('--public-key', help='check against this public key (hex) instead of key.h: for test builds')
    args = ap.parse_args()
    global PUBLIC_KEY_OVERRIDE
    PUBLIC_KEY_OVERRIDE = args.public_key
    if args.new_key:
        new_key(args.new_key)
    elif args.verify:
        verify(args.verify, args.zip)
    elif args.zip and args.out:
        write(args.zip, args.out, args.key)
    else:
        ap.error('give --zip and --out, --verify DIR or --new-key DIR')


if __name__ == '__main__':
    main()
