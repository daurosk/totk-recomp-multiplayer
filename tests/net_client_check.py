"""The game's online client (runtime/net/net_client.c) against the real server and headless bots, on localhost only.

    python tests/net_client_check.py [--keep]

Builds the server and the bot with tools/build_server.py and tests/net_client_main.c (the client with the runtime
replaced by small shims, no game) with MSYS2 Clang or $CC, then:

- A, an open room: the client joins first (the host) and two bots join; the player list, positions both ways, chat
  on every channel (all, team, whisper, /me, system lines, server messages), teams (create, invite, an invite to the
  client refused and accepted, leave), teleports (the client asks and is accepted or refused, a bot asks and the
  client accepts or refuses), every host tool (server message, summon, mute and unmute, moderator granted and taken,
  kick, ban), a reconnect within the hold that the others do not see, a kick and a ban of the client from the console,
  the server going away (lost connection after the hold) and another server on the same address (a changed key).
- B, a room with a password and two places: no password, a wrong one, the right one, then a full room.
- A fake server of another protocol version, the client with no address, and TOTK_NET_FAKE=1.

Each check prints ok or FAIL; the exit code is the number of failures. docs/online-ui.md describes the client.
"""
import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tests'))
import build_server  # noqa: E402
import net_server_check as sc  # noqa: E402

check, hx, free_port, Server, Bots, WHY = sc.check, sc.hx, sc.free_port, sc.Server, sc.Bots, sc.WHY

# net_client.h
OFFLINE, CONNECTING, ONLINE = range(3)
EV = dict(joined=1, left=2, team_invite=3, tp_request=4, tp_answer=5, tp_go=6, summon=7, kicked=8, muted=9, role=10,
          server_message=11, state=12)
CH_ALL, CH_TEAM, CH_WHISPER, CH_SYSTEM = range(4)
H_SUMMON, H_KICK, H_MUTE, H_BAN, H_MESSAGE, H_GRANT, H_REVOKE = range(1, 8)
ROLE_PLAYER, ROLE_MOD, ROLE_HOST = range(3)


