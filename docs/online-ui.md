# Online UI

The in-game side of online play for release 1: a chat box, a player list, toasts, a wheel menu and an Online tab in
the F1 menu. It is drawn by the menu's own Dear ImGui layer (`runtime/ui/`) and talks to the world only through
`runtime/net/net_client.h`. Nothing else is synchronised in release 1: no world, enemies, loot, saves or other players'
bodies.

| File | What |
|---|---|
| `runtime/net/net_client.h` | The whole contract with the network side: players, teams, chat, teleport requests, host actions, events. |
| `runtime/net/net_stub.c` | An offline implementation. Without `TOTK_NET_FAKE=1` nothing is online and the UI stays hidden. |
| `runtime/ui/online_ui.cpp` | Hotkeys, events, toasts, confirmation dialog, chat box, player list. |
| `runtime/ui/online_wheel.cpp` | The wheel menu and its add-on slices. |
| `runtime/ui/online_tab.cpp` | The Online tab of the F1 menu. |
| `runtime/teleport.c` | Moves the local player. |

The real client implements the same functions as `net_stub.c` and replaces it in the build.

## Trying it without a server

`TOTK_NET_FAKE=1` plays an invented server: you and four players (Alice, Bao, Cleo, Dax) with characters, areas, pings,
a team ("Red Squad"), scripted chat, a team invite and a teleport request that arrive a few seconds after the game
starts, a player joining and one leaving, and replies to whispers. `TOTK_NET_FAKE_ROLE=player|mod|host` (default `host`)
sets your role; the host wheel slice only exists for `mod` and `host`. The invented players stand within 100 m of you,
so teleporting to one lands on ground the game has loaded. With the switch on, F7 and F6 hop the player 25 m east or
west, which is how the teleport is tried alone in single player.

## Screens

**Chat box** (bottom left). The chat key (default Enter) opens it and takes the keyboard; Enter sends, Esc closes, Tab
changes the channel. Three channels: all, team and whisper (`/w name message`; `/r message` answers the last whisper;
`/t` and `/a` pick a channel for one line). System lines are gold, team lines green, whispers violet. While closed it
shows the latest lines and fades out a few seconds after the last one (`net_chat_fade`); open, it scrolls with the mouse
wheel. The game gets no input while you type.

**Player list.** The list key (default P), or Team > Players in the wheel, shows name, character, area, team and ping,
with HOST and MOD badges.

**Toasts** (right side). Joins, leaves, answers to your teleport requests and so on fade out by themselves. Teleport
requests and team invites wait 30 s for an answer: Y accepts, N refuses (rebindable), or click the buttons while the
cursor is free. An unanswered request counts as a refusal. With the wheel open, X (pad) accepts and Y refuses.

**Online tab** (F1 menu). Name, server address, connect on start, the keys of the chat, the wheel, the list and the
answers (click a button, press the new key; Delete clears, Esc cancels), the wheel's controller buttons (click, hold the
buttons together, let go), the wheel's size and the chat's fade time and line count. Everything is written to
`totk.ini`, section `[online]`.

## The wheel menu

Hold the wheel key to open it, aim, let go to choose.

| Input | Default |
|---|---|
| Keyboard and mouse | hold **V** |
| Controller | hold **D-pad down** for 0.35 s |

V is unbound in the game ([controls.md](controls.md)). D-pad down keeps its normal use: the game does not see it while
it is held, and a press let go within the hold time (0.35 s, `net_wheel_hold_ms`) reaches the game afterwards as a quick
press and release. Once the wheel is open the game gets no input at all. A chord of several buttons (for example
`minus+lstick`) opens at once, without the hold time. Both are rebindable in the Online tab, which also has the hold
time, or in `totk.ini` (`net_key_wheel`, `net_pad_wheel`).

- Aim with the left stick, the mouse, or the number keys 1 to 4. Let go of the key to choose.
- A, Enter or a click chooses at once. Esc, B or a right click goes back one ring, or closes the wheel.
- A slice that opens a ring stays open after the key is let go; choose in it with A, Enter, a click or a number.

Top ring, four slices at most:

1. **Teleport**: a ring of the other players; the target gets a request to accept or refuse (the server moves you when
   they accept).
2. **Chat**: opens the chat box.
3. **Team**: Create, Invite (a ring of players), Leave, Players (the list), Team chat.
4. **Host** (only for the host and moderators): Summon all, Kick, Mute, Ban, Message (opens the chat box in server
   message mode), Give mod, Take mod (the last two only for the host). Every one of them but Message asks for a
   confirmation first; the confirmation starts on Cancel.

A ring that needs a person shows the players as slices (seven per page and a "More" slice when there are more than
eight).

### Add-on slices

An add-on (companions, later) registers a slice with `online_wheel_register` (`runtime/ui/online_ui.h`): label, a detail
line, an icon drawing callback, an `activate` callback and an optional `visible` callback. Up to two slices are added to
the top ring; they follow the four built-in ones, so the ring only goes past four slices when an add-on is present.

## Teleport

`teleport_to` (`runtime/teleport.c`) writes the player's position once per presented frame until it has stayed put, and
logs `teleport: ...` lines with each frame's read-back.

**Status: not working yet.** On 1.2.1 the write does not hold. The player actor's position (`+0x2b4`, the field
`frame_rate.c` reads for the speed probe) is a copy the game rewrites every frame from a physics source we have not
found: the move reads back as the old position at every present. Measured with `TOTK_TELEPORT_SCAN=1`, which logs the
objects reachable from the actor that hold the position:

- copies with the same height: `actor+0x284`, `actor+0x2b4`, `[actor+0x1c0]+0xb0` and `+0xe0`, and about forty more
  deeper in the actor's pointer graph;
- copies 0.9 m higher (the body's centre) and 1.5 m higher: `[actor+0x238]+0x2e4`, `+0x2fc`, `+0x308`,
  `[actor+0x608]+0x240` and many below `actor->0x8`.

Writing the first eight of those (`kSlots`), and with `TOTK_TELEPORT_SCAN=all` every copy the scan found (62), changed
nothing. So the source is something the scan cannot match: a position stored another way (relative to a cell origin,
double precision, a transform with the translation split from the rotation), or deeper than three pointers. Next steps:
find the guest function that places the player (the game's own setter for a warp, used for fast travel and respawn) and
call it from the guest's main thread at a safe point, or find the Havok body of the player and set its transform and
velocity together. `kSlots` stays as the list of copies written, each only when it holds the current position.

## Settings

Section `[online]` of `totk.ini` ([totk.ini.example](../totk.ini.example)): `net_name`, `net_address`,
`net_autoconnect`, `net_key_chat`, `net_key_wheel`, `net_key_list`, `net_key_accept`, `net_key_refuse`, `net_pad_wheel`, `net_wheel_hold_ms`,
`net_wheel_scale`, `net_chat_fade`, `net_chat_lines`, `net_fake`, `net_fake_role`.

## Screenshots without input

`TOTK_NET_DEMO_DIR=<dir>` with `TOTK_NET_FAKE=1` walks through the screens by itself once the save has loaded (chat open
and idle, the list, toasts, the wheel's rings, the kick confirmation, a teleport hop) and saves the swapchain image of
each into `<dir>`. `TOTK_MENU_TEST` saves the F1 menu's tabs, the Online tab among them.
