"""The actors add-on through the real server: bots over TCP, sealed, with the add-on granted.

    python tests/actor_server_check.py [--release]

tests/actor_lab.py checks the owner rules on a virtual clock; this checks that the server built by
tools/build_server.py carries them end to end: the grant, offers and the owner message, the relay of states within
interest, hits to the owner, spawns, take-once, holds, an unshared name, a stale epoch, a game without the add-on, and
the hand-over when an owner leaves. Uses the harness of tests/net_server_check.py (Server, Bots).
"""
import argparse
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_server_check as h  # noqa: E402

ACTORS = 4
PLAYING = 1
CLASS_ENEMY, CLASS_ITEM = 2, 6
OWNER_OFFER, OWNER_LEFT = 0, 4
GONE_REMOVED = 0
HOLD_ULTRAHAND, HOLD_CARRY = 1, 2


def boko(serial=501):
    return dict(**{'id.origin': 0, 'id.serial': serial})


def body(bots):
    a = bots.connect('alice', 'actAlice', 'Alice', features=ACTORS)
    h.check(a.get('ev') == 'admit' and a.get('features') == ACTORS, 'the actors add-on is granted when offered')
    b = bots.connect('bob', 'actBob', 'Bob', features=ACTORS)
    c = bots.connect('carol', 'actCarol', 'Carol', features=0)
    h.check(c.get('ev') == 'admit' and c.get('features') == 0, 'a game that does not offer it is not granted it')
    ALICE, BOB = a['account'], b['account']
    for name, pos in (('alice', (10, 0, 0)), ('bob', (-20, 0, 0)), ('carol', (0, 0, 5))):
        bots.send(name, 'area', **{'where.scene': 1, 'where.state': PLAYING})
        bots.send(name, 'position', scene=1, pos=pos, yaw=0)
    bots.sync()
    bots.drain()

    # ---- an offer gives the first owner, and every game in interest hears it ----
    bots.send('alice', 'offer', class_=CLASS_ENEMY, name='Enemy_Bokoblin_Junior', pos=(0, 0, 0), **boko())
    o = bots.expect('alice', 'owner')
    h.check(o is not None and o['owner'] == ALICE and o['cause'] == OWNER_OFFER and o['epoch'] > 0,
            'the first offer within 120 m makes its game the owner')
    epoch = o['epoch'] if o else 0
    # The owner keeps sending the actor's state, as a game must (rule 9 takes it from a silent owner).
    keep_state = ('repeat alice 200 actor_send time_us=1000 items.0.id.origin=0 items.0.id.serial=501 '
                  f'items.0.epoch={epoch} items.0.body.pos=1,0,0 items.0.body.rot=0,0,0,1')
    ob = bots.expect('bob', 'owner')
    h.check(ob is not None and ob['owner'] == ALICE and ob['epoch'] == epoch and ob['name'] == 'Enemy_Bokoblin_Junior',
            'another game in interest gets the owner message with the name to make its copy')
    h.check(bots.none('carol', 'owner', 1.0), 'a game without the add-on gets nothing of it')
    bots.send('bob', 'offer', class_=CLASS_ENEMY, name='Enemy_Bokoblin_Junior', pos=(0, 0, 0), **boko())
    h.check(bots.none('bob', 'owner', 1.5), 'a second offer while the owner is near changes nothing')

    # ---- states go to the games in interest under the owner's epoch only ----
    bots.send('alice', 'actor_send', time_us=1000, **{'items.0.id.origin': 0, 'items.0.id.serial': 501,
              'items.0.epoch': epoch, 'items.0.body.pos': (1, 0, 0), 'items.0.body.rot': (0, 0, 0, 1)})
    bots.cmd(keep_state)
    r = bots.expect('bob', 'actor_recv')
    h.check(r is not None and r['source'] == ALICE and len(r['items']) == 1 and r['items'][0]['epoch'] == epoch,
            "the owner's state reaches the other game in interest, with its source")
    bots.send('bob', 'actor_send', time_us=1000, **{'items.0.id.origin': 0, 'items.0.id.serial': 501,
              'items.0.epoch': epoch, 'items.0.body.pos': (5, 0, 0), 'items.0.body.rot': (0, 0, 0, 1)})
    h.check(bots.none('alice', 'actor_recv', 1.0), "a state from a game that is not the owner is dropped")
    bots.drain()
    bots.send('alice', 'actor_send', time_us=1000, **{'items.0.id.origin': 0, 'items.0.id.serial': 501,
              'items.0.epoch': epoch - 1, 'items.0.body.pos': (1, 0, 0), 'items.0.body.rot': (0, 0, 0, 1)})
    h.check(bots.none('bob', 'actor_recv', 1.0, pred=lambda e: e['items'][0]['epoch'] != epoch),
            'a state under an old epoch is dropped')

    # ---- hits go to the owner ----
    bots.send('bob', 'hit', **{'info.target.origin': 0, 'info.target.serial': 501, 'info.epoch': epoch,
              'info.amount': 7, 'info.at': (1, 0, 0)})
    hi = bots.expect('alice', 'hit_in')
    h.check(hi is not None and hi['attacker'] == BOB and hi['info']['amount'] == 7, 'a hit on a copy reaches the owner')
    bots.send('bob', 'hit', **{'info.target.origin': 0, 'info.target.serial': 501, 'info.epoch': epoch + 7,
              'info.amount': 7})
    h.check(bots.none('alice', 'hit_in', 1.0), 'a hit under another epoch is dropped')

    # ---- an unshared name is ignored ----
    bots.send('bob', 'offer', class_=CLASS_ITEM, name='Armor_001_Head', pos=(0, 0, 0), **boko(9))
    stray = bots.expect('bob', 'owner', 1.0)
    h.check(stray is None, 'an offer of a name the class table keeps local is ignored' +
            (f' (got owner of {stray["id"]} cause {stray["cause"]} owner {stray["owner"]})' if stray else ''))

    # ---- holds ----
    bots.send('alice', 'hold', what=HOLD_CARRY, on=1, **boko())
    bots.send('bob', 'hold', what=HOLD_ULTRAHAND, on=1, **boko())
    rb = bots.expect('bob', 'owner')
    h.check(rb is not None and rb['owner'] == ALICE, 'a grab of what another player holds is answered with its holder')
    bots.send('alice', 'hold', what=HOLD_CARRY, on=0, **boko())

    # ---- spawns and take-once ----
    bots.send('alice', 'spawn', **{'info.id.origin': ALICE, 'info.id.serial': 77, 'info.class_': CLASS_ITEM,
              'info.name': 'Item_Apple', 'info.body.pos': (2, 0, 0), 'info.body.rot': (0, 0, 0, 1)})
    sp = bots.expect('bob', 'spawned')
    h.check(sp is not None and sp['creator'] == ALICE and sp['info']['name'] == 'Item_Apple',
            'a spawn reaches the other game in interest')
    h.check(bots.none('alice', 'owner', 0.5, pred=lambda e: e['id']['serial'] == 77),
            'the creator gets no owner message for its own spawn')
    bots.send('bob', 'spawn', **{'info.id.origin': ALICE, 'info.id.serial': 78, 'info.class_': CLASS_ITEM,
              'info.name': 'Item_Apple', 'info.body.rot': (0, 0, 0, 1)})
    h.check(bots.none('alice', 'spawned', 1.0), "a spawn under another game's account is refused")
    bots.send('bob', 'spawn', **{'info.id.origin': BOB, 'info.id.serial': 5, 'info.parent.origin': 0,
              'info.parent.serial': 501, 'info.class_': CLASS_ENEMY, 'info.name': 'Enemy_Keese',
              'info.body.rot': (0, 0, 0, 1)})
    g = bots.expect('bob', 'gone')
    h.check(g is not None and g['id']['serial'] == 5 and g['cause'] == GONE_REMOVED,
            "a copy's summons is refused with gone, so its game removes it")
    # Two connections have no order between them, so the second take goes once the first is answered.
    bots.send('bob', 'take', **{'id.origin': ALICE, 'id.serial': 77, 'epoch': 1})
    t1 = bots.expect('bob', 'taken')
    bots.send('alice', 'take', **{'id.origin': ALICE, 'id.serial': 77, 'epoch': 1})
    t2 = bots.expect('alice', 'taken', pred=lambda e: e['id']['serial'] == 77)
    h.check(t1 is not None and t1['yours'] == 1 and t1['taker'] == BOB, 'the first take gets the thing')
    h.check(t2 is not None and t2['yours'] == 0 and t2['taker'] == BOB,
            'a game that takes it later learns who has it, and removes its copy')

    # ---- the owner leaves: a near candidate takes over with the last state ----
    bots.drain()
    bots.cmd('send alice bye reason=0 detail=')
    o2 = bots.expect('bob', 'owner', 4.0, pred=lambda e: e['id']['serial'] == 501)
    h.check(o2 is not None and o2['owner'] == BOB and o2['cause'] == OWNER_LEFT and o2['epoch'] > epoch,
            "when the owner leaves, the near candidate gets the actor by owner left, under a newer epoch")
    h.check(o2 is not None and abs(o2['body']['pos'][0] - 1.0) < 1e-6,
            "the hand-over carries the owner's last state")

    # ---- a game without the add-on that sends one of its messages has broken the protocol ----
    bots.cmd('sealraw carol 0400 ' + '00' * 31)  # past the bot's own writer, which refuses it too
    ev = bots.expect('carol', None, 4.0, pred=lambda e: e['ev'] in ('refuse', 'closed'))
    h.check(ev is not None, 'an actor message on a link without the add-on ends the connection')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--release', action='store_true', help='test the optimised build instead of the checked one')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='actor_server_check_'))
    t0 = time.time()
    try:
        exe_srv, exe_bot = h.build_server.build(tmp / 'bin', debug=not a.release, werror=True)
        port = h.free_port()
        srv = h.Server(exe_srv, tmp / 'data', port, dict(max_players=8, open=True, hold_seconds=3, idle_seconds=6))
        bots = h.Bots(exe_bot, port)
        try:
            body(bots)
        finally:
            bots.stop()
            code = srv.stop()
            h.check(code == 0, f'the server exits cleanly (exit code {code})')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'{len(h.FAILS)} failures in {time.time() - t0:.0f} s')
    for f in h.FAILS:
        print('  ' + f)
    return min(len(h.FAILS), 100)


if __name__ == '__main__':
    sys.exit(main())
