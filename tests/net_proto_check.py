"""The wire codec (online/proto/) against docs/online-wire.md, without the game.

    python tests/net_proto_check.py [--fuzz-seconds 600] [--jobs 4] [--seed N] [--per-type 60]

Described in docs/online-wire.md.

Compiles tests/net_proto_check.c with the codec and Monocypher twice with MSYS2 Clang: optimised, and with
undefined-behaviour checks that trap. Then:

1. The tables. The C tables (`list`) and sections 11 to 13 of docs/online-wire.md name the same messages, numbers,
   directions, stages, limits, smallest sizes, structs, components and fields in the same order, the doc's
   constants table agrees with ow_wire.h, and the tables match the fingerprint recorded for OW_VERSION.
2. The checks (`check`, both builds, a fixed seed and a random one): round trips of every message and component;
   every message through the stream reader in chunks of 1 to 7 bytes, plain and sealed, both ways; add-on types
   refused on a link without their feature; every cut of a
   payload refused and every cut of a stream waiting; flipped bytes refused or decoding canonically; every flipped bit
   of a sealed frame refused; headers over and under each type's limits, of the wrong direction or stage, and of every
   unknown type refused; bad values refused by encoder and decoder; the handshake, its forgeries and the version
   refusal; Ed25519 against RFC 8032.
3. The fuzz run (`fuzz`, the checked build, --jobs processes with their own seeds for --fuzz-seconds each):
   mutated payloads decode only to their own canonical bytes, mutated plain streams never crash the reader or pass a
   frame outside its limits, mutated sealed frames are never accepted. Inputs sit against a no-access page.

Exit 0 when everything passes.
"""
import argparse
import hashlib
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / 'docs' / 'online-wire.md'
PROTO = ROOT / 'online' / 'proto'
MONO = ROOT / 'online' / 'third_party' / 'monocypher'

# SHA-256 of `net_proto_check list` for each released protocol version. Within a version the tables never change
# (docs/online-wire.md, "Versions"): a change to them raises OW_VERSION and adds that version's line here.
TABLE_FINGERPRINTS = {
    '1': 'c9a91a9c25039a1f1526c2068978a9014bf3e3c1bc57c490a4a3c162b93b83d3',
}


def compiler():
    cc = os.environ.get('CC')
    if cc:
        return cc.split()
    ucrt = Path(r'C:\msys64\ucrt64\bin\clang.exe')
    return [str(ucrt)] if ucrt.exists() else ['clang']


def build(out, checked):
    sources = [ROOT / 'tests' / 'net_proto_check.c'] + sorted(PROTO.glob('*.c')) + sorted(MONO.glob('*.c'))
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(PROTO)]
    flags += ['-O1', '-g', '-fsanitize=undefined', '-fsanitize-trap=undefined'] if checked else ['-O2']
    libs = ['-lbcrypt'] if sys.platform == 'win32' else []
    env = dict(os.environ)
    if sys.platform == 'win32':
        env['PATH'] = r'C:\msys64\ucrt64\bin;' + env.get('PATH', '')
    # Monocypher is compiled without -Werror: it is vendored unmodified and its warnings are not ours to fix.
    objs = []
    for src in sources:
        obj = out.parent / f'{out.stem}-{src.stem}.o'
        f = [x for x in flags if x != '-Werror'] if src.parent == MONO else flags
        p = subprocess.run(compiler() + f + ['-c', str(src), '-o', str(obj)], capture_output=True, text=True, env=env)
        if p.returncode:
            sys.exit(f'compile {src.name} failed:\n{p.stderr}')
        objs.append(str(obj))
    p = subprocess.run(compiler() + flags + objs + ['-o', str(out)] + libs, capture_output=True, text=True, env=env)
    if p.returncode:
        sys.exit(f'link failed:\n{p.stderr}')
    return env


