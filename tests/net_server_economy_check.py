"""The shared economy of the release 1 server against headless bots, on localhost only.

    python tests/net_server_economy_check.py [--release] [--keep]

Builds the server and the bot (as tests/net_server_check.py does) and runs one server whose rules are small enough to
reach in a test, then checks:

- the add-ons granted (economy and sky, nothing else) and what a joining game is told at once;
- the wallet: a change is accepted or clamped (wrong sign, over the balance, over the cap of one request, over the
  rate per minute), is per account, and survives a restart;
- the Lights ledger: a Light or Korok seed anyone finds counts for every player once, what a player spends is that
  player's own and never more than was found, and it survives a restart;
- the Blood Moon leader: the first to join leads, only the leader's clock is relayed, the lead passes on when the
  leader leaves and a joining game gets the clock;
- the away flag in the player list: set after afk_seconds of nothing, cleared by moving or chatting.

Each check prints ok or FAIL; the exit code is the number of failures. docs/server.md ("The economy") describes it.
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
import net_server_check as base  # noqa: E402
from net_server_check import Bots, Server, check, free_port  # noqa: E402

ECONOMY, SKY = 16, 64
SPEND, EARN, TRADE, STAKE = range(4)
LIGHT_FOUND, LIGHT_SPENT, SEED_FOUND, SEED_SPENT = range(4)

SETTINGS = dict(max_players=8, open=True, hold_seconds=2, handshake_seconds=2, idle_seconds=6, max_per_ip=64,
                max_pending_per_ip=8, connects_per_minute=100000, new_accounts_per_hour=1000, chat_burst=5,
                chat_per_second=1.0, action_burst=10, action_per_second=4.0, frame_burst=200, frames_per_second=60.0,
                afk_seconds=2, start_rupees=100, wallet_earn_max=1000, wallet_earn_per_minute=1500)


def wallet(bots, who, seq, delta, cause):
    bots.send(who, 'wallet_ask', seq=seq, delta=delta, cause=cause)
    return bots.expect(who, 'wallet', 3, seq=seq)


def ledger(bots, who, what, key=0, count=1):
    bots.send(who, 'ledger_note', what=what, key=key, count=count)


def last_ledger(bots, who):
    return bots.expect(who, 'ledger', 3)


def test_join(srv, bots):
    a = bots.connect('alice', 'seedAlice', 'Alice', features=255)
    check(a.get('features') == ECONOMY | SKY, f'only the economy and the sky clock are granted ({a.get("features")})')
    w = bots.expect('alice', 'wallet', 2)
    check(w is not None and w['seq'] == 0 and w['rupees'] == 100, 'a joining game is told its rupees (the start amount)')
    l = bots.expect('alice', 'ledger', 2)
    check(l is not None and l['lights_found'] == 0 and l['seeds_found'] == 0, 'and the ledger, empty at first')
    s = bots.expect('alice', 'sky_set', 2)
    check(s and s['leader'] == a['account'] and s['info']['day'] == 0xFFFFFFFF,
          'the first game is told it leads, and that there is no clock yet')
    b = bots.connect('bob', 'seedBob', 'Bob', features=ECONOMY | SKY)
    check(b.get('features') == ECONOMY | SKY, 'a second game is granted the same')
    c = bots.connect('carol', 'seedCarol', 'Carol', features=0)
    check(c.get('features') == 0, 'a game that offers nothing is granted nothing')
    bots.cmd('sealraw carol 0710 01000000' + '05000000' + '01')
    check(bots.expect('carol', 'refuse', 3) is not None, 'a wallet request on a link without the economy is refused')
    bots.forget('carol')
    bots.drain()
    return a['account'], b['account']


def test_wallet(srv, bots):
    r = wallet(bots, 'alice', 1, -30, SPEND)
    check(r and r['granted'] == -30 and r['rupees'] == 70, 'a spend within the balance is accepted')
    r = wallet(bots, 'alice', 2, -500, SPEND)
    check(r and r['granted'] == -70 and r['rupees'] == 0, 'a spend over the balance is cut to the balance')
    r = wallet(bots, 'alice', 3, 50, SPEND)
    check(r and r['granted'] == 0 and r['rupees'] == 0, 'a spend that adds is refused')
    r = wallet(bots, 'alice', 4, -5, EARN)
    check(r and r['granted'] == 0, 'an earn that takes is refused')
    r = wallet(bots, 'alice', 5, 800, EARN)
    check(r and r['granted'] == 800 and r['rupees'] == 800, 'an earn within the cap is accepted')
    r = wallet(bots, 'alice', 6, 5000, EARN)
    # the rate refills 25 a second, so a slow machine may add a little to the 700 that is left
    check(r and 700 <= r['granted'] <= 800 and r['rupees'] == 800 + r['granted'],
          'an earn over the rate per minute is cut to what is left of it (1500 a minute)')
    r = wallet(bots, 'alice', 7, 900, TRADE)
    check(r and 0 <= r['granted'] <= 100, 'and a gain by trade counts against the same rate')
    before = r['rupees'] if r else 0
    r = wallet(bots, 'alice', 8, -1000, TRADE)
    check(r and r['granted'] == -1000 and r['rupees'] == before - 1000, 'a trade may take')
    left = r['rupees'] if r else 0
    r = wallet(bots, 'bob', 1, 5000, EARN)
    check(r and r['granted'] == 1000 and r['rupees'] == 1100, 'one request adds at most wallet_earn_max (1000)')
    r = wallet(bots, 'bob', 2, 2 ** 31 - 1, EARN)
    check(r and r['granted'] <= 500, 'a huge request is cut as well')
    bots.send('alice', 'wallet_ask', seq=9, delta=-2 ** 31, cause=SPEND)
    r = bots.expect('alice', 'wallet', 3, seq=9)
    check(r and r['granted'] == -left and r['rupees'] == 0, 'the most negative number takes the balance and no more')
    check(base_value(srv, 'wallet', 1) is not None, 'economy.txt lists the wallets after a moment')


def base_value(srv, kind, _):
    time.sleep(1.3)
    text = (srv.data / 'economy.txt').read_text() if (srv.data / 'economy.txt').exists() else ''
    return text if kind in text else None


def test_ledger(srv, bots, alice, bob):
    ledger(bots, 'alice', LIGHT_FOUND, key=0xAAAA01)
    la = last_ledger(bots, 'alice')
    lb = last_ledger(bots, 'bob')
    check(la and la['lights_found'] == 1 and lb and lb['lights_found'] == 1,
          'a Light one player finds counts for every player')
    ledger(bots, 'bob', LIGHT_FOUND, key=0xAAAA01)
    lb = last_ledger(bots, 'bob')
    check(lb and lb['lights_found'] == 1, 'the same Light found again counts once')
    check(bots.none('alice', 'ledger', 0.4), 'and the others are not told again')
    ledger(bots, 'bob', LIGHT_FOUND, key=0xAAAA02)
    ledger(bots, 'bob', SEED_FOUND, key=0xBBBB01)
    bots.drain(0.5)
    ledger(bots, 'alice', LIGHT_SPENT, count=1)
    la = last_ledger(bots, 'alice')
    check(la and la['lights_spent'] == 1 and la['lights_found'] == 2, 'a player spends a shared Light')
    ledger(bots, 'bob', LIGHT_SPENT, count=1)
    lb = last_ledger(bots, 'bob')
    check(lb and lb['lights_spent'] == 1, "and the other still has their own to spend")
    ledger(bots, 'alice', LIGHT_SPENT, count=5)
    la = last_ledger(bots, 'alice')
    check(la and la['lights_spent'] == 2, 'a spend is never more than was found')
    ledger(bots, 'alice', SEED_SPENT, count=1)
    la = last_ledger(bots, 'alice')
    check(la and la['seeds_spent'] == 1 and la['seeds_found'] == 1, 'Korok seeds work the same')
    ledger(bots, 'bob', LIGHT_FOUND, key=0)
    check(bots.none('alice', 'ledger', 0.4), 'a find without its key is ignored')
    for i in range(40):
        ledger(bots, 'bob', SEED_FOUND, key=0xCC0000 + i)
    bots.sync()
    time.sleep(0.5)
    bots.drain()
    ledger(bots, 'bob', SEED_SPENT, count=0)
    lb = last_ledger(bots, 'bob')
    check(lb and 10 < lb['seeds_found'] < 40, f'a flood of invented finds is cut short ({lb and lb["seeds_found"]} kept)')


def test_sky(srv, bots, alice, bob):
    bots.drain()
    bots.send('bob', 'sky', **{'info.day': 9, 'info.hour': 3.0, 'info.blood_moon_due': 0})
    check(bots.none('alice', 'sky_set', 0.5), "a clock from a game that does not lead is not relayed")
    bots.send('alice', 'sky', **{'info.day': 12, 'info.hour': 21.5, 'info.blood_moon_due': 1})
    s = bots.expect('bob', 'sky_set', 3)
    check(s and s['leader'] == alice and s['info']['day'] == 12 and s['info']['blood_moon_due'] == 1,
          "the first to join leads, and the leader's clock reaches the others")
    check(bots.none('alice', 'sky_set', 0.3), 'but not the leader itself')
    bots.send('alice', 'sky', **{'info.day': 12, 'info.hour': 30.0, 'info.blood_moon_due': 1})
    check(bots.none('bob', 'sky_set', 0.5), 'an impossible hour is dropped')
    c = bots.connect('dave', 'seedDave', 'Dave', features=ECONOMY | SKY)
    s = bots.expect('dave', 'sky_set', 3)
    check(s and s['leader'] == alice and s['info']['day'] == 12, 'a joining game is told the clock and who leads')
    bots.cmd('bye alice')
    s = bots.expect('bob', 'sky_set', 4)
    check(s and s['leader'] == bob, 'when the leader leaves the next to have joined leads and everyone is told')
    check(bots.expect('dave', 'sky_set', 2, leader=bob) is not None, 'including the others')
    bots.send('bob', 'sky', **{'info.day': 13, 'info.hour': 1.0, 'info.blood_moon_due': 0})
    s = bots.expect('dave', 'sky_set', 3)
    check(s and s['info']['day'] == 13, "and now the new leader's clock is the one relayed")


def test_afk(srv, bots, bob):
    bots.send('dave', 'position', scene=1, pos=[0, 0, 0], yaw=0.0)
    bots.drain()
    time.sleep(2.6)
    r = bots.expect('dave', 'roster', 3, pred=lambda e: any(p['account'] == bob and p['afk'] for p in e['players']))
    check(r is not None, 'a player who did nothing for afk_seconds is marked away in the list')
    bots.send('bob', 'say', kind=0, to=0, text='back')
    r = bots.expect('dave', 'roster', 3, pred=lambda e: any(p['account'] == bob and not p['afk'] for p in e['players']))
    check(r is not None, 'chatting clears the mark')
    time.sleep(2.6)
    bots.send('bob', 'position', scene=1, pos=[0, 0, 0], yaw=0.0)
    time.sleep(1.2)
    bots.drain()
    bots.send('bob', 'position', scene=1, pos=[0, 0, 0], yaw=0.0)
    bots.send('dave', 'position', scene=1, pos=[0, 0, 0], yaw=0.0)
    r = bots.expect('dave', 'roster', 2, pred=lambda e: any(p['account'] == bob and p['afk'] for p in e['players']))
    check(r is not None, 'standing still is not an action')
    bots.send('bob', 'position', scene=1, pos=[30, 0, 0], yaw=0.0)
    r = bots.expect('dave', 'roster', 3, pred=lambda e: any(p['account'] == bob and not p['afk'] for p in e['players']))
    check(r is not None, 'moving clears the mark')


def test_restart(srv, bots, exe_bot, port):
    bots.stop()
    code = srv.stop()
    check(code == 0, f'the server stops cleanly (exit code {code})')
    srv.start()
    bots2 = Bots(exe_bot, port)
    try:
        a = bots2.connect('alice', 'seedAlice', 'Alice', features=ECONOMY | SKY)
        check(a.get('ev') == 'admit', 'a known key returns after a restart')
        w = bots2.expect('alice', 'wallet', 2)
        check(w and w['rupees'] == 0, 'its rupees are kept')
        w2 = bots2.connect('bob', 'seedBob', 'Bob', features=ECONOMY | SKY)
        wb = bots2.expect('bob', 'wallet', 2)
        check(wb and wb['rupees'] > 1000, 'and the other account has its own')
        l = bots2.expect('alice', 'ledger', 2)
        check(l and l['lights_found'] == 2 and l['lights_spent'] == 2 and l['seeds_found'] > 10,
              'the ledger is kept, the finds and what was spent')
    finally:
        bots2.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--release', action='store_true')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='net_economy_check_'))
    try:
        exe_srv, exe_bot = build_server.build(tmp / 'bin', debug=not a.release, werror=True)
        port = free_port()
        srv = Server(exe_srv, tmp / 'e', port, SETTINGS)
        bots = Bots(exe_bot, port)
        t0 = time.time()
        try:
            alice, bob = test_join(srv, bots)
            test_wallet(srv, bots)
            test_ledger(srv, bots, alice, bob)
            test_sky(srv, bots, alice, bob)
            test_afk(srv, bots, bob)
            test_restart(srv, bots, exe_bot, port)
        finally:
            bots.stop()
            srv.stop()
        print(f'{len(base.FAILS)} failures in {time.time() - t0:.0f} s')
        for f in base.FAILS:
            print('  ' + f)
    finally:
        if a.keep:
            print('data folders in', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return min(len(base.FAILS), 100)


if __name__ == '__main__':
    sys.exit(main())
