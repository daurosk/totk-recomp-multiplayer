"""The release 1 server (server/) against headless bots (tools/net_bot.c), on localhost only.

    python tests/net_server_check.py [--release] [--keep]

Builds the server and the bot with tools/build_server.py (with undefined-behaviour checks that trap, unless
--release), then runs three servers on free localhost ports, each with its own data folder in a temporary folder
(kept with --keep):

- A, an open room: join and the join sequence, feature bits (none granted), names, leave, reconnect in the hold,
  the hold running out, a second sign-in, the keep-alive timeout, chat on every channel with its limits, teams,
  markers, positions, teleport requests and the host's summon, every host tool from a client and from the console,
  and bad clients (garbage, wrong magic, another version, oversized and malformed frames, bad signatures, slow and
  stalled senders, floods, too many handshakes). Then a restart, which must keep accounts, ranks and bans.
- B, invite only with two places: claim codes, a full room, the limit on new connections per minute.
- C, a password, a host named in the settings, the limit on new accounts per hour.

Each check prints ok or FAIL; the exit code is the number of failures. docs/server.md describes the server.
"""
import argparse
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_server  # noqa: E402

FAILS = []
NO_WINDOW = 0x08000000 if sys.platform == 'win32' else 0

# Reason, chat, teleport and host tool numbers of docs/online-wire.md, section 11.
WHY = dict(none=0, version=1, protocol=2, auth=3, unknown_key=4, bad_claim=5, full=6, banned=7, shutdown=8,
           timeout=9, duplicate=10, kicked=11)
CHAT_ALL, CHAT_TEAM, CHAT_WHISPER, CHAT_ME, CHAT_SYSTEM = range(5)
TP_ACCEPTED, TP_REFUSED, TP_TIMEOUT, TP_NOT_ALLOWED, TP_GONE, TP_BUSY = range(6)
MOD_KICK, MOD_BAN, MOD_UNBAN, MOD_MUTE, MOD_UNMUTE, MOD_SET_RANK = range(6)
DONE, NOT_ALLOWED, UNKNOWN, ALREADY = range(4)


def check(ok, what):
    print(('ok    ' if ok else 'FAIL  ') + what, flush=True)
    if not ok:
        FAILS.append(what)
    return ok


