"""Make the key table of the saves add-on (docs/online-saves.md) from the user's own game files.

The game's GameData list (GameData/GameDataList.Product.*.byml.zs) names every save value by a 32-bit hash of its
name only. This tool reads the list, recovers names by hashing candidate strings found in the game's own files (whole
strings, and the prefixes of server/data/world-keys.toml joined to them), matches the names against the groups of that
table, and writes one line per key:

    <8 hex digits key hash>\\t<rule>\\t<kind>\\t<name or ->

server/sv_save.c `load_rules` reads the first two columns; the game (runtime/net/net_save.c) reads the first three.
Keys whose name was not recovered get the table's `default_unnamed` rule.

    .venv\\Scripts\\python tools\\world_keys.py --romfs build\\extracted\\v143\\romfs --out generated\\world-keys.tsv

The RomFS folder is the one tools/extract.py writes (index.json plus romfs.bin). Needs the zstandard package
(tools/requirements.txt). The output lives under generated/ and never goes into the repository.
"""
import argparse
import fnmatch
import json
import re
import struct
import sys
import time
import tomllib
from pathlib import Path

import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_content import GENERATED_MARKER  # noqa: E402

RULES = ('latest', 'forward', 'steps', 'owner', 'player')

# GameData list section -> the wire kind (online/proto/ow_wire.h OW_GD_*). Sections not named here are left out:
# the hooks never send their values. Bool64bitKey (64-bit hashes) stays in each game's save and is never sent live.
KINDS = {
    'Bool': 'bool', 'BoolArray': 'bool', 'Int': 's32', 'IntArray': 's32', 'Float': 'f32', 'FloatArray': 'f32',
    'Enum': 'u32', 'EnumArray': 'u32', 'Vector2': 'bin', 'Vector2Array': 'bin', 'Vector3': 'vec3',
    'Vector3Array': 'vec3', 'String16': 'str', 'String16Array': 'str', 'String32': 'str', 'String32Array': 'str',
    'String64': 'str', 'String64Array': 'str', 'Binary': 'bin', 'BinaryArray': 'bin', 'UInt': 'u32',
    'UIntArray': 'u32', 'Int64': 's64', 'Int64Array': 's64', 'UInt64': 'u64', 'UInt64Array': 'u64',
    'WString16': 'str', 'WString16Array': 'str', 'WString32': 'str', 'WString32Array': 'str', 'WString64': 'str',
    'WString64Array': 'str', 'Struct': 'bin', 'BoolExp': 'bin',
}

# RomFS folders whose files hold GameData names (AI and logic graphs, event flows, scene and actor data). Sound,
# textures, models and terrain are left out: they are large and hold none.
SCAN_DIRS = ('AI', 'AISchedule', 'AS', 'ActorSystem', 'Banc', 'Component', 'Cooking', 'Ecosystem', 'Event', 'Game',
             'GameBalance', 'GameData', 'GameParameter', 'Life', 'Logic', 'Pack', 'Preload', 'RSDB', 'Scene',
             'Sequence', 'System', 'UI', 'WorldMgr')
SKIP_SUFFIXES = ('.bntx', '.bfres', '.bfsha', '.bwav', '.bars', '.bfstm', '.bnvib', '.bkres', '.bphhb', '.bphsh',
                 '.bphsc', '.mp4', '.webm', '.bfarc', '.bffnt', '.bfttf', '.bfotf')
TOKEN = re.compile(rb'[A-Za-z_][A-Za-z0-9_.\-\[\]]{2,127}')


def murmur3(data, seed=0):
    """MurmurHash3 x86 32-bit: the hash the GameData list names its keys by."""
    c1, c2, m = 0xcc9e2d51, 0x1b873593, 0xffffffff
    h = seed
    n = len(data) & ~3
    for (k,) in struct.iter_unpack('<I', data[:n]):
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
        h = ((h << 13) | (h >> 19)) & m
        h = (h * 5 + 0xe6546b64) & m
    tail = data[n:]
    if tail:
        k = int.from_bytes(tail, 'little')
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85ebca6b) & m
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & m
    return h ^ (h >> 16)


class Romfs:
    """Files of an extracted RomFS image by path."""

    def __init__(self, folder):
        folder = Path(folder)
        manifest = json.loads((folder / 'index.json').read_text())
        self.files = {f['path']: f for f in manifest['files']}
        self.image = open(folder / 'romfs.bin', 'rb')

    def read(self, path):
        f = self.files[path]
        self.image.seek(f['offset'])
        return self.image.read(f['size'])