def build_client(out):
    """tests/net_client_main.c with the client, the fake, the wire and Monocypher; returns the program's path."""
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ('net_client_main' + ('.exe' if sys.platform == 'win32' else ''))
    srcs = [ROOT / 'runtime/net/net_client.c', ROOT / 'runtime/net/net_stub.c', ROOT / 'tests/net_client_main.c']
    srcs += sorted((ROOT / 'online/proto').glob('*.c')) + sorted((ROOT / 'online/third_party/monocypher').glob('*.c'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-O1', '-g', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'online/proto')]
    libs = ['-lws2_32', '-lbcrypt'] if sys.platform == 'win32' else ['-pthread']
    p = subprocess.run(build_server.compiler() + flags + [str(s) for s in srcs] + ['-o', str(exe)] + libs,
                       capture_output=True, text=True, env=build_server.env())
    if p.returncode:
        sys.exit(f'building the client failed:\n{p.stderr}')
    ours = [ln for ln in p.stderr.splitlines() if 'monocypher' not in ln and ln.strip()]
    check(not any('warning' in ln for ln in ours), 'the client builds without warnings' + ('\n' + '\n'.join(ours) if ours else ''))
    return exe


class Client:
    """One net_client_main process: the game's client as the UI sees it."""

    def __init__(self, exe, profile, env=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
        e.update(env or {})
        self.p = subprocess.Popen([str(exe), '--profile', str(profile)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, encoding='utf-8', env=e,
                                  creationflags=sc.NO_WINDOW)
        self.events, self.cv = [], threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            with self.cv:
                self.events.append([ev, False])
                self.cv.notify_all()

    def cmd(self, line):
        with self.cv:   # a command's return value is the next one: the earlier ones are spent
            for item in self.events:
                if item[0]['ev'] == 'ret':
                    item[1] = True
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()

    def expect(self, kind, timeout=5.0, pred=None):
        end = time.time() + timeout
        with self.cv:
            while True:
                for item in self.events:
                    if not item[1] and item[0]['ev'] == kind and (pred is None or pred(item[0])):
                        item[1] = True
                        return item[0]
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def event(self, name, timeout=5.0, pred=None):
        return self.expect('event', timeout, lambda e: e['type'] == EV[name] and (pred is None or pred(e)))

    def chat(self, timeout=5.0, pred=None):
        return self.expect('chat', timeout, pred)

    def none(self, kind, within=1.0, pred=None):
        return self.expect(kind, within, pred) is None

    def ret(self, cmd, timeout=5.0):
        ev = self.expect('ret', timeout, lambda e: e['cmd'] == cmd)
        return ev['r'] if ev else None

    def drain(self, settle=0.3):
        time.sleep(settle)
        with self.cv:
            for item in self.events:
                item[1] = True

    def dump(self):
        self.cmd('dump')
        return self.expect('dump', 5) or {}

    def wait_dump(self, pred, timeout=5.0):
        """The first dump that satisfies pred, polled; the last one when none did."""
        end = time.time() + timeout
        d = {}
        while time.time() < end:
            d = self.dump()
            try:
                if pred(d):
                    return d
            except (KeyError, StopIteration, TypeError):
                pass
            time.sleep(0.2)
        return None

    def online(self, address, name, password='', timeout=8):
        self.cmd(f'connect {address} {name} {password}'.rstrip())
        return self.event('state', timeout, lambda e: e['state'] in (ONLINE, OFFLINE))

    def stop(self):
        try:
            self.cmd('quit')
            self.p.wait(5)
        except Exception:
            self.p.kill()


def player(d, pid):
    return next((p for p in d.get('players', []) if p['id'] == pid), None)


def named(d, name):
    return next((p for p in d.get('players', []) if p['name'] == name), None)


def team_named(d, name):
    return next((t for t in d.get('teams', []) if t['name'] == name), None)


def run_a(exe_srv, exe_bot, exe_cl, tmp):
    port = free_port()
    addr = f'127.0.0.1:{port}'
    settings = dict(max_players=8, open=True, hold_seconds=4, max_per_ip=64, max_pending_per_ip=16,
                    connects_per_minute=100000, new_accounts_per_hour=1000, chat_burst=100, chat_per_second=100.0,
                    action_burst=100, action_per_second=100.0, motd='Welcome to the test room')
    srv = Server(exe_srv, tmp / 'a', port, settings)
    bots = Bots(exe_bot, port)
    cl = Client(exe_cl, tmp / 'profile')
    try:
        body_a(srv, bots, cl, addr)
        ends_a(srv, bots, cl, addr, exe_srv, tmp, port, settings)
    finally:
        cl.stop()
        bots.stop()
        srv.stop()
    key = tmp / 'profile' / 'online' / 'identity.key'
    check(key.is_file() and key.stat().st_size == 32, 'the identity key is made in <profile>/online/identity.key')


def body_a(srv, bots, cl, addr):
    # ---- join ----
    d = cl.dump()
    check(d.get('enabled') == 0 and d.get('state') == OFFLINE, 'with no address the client is disabled and offline')
    cl.cmd(f'connect {addr} Cal')
    check(cl.ret('connect') == 1, 'net_connect starts the attempt')
    check(cl.event('state', 3, lambda e: e['state'] == CONNECTING) is not None, 'the state goes to connecting')
    ev = cl.event('state', 8, lambda e: e['state'] in (ONLINE, OFFLINE))
    check(ev is not None and ev['state'] == ONLINE, f'the client joins ({ev and ev["text"]})')
    d = cl.dump()
    me = d.get('self', 0)
    mine = player(d, me)
    check(me != 0 and mine is not None and mine['name'] == 'Cal' and mine['role'] == ROLE_HOST,
          'the client is in the list under its name, as the host (first account)')
    check(cl.chat(3, lambda c: c['channel'] == CH_SYSTEM and 'Welcome to the test room' in c['text']) is not None,
          'the message of the day arrives as a system line')
    a = bots.connect('ann', 'seedAnn', 'Ann')
    b = bots.connect('bo', 'seedBo', 'Bo')
    ANN, BO = a.get('account', 0), b.get('account', 0)
    check(cl.event('joined', 3, lambda e: e['player'] == ANN and e['name'] == 'Ann') is not None, 'Ann joining is an event')
    check(cl.event('joined', 3, lambda e: e['player'] == BO and e['name'] == 'Bo') is not None, 'Bo joining is an event')
    d = cl.wait_dump(lambda d: len(d['players']) == 3)
    check(d is not None and named(d, 'Ann') and named(d, 'Bo'), 'the list holds the client and both bots')
    check(d is not None and named(d, 'Ann')['character'] == 'Link' and named(d, 'Ann')['role'] == ROLE_PLAYER,
          'list rows carry character and role')
    check(bots.expect('ann', 'roster', 3, pred=lambda e: sc.roster_row(e, me)) is not None, 'the bots see the client')

    # ---- positions ----
    cl.cmd('pos 100 5 200')
    for bot in ('ann', 'bo'):
        bots.cmd(f'send {bot} area where.state=1')
    bots.send('ann', 'position', scene=0, pos=(10, 20, 30), yaw=0)
    bots.send('bo', 'position', scene=0, pos=(-40, 1, 60), yaw=0)
    p = bots.expect('ann', 'positions', 4, pred=lambda e: any(s['account'] == me and s['pos'] == [100, 5, 200]
                                                              for s in e['spots']))
    check(p is not None, 'the client reports its position (the others get it within about a second)')
    d = cl.wait_dump(lambda d: player(d, ANN)['pos'] == [10, 20, 30] and player(d, BO)['pos'] == [-40, 1, 60])
    check(d is not None, "the others' positions reach the client's list")
    check(d is not None and player(d, ANN)['area'] == 'Surface (10, 30)', 'a place name from the position')
    check(d is not None and player(d, me)['pos'] == [100, 5, 200], "the client's own row shows the local position")
    cl.cmd('pos 400 5 200')
    p = bots.expect('ann', 'positions', 3, pred=lambda e: any(s['account'] == me and s['pos'][0] == 400 for s in e['spots']))
    check(p is not None, 'a jump of 300 m is reported')
    cl.cmd('pos 100 5 200')

    # ---- chat ----
    bots.drain()
    cl.drain()
    cl.cmd('say all hello everyone')
    check(bots.expect('ann', 'chat', pred=lambda e: e['kind'] == 0 and e['from'] == me and e['text'] == 'hello everyone')
          is not None, 'all: a bot gets the line from the client')
    check(cl.chat(pred=lambda c: c['channel'] == CH_ALL and c['from'] == me and c['text'] == 'hello everyone') is not None,
          'all: the client sees its own line')
    bots.send('ann', 'say', kind=0, to=0, text='hi Cal')
    check(cl.chat(pred=lambda c: c['channel'] == CH_ALL and c['from'] == ANN and c['from_name'] == 'Ann'
                  and c['text'] == 'hi Cal') is not None, "all: the client gets a bot's line with its name")
    cl.cmd('say me waves')
    check(bots.expect('bo', 'chat', pred=lambda e: e['kind'] == 3 and e['text'] == 'waves') is not None, '/me goes as an action')
    check(cl.chat(pred=lambda c: c['channel'] == CH_SYSTEM and c['text'] == 'Cal waves') is not None,
          '/me shows as an action line')
    cl.cmd('whisper bo psst')
    check(bots.expect('bo', 'chat', pred=lambda e: e['kind'] == 2 and e['text'] == 'psst') is not None, 'whisper: Bo gets it')
    check(bots.none('ann', 'chat', 1.0, pred=lambda e: e['text'] == 'psst'), 'whisper: Ann does not')
    check(cl.chat(pred=lambda c: c['channel'] == CH_WHISPER and c['from'] == me and c['to_name'] == 'Bo') is not None,
          'whisper: the client sees its line to Bo')
    bots.send('bo', 'say', kind=2, to=me, text='psst back')
    check(cl.chat(pred=lambda c: c['channel'] == CH_WHISPER and c['from'] == BO and c['text'] == 'psst back'
                  and c['to_name'] == 'Cal') is not None, 'whisper: the client gets one from Bo')
    cl.cmd('whisper nobody hi')
    check(cl.ret('whisper') == 0 and cl.chat(pred=lambda c: 'No player called' in c['text']) is not None,
          'whisper: an unknown name is answered locally')
    srv.cmd('say Maintenance at noon')
    check(cl.chat(pred=lambda c: c['channel'] == CH_SYSTEM and c['from'] == 0 and 'Maintenance at noon' in c['text'])
          is not None, "system: the console's line")
    cl.cmd('say all /who')
    check(cl.chat(pred=lambda c: c['channel'] == CH_SYSTEM and 'Ann' in c['text'] and 'Bo' in c['text']) is not None,
          'a / command is answered by the server')
    cl.cmd('say all ' + 'x' * 300)
    check(bots.expect('ann', 'chat', pred=lambda e: e['text'] == 'x' * 256) is not None,
          'an over-long line is cut to the wire limit, not refused')

    # ---- teams ----
    cl.cmd('say team nobody')
    check(cl.ret('say') == 0 and cl.chat(pred=lambda c: 'not in a team' in c['text']) is not None,
          'team chat without a team is answered locally')
    cl.cmd('teamcreate Reds')
    d = cl.wait_dump(lambda d: team_named(d, 'Reds') and player(d, me)['team'] == team_named(d, 'Reds')['id'])
    check(d is not None and team_named(d, 'Reds')['leader'] == me, 'the client makes a team and leads it')
    RED = team_named(d, 'Reds')['id'] if d else 0
    cl.cmd(f'invite {ANN}')
    inv = bots.expect('ann', 'team_invited', pred=lambda e: e['team'] == RED and e['from'] == me)
    check(inv is not None and inv['name'] == 'Reds', 'the client invites Ann')
    bots.send('ann', 'team_join', team=RED)
    check(cl.wait_dump(lambda d: player(d, ANN)['team'] == RED) is not None, 'Ann joins the team')
    bots.drain()
    cl.cmd('say team hi team')
    check(bots.expect('ann', 'chat', pred=lambda e: e['kind'] == 1 and e['text'] == 'hi team') is not None,
          'team: Ann gets the line')
    check(bots.none('bo', 'chat', 1.0, pred=lambda e: e['text'] == 'hi team'), 'team: Bo does not')
    bots.send('ann', 'say', kind=1, to=0, text='team ok')
    check(cl.chat(pred=lambda c: c['channel'] == CH_TEAM and c['from'] == ANN and c['text'] == 'team ok') is not None,
          'team: the client gets a team line')
    bots.send('bo', 'team_create', name='Blues', open=0)
    d = cl.wait_dump(lambda d: team_named(d, 'Blues'))
    BLUE = team_named(d, 'Blues')['id'] if d else 0
    bots.send('bo', 'team_invite', target=me)
    ev = cl.event('team_invite', pred=lambda e: e['player'] == BO)
    check(ev is not None and ev['request'] == BLUE and ev['text'] == 'Blues' and ev['name'] == 'Bo',
          'an invite to the client is an event with the team')
    cl.cmd(f'teamanswer {BLUE} 0')
    check(cl.chat(pred=lambda c: 'declined' in c['text']) is not None, 'refusing an invite says so')
    time.sleep(0.5)
    check(player(cl.dump(), me)['team'] == RED, 'refusing an invite changes nothing')
    bots.send('bo', 'team_invite', target=me)
    check(cl.event('team_invite', pred=lambda e: e['player'] == BO) is not None, 'a second invite')
    cl.cmd(f'teamanswer {BLUE} 1')
    check(cl.wait_dump(lambda d: player(d, me)['team'] == BLUE) is not None, 'accepting an invite joins that team')
    cl.cmd('leave')
    check(cl.wait_dump(lambda d: player(d, me)['team'] == 0) is not None, 'the client leaves the team')
    d = cl.dump()
    check(team_named(d, 'Reds') is not None and team_named(d, 'Reds')['leader'] == ANN,
          'the lead of the first team passed to Ann when the client left it')

    # ---- teleports ----
    bots.drain()
    cl.drain()
    cl.cmd(f'tp {ANN}')
    check(cl.ret('tp') == 1, 'a teleport request goes out')
    ask = bots.expect('ann', 'tp_asked', pred=lambda e: e['from'] == me)
    check(ask is not None and ask['nickname'] == 'Cal', 'Ann is asked')
    bots.send('ann', 'tp_reply', **{'from': me}, accept=1)
    ev = cl.event('tp_answer', pred=lambda e: e['player'] == ANN)
    check(ev is not None and ev['accepted'] == 1 and ev['name'] == 'Ann', 'accepted: the answer is an event')
    go = cl.event('tp_go', pred=lambda e: e['player'] == ANN)
    check(go is not None and go['pos'] == [10, 20, 30] and go['name'] == 'Ann', "accepted: the move to Ann's position")
    cl.cmd(f'tp {BO}')
    check(bots.expect('bo', 'tp_asked', pred=lambda e: e['from'] == me) is not None, 'Bo is asked')
    bots.send('bo', 'tp_reply', **{'from': me}, accept=0)
    ev = cl.event('tp_answer', pred=lambda e: e['player'] == BO)
    check(ev is not None and ev['accepted'] == 0, 'refused: the answer is an event')
    check(cl.none('event', 1.0, lambda e: e['type'] == EV['tp_go']), 'refused: no move')
    bots.send('ann', 'tp_ask', target=me)
    ev = cl.event('tp_request', pred=lambda e: e['player'] == ANN)
    check(ev is not None and ev['request'] == ANN and ev['name'] == 'Ann', 'a request to the client is an event')
    cl.cmd(f'tpanswer {ANN} 1')
    go = bots.expect('ann', 'go_to', pred=lambda e: e['target'] == me)
    check(go is not None and go['pos'] == [100, 5, 200] and go['cause'] == 0, "the client accepts: Ann goes to the client")
    check(bots.expect('ann', 'tp_result', pred=lambda e: e['outcome'] == sc.TP_ACCEPTED) is not None, 'Ann hears yes')
    bots.send('bo', 'tp_ask', target=me)
    check(cl.event('tp_request', pred=lambda e: e['player'] == BO) is not None, 'a second request')
    cl.cmd(f'tpanswer {BO} 0')
    check(bots.expect('bo', 'tp_result', pred=lambda e: e['outcome'] == sc.TP_REFUSED) is not None, 'the client refuses')
    check(bots.none('bo', 'go_to', 0.8), 'refused: Bo does not move')
    cl.cmd(f'tp {me}')
    check(cl.ret('tp') == 0, 'a request to oneself is not sent')

    # ---- host tools ----
    bots.drain()
    cl.drain()
    cl.cmd(f'host {H_MESSAGE} 0 Restart in five minutes')
    check(cl.ret('host') == 1, 'server message: sent')
    check(bots.expect('ann', 'notice', pred=lambda e: e['kind'] == 3 and e['text'] == 'Restart in five minutes')
          is not None, 'server message: the bots get the announcement')
    check(cl.event('server_message', pred=lambda e: e['text'] == 'Restart in five minutes') is not None,
          'server message: the client gets the event')
    check(cl.chat(pred=lambda c: c['channel'] == CH_SYSTEM and c['text'] == 'Restart in five minutes') is not None,
          'server message: and the system line')
    cl.cmd(f'host {H_SUMMON} 0')
    for bot in ('ann', 'bo'):
        go = bots.expect(bot, 'go_to', pred=lambda e: e['cause'] == 1)
        check(go is not None and go['pos'] == [100, 5, 200] and go['target'] == me, f'summon: {bot} goes to the host')
    cl.cmd(f'host {H_MUTE} {BO} 1')
    check(cl.wait_dump(lambda d: player(d, BO)['muted'] == 1) is not None, 'mute: Bo is muted in the list')
    check(cl.chat(pred=lambda c: 'muted Bo' in c['text']) is not None, 'mute: the system line')
    bots.send('bo', 'say', kind=0, to=0, text='can you hear me')
    check(cl.none('chat', 1.0, lambda c: c['text'] == 'can you hear me'), "mute: Bo's line does not arrive")
    cl.cmd(f'host {H_MUTE} {BO} 0')
    check(cl.wait_dump(lambda d: player(d, BO)['muted'] == 0) is not None, 'unmute: Bo is no longer muted')
    cl.cmd(f'host {H_GRANT} {ANN}')
    check(cl.wait_dump(lambda d: player(d, ANN)['role'] == ROLE_MOD) is not None, 'grant: Ann is a moderator')
    check(bots.expect('ann', 'roster', 3, pred=lambda e: sc.roster_row(e, ANN) and sc.roster_row(e, ANN)['rank'] == 1)
          is not None, "grant: Ann's own list says so")
    cl.cmd(f'host {H_REVOKE} {ANN}')
    check(cl.wait_dump(lambda d: player(d, ANN)['role'] == ROLE_PLAYER) is not None, 'revoke: Ann is a player again')
    cl.cmd(f'host {H_REVOKE} {ANN}')
    check(cl.chat(pred=lambda c: 'Nothing to change' in c['text']) is not None, 'a host tool that changes nothing says so')
    cl.cmd(f'host {H_KICK} {BO} spamming')
    r = bots.expect('bo', 'refuse', 4)
    check(r is not None and r['reason'] == WHY['kicked'] and 'spamming' in r['detail'], 'kick: Bo is kicked with the reason')
    check(cl.event('left', pred=lambda e: e['player'] == BO and e['name'] == 'Bo') is not None, 'kick: Bo leaving is an event')
    check(cl.wait_dump(lambda d: player(d, BO) is None) is not None, 'kick: Bo is off the list')
    cl.cmd(f'host {H_BAN} {ANN} cheating')
    r = bots.expect('ann', 'refuse', 4)
    check(r is not None and r['reason'] == WHY['banned'], 'ban: Ann is banned')
    check(cl.event('left', pred=lambda e: e['player'] == ANN) is not None, 'ban: Ann leaving is an event')
    r = bots.connect('ann', 'seedAnn', 'Ann')
    check(r.get('ev') == 'refuse' and r['reason'] == WHY['banned'], 'ban: Ann cannot come back')
    srv.cmd('unban Ann')
    time.sleep(0.3)
    r = bots.connect('ann', 'seedAnn', 'Ann')
    check(r.get('ev') == 'admit', 'unbanned from the console, Ann is back')
    cl.cmd(f'host {H_KICK} {me} self')
    check(cl.ret('host') == 0, 'the client does not kick itself')

    # ---- a player's view: the host tools are refused locally ----
    # (the client is the host here; a moderator's limits are the server's, tests/net_server_check.py)

    # ---- reconnect within the hold ----
    bots.connect('bo', 'seedBo', 'Bo')
    bots.drain()
    cl.drain()
    cl.cmd('drop')
    check(cl.event('state', 4, lambda e: e['state'] == CONNECTING) is not None, 'a dropped connection: reconnecting')
    ev = cl.event('state', 8, lambda e: e['state'] == ONLINE)
    check(ev is not None, 'a dropped connection: back online within the hold')
    check(cl.chat(3, lambda c: c['text'] == 'Reconnected.') is not None, 'a dropped connection: a system line says so')
    d = cl.dump()
    check(d.get('self') == me and player(d, me) and player(d, me)['role'] == ROLE_HOST,
          'a dropped connection: same account and rank')
    check(bots.none('bo', 'depart', 1.0), 'a dropped connection: the others see no departure')
    cl.cmd('say all still here')
    check(bots.expect('bo', 'chat', pred=lambda e: e['text'] == 'still here') is not None, 'chat works after the reconnect')


def ends_a(srv, bots, cl, addr, exe_srv, tmp, port, settings):
    # ---- kicked from the console ----
    cl.drain()
    srv.cmd('kick Cal see you later')
    ev = cl.event('kicked', 4)
    check(ev is not None and 'see you later' in ev['text'], f'kicked: the event carries the reason ({ev and ev["text"]})')
    check(cl.event('state', 4, lambda e: e['state'] == OFFLINE) is not None, 'kicked: offline')
    time.sleep(2)
    d = cl.dump()
    check(d.get('state') == OFFLINE and 'see you later' in d.get('status', ''), 'kicked: no reconnect, the status says why')
    check(d.get('players') == [], 'kicked: the list is empty')
    ev = cl.online(addr, 'Cal')
    check(ev is not None and ev['state'] == ONLINE, 'after a kick the client can join again')
    # ---- disconnect ----
    bots.drain()
    cl.cmd('disconnect')
    ev = cl.event('state', 4, lambda e: e['state'] == OFFLINE)
    check(ev is not None and ev['text'] == '', 'disconnect: offline, with no complaint')
    dep = bots.expect('ann', 'depart', 4)
    check(dep is not None and dep['reason'] == WHY['none'], 'disconnect: the others see a goodbye')
    # ---- the server goes away ----
    ev = cl.online(addr, 'Cal')
    check(ev is not None and ev['state'] == ONLINE, 'online again before the server goes away')
    cl.drain()
    srv.p.kill()
    srv.p.wait()
    t0 = time.time()
    check(cl.event('state', 12, lambda e: e['state'] == CONNECTING) is not None, 'server gone: reconnecting')
    ev = cl.event('state', 15, lambda e: e['state'] == OFFLINE)
    took = time.time() - t0
    check(ev is not None and 'Lost the connection' in ev['text'], f'server gone: lost connection ({ev and ev["text"]})')
    check(took < 30, f'server gone: gives up after about the hold ({took:.0f} s)')
    # ---- another server on the same address ----
    other = Server(exe_srv, tmp / 'a2', port, settings)
    try:
        ev = cl.online(addr, 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'another key' in ev['text'],
              f'a different server key on a known address is refused ({ev and ev["text"]})')
    finally:
        other.stop()


def run_b(exe_srv, exe_bot, exe_cl, tmp):
    port = free_port()
    addr = f'127.0.0.1:{port}'
    srv = Server(exe_srv, tmp / 'b', port, dict(open=True, password='letmein', max_players=2, hold_seconds=4,
                                                max_per_ip=64, max_pending_per_ip=16, connects_per_minute=100000,
                                                new_accounts_per_hour=1000))
    bots = Bots(exe_bot, port)
    cl = Client(exe_cl, tmp / 'profile_b')
    try:
        h = bots.connect('h', 'seedHost', 'Hana', claim=hx('letmein'))
        check(h.get('ev') == 'admit', 'B: a bot joins first with the password')
        ev = cl.online(addr, 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'needs a password' in ev['text'],
              f'B: no password: a plain message ({ev and ev["text"]})')
        ev = cl.online(addr, 'Cal', 'wrong')
        check(ev is not None and ev['state'] == OFFLINE and ev['text'] == 'Wrong password or invite code.',
              f'B: a wrong password: a plain message ({ev and ev["text"]})')
        ev = cl.online(addr, 'Cal', 'letmein')
        check(ev is not None and ev['state'] == ONLINE, 'B: the right password joins')
        d = cl.dump()
        HANA = (named(d, 'Hana') or {}).get('id', 0)
        check((player(d, d.get('self')) or {}).get('role') == ROLE_PLAYER, 'B: the client is a player here')
        for act, text in ((H_KICK, 'x'), (H_MESSAGE, 'hi'), (H_SUMMON, ''), (H_MUTE, '1')):
            cl.cmd(f'host {act} {HANA} {text}')
            check(cl.ret('host') == 0, f"B: a player's host tool {act} is refused before it is sent")
        # banned (the server never bans the host, so this is a player's ban)
        cl.drain()
        srv.cmd('ban Cal 5 griefing')
        ev = cl.event('kicked', 4)
        check(ev is not None and 'griefing' in ev['text'],
              f'banned while playing: the event carries the reason ({ev and ev["text"]})')
        check(cl.event('state', 4, lambda e: e['state'] == OFFLINE) is not None, 'banned while playing: offline')
        ev = cl.online(addr, 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'banned' in ev['text'] and 'griefing' in ev['text'],
              f'banned: joining is refused with a plain message ({ev and ev["text"]})')
        srv.cmd('unban Cal')
        time.sleep(0.3)
        ev = cl.online(addr, 'Cal')
        check(ev is not None and ev['state'] == ONLINE, 'B: unbanned, a known key joins without the password')
        cl.cmd('disconnect')
        cl.event('state', 4, lambda e: e['state'] == OFFLINE)
        bots.expect('h', 'depart', 4)
        x = bots.connect('x', 'seedX', 'Xi', claim=hx('letmein'))
        check(x.get('ev') == 'admit', 'B: a second bot takes the last place')
        ev = cl.online(addr, 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'full' in ev['text'],
              f'B: a full room: a plain message ({ev and ev["text"]})')
        d = cl.dump()
        check('full' in d.get('status', ''), 'B: the status keeps the reason')
    finally:
        cl.stop()
        bots.stop()
        srv.stop()


def fake_server(port, stop):
    """Answers every greet with refuse OW_WHY_VERSION from a server of protocol 2, in the clear, as section 10 says."""
    ls = socket.socket()
    ls.bind(('127.0.0.1', port))
    ls.listen(4)
    ls.settimeout(0.2)
    while not stop.is_set():
        try:
            c, _ = ls.accept()
        except socket.timeout:
            continue
        c.settimeout(3)
        try:
            c.recv(4096)
            build = b'totk-server 0.9'
            payload = struct.pack('<HH', WHY['version'], 2) + struct.pack('<H', len(build)) + build + struct.pack('<H', 0)
            c.sendall(struct.pack('<IH', len(payload), 2) + payload)
            time.sleep(0.2)
        except OSError:
            pass
        c.close()
    ls.close()


def run_c(exe_cl, tmp):
    port = free_port()
    stop = threading.Event()
    t = threading.Thread(target=fake_server, args=(port, stop), daemon=True)
    t.start()
    cl = Client(exe_cl, tmp / 'profile_c', {'TOTK_NET_ADDRESS': f'127.0.0.1:{port}'})
    try:
        check(cl.dump().get('enabled') == 1, 'an address makes the client enabled')
        ev = cl.online(f'127.0.0.1:{port}', 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'another version' in ev['text'] and 'protocol 2' in ev['text'],
              f'another protocol version: a plain message ({ev and ev["text"]})')
        cl.cmd('connect')
        check(cl.ret('connect') == 0, 'net_connect with no address does nothing')
        ev = cl.online(f'127.0.0.1:{free_port()}', 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'Could not reach' in ev['text'],
              f'nobody listening: a plain message ({ev and ev["text"]})')
        ev = cl.online('no-such-host.invalid:27120', 'Cal')
        check(ev is not None and ev['state'] == OFFLINE and 'Could not find' in ev['text'],
              f'an unknown host name: a plain message ({ev and ev["text"]})')
    finally:
        stop.set()
        cl.stop()
    fk = Client(exe_cl, tmp / 'profile_f', {'TOTK_NET_FAKE': '1'})
    try:
        check(fk.dump().get('enabled') == 1, 'TOTK_NET_FAKE=1: enabled without an address')
        ev = fk.online('', 'Cal')
        d = fk.dump()
        check(ev is not None and ev['state'] == ONLINE and named(d, 'Alice') is not None,
              'TOTK_NET_FAKE=1: the invented server plays')
    finally:
        fk.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--keep', action='store_true', help='keep the data folders')
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix='net_client_check_'))
    try:
        exe_srv, exe_bot = build_server.build(tmp / 'bin', debug=True, werror=True)
        exe_cl = build_client(tmp / 'bin')
        t0 = time.time()
        run_a(exe_srv, exe_bot, exe_cl, tmp)
        run_b(exe_srv, exe_bot, exe_cl, tmp)
        run_c(exe_cl, tmp)
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