def hx(text):
    return text.encode().hex()


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    """A server process with its console on stdin and its log lines collected from stdout."""

    def __init__(self, exe, data, port, settings):
        self.exe, self.data, self.port = exe, Path(data), port
        self.data.mkdir(parents=True, exist_ok=True)
        lines = [f'bind = "127.0.0.1"', f'port = {port}']
        for k, v in settings.items():
            lines.append(f'{k} = ' + (('true' if v else 'false') if isinstance(v, bool) else
                                      f'"{v}"' if isinstance(v, str) else str(v)))
        (self.data / 'server.toml').write_text('\n'.join(lines) + '\n')
        self.start()

    def start(self):
        self.lines, self.cv = [], threading.Condition()
        self.p = subprocess.Popen([str(self.exe), '--data', str(self.data)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                  creationflags=NO_WINDOW)
        threading.Thread(target=self._read, daemon=True).start()
        self.wait_line(r'listening on', 10)

    def _read(self):
        for line in self.p.stdout:
            with self.cv:
                self.lines.append(line.rstrip('\n'))
                self.cv.notify_all()

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()

    def wait_line(self, pattern, timeout=5, start=0):
        rx = re.compile(pattern)
        end = time.time() + timeout
        with self.cv:
            while True:
                for line in self.lines[start:]:
                    m = rx.search(line)
                    if m:
                        return m
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def mark(self):
        with self.cv:
            return len(self.lines)

    def stop(self):
        if self.p.poll() is None:
            self.cmd('stop')
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        return self.p.returncode


class Bots:
    """One net_bot process; events per bot, each taken once by expect()."""

    def __init__(self, exe, port):
        self.p = subprocess.Popen([str(exe), '--port', str(port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding='utf-8', creationflags=NO_WINDOW)
        self.events, self.cv, self.keys = [], threading.Condition(), {}
        self.keep = set()  # bots whose events drain() leaves alone
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            with self.cv:
                if ev['ev'] == 'key':
                    self.keys[ev['bot']] = ev['key']
                self.events.append([ev, False])
                self.cv.notify_all()

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()

    def _match(self, ev, bot, kind, fields, pred):
        if bot is not None and ev['bot'] != bot:
            return False
        if kind is not None and ev['ev'] != kind:
            return False
        if any(ev.get(k) != v for k, v in fields.items()):
            return False
        return pred is None or pred(ev)

    def expect(self, bot, kind, /, timeout=4.0, pred=None, **fields):
        """The first unseen event of bot of this kind with these fields, or None after timeout."""
        end = time.time() + timeout
        with self.cv:
            while True:
                for item in self.events:
                    if not item[1] and self._match(item[0], bot, kind, fields, pred):
                        item[1] = True
                        return item[0]
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def none(self, bot, kind, /, within=1.0, pred=None, **fields):
        """True when no such event arrives within the given time."""
        return self.expect(bot, kind, within, pred, **fields) is None

    def drain(self, settle=0.3):
        time.sleep(settle)
        with self.cv:
            for item in self.events:
                if item[0]['bot'] not in self.keep:
                    item[1] = True

    def forget(self, name):
        """Marks the bot's events seen, after waiting for its last connection to report its close."""
        end = time.time() + 3
        with self.cv:
            while True:
                last = [e[0]['ev'] for e in self.events if e[0]['bot'] == name][-1:]
                if not last or last[0] == 'closed' or time.time() > end:
                    break
                self.cv.wait(0.1)
            for item in self.events:
                if item[0]['bot'] == name:
                    item[1] = True

    def sync(self):
        tag = f's{time.time_ns()}'
        self.cmd('echo ' + tag)
        self.expect('', 'echo', 5, text=tag)

    def connect(self, name, seed, nick, timeout=5, **opts):
        args = ' '.join(f'{k}={v}' for k, v in opts.items())
        self.forget(name)  # what the bot's earlier connection left is not this connection's answer
        self.cmd(f'connect {name} {seed} {nick} {args}'.strip())
        ev = self.expect(name, None, timeout, pred=lambda e: e['ev'] in ('admit', 'refuse', 'closed', 'error'))
        if ev and ev['ev'] == 'admit':
            self.expect(name, 'join_ready', timeout)
        return ev or {'ev': 'nothing'}

    def send(self, name, msg, /, **fields):
        def val(v):
            if isinstance(v, str):
                return hx(v)
            if isinstance(v, (list, tuple)):
                return ','.join(repr(float(x)) for x in v)
            return str(int(v))
        self.cmd(f'send {name} {msg} ' + ' '.join(f'{k}={val(v)}' for k, v in fields.items()))

    def stop(self):
        try:
            self.cmd('quit')
            self.p.wait(5)
        except Exception:
            self.p.kill()


def roster_row(ev, account):
    return next((r for r in ev['players'] if r['account'] == account), None)


def wait_roster(bots, bot, pred, timeout=4.0):
    return bots.expect(bot, 'roster', timeout, pred=pred)


def sealed_frame_tests(srv, bots):
    """Malformed payloads past the seal: each must end the connection with refuse OW_WHY_PROTOCOL."""
    say_ok = '00' + '00' * 8
    cases = [
        ('say text over 256 bytes', '0500', say_ok + (257).to_bytes(2, 'little').hex() + '61' * 257),
        ('say text not UTF-8', '0500', say_ok + '0200' + 'c328'),
        ('say text with a control character', '0500', say_ok + '0200' + '610a'),
        ('say kind outside its set', '0500', '09' + '00' * 8 + '0000'),
        ('say with a trailing byte', '0500', say_ok + '0000' + '00'),
        ('say cut short', '0500', say_ok + '0500' + '6161'),
        ('bool other than 0 or 1', '0300', '0000' + '02'),
        ('team_join under its smallest size', '0303', '01'),
        ('position with NaN', '0205', '00000000' + '0000c07f' + '00' * 8 + '00000000'),
        ('an add-on type not granted', '0280', '00' * 61),
        ('a server-to-client type', '0501', '00' * 21),
        ('a handshake type after sealing', '0001', '00' * 72),
        ('an unknown type', '7777', '00' * 4),
        ('a second proof', '0004', '00' * 96 + '0000' + '00' + '0000' + '00000000'),
    ]
    for i, (what, typ, payload) in enumerate(cases):
        name = f'mal{i}'
        ev = bots.connect(name, f'malseed{i}', f'Mal{i}')
        if not check(ev['ev'] == 'admit', f'bad client {what}: joins first'):
            continue
        bots.drain(0.1)
        bots.cmd(f'sealraw {name} {typ} {payload}')
        r = bots.expect(name, 'refuse', 3)
        check(r is not None and r['reason'] == WHY['protocol'], f'bad client: {what} is refused ({r and r["detail"]})')
        check(bots.expect(name, 'closed', 3) is not None, f'bad client: {what} is disconnected')


def run_a(exe_srv, exe_bot, tmp, keys):
    port = free_port()
    settings = dict(max_players=8, open=True, hold_seconds=3, handshake_seconds=2, idle_seconds=4,
                    max_per_ip=64, max_pending_per_ip=8, connects_per_minute=100000, new_accounts_per_hour=1000, chat_burst=5,
                    chat_per_second=1.0, action_burst=10, action_per_second=4.0, frame_burst=200,
                    frames_per_second=60.0, motd='Hello from the test')
    srv = Server(exe_srv, tmp / 'a', port, settings)
    bots = Bots(exe_bot, port)
    try:
        body_a(srv, bots, keys)
        restart_a(srv, bots, exe_bot, port)
    finally:
        bots.stop()
        code = srv.stop()
        check(code == 0, f'server A exits cleanly on stop (exit code {code})')
        log = (tmp / 'a' / 'server.log').read_text(encoding='utf-8', errors='replace')
        check('chat [all] Bob: hello everyone' in log, 'server.log holds chat lines')
        check('kick' in log and 'ban' in log, 'server.log holds host tools')


def body_a(srv, bots, keys):
    # ---- join ----
    a = bots.connect('alice', 'seedAlice', 'Alice', features=255)
    check(a.get('ev') == 'admit' and a['rank'] == 2 and a['resumed'] == 0, 'first account is admitted as the host')
    check(a.get('features') == 16 | 64, 'release 1 grants only the economy and the sky clock even when all are offered')
    check(a.get('rules', {}).get('hold_ms') == 3000 and a['rules']['options'] & 2, 'admit carries the room rules')
    keys['alice'] = bots.keys.get('alice')
    check((srv.data / 'host.txt').read_text().strip() == keys['alice'], 'the first account is written to host.txt')
    ALICE = a['account']
    b = bots.connect('bob', 'seedBob', 'Bob')
    check(b.get('ev') == 'admit' and b['rank'] == 0, 'second account is a player')
    BOB = b['account']
    arr = bots.expect('alice', 'arrive', account=BOB)
    check(arr is not None and arr['nickname'] == 'Bob', 'others get arrive for a new player')
    check(wait_roster(bots, 'alice', lambda e: roster_row(e, BOB)) is not None, 'others get a roster with the new player')
    # the join sequence, in order, for one more player
    bots.cmd('connect cara seedCara Cara')
    seq = []
    for _ in range(6):
        ev = bots.expect('cara', None, 4, pred=lambda e: e['ev'] not in ('key', 'challenge') and e['ev'] not in seq)
        if ev:
            seq.append(ev['ev'])
    check(seq[:5] == ['admit', 'roster', 'teams', 'positions', 'join_ready'],
          f'join order: admit, roster, teams, positions, join_ready ({seq})')
    CARA = next((e[0]['account'] for e in bots.events if e[0]['bot'] == 'cara' and e[0]['ev'] == 'admit'), 0)
    d = bots.connect('dan', 'seedDan', 'Dan')
    DAN = d.get('account', 0)
    keys['dan'] = bots.keys.get('dan')
    # names
    n = bots.connect('dup', 'seedDup', '  alice ')
    check(n.get('nickname') == 'alice 2', f'a taken name gets a number ({n.get("nickname")})')
    s = bots.connect('srvname', 'seedSrv', 'Server')
    check(s.get('nickname') == 'Player', f'a name that passes for the server is replaced ({s.get("nickname")})')
    # leave
    bots.drain()
    bots.cmd('bye dup')
    dep = bots.expect('alice', 'depart', 3, account=n.get('account'))
    check(dep is not None and dep['reason'] == 0, 'bye: others get depart with reason 0 at once')
    bots.cmd('bye srvname')
    bots.drain()

    # ---- chat ----
    bots.send('bob', 'say', kind=0, to=0, text='hello everyone')
    got = [bots.expect(x, 'chat', 3, kind=CHAT_ALL, **{'from': BOB}) for x in ('alice', 'bob', 'cara', 'dan')]
    check(all(got) and got[0]['nickname'] == 'Bob' and got[0]['text'] == 'hello everyone',
          'chat to all reaches everyone, the sender too')
    bots.send('bob', 'say', kind=3, to=0, text='waves')
    check(bots.expect('cara', 'chat', 3, kind=CHAT_ME, text='waves') is not None, 'chat /me reaches everyone')
    bots.send('bob', 'say', kind=2, to=CARA, text='psst')
    w1 = bots.expect('cara', 'chat', 3, kind=CHAT_WHISPER, text='psst')
    w2 = bots.expect('bob', 'chat', 3, kind=CHAT_WHISPER, text='psst')
    check(w1 is not None and w2 is not None and w1['to'] == CARA, 'whisper reaches the target and the sender')
    check(bots.none('alice', 'chat', 0.7, kind=CHAT_WHISPER), 'whisper reaches nobody else')
    bots.send('bob', 'say', kind=2, to=999999, text='anyone?')
    check(bots.expect('bob', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'not online' in e['text']) is not None,
          'whisper to nobody gets a system line')
    bots.send('bob', 'say', kind=1, to=0, text='team?')
    check(bots.expect('bob', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'not in a team' in e['text']) is not None,
          'team chat without a team gets a system line')
    srv.cmd('say Maintenance at noon')
    check(all(bots.expect(x, 'chat', 3, kind=CHAT_SYSTEM, text='Maintenance at noon', **{'from': 0})
              for x in ('alice', 'bob', 'cara', 'dan')), 'console say is a system line for everyone')
    bots.send('dan', 'say', kind=0, to=0, text='/who')
    who = bots.expect('dan', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'online' in e['text'])
    check(who is not None and 'Alice (host)' in who['text'] and 'Bob' in who['text'], '/who lists players')
    check(bots.none('alice', 'chat', 0.5, text='/who'), 'a command is not shown to others')
    bots.send('dan', 'say', kind=0, to=0, text='   ')
    check(bots.none('alice', 'chat', 0.5, **{'from': DAN}), 'an empty line is dropped')
    # chat rate limit, on its own bot
    bots.connect('chatty', 'seedChatty', 'Chatty')
    bots.drain()
    bots.cmd(f'flood chatty 15 say kind=0 to=0 text={hx("spam")}')
    time.sleep(1.0)
    seen = 0
    while bots.expect('alice', 'chat', 0.3, text='spam'):
        seen += 1
    check(5 <= seen <= 7, f'chat burst limit: {seen} of 15 lines delivered (burst 5)')
    check(bots.expect('chatty', 'chat', 2, kind=CHAT_SYSTEM, pred=lambda e: 'too fast' in e['text']) is not None,
          'over the chat limit the sender gets a system line, not a disconnect')
    check(bots.none('chatty', 'refuse', 0.3), 'the chat flood does not end the session')
    bots.cmd('bye chatty')

    # ---- teams ----
    bots.drain()
    bots.send('bob', 'team_create', name='Reds', open=0)
    t = bots.expect('alice', 'teams', 3, pred=lambda e: any(r['name'] == 'Reds' for r in e['list']))
    RED = t and next(r for r in t['list'] if r['name'] == 'Reds')
    check(RED is not None and RED['leader'] == BOB and RED['members'] == 1 and RED['id'] == 1,
          'team_create makes a team led by its creator, id 1')
    RED = RED['id'] if RED else 1
    check(wait_roster(bots, 'cara', lambda e: (roster_row(e, BOB) or {}).get('team') == RED) is not None,
          'the roster shows the new team')
    bots.send('cara', 'team_join', team=RED)
    check(bots.expect('cara', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'invite only' in e['text']) is not None,
          'an invite-only team cannot be joined without an invitation')
    bots.send('bob', 'team_invite', target=CARA)
    inv = bots.expect('cara', 'team_invited', 3)
    check(inv is not None and inv['team'] == RED and inv['name'] == 'Reds' and inv['from'] == BOB,
          'team_invite sends team_invited to the target')
    bots.send('cara', 'team_join', team=RED)
    check(bots.expect('alice', 'teams', 3, pred=lambda e: any(r['id'] == RED and r['members'] == 2
                                                             for r in e['list'])) is not None,
          'the invited player joins')
    bots.drain()
    bots.send('bob', 'say', kind=1, to=0, text='reds only')
    check(bots.expect('cara', 'chat', 3, kind=CHAT_TEAM, text='reds only') is not None and
          bots.expect('bob', 'chat', 3, kind=CHAT_TEAM, text='reds only') is not None, 'team chat reaches the team')
    check(bots.none('alice', 'chat', 0.6, text='reds only'), 'team chat reaches nobody else')
    bots.send('cara', 'team_kick', target=BOB)
    check(bots.expect('cara', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'leader' in e['text']) is not None,
          'only the leader may remove members')
    bots.send('bob', 'team_kick', target=CARA)
    check(bots.expect('cara', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'removed you' in e['text']) is not None,
          'the leader removes a member')
    bots.send('dan', 'team_create', name='Blues', open=1)
    blue = bots.expect('alice', 'teams', 3, pred=lambda e: any(r['name'] == 'Blues' for r in e['list']))
    BLUE = blue and next(r['id'] for r in blue['list'] if r['name'] == 'Blues')
    bots.send('alice', 'team_join', team=BLUE or 0)
    check(bots.expect('dan', 'teams', 3, pred=lambda e: any(r['id'] == BLUE and r['members'] == 2
                                                           for r in e['list'])) is not None,
          'an open team is joined without an invitation')
    bots.send('dan', 'team_leave', team=BLUE or 0)
    check(bots.expect('alice', 'teams', 3, pred=lambda e: any(r['id'] == BLUE and r['leader'] == ALICE
                                                             for r in e['list'])) is not None,
          'a leader leaving hands the lead to the earliest member')
    bots.send('alice', 'team_leave', team=BLUE or 0)
    check(bots.expect('dan', 'teams', 3, pred=lambda e: all(r['id'] != BLUE for r in e['list'])) is not None,
          'the last member leaving ends the team')
    bots.send('dan', 'team_create', name='Greens', open=1)
    g = bots.expect('dan', 'teams', 3, pred=lambda e: any(r['name'] == 'Greens' for r in e['list']))
    check(g is not None and all(r['id'] not in (RED, BLUE) for r in g['list'] if r['name'] == 'Greens'),
          'team ids are not reused')
    bots.send('dan', 'team_leave', team=next((r['id'] for r in (g or {'list': []})['list'] if r['name'] == 'Greens'), 0))
    bots.send('bob', 'team_invite', target=CARA)
    bots.expect('cara', 'team_invited', 3)
    bots.send('cara', 'team_join', team=RED)
    bots.expect('alice', 'teams', 3, pred=lambda e: any(r['id'] == RED and r['members'] == 2 for r in e['list']))
    # markers
    bots.drain()
    bots.send('bob', 'mark', slot=1, on=1, scene=7, pos=(1, 2, 3))
    mk = bots.expect('cara', 'marker', 3)
    check(mk is not None and mk['account'] == BOB and mk['slot'] == 1 and mk['pos'] == [1, 2, 3],
          'a marker reaches the team')
    check(bots.none('alice', 'marker', 0.6), 'a marker does not reach other teams')

    # ---- positions ----
    bots.send('bob', 'position', scene=7, pos=(10, 20, 30), yaw=1.5)
    bots.send('cara', 'position', scene=7, pos=(11, 21, 31), yaw=0)
    bots.send('dan', 'position', scene=8, pos=(-5, 0, 5), yaw=0)
    bots.send('alice', 'position', scene=9, pos=(100, 50, -100), yaw=0)
    p = bots.expect('alice', 'positions', 3, pred=lambda e: len(e['spots']) == 3)
    spot = p and next((s for s in p['spots'] if s['account'] == BOB), None)
    check(spot is not None and spot['pos'] == [10, 20, 30] and spot['scene'] == 7,
          'positions relay the others\' last positions once a second')
    check(p is not None and all(s['account'] != ALICE for s in p['spots']), 'positions leave out the viewer')
    srv.cmd('positions team')
    bots.drain(1.2)
    pb = bots.expect('bob', 'positions', 3)
    pa = bots.expect('alice', 'positions', 3)
    check(pb is not None and {s['account'] for s in pb['spots']} == {CARA}, 'positions team: players see their team')
    check(pa is not None and len(pa['spots']) == 3, 'positions team: the host still sees everyone')
    srv.cmd('positions all')

    # ---- teleports ----
    bots.drain()
    bots.connect('tpw', 'seedTpw', 'Waiter')
    bots.keep.add('tpw')
    bots.send('tpw', 'tp_ask', target=ALICE)  # never answered: times out after 30 s, checked at the end
    bots.expect('alice', 'tp_asked', 3)
    bots.send('dan', 'tp_ask', target=BOB)
    ask = bots.expect('bob', 'tp_asked', 3)
    check(ask is not None and ask['from'] == DAN and ask['nickname'] == 'Dan', 'tp_ask reaches the target')
    bots.send('dan', 'tp_ask', target=CARA)
    check(bots.expect('dan', 'tp_result', 3, target=CARA, outcome=TP_BUSY) is not None,
          'a second request while one is open is busy')
    bots.send('bob', 'tp_reply', **{'from': DAN, 'accept': 1})
    go = bots.expect('dan', 'go_to', 3)
    check(go is not None and go['target'] == BOB and go['pos'] == [10, 20, 30] and go['cause'] == 0,
          'accept sends go_to with the target\'s position')
    check(bots.expect('dan', 'tp_result', 3, target=BOB, outcome=TP_ACCEPTED) is not None, 'accept: tp_result accepted')
    bots.send('bob', 'tp_reply', **{'from': DAN, 'accept': 1})
    check(bots.none('dan', 'go_to', 0.6), 'a reply to no open request does nothing')
    bots.send('dan', 'tp_ask', target=CARA)
    bots.expect('cara', 'tp_asked', 3)
    bots.send('cara', 'tp_reply', **{'from': DAN, 'accept': 0})
    check(bots.expect('dan', 'tp_result', 3, target=CARA, outcome=TP_REFUSED) is not None, 'refuse: tp_result refused')
    bots.send('dan', 'tp_ask', target=DAN)
    check(bots.expect('dan', 'tp_result', 3, target=DAN, outcome=TP_NOT_ALLOWED) is not None,
          'a request to oneself is not allowed')
    bots.send('dan', 'tp_ask', target=424242)
    check(bots.expect('dan', 'tp_result', 3, target=424242, outcome=TP_GONE) is not None,
          'a request to nobody: target gone')
    srv.cmd('teleport off')
    check(bots.expect('dan', 'rules', 3, pred=lambda e: not e['rules']['options'] & 2) is not None,
          'teleport off reaches every game as rules')
    bots.send('dan', 'tp_ask', target=BOB)
    check(bots.expect('dan', 'tp_result', 3, target=BOB, outcome=TP_NOT_ALLOWED) is not None,
          'with teleport off requests are not allowed')
    srv.cmd('teleport on')
    bots.expect('dan', 'rules', 3)
    # the target leaving
    bots.send('dan', 'tp_ask', target=CARA)
    bots.expect('cara', 'tp_asked', 3)
    bots.cmd('drop cara')
    check(bots.expect('dan', 'tp_result', 3, target=CARA, outcome=TP_GONE) is not None,
          'the target dropping ends the request as gone')
    # reconnect within the hold
    check(bots.none('alice', 'depart', 1.0, account=CARA), 'a dropped player is held: no depart')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, CARA) or {}).get('afk') == 1) is not None,
          'a held player shows as away')
    r = bots.connect('cara', 'seedCara', 'Cara')
    check(r.get('ev') == 'admit' and r['resumed'] == 1 and r['account'] == CARA, 'reconnect in the hold resumes')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, CARA) or {}).get('team') == RED and
                      roster_row(e, CARA)['afk'] == 0) is not None, 'the resumed session keeps its team')
    check(bots.none('alice', 'arrive', 0.5, account=CARA), 'a resumed player is not announced again')
    # summon
    bots.drain()
    bots.send('bob', 'summon', scope=2, target=0)
    check(bots.expect('bob', 'tp_result', 3, outcome=TP_NOT_ALLOWED) is not None, 'summon is the host\'s only')
    bots.send('alice', 'summon', scope=2, target=0)
    gs = [bots.expect(x, 'go_to', 3, cause=1, target=ALICE) for x in ('bob', 'cara', 'dan')]
    check(all(gs) and gs[0]['pos'] == [100, 50, -100], 'summon everyone sends go_to with the host\'s position')
    bots.send('alice', 'summon', scope=0, target=BOB)
    check(bots.expect('bob', 'go_to', 3, cause=1) is not None and bots.none('dan', 'go_to', 0.6),
          'summon one reaches only that player')
    bots.send('alice', 'summon', scope=1, target=RED)
    check(bots.expect('bob', 'go_to', 3, cause=1) is not None and bots.expect('cara', 'go_to', 3, cause=1) is not None
          and bots.none('dan', 'go_to', 0.6), 'summon a team reaches its members')

    # ---- host tools ----
    def mod(who, action, target, rank=0, minutes=0, reason=''):
        bots.send(who, 'mod_action', action=action, target=target, rank=rank, minutes=minutes, reason=reason)
        return bots.expect(who, 'mod_done', 3, action=action, target=target)

    bots.drain()
    m = mod('bob', MOD_KICK, DAN)
    check(m is not None and m['outcome'] == NOT_ALLOWED, 'a player may not kick')
    m = mod('alice', MOD_SET_RANK, BOB, rank=1)
    check(m is not None and m['outcome'] == DONE, 'the host makes a moderator')
    check(wait_roster(bots, 'dan', lambda e: (roster_row(e, BOB) or {}).get('rank') == 1) is not None,
          'the roster shows the new rank')
    check(bots.expect('dan', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'moderator' in e['text']) is not None,
          'a host tool is told to everyone as a system line')
    m = mod('alice', MOD_SET_RANK, BOB, rank=1)
    check(m is not None and m['outcome'] == ALREADY, 'set rank twice is already so')
    m = mod('alice', MOD_SET_RANK, BOB, rank=2)
    check(m is not None and m['outcome'] == NOT_ALLOWED, 'a client cannot make a second host')
    check(keys['alice'] and bots.keys.get('bob', '') in (srv.data / 'moderators.txt').read_text(),
          'moderators.txt lists the moderator')
    m = mod('bob', MOD_KICK, ALICE)
    check(m is not None and m['outcome'] == NOT_ALLOWED, 'a moderator may not kick the host')
    m = mod('bob', MOD_BAN, DAN)
    check(m is not None and m['outcome'] == NOT_ALLOWED, 'a moderator may not ban')
    m = mod('alice', MOD_KICK, 31337)
    check(m is not None and m['outcome'] == UNKNOWN, 'an unknown account is unknown')
    m = mod('alice', MOD_KICK, ALICE)
    check(m is not None and m['outcome'] == NOT_ALLOWED, 'the host may not act on itself')
    # mute
    m = mod('bob', MOD_MUTE, DAN, minutes=5, reason='shouting')
    check(m is not None and m['outcome'] == DONE, 'a moderator mutes a player')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, DAN) or {}).get('muted') == 1) is not None,
          'the roster shows the mute')
    bots.drain()
    bots.send('dan', 'say', kind=0, to=0, text='can you hear me')
    check(bots.expect('dan', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'muted' in e['text']) is not None,
          'a muted player is told so')
    check(bots.none('alice', 'chat', 0.6, text='can you hear me'), 'a muted player\'s line is dropped')
    m = mod('bob', MOD_MUTE, DAN)
    check(m is not None and m['outcome'] == ALREADY, 'mute twice is already so')
    m = mod('bob', MOD_UNMUTE, DAN)
    check(m is not None and m['outcome'] == DONE, 'unmute')
    bots.send('dan', 'say', kind=0, to=0, text='back again')
    check(bots.expect('alice', 'chat', 3, text='back again') is not None, 'an unmuted player is heard')
    # announce
    bots.send('bob', 'announce', text='Event at the stable')
    check(all(bots.expect(x, 'notice', 3, kind=3, text='Event at the stable') for x in ('alice', 'cara', 'dan')),
          'a moderator\'s announce is a notice for everyone')
    bots.send('dan', 'announce', text='I am the host now')
    check(bots.expect('dan', 'chat', 3, kind=CHAT_SYSTEM, pred=lambda e: 'Only the host' in e['text']) is not None
          and bots.none('alice', 'notice', 0.5), 'a player\'s announce is refused')
    # kick
    m = mod('bob', MOD_KICK, DAN, reason='go cool off')
    r = bots.expect('dan', 'refuse', 3)
    check(m is not None and m['outcome'] == DONE and r is not None and r['reason'] == WHY['kicked'] and
          'go cool off' in r['detail'], 'kick: the target gets refuse kicked with the reason')
    check(bots.expect('alice', 'depart', 3, account=DAN, reason=WHY['kicked']) is not None,
          'kick: others get depart kicked at once')
    check(bots.connect('dan', 'seedDan', 'Dan').get('ev') == 'admit', 'a kicked player may come back')
    # ban
    bots.drain()
    m = mod('alice', MOD_BAN, DAN, reason='griefing')
    r = bots.expect('dan', 'refuse', 3)
    check(m is not None and m['outcome'] == DONE and r is not None and r['reason'] == WHY['banned'],
          'ban: the target gets refuse banned')
    check(bots.expect('alice', 'depart', 3, account=DAN, reason=WHY['banned']) is not None, 'ban: others get depart')
    r = bots.connect('dan', 'seedDan', 'Dan')
    check(r.get('ev') == 'refuse' and r['reason'] == WHY['banned'] and 'griefing' in r['detail'],
          'a banned key is refused with the reason')
    check(keys['dan'] in (srv.data / 'bans.txt').read_text(), 'bans.txt lists the banned key')
    m = mod('alice', MOD_BAN, DAN)
    check(m is not None and m['outcome'] == ALREADY, 'ban twice is already so')
    m = mod('alice', MOD_UNBAN, DAN)
    check(m is not None and m['outcome'] == DONE, 'unban')
    m = mod('alice', MOD_UNBAN, DAN)
    check(m is not None and m['outcome'] == ALREADY, 'unban twice is already so')
    check(bots.connect('dan', 'seedDan', 'Dan').get('ev') == 'admit', 'an unbanned key is admitted')
    # the console's host tools
    bots.drain()
    srv.cmd('mute Dan 10 console test')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, DAN) or {}).get('muted') == 1) is not None,
          'console mute')
    srv.cmd('unmute Dan')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, DAN) or {}).get('muted') == 0) is not None,
          'console unmute')
    srv.cmd(f'mod #{DAN}')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, DAN) or {}).get('rank') == 1) is not None,
          'console mod by account id')
    srv.cmd(f'unmod {keys["dan"]}')
    check(wait_roster(bots, 'alice', lambda e: (roster_row(e, DAN) or {}).get('rank') == 0) is not None,
          'console unmod by key')
    srv.cmd('announce Restart soon')
    check(bots.expect('dan', 'notice', 3, kind=3, text='Restart soon') is not None, 'console announce')
    mk = srv.mark()
    srv.cmd('players')
    check(srv.wait_line(r'#\d+ Dan', 3, mk) is not None, 'console players lists who is here')
    srv.cmd('ban Dan 5 console ban')
    r = bots.expect('dan', 'refuse', 3)
    check(r is not None and r['reason'] == WHY['banned'] and 'console ban' in r['detail'], 'console ban with minutes')
    line = next((l for l in (srv.data / 'bans.txt').read_text().splitlines() if l.startswith(keys['dan'])), '')
    until = int(line.split()[1]) if line else 0
    check(abs(until - (time.time() + 300)) < 30, 'a ban for minutes ends then')
    srv.cmd('unban Dan')
    time.sleep(0.3)
    check(bots.connect('dan', 'seedDan', 'Dan').get('ev') == 'admit', 'console unban')
    srv.cmd('kick Dan console kick')
    r = bots.expect('dan', 'refuse', 3)
    check(r is not None and r['reason'] == WHY['kicked'], 'console kick')
    bots.connect('dan', 'seedDan', 'Dan')
    m = mod('bob', MOD_KICK, DAN)  # bob is still a moderator
    check(m is not None and m['outcome'] == DONE, 'a moderator kicks a player')
    bots.connect('dan', 'seedDan', 'Dan')

    # ---- sessions ----
    bots.drain()
    bots.connect('bob2', 'seedBob', 'Bob')
    r = bots.expect('bob', 'refuse', 3)
    check(r is not None and r['reason'] == WHY['duplicate'], 'a second sign-in ends the older session as duplicate')
    b2 = [e[0] for e in bots.events if e[0]['bot'] == 'bob2' and e[0]['ev'] == 'admit']
    check(b2 and b2[-1]['resumed'] == 1 and b2[-1]['account'] == BOB, 'the newer connection resumes the session')
    check(bots.none('alice', 'depart', 0.6, account=BOB), 'a second sign-in is not a departure')
    bots.cmd('drop cara')
    time.sleep(0.5)
    check(bots.expect('alice', 'depart', 5, account=CARA, reason=WHY['timeout']) is not None,
          'a hold that runs out ends in depart timeout')
    bots.connect('cara', 'seedCara', 'Cara')
    bots.drain()
    bots.cmd('quiet cara 1')
    r = bots.expect('cara', 'refuse', 7)
    check(r is not None and r['reason'] == WHY['timeout'], 'a silent session ends with refuse timeout')
    check(bots.expect('alice', 'depart', 6, account=CARA, reason=WHY['timeout']) is not None,
          'and its hold runs out')
    bots.connect('cara', 'seedCara', 'Cara')

    # ---- bad clients ----
    bots.drain()
    for name, payload, what in [
            ('g1', os.urandom(64).hex(), 'random bytes'),
            ('g2', b'GET / HTTP/1.1\r\nHost: x\r\n\r\n'.hex(), 'an HTTP request'),
            ('g3', (72).to_bytes(4, 'little').hex() + '0100' + 'deadbeef0100' + '0000' + '00' * 64, 'a wrong magic'),
            ('g4', 'ffffffff0100', 'a frame header over the limit'),
            ('g5', '000000000100', 'a greet under its smallest size')]:
        bots.cmd(f'raw {name}')
        bots.cmd(f'rawsend {name} {payload}')
        check(bots.expect(name, 'closed', 3) is not None, f'bad client: {what} is disconnected at once')
    r = bots.connect('v2', 'seedV2', 'V2', version=2)
    check(r.get('ev') == 'refuse' and r['reason'] == WHY['version'] and r['version'] == 1,
          'another protocol version gets refuse version with the server\'s version')
    r = bots.connect('forger', 'seedForger', 'Forger', badsig=1)
    check(r.get('ev') == 'refuse' and r['reason'] == WHY['auth'], 'a proof with a bad signature gets refuse auth')
    sealed_frame_tests(srv, bots)
    ev = bots.connect('tamper', 'seedTamper', 'Tamper')
    bots.cmd('rawsend tamper ' + os.urandom(40).hex())
    r = bots.expect('tamper', 'refuse', 3)
    check(r is not None and r['reason'] == WHY['protocol'], 'bytes that fail the seal end the session')
    # slow and stalled senders
    t0 = time.time()
    bots.cmd('raw slow1')
    bots.cmd('raw slow2')
    bots.cmd('rawsend slow2 480000')
    c1, c2 = bots.expect('slow1', 'closed', 5), bots.expect('slow2', 'closed', 5)
    dt = time.time() - t0
    check(c1 is not None and c2 is not None and 1.5 < dt < 4.5,
          f'slowloris: a handshake not finished in 2 s is closed ({dt:.1f} s)')
    bots.connect('stall', 'seedStall', 'Stall')
    bots.cmd('quiet stall 1')
    bots.cmd('rawsend stall 0a00')  # the start of a frame header, never finished
    r = bots.expect('stall', 'refuse', 7)
    check(r is not None and r['reason'] == WHY['timeout'], 'a session stalled inside a frame times out')
    # too many handshakes from one address
    for i in range(8):
        bots.cmd(f'raw pend{i}')
    bots.sync()
    time.sleep(0.3)
    bots.cmd('raw pend8')
    check(bots.expect('pend8', 'closed', 1.0) is not None, 'more handshakes at once than allowed: closed at once')
    check(all(bots.none(f'pend{i}', 'closed', 0.05) for i in range(8)), 'the allowed handshakes stay open')
    for i in range(8):
        bots.expect(f'pend{i}', 'closed', 5)
    # frame flood
    bots.connect('flooder', 'seedFlooder', 'Flooder')
    bots.cmd('flood flooder 600 ping seq=1 sent_us=1')
    r = bots.expect('flooder', 'refuse', 4)
    check(r is not None and r['reason'] == WHY['protocol'] and 'too many' in r['detail'],
          'a frame flood is cut off')
    bots.drain()
    bots.send('alice', 'say', kind=0, to=0, text='still here')
    check(bots.expect('bob2', 'chat', 3, text='still here') is not None, 'the room keeps working after the attacks')
    # the request nobody answered
    check(bots.expect('tpw', 'tp_result', 30, target=ALICE, outcome=TP_TIMEOUT) is not None,
          'an unanswered teleport request times out after 30 s')
    bots.drain()
    srv.cmd('stop')
    check(bots.expect('alice', 'bye', 5, reason=WHY['shutdown']) is not None, 'stop says bye with reason shutdown')