def run(exe, args, env, timeout):
    p = subprocess.run([str(exe)] + args, capture_output=True, text=True, env=env, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def parse_list(text):
    """The harness's `list`: constants, then messages, structs and components with their fields."""
    consts, items, cur = {}, [], None
    for line in text.splitlines():
        if line.startswith('  '):
            name, kind = line.split(maxsplit=1)
            cur['fields'].append((name, kind))
            continue
        w = line.split()
        if w[0] == 'message':
            cur = {'key': ('message', w[2]), 'meta': (w[1], w[3], w[4], int(w[5]), int(w[6]), w[7]), 'fields': []}
        elif w[0] == 'struct':
            cur = {'key': ('struct', w[1]), 'meta': (int(w[3]), int(w[2])), 'fields': []}
        elif w[0] == 'component':
            cur = {'key': ('component', w[2]), 'meta': (int(w[1]), int(w[4]), int(w[3])), 'fields': []}
        else:
            consts[w[0]] = w[1]
            continue
        items.append(cur)
    return consts, items


def parse_doc(text):
    items, cur, errors = [], None, []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r'#### (0x[0-9A-F]{4}) (\w+)$', line)
        s = re.match(r'#### struct (\w+)$', line)
        c = re.match(r'#### component (\d+) (\w+)$', line)
        if m or s or c:
            meta_line = next((x for x in lines[i + 1:i + 4] if x.strip()), '')
            if m:
                mm = re.match(r'(c2s|s2c|both) · (plain|sealed|any) · limit (\d+) · smallest (\d+) · (\w+)$',
                              meta_line)
                meta = mm and (m.group(1), mm.group(1), mm.group(2), int(mm.group(3)), int(mm.group(4)), mm.group(5))
                key = ('message', m.group(2))
            elif s:
                mm = re.match(r'size (\d+) to (\d+)$', meta_line)
                meta = mm and (int(mm.group(1)), int(mm.group(2)))
                key = ('struct', s.group(1))
            else:
                mm = re.match(r'body (\d+) to (\d+)$', meta_line)
                meta = mm and (int(c.group(1)), int(mm.group(1)), int(mm.group(2)))
                key = ('component', c.group(2))
            if not meta:
                errors.append(f'{line}: no size line under the heading')
            cur = {'key': key, 'meta': meta, 'fields': []}
            items.append(cur)
            continue
        if cur and line.startswith('| `'):
            cells = [x.strip() for x in line.strip('|').split('|')]
            cur['fields'].append((cells[0].strip('`'), cells[1].strip('`')))
        elif line.startswith('## '):
            cur = None
    return items, errors