def sarc_files(data):
    """(name, bytes) of every file in a SARC archive."""
    if data[:4] != b'SARC':
        raise ValueError('not a SARC archive')
    header_len, = struct.unpack_from('<H', data, 4)
    data_start, = struct.unpack_from('<I', data, 12)
    pos = header_len
    if data[pos:pos + 4] != b'SFAT':
        raise ValueError('SARC without SFAT')
    sfat_len, count = struct.unpack_from('<HH', data, pos + 4)
    nodes = pos + sfat_len
    names_at = nodes + count * 16 + 8          # SFNT header follows the nodes
    for i in range(count):
        _, attrs, start, end = struct.unpack_from('<IIII', data, nodes + i * 16)
        name = ''
        if attrs & 0x01000000:
            at = names_at + (attrs & 0xffff) * 4
            name = data[at:data.index(b'\0', at)].decode('utf-8', 'replace')
        yield name, data[data_start + start:data_start + end]


class Unzstd:
    """zstd frames, with the game's dictionaries (Pack/ZsDic.pack.zs) for frames that name one."""

    def __init__(self, romfs):
        plain = zstandard.ZstdDecompressor()
        self.by_id = {0: plain}
        if 'Pack/ZsDic.pack.zs' in romfs.files:
            pack = plain.decompress(romfs.read('Pack/ZsDic.pack.zs'), max_output_size=1 << 28)
            for _, raw in sarc_files(pack):
                d = zstandard.ZstdCompressionDict(raw)
                self.by_id[d.dict_id()] = zstandard.ZstdDecompressor(dict_data=d)

    def __call__(self, data):
        dict_id = zstandard.get_frame_parameters(data).dict_id
        if dict_id not in self.by_id:
            raise ValueError(f'zstd frame wants dictionary {dict_id}, which ZsDic.pack.zs does not hold')
        return self.by_id[dict_id].stream_reader(data).read()


def byml(data):
    """A BYML document (version 2 to 7, either byte order) as Python values."""
    if data[:2] == b'YB':
        e = '<'
    elif data[:2] == b'BY':
        e = '>'
    else:
        raise ValueError('not a BYML document')
    u32 = struct.Struct(e + 'I')
    keys_at, strings_at, root_at = struct.unpack_from(e + 'III', data, 4)

    def word(at):
        return u32.unpack_from(data, at)[0]

    def head(at):
        """(type, count) of a container: a type byte and a 24-bit count in the file's byte order."""
        if e == '<':
            return data[at], int.from_bytes(data[at + 1:at + 4], 'little')
        return data[at], int.from_bytes(data[at + 1:at + 4], 'big')

    def table(at):
        if not at:
            return []
        _, count = head(at)
        offs = struct.unpack_from(f'{e}{count + 1}I', data, at + 4)
        return [data[at + o:data.index(b'\0', at + o)].decode('utf-8', 'replace') for o in offs[:count]]

    keys, strings = table(keys_at), table(strings_at)

    def value(kind, raw):
        if kind == 0xa0:
            return strings[raw]
        if kind == 0xa1:
            return data[raw + 4:raw + 4 + word(raw)]
        if kind in (0xc0, 0xc1):
            return node(raw)
        if kind == 0xd0:
            return bool(raw)
        if kind == 0xd1:
            return raw - (1 << 32) if raw & 0x80000000 else raw
        if kind == 0xd2:
            return struct.unpack('<f', struct.pack('<I', raw))[0]
        if kind == 0xd3:
            return raw
        if kind in (0xd4, 0xd5, 0xd6):
            return struct.unpack_from(e + {0xd4: 'q', 0xd5: 'Q', 0xd6: 'd'}[kind], data, raw)[0]
        if kind == 0xff:
            return None
        raise ValueError(f'BYML node type {kind:#x}')

    def node(at):
        kind, count = head(at)
        if kind == 0xc0:
            types = data[at + 4:at + 4 + count]
            vals = at + 4 + ((count + 3) & ~3)
            return [value(t, word(vals + 4 * i)) for i, t in enumerate(types)]
        if kind == 0xc1:
            out = {}
            for i in range(count):
                at_i = at + 4 + 8 * i
                if e == '<':
                    name, kind_i = int.from_bytes(data[at_i:at_i + 3], 'little'), data[at_i + 3]
                else:
                    name, kind_i = int.from_bytes(data[at_i:at_i + 3], 'big'), data[at_i + 3]
                out[keys[name]] = value(kind_i, word(at_i + 4))
            return out
        raise ValueError(f'BYML container type {kind:#x}')

    return node(root_at) if root_at else None