def restart_a(srv, bots, exe_bot, port):
    srv.p.wait(10)
    srv.start()
    bots2 = Bots(exe_bot, port)
    try:
        a = bots2.connect('alice', 'seedAlice', 'Alice')
        check(a.get('ev') == 'admit' and a['rank'] == 2 and a['resumed'] == 0, 'after a restart the host is still host')
        b = bots2.connect('bob', 'seedBob', 'Bob')
        check(b.get('rank') == 1, 'after a restart the moderator is still moderator')
        bots2.cmd('connect dan seedDan Dan')
        bots2.expect('dan', 'admit', 4)
        accounts = (srv.data / 'accounts.txt').read_text()
        check(accounts.count('\n') >= 6 and ' Bob' in accounts, 'accounts.txt keeps every account')
        bots2.cmd('bye bob')
        dan_id = next((e[0]['account'] for e in bots2.events if e[0]['bot'] == 'dan' and e[0]['ev'] == 'admit'), 0)
        bots2.send('alice', 'mod_action', action=MOD_BAN, target=dan_id, rank=0, minutes=0, reason='persist')
        bots2.expect('dan', 'refuse', 3)
    finally:
        bots2.stop()
    srv.stop()
    srv.start()
    bots3 = Bots(exe_bot, port)
    try:
        r = bots3.connect('dan', 'seedDan', 'Dan')
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['banned'], 'a ban outlives a restart')
    finally:
        bots3.stop()


