# Hosting a TotK Recomp Multiplayer server

The server is one small program, `totk-server`, that runs on Windows or Linux. Players' games connect to it over
TCP. Release 1 carries the core of online play: the player list, chat, teams, teleport requests, the host's tools
and the positions behind the map and teleports. With a game that has it, the server also runs the actors add-on:
shared enemies, objects and items, each run by one game at a time ([online-wire.md](online-wire.md) 9.1).

It needs no game files and no account anywhere. It keeps everything in one data folder next to it.

| Part | Where |
|---|---|
| Source | `server/` (the program), `online/proto/` (the protocol it shares with the game) |
| Protocol | [online-wire.md](online-wire.md) |
| Test bot | `tools/net_bot.c` |
| Download | `TotKMP-Server-<version>-windows.zip` or `-linux.tar.gz` ([releasing.md](releasing.md)): the server, a start script, `server-data/server.toml` and this file as its README |
| Tests | `tests/net_server_check.py`, `tests/actor_lab.py`, `tests/actor_server_check.py` ([building.md](building.md), "Checks") |

## Build

You need Python 3.8 or newer and a C compiler: Clang or GCC.

**Windows.** Install MSYS2 to `C:\msys64` and its Clang (`pacman -S mingw-w64-ucrt-x86_64-clang` in the UCRT64
shell), then, from the repository:

```bash
python tools/build_server.py
```

The program lands in `build\server\totk-server.exe`, with `net_bot.exe` beside it.

**Linux.** Install `clang` or `gcc` and Python from your distribution, then:

```bash
python3 tools/build_server.py
```

`build/server/totk-server` is the program. `CC=gcc` picks GCC; without `CC` the script uses `cc`.

**By hand**, without the script (Linux; on Windows add `-lws2_32 -lbcrypt` and drop `-pthread` and the `-D` flags):

```bash
cc -std=c11 -O2 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -pthread -I online/proto server/sv_*.c online/proto/*.c online/third_party/monocypher/*.c -o totk-server -lm
```

## Run

```bash
build/server/totk-server --data server-data
```

| Option | What |
|---|---|
| `--data DIR` | The data folder (default `server-data` in the current folder). Made on the first start. |
| `--port N` | Listen on port N instead of the one in the settings. |
| `--bind ADDR` | Listen on ADDR instead of the one in the settings. |

On the first start the server writes `server.toml` with the default settings, makes its key and starts. Stop it,
edit `server.toml` if you like, and start it again. The console prints the address, the port and the server's key:

```
[2026-11-08 15:00:00] "TotK Recomp room" listening on 0.0.0.0 port 27120 (TCP), protocol 1, up to 8 players
[2026-11-08 15:00:00] server key 1a587f18e13e7c3593db695237ae79c39cfda233c8a0251b7d7c7b21db6b8221
```

Players connect to your address and port. A game can remember the server key the first time and warn if it ever
changes, so a stranger cannot pose as your server. Tell your players the key if they want to check it.

The first player to join becomes the host (written to `host.txt`), unless `host` in the settings names someone.
Join first yourself.

Type `stop` in the console, or press Ctrl+C, to end it: everyone gets a goodbye first. On Linux it can run under
systemd or in `screen`/`tmux`; without a console it still runs, and `kill` (SIGTERM) stops it cleanly.

## Ports and firewall

The server uses **one TCP port, 27120** unless you change `port`. Nothing else: no UDP, no second port.

- **Windows Firewall.** The first start shows a prompt: allow it on private networks (and public ones if your
  players come from the internet). Or, from an administrator prompt:
  ```bash
  netsh advfirewall firewall add rule name="TotK Recomp server" dir=in action=allow protocol=TCP localport=27120
  ```
- **Linux.** With ufw: `sudo ufw allow 27120/tcp`. With firewalld:
  `sudo firewall-cmd --add-port=27120/tcp --permanent && sudo firewall-cmd --reload`.
- **Router.** For players outside your home, forward TCP port 27120 on your router to the computer running the
  server, and give players your public address. Players on the same network use the computer's local address.
- **Hosted machines** (a VPS): open TCP 27120 in the provider's firewall as well.

To check it from another machine, run the bot there (see "Checking a server") or `nc -vz your.address 27120`.

## Who may join

Accounts are keys, not passwords. Each game makes its own key on its first start and proves it holds it on every
connection. The first time a key joins, the server makes an account for it (`accounts.txt`); after that the same
game is always let back in, under the same account, rank and bans.