def gamedata_keys(romfs, unzstd):
    """{hash: kind} of every key in the GameData list (the newest Product list in the image)."""
    lists = sorted(p for p in romfs.files if re.fullmatch(r'GameData/GameDataList\.Product\.\d+\.byml\.zs', p))
    if not lists:
        sys.exit('world_keys: the RomFS holds no GameData/GameDataList.Product.*.byml.zs')
    path = max(lists, key=lambda p: int(p.split('.')[2]))
    doc = byml(unzstd(romfs.read(path)))
    keys = {}
    for section, entries in doc['Data'].items():
        kind = KINDS.get(section)
        if kind is None or not isinstance(entries, list):
            continue
        for e in entries:
            if isinstance(e, dict) and 'Hash' in e:
                keys.setdefault(e['Hash'] & 0xffffffff, kind)
    return path, keys


def byml_atoms(data, strings, numbers):
    """Adds the key and string tables of a BYML document to strings and its 64-bit integers to numbers."""
    e = '<' if data[:2] == b'YB' else '>'
    keys_at, strings_at, root_at = struct.unpack_from(e + 'III', data, 4)
    order = 'little' if e == '<' else 'big'
    for at in (keys_at, strings_at):
        if at:
            count = int.from_bytes(data[at + 1:at + 4], order)
            offs = struct.unpack_from(f'{e}{count + 1}I', data, at + 4)
            for o in offs[:count]:
                strings.add(data[at + o:data.index(b'\0', at + o)])
    todo, seen = [root_at] if root_at else [], set()
    while todo:
        at = todo.pop()
        if at in seen or at + 4 > len(data):
            continue
        seen.add(at)
        kind, count = data[at], int.from_bytes(data[at + 1:at + 4], order)
        if kind == 0xc0:
            types = data[at + 4:at + 4 + count]
            vals = at + 4 + ((count + 3) & ~3)
            pairs = ((t, vals + 4 * i) for i, t in enumerate(types))
        elif kind == 0xc1:
            pairs = ((data[at + 4 + 8 * i + (3 if e == '<' else 3)], at + 8 + 8 * i) for i in range(count))
        else:
            continue
        for t, where in pairs:
            raw = struct.unpack_from(e + 'I', data, where)[0]
            if t in (0xc0, 0xc1):
                todo.append(raw)
            elif t in (0xd4, 0xd5):
                numbers.add(struct.unpack_from(e + ('q' if t == 0xd4 else 'Q'), data, raw)[0])


# A plausible GameData name: an identifier, then dot- or dash-joined parts (array-like suffixes allowed).
NAME = re.compile(rb'[A-Za-z][A-Za-z0-9_]{2,}(?:[.\-][A-Za-z0-9_\[\]\-]+)*')


def candidate_strings(romfs, unzstd, exefs, log):
    """(whole, parts): strings that may be whole names, and the cleaner set that may follow a head such as 'IsGet.'.

    whole: identifier-like runs of bytes in the game's data files and its main module, plus everything in parts.
    parts: strings of parsed BYML documents, the stems of RomFS and archive file names, and the 64-bit integers of
    BYML documents in decimal (placed objects are named by their instance hash, as in a chest's key)."""
    raw, parts, numbers = set(), set(), set()

    def stem(name):
        base = name.rsplit('/', 1)[-1]
        parts.add(base.split('.', 1)[0].encode())

    def walk(name, data, depth=0):
        if name.endswith('.zs'):
            try:
                data = unzstd(data)
            except (ValueError, zstandard.ZstdError):
                return
            name = name[:-3]
        if data[:4] == b'SARC' and depth < 3:
            try:
                for inner, blob in sarc_files(data):
                    stem(inner)
                    walk(inner, blob, depth + 1)
                return
            except (ValueError, struct.error):
                pass
        if data[:2] in (b'YB', b'BY'):
            try:
                byml_atoms(data, parts, numbers)
                return
            except (ValueError, struct.error, IndexError):
                pass
        raw.update(TOKEN.findall(data))

    paths = [p for p in romfs.files if p.split('/')[0] in SCAN_DIRS and not p.endswith(SKIP_SUFFIXES)]
    t0 = time.time()
    for i, p in enumerate(paths):
        stem(p)
        walk(p, romfs.read(p))
        if log and i and i % 20000 == 0:
            log(f'  {i}/{len(paths)} files, {len(raw) + len(parts)} strings, {time.time() - t0:.0f} s')
    if exefs:
        main = Path(exefs) / 'main'
        if main.exists():
            raw.update(TOKEN.findall(main.read_bytes()))
    parts = {s for s in parts if NAME.fullmatch(s)}
    parts.update(str(n).encode() for n in numbers)
    whole = {s for s in raw if len(s) >= 6 and NAME.fullmatch(s)} | parts
    return whole, parts