def run_b(exe_srv, exe_bot, tmp):
    port = free_port()
    srv = Server(exe_srv, tmp / 'b', port, dict(open=False, max_players=2, hold_seconds=0, handshake_seconds=5,
                                                 connects_per_minute=60, max_per_ip=64, max_pending_per_ip=32))
    bots = Bots(exe_bot, port)
    try:
        r = bots.connect('k1', 'seedK1', 'One')
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['unknown_key'], 'invite only: a new key needs a claim code')
        r = bots.connect('k1', 'seedK1', 'One', claim=hx('NOPE-NOPE-NOPE'))
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['bad_claim'], 'invite only: an unknown code is refused')
        mk = srv.mark()
        srv.cmd('claim 3')
        codes = []
        for _ in range(3):
            m = srv.wait_line(r'claim code: (\S+)', 3, mk)
            if m:
                codes.append(m.group(1))
                mk = srv.mark()
        codes = list(dict.fromkeys(re.findall(r'claim code: (\S+)', '\n'.join(srv.lines))))
        check(len(codes) == 3, 'the console makes claim codes')
        r = bots.connect('k1', 'seedK1', 'One', claim=hx(codes[0]))
        check(r.get('ev') == 'admit' and r['rank'] == 2, 'a claim code makes an account (the first is the host)')
        r = bots.connect('k2', 'seedK2', 'Two', claim=hx(codes[0]))
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['bad_claim'], 'a used claim code is refused')
        r = bots.connect('k2', 'seedK2', 'Two', claim=hx(codes[1]))
        check(r.get('ev') == 'admit', 'a second code works')
        r = bots.connect('k3', 'seedK3', 'Three', claim=hx(codes[2]))
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['full'], 'a full room refuses with full')
        bots.cmd('bye k2')
        time.sleep(0.3)
        r = bots.connect('k2', 'seedK2', 'Two')
        check(r.get('ev') == 'admit', 'a known key needs no code')
        check(codes[2] in (srv.data / 'claims.txt').read_text(), 'a code not used up stays in claims.txt')
        # new connections per minute from one address: 60 a minute, bursts of 15, some used above
        for i in range(30):
            bots.cmd(f'raw burst{i}')
        bots.sync()
        time.sleep(0.8)
        closed = sum(bots.expect(f'burst{i}', 'closed', 0.01) is not None for i in range(30))
        check(10 <= closed <= 25, f'connections over the per-minute limit are closed at once ({closed} of 30)')
    finally:
        bots.stop()
        srv.stop()


