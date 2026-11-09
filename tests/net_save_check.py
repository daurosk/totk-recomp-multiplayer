"""The game's side of saves and world state (runtime/net/net_save.c) against the real server, two clients, localhost.

    python tests/net_save_check.py [--keep]

Builds the server and tests/net_client_main.c (the client with the runtime replaced by shims) like
tests/net_client_check.py, gives both a small made-up key table (world-keys.tsv) and plays the GameData hooks through
the harness's gdwrite / gdput / gdtake / gdget commands:

- a shared quest step written by one game reaches the other; a chest and the rupee count (the player's own) do not
- a write the key's rule refuses (a Korok flag going back to false) comes back to its writer with the room's value
- a value the game already holds is not sent again
- the join snapshot: after a reconnect the game gets its own save and the room's world back before join_ready, and
  net_save_snapshot_done says so
- net_save_get / net_save_put / net_save_shared for features, and a put of a shared key refused

Each check prints ok or FAIL; the exit code is the number of failures. docs/online-saves.md describes the add-on.
"""
import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import build_server  # noqa: E402
import net_client_check as cc  # noqa: E402
import net_server_check as sc  # noqa: E402

free_port, Server = sc.free_port, sc.Server


def check(ok, what, seen=None):
    sc.check(ok, what if ok or seen is None else f'{what} (got {seen})')
BOOL, S32, U32 = 0, 1, 5

QUEST, KOROK, CHEST, RUPEES, SHRINE = 0x51000001, 0x4b000002, 0x43000003, 0x52000004, 0x53000005
TABLE = {QUEST: 'steps', KOROK: 'forward', CHEST: 'player', RUPEES: 'player', SHRINE: 'steps'}


def u32(v):
    return v.to_bytes(4, 'little').hex()


def take(cl, settle=0.6):
    """{(key, index): (kind, hex)} of what the game would take now, after a short wait; plus snapshot_done."""
    time.sleep(settle)
    cl.drain(0.05)
    cl.cmd('gdtake')
    e = cl.expect('gd')
    return {(k, i): (kind, h) for k, i, kind, h in e['list']}, e['snapshot_done']


def get(cl, key, index=0xFFFF):
    cl.cmd(f'gdget {key:08x} {index}')
    return cl.expect('gdget')


def run(exe_srv, exe_cl, tmp):
    keys = tmp / 'world-keys.tsv'
    keys.write_text('# made up for the check\n' + ''.join(f'{k:08x}\t{r}\n' for k, r in sorted(TABLE.items())))
    port = free_port()
    addr = f'127.0.0.1:{port}'
    settings = dict(max_players=8, open=True, hold_seconds=2, max_per_ip=64, max_pending_per_ip=16,
                    connects_per_minute=100000, new_accounts_per_hour=1000)
    (tmp / 'srv').mkdir()
    shutil.copy(keys, tmp / 'srv' / 'world-keys.tsv')
    srv = Server(exe_srv, tmp / 'srv', port, settings)
    env = {'TOTK_WORLD_KEYS': str(keys)}
    a = cc.Client(exe_cl, tmp / 'pa', env)
    b = cc.Client(exe_cl, tmp / 'pb', env)
    try:
        body(srv, a, b, addr)
    finally:
        if sc.FAILS:
            print('server log:\n  ' + '\n  '.join(srv.lines[-40:]))
        a.stop()
        b.stop()
        srv.stop()