def load_table(path):
    doc = tomllib.loads(Path(path).read_text(encoding='utf-8'))
    groups = []
    for g in doc['group']:
        if g['rule'] not in RULES:
            sys.exit(f'world_keys: group {g["name"]!r} has unknown rule {g["rule"]!r}')
        groups.append((g['name'], g['rule'], [re.compile(fnmatch.translate(p)) for p in g['patterns']],
                       g['patterns']))
    for d in ('default_named', 'default_unnamed'):
        if doc[d] not in RULES:
            sys.exit(f'world_keys: {d} is not a rule')
    return groups, doc['default_named'], doc['default_unnamed']


def prefixes(groups):
    """The literal heads of the table's patterns that end in a separator, such as 'IsGet.' or 'Npc_'."""
    out = set()
    for _, _, _, pats in groups:
        for p in pats:
            head = re.split(r'[*?\[]', p, 1)[0]
            if head and head != p and head[-1] in '._':
                out.add(head.encode())
    return sorted(out)


DECOY = 0x5bd1e995   # keys xor this make a decoy list: a hit on it measures how often a candidate matches by chance


def recover_names(keys, whole, parts, heads):
    """({hash: name}, decoy hits) for every key hash a whole candidate or head + part hashes to."""
    names, decoy = {}, 0
    fake = {k ^ DECOY for k in keys}

    def try_name(s):
        nonlocal decoy
        h = murmur3(s)
        if h in keys:
            names.setdefault(h, s.decode())
        elif h in fake:
            decoy += 1

    for s in whole:
        try_name(s)
    # Names such as 'IsGet.Weapon_Sword_001' join a head to an actor or place name; a part may itself carry the head
    # already, so strip any known head before joining.
    bare = set()
    for s in parts:
        if len(s) > 96:
            continue
        bare.add(s)
        for hd in heads:
            if s.startswith(hd):
                bare.add(s[len(hd):])
    for hd in heads:
        for s in bare:
            if s:
                try_name(hd + s)
    return names, decoy


def classify(name, groups, default_named, default_unnamed):
    if name is None:
        return default_unnamed, None
    for gname, rule, pats, _ in groups:
        if any(p.match(name) for p in pats):
            return rule, gname
    return default_named, None


def write_table(out, keys, names, groups, default_named, default_unnamed, source):
    rows = []
    counts = {}
    for h in sorted(keys):
        rule, group = classify(names.get(h), groups, default_named, default_unnamed)
        counts[group or ('(unnamed)' if h not in names else '(no group)')] = \
            counts.get(group or ('(unnamed)' if h not in names else '(no group)'), 0) + 1
        rows.append(f'{h:08x}\t{rule}\t{keys[h]}\t{names.get(h, "-")}\n')
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = (f'# {GENERATED_MARKER}: tools/world_keys.py from {source}. hash, rule, kind, name.\n' + ''.join(rows))
    out.write_text(text, encoding='utf-8', newline='\n')
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--romfs', required=True, help='extracted RomFS folder (index.json and romfs.bin)')
    ap.add_argument('--exefs', help='extracted ExeFS folder; its main module adds candidate names')
    ap.add_argument('--table', default=str(Path(__file__).resolve().parent.parent / 'server/data/world-keys.toml'))
    ap.add_argument('--out', default='generated/world-keys.tsv')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    log = None if a.quiet else print
    groups, default_named, default_unnamed = load_table(a.table)
    romfs = Romfs(a.romfs)
    unzstd = Unzstd(romfs)
    source, keys = gamedata_keys(romfs, unzstd)
    log and log(f'{source}: {len(keys)} keys')
    whole, parts = candidate_strings(romfs, unzstd, a.exefs, log)
    heads = prefixes(groups)
    log and log(f'{len(whole)} whole candidates, {len(parts)} parts after {len(heads)} heads')
    names, decoy = recover_names(keys, whole, parts, heads)
    counts = write_table(a.out, keys, names, groups, default_named, default_unnamed, source)
    if log:
        log(f'{len(names)} of {len(keys)} names recovered (the decoy list got {decoy} chance hits); wrote {a.out}')
        for g, n in sorted(counts.items(), key=lambda kv: -kv[1]):
            log(f'  {n:7d}  {g}')


if __name__ == '__main__':
    main()