def run_c(exe_srv, exe_bot, tmp, keys):
    port = free_port()
    # the host is named in the settings: the bot "zed" from a seed whose key a first bot run tells us
    probe = Bots(exe_bot, 9)
    probe.cmd('connect z seedZed Zed')
    probe.expect('z', 'key', 3)
    zkey = probe.keys.get('z', '')
    probe.stop()
    srv = Server(exe_srv, tmp / 'c', port, dict(open=True, password='letmein', host=zkey, new_accounts_per_hour=5))
    bots = Bots(exe_bot, port)
    try:
        r = bots.connect('y', 'seedYan', 'Yan')
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['unknown_key'] and 'password' in r['detail'],
              'a password room refuses a new key without it')
        r = bots.connect('y', 'seedYan', 'Yan', claim=hx('letmeout'))
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['bad_claim'], 'a wrong password is refused')
        r = bots.connect('y', 'seedYan', 'Yan', claim=hx('letmein'))
        check(r.get('ev') == 'admit' and r['rank'] == 0, 'the right password makes an account; not the host')
        bots.cmd('bye y')
        time.sleep(0.3)
        check(bots.connect('y', 'seedYan', 'Yan').get('ev') == 'admit', 'a known key needs no password')
        r = bots.connect('z', 'seedZed', 'Zed', claim=hx('letmein'))
        check(r.get('ev') == 'admit' and r['rank'] == 2, 'the host named in the settings is host')
        check(bots.connect('q1', 'seedQ1', 'Q1', claim=hx('letmein')).get('ev') == 'admit', 'a fifth new account')
        r = bots.connect('q2', 'seedQ2', 'Q2', claim=hx('letmein'))
        check(r.get('ev') == 'refuse' and r['reason'] == WHY['full'] and 'Too many new accounts' in r['detail'],
              'tries at new accounts per hour from one address are limited (5 here)')
    finally:
        bots.stop()
        srv.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--release', action='store_true', help='test the optimised build instead of the checked one')
    ap.add_argument('--keep', action='store_true', help='keep the data folders')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='net_server_check_'))
    try:
        exe_srv, exe_bot = build_server.build(tmp / 'bin', debug=not a.release, werror=True)
        keys = {}
        t0 = time.time()
        run_a(exe_srv, exe_bot, tmp, keys)
        run_b(exe_srv, exe_bot, tmp)
        run_c(exe_srv, exe_bot, tmp, keys)
        print(f'{len(FAILS)} failures in {time.time() - t0:.0f} s')
        for f in FAILS:
            print('  ' + f)
    finally:
        if a.keep:
            print('data folders in', tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return min(len(FAILS), 100)


if __name__ == '__main__':
    sys.exit(main())