def body(srv, a, b, addr):
    for cl, name in ((a, 'Ana'), (b, 'Ben')):
        e = cl.online(addr, name)
        check(e['state'] == cc.ONLINE, f'{name} joins')
    ga, done_a = take(a)
    _, done_b = take(b)
    check(done_a == 1 and done_b == 1, 'both games have their join snapshot')
    check(get(a, QUEST)['shared'] == 1 and get(a, CHEST)['shared'] == 0, 'the key table says shared and own')

    # ---- shared and own writes
    a.cmd(f'gdwrite {QUEST:08x} 65535 {U32} {u32(0xaaaa0001)}')
    a.cmd(f'gdwrite {CHEST:08x} 7 {BOOL} 01')
    a.cmd(f'gdwrite {RUPEES:08x} 65535 {S32} {u32(500)}')
    gb, _ = take(b, 1.0)
    check(gb.get((QUEST, 0xFFFF)) == (U32, u32(0xaaaa0001)), "Ana's quest step reaches Ben", gb)
    check((CHEST, 7) not in gb and (RUPEES, 0xFFFF) not in gb, "Ana's chest and rupees stay hers", gb)
    ga, _ = take(a, 0.2)
    check(not ga, 'the writer gets nothing back for a write the room took', ga)
    check(get(a, RUPEES)['hex'] == u32(500), 'net_save_get gives the game its own value')
    check(get(b, RUPEES)['found'] == 0, "the other game has no value of Ana's rupees")

    # ---- a refused write goes back to the room's value
    b.cmd(f'gdwrite {KOROK:08x} 3 {BOOL} 01')
    ga, _ = take(a, 1.0)
    check(ga.get((KOROK, 3)) == (BOOL, '01'), "Ben's Korok reaches Ana", ga)
    b.cmd(f'gdwrite {KOROK:08x} 3 {BOOL} 00')
    gb, _ = take(b, 1.0)
    check(gb.get((KOROK, 3)) == (BOOL, '01'), 'a Korok flag going back is refused and set again for its writer', gb)
    ga, _ = take(a, 0.2)
    check((KOROK, 3) not in ga, 'the refused write does not reach the other game', ga)

    # ---- a value the game holds is not sent again: Ana writes the quest step Ben already has, Ben sees nothing
    b.cmd(f'gdwrite {QUEST:08x} 65535 {U32} {u32(0xaaaa0001)}')
    ga, _ = take(a, 0.8)
    check((QUEST, 0xFFFF) not in ga, 'a value the game took from the room is not sent back', ga)

    # ---- steps: a quest step the key held before is refused
    a.cmd(f'gdwrite {QUEST:08x} 65535 {U32} {u32(0xaaaa0002)}')
    take(b, 1.0)
    b.cmd(f'gdwrite {QUEST:08x} 65535 {U32} {u32(0xaaaa0001)}')
    gb, _ = take(b, 1.0)
    check(gb.get((QUEST, 0xFFFF)) == (U32, u32(0xaaaa0002)), 'an earlier quest step is refused', gb)

    # ---- features
    a.cmd(f'gdput {RUPEES:08x} 65535 {S32} {u32(650)}')
    check(a.ret('gdput') == 1, 'net_save_put of an own key')
    a.cmd(f'gdput {QUEST:08x} 65535 {U32} {u32(1)}')
    check(a.ret('gdput') == 0, 'net_save_put of a shared key is refused')
    ga, _ = take(a, 0.6)
    check(ga.get((RUPEES, 0xFFFF)) == (S32, u32(650)), 'the game takes a put value at its next commit', ga)

    # ---- join snapshot after a reconnect
    a.cmd('disconnect')
    a.event('state', 5, lambda e: e['state'] == cc.OFFLINE)
    time.sleep(0.3)
    e = a.online(addr, 'Ana')
    check(e['state'] == cc.ONLINE, 'Ana joins again')
    ga, done = take(a, 0.5)
    check(done == 1, 'the snapshot is complete once the game is live')
    check(ga.get((RUPEES, 0xFFFF)) == (S32, u32(650)) and ga.get((CHEST, 7)) == (BOOL, '01'),
          "Ana's own save comes back on join", ga)
    check(ga.get((QUEST, 0xFFFF)) == (U32, u32(0xaaaa0002)) and ga.get((KOROK, 3)) == (BOOL, '01'),
          "the room's world comes with it", ga)
    gb, _ = take(b, 0.2)
    check((CHEST, 7) not in gb and (RUPEES, 0xFFFF) not in gb, "Ben never gets Ana's own values", gb)

    # ---- files on the server
    srv.cmd('stop')
    srv.p.wait(10)
    world = (srv.data / 'world.json').read_text()
    check(f'{QUEST:08x}' in world and f'{CHEST:08x}' not in world, 'world.json holds the shared keys only')
    saves = list((srv.data / 'accounts').glob('*/save.json'))
    check(any(f'{CHEST:08x}' in p.read_text() for p in saves), "an account's save.json holds its chest")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--keep', action='store_true', help='keep the data folders')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='net_save_check_'))
    try:
        exe_srv, _ = build_server.build(tmp / 'bin', debug=True, werror=True)
        exe_cl = cc.build_client(tmp / 'bin')
        t0 = time.time()
        run(exe_srv, exe_cl, tmp)
        print(f'{len(sc.FAILS)} failures in {time.time() - t0:.0f} s')
        for f in sc.FAILS:
            print('  ' + f)
    finally:
        if a.keep:
            print('data folders in', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return min(len(sc.FAILS), 100)


if __name__ == '__main__':
    sys.exit(main())