| Settings | A new player |
|---|---|
| `open = true`, `password = ""` | joins. |
| `open = true`, `password = "..."` | types the password in the game's join screen (the claim field) once. |
| `open = false` | needs a one-time claim code: type `claim` in the console, give the code to the player. |

Players who already have an account never need the password or a code again. Each address may try at most
`new_accounts_per_hour` new accounts an hour, wrong passwords and codes included.

## Ranks and host tools

| Action | Player | Moderator | Host |
|---|---|---|---|
| chat, whisper, teams, teleport requests, map markers | yes | yes | yes |
| server message (announce) | no | yes | yes |
| kick, mute, unmute | no | players only | anyone but the host |
| ban, unban, make or unmake moderators | no | no | yes |
| summon (bring players to you without asking) | no | no | yes |

The host uses these from the game's online menu and radial menu. The console can do all of them, to anyone.
Every kick, ban, mute and rank change is shown to everyone as a system line and written to the log.

A ban or mute lasts the minutes given, or until lifted when none are. Bans and mutes are by key, so a new name does
not help a banned player, and both outlast a restart (`bans.txt`, `mutes.txt`).

## The console

Type commands into the window the server runs in. `<who>` is an account id (`12` or `#12`), a player's name, or a
full 64-character key.

| Command | What |
|---|---|
| `help` | the list of commands |
| `status` | name, port, players, who may join, teleport and positions settings, the server key |
| `players` | everyone here: id, name, rank, team, online or held, ping, address, start of the key |
| `say <text>` | a system line in everyone's chat |
| `announce <text>` | a server message on everyone's screen |
| `kick <who> [reason]` | |
| `ban <who> [minutes] [reason]` | no minutes: until lifted |
| `unban <who>` | also takes a bare key |
| `mute <who> [minutes] [reason]`, `unmute <who>` | |
| `mod <who>`, `unmod <who>` | make or unmake a moderator |
| `host <who>` | give the host's rank to another account (when `host` in the settings is empty) |
| `bans` | bans and mutes with their keys and how long they last |
| `claim [count]` | one-time claim codes for new players (kept in `claims.txt` until used) |
| `teleport on`, `teleport off` | allow or forbid teleport requests; every game is told at once |
| `positions all`, `positions team` | who sees whose position on the map; the host always sees everyone |
| `motd <text>` | the welcome message, until the next restart (put it in `server.toml` to keep it) |
| `reload` | re-read `bans.txt`, `mutes.txt` and `moderators.txt` after editing them |
| `stop` | say goodbye to everyone and exit |

Players have a few chat commands of their own: `/who`, `/teams`, `/motd` and `/help`.

## The data folder

| File | What |
|---|---|
| `server.toml` | the settings (below) |
| `server.key` | the server's secret key. Keep it private and back it up: a new key makes every game warn its player. |
| `accounts.txt` | account id, key and last name of every account |
| `host.txt` | the host's key, when the settings do not name one |
| `moderators.txt` | one key per line |
| `bans.txt`, `mutes.txt` | key, end time (seconds since 1970, 0 for until lifted), reason |
| `claims.txt` | unused one-time claim codes |
| `economy.txt` | rupees of every account, what each spent of the shared Lights and Korok seeds, and every Light and seed found |
| `server.log` | everything the console shows: joins, leaves, chat (unless `log_chat = false`), host tools, refusals |

All are plain text. Edit them only while the server is stopped (or use `reload` for bans, mutes and moderators):
the server rewrites the lists when they change. To move a server, copy the whole folder.

## Settings

`server.toml`, one `key = value` per line; text in quotes, `true` or `false`, numbers bare; `#` starts a comment.