def check_doc(list_text):
    consts, code = parse_list(list_text)
    doc_text = DOC.read_text(encoding='utf-8')
    doc, errors = parse_doc(doc_text)
    code_map = {i['key']: i for i in code}
    doc_map = {}
    for i in doc:
        if i['key'] in doc_map:
            errors.append(f'{i["key"]} twice in the doc')
        doc_map[i['key']] = i
    for k in code_map.keys() - doc_map.keys():
        errors.append(f'{k[0]} {k[1]} is in online/proto but not in docs/online-wire.md')
    for k in doc_map.keys() - code_map.keys():
        errors.append(f'{k[0]} {k[1]} is in docs/online-wire.md but not in online/proto')
    for k in code_map.keys() & doc_map.keys():
        a, b = code_map[k], doc_map[k]
        if a['meta'] != b['meta']:
            errors.append(f'{k[0]} {k[1]}: code says {a["meta"]}, doc says {b["meta"]}')
        if a['fields'] != b['fields']:
            errors.append(f'{k[0]} {k[1]}: fields differ\n  code: {a["fields"]}\n  doc:  {b["fields"]}')
    code_order = [i['key'] for i in code if i['key'][0] == 'message']
    doc_order = [i['key'] for i in doc if i['key'][0] == 'message']
    if code_order != doc_order and not errors:
        errors.append('messages are in another order in the doc')
    title = re.search(r'^# .*version (\d+)$', doc_text, re.M)
    if not title or title.group(1) != consts.get('version'):
        errors.append(f'doc title version {title and title.group(1)} is not OW_VERSION {consts.get("version")}')
    # The constants table against ow_wire.h.
    header = (PROTO / 'ow_wire.h').read_text(encoding='utf-8')
    defined = dict(re.findall(r'^#define (OW_[A-Z_]+) (\d+)\s*$', header, re.M))
    rows = re.findall(r'^\| `(OW_[A-Z_]+)` \| (\d+) \|', doc_text, re.M)
    if len(rows) < 10:
        errors.append('the doc\'s limits table is missing')
    for name, value in rows:
        if defined.get(name) != value:
            errors.append(f'doc limits table: {name} = {value}, ow_wire.h has {defined.get(name)}')
    version = consts.get('version')
    digest = hashlib.sha256(list_text.replace('\r\n', '\n').encode()).hexdigest()
    if version not in TABLE_FINGERPRINTS:
        errors.append(f'OW_VERSION {version} has no fingerprint: add {version!r}: {digest!r} to TABLE_FINGERPRINTS')
    elif TABLE_FINGERPRINTS[version] != digest:
        errors.append(f'the tables of version {version} changed (fingerprint {digest}); a released version never '
                      f'changes: raise OW_VERSION and the doc title, and add the new fingerprint')
    return errors, len(code_map)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--fuzz-seconds', type=float, default=600)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--seed', type=int, default=None, help='seed of the random check and the fuzz runs')
    ap.add_argument('--per-type', type=int, default=60, help='random messages per type in the check')
    args = ap.parse_args()
    seed = args.seed if args.seed is not None else random.SystemRandom().randrange(1, 2**62)
    ok = True
    with tempfile.TemporaryDirectory(prefix='net_proto_') as tmp:
        tmp = Path(tmp)
        fast, checked = tmp / 'npc.exe', tmp / 'npc_ub.exe'
        env = build(fast, False)
        build(checked, True)
        print('built: optimised and with trapping undefined-behaviour checks')

        rc, out, err = run(fast, ['list'], env, 60)
        if rc:
            sys.exit(f'list failed: {err}')
        errors, count = check_doc(out)
        for e in errors:
            print('FAIL doc:', e)
        ok &= not errors
        print(f'tables: {count} entries; docs/online-wire.md {"agrees" if not errors else "differs"}')

        # The class table of the actors add-on is compiled in as text: the header must be the text as it stands.
        p = subprocess.run([sys.executable, str(ROOT / 'tools' / 'gen_classes.py'), '--check'], capture_output=True,
                           text=True)
        print(p.stdout.strip() or p.stderr.strip())
        ok &= p.returncode == 0

        for exe, name in ((fast, 'optimised'), (checked, 'checked')):
            for s in (1, seed):
                rc, out, err = run(exe, ['check', str(s), str(args.per_type)], env, 1800)
                last = out.strip().splitlines()[-1] if out.strip() else ''
                passed = rc == 0 and last == 'check: passed'
                ok &= passed
                print(f'check ({name}, seed {s}): {"passed" if passed else f"FAILED, exit {rc}"}')
                if not passed:
                    print(out[-4000:], err[-2000:])

        if args.fuzz_seconds > 0:
            procs = []
            for j in range(args.jobs):
                s = (seed + 7919 * (j + 1)) % 2**62
                procs.append((s, subprocess.Popen([str(checked), 'fuzz', str(args.fuzz_seconds), str(s)], env=env,
                                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)))
            for s, p in procs:
                out, err = p.communicate(timeout=args.fuzz_seconds + 600)
                passed = p.returncode == 0 and 'FAIL' not in out
                ok &= passed
                print(f'fuzz seed {s}, {args.fuzz_seconds:g} s: {"clean" if passed else f"FAILED, exit {p.returncode}"}'
                      f'\n  {out.strip().splitlines()[-1] if out.strip() else err.strip()}')
                if not passed:
                    print(out[-4000:], err[-2000:])
    print('net_proto_check: passed' if ok else 'net_proto_check: FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