| Setting | Default | What |
|---|---|---|
| `bind` | `"0.0.0.0"` | address to listen on: `"0.0.0.0"` every IPv4 address, `"::"` IPv4 and IPv6, `"127.0.0.1"` this computer only |
| `port` | `27120` | TCP port |
| `name` | `"TotK Recomp room"` | the room's name |
| `motd` | a greeting | the welcome message every player sees on joining |
| `max_players` | `8` | 1 to 32. The host can always get in. Held players count. |
| `open` | `true` | see "Who may join" |
| `password` | `""` | see "Who may join" |
| `host` | `""` | the host's key (64 hex characters, shown in the log when they join); empty: the first account |
| `hold_seconds` | `30` | how long a dropped player keeps their place, team and rank to reconnect |
| `teleport` | `true` | players may ask to teleport to each other |
| `positions` | `"all"` | `"all"` or `"team"`: whose positions players see |
| `handshake_seconds` | `5` | a new connection must finish signing in within this |
| `idle_seconds` | `10` | a session that sends nothing for this long ends (games send a ping each second) |
| `max_per_ip` | `4` | connections at once from one address |
| `max_pending_per_ip` | `4` | connections still signing in at once from one address |
| `connects_per_minute` | `30` | new connections per minute from one address (a quarter of them at once) |
| `new_accounts_per_hour` | `20` | tries at new accounts per hour from one address |
| `chat_burst`, `chat_per_second` | `5`, `1.0` | chat lines at once, then per second; more are dropped with a warning |
| `action_burst`, `action_per_second` | `10`, `2.0` | the same for team, teleport, marker and host requests |
| `frame_burst`, `frames_per_second` | `200`, `60.0` | messages of any kind; a game over this is disconnected |
| `log_chat` | `true` | write chat lines to `server.log` |
| `afk_seconds` | `300` | seconds without moving, chatting or acting before the player list marks a player away; `0` for never |
| `start_rupees` | `0` | rupees a new account starts with |
| `wallet_earn_max` | `5000` | the most rupees one request may add |
| `wallet_earn_per_minute` | `20000` | the most rupees an account may gain in a minute |

Friends behind one router share an address: raise `max_per_ip` and `max_pending_per_ip` to the number of them.

## What the server protects against

Every game is treated as hostile. Anything that breaks the protocol (a malformed, oversized, unencrypted, replayed
or out-of-order message, a message type this server does not offer) ends that connection at once with a reason;
nothing reaches the room first. Connections that sign in too slowly or go silent are closed. The limits above cap
connections, sign-ins, new accounts, chat, requests and messages per address or per player, and a game that does
not read what it is sent is cut off before it can make the server hold much memory for it. Every request is checked
against the sender's rank and the room's state, and a refused one changes nothing.

Signing in uses the protocol's handshake ([online-wire.md](online-wire.md), section 6): everything after the first
two messages is encrypted, and both sides prove their keys.

## Add-ons

The protocol has room for add-ons (shared saves, other players' bodies, companions and more), negotiated when a game
signs in. A game gets the add-ons it offers that this server has; the rest of its offer is answered with nothing.
This server has the economy and the sky clock, described next.

## The economy

A game that offers the `economy` add-on is told its rupees and the ledger when it joins, and again whenever they
change. The server decides; a game's change is a request.

- **Rupees** belong to an account (`economy.txt`). A request to change them is accepted or cut down: a spend that
  adds or an earn that takes does nothing, a take is at most the balance, a gain is at most `wallet_earn_max` and at
  most what is left of `wallet_earn_per_minute`, and the balance stays between 0 and 999999. The answer carries the
  change that was applied, so a game can put its pouch right.
- **Lights of Blessing and Korok seeds** are counted once for the whole room: a Light or seed any player finds,
  named by its game key, counts for every player's ledger. What a player spends is that player's own and never more
  than the room has found. A player may report 20 new finds at once and 2 each second; the ledger keeps at most 2048
  of each kind.

The `sky` add-on makes one Blood Moon for the room. The leader is the player who joined first of those here now; when
they leave, the next to have joined leads and everyone is told. Only the leader's clock (day, hour of the day, Blood
Moon due) is passed on; a clock from anyone else, or with an hour outside 0 to 24, is dropped. A game that joins is
told the clock and who leads (while no clock has come yet, `sky_set` carries the day 4294967295 and only names the
leader).

The player list marks a player away (`afk`) after `afk_seconds` without moving, chatting or a request such as a
team or teleport request, and for as long as the player's connection is held; the mark goes when they act again.

## Checking a server

`net_bot` is a headless test client. It reads commands and prints what it receives as JSON lines:

```bash
build/server/net_bot --host your.address --port 27120
```

```
connect me myseed Tester
send me say kind=0 to=0 text=68656c6c6f
quit
```

`connect` signs in with a key made from the word `myseed` and prints `admit` (or `refuse` with the reason); the
`say` sends "hello" (text is written in hex). The comment at the top of `tools/net_bot.c` lists every command. The
bot makes an account like any player, so use it on your own server only.
