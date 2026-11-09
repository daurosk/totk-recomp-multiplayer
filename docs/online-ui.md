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

## Look

Everything drawn over the game (chat, player list, toasts, the wheel, the confirmation) looks like the game's own HUD, not
like the F1 menu: the Switch standard shared font, cream text with a one-pixel dark shadow, sharp-cornered translucent
dark bars that fade out on the right, and a small coloured square marking a person (team colour) or a kind of notice.
The F1 menu's Online tab keeps the menu's style. The font is read at run time from `shared-fonts/0-standard.bfttf`
(`ov_shared_font_dir`: `TOTK_SHARED_FONTS`, `shared-fonts` next to `totk.exe`, or the extracted firmware folder that
`tools/extract_fonts.py` fills) and decoded in memory (`ui/menu.cpp`, `fonts_up`); nothing of it is stored or shipped.
Without that file the menu font is used. The drawing helpers are `look_*` in `ui/online_ui.cpp`.

## The wheel menu

Hold the wheel key to open it, aim, let go to choose.

| Input | Default |
|---|---|
| Keyboard and mouse | hold **V** |
| Controller | hold **Minus + left stick click** together |

Neither is used by the game ([controls.md](controls.md)): V is unbound, and the game sees none of the pad chord's buttons
while both are held. Both are rebindable in the Online tab or `totk.ini` (`net_key_wheel`, `net_pad_wheel`). While the
wheel is open the game gets no input.

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

**Status: the physics path is found on 1.2.1 and moves the player; the full teleport is not finished.**

Where the position comes from (1.2.1, found with `TOTK_TELEPORT_WATCH`, a hardware write watchpoint):

- `actor+0x2b4` is written once per frame by the actor's set-transform `main+0x823fd8` (actor, row-major 3x4 matrix,
  frame step), called from `main+0x82133c` (the matrix pointer is in x26 at `main+0x821500`), called from the actor
  update `main+0x819f34`. Inside it, `main+0x8250f8` refills the matrix from the physics body, so substituting the
  matrix alone changes nothing.
- The body is `B = [[[[actor+0x228]+0x50]+0x20]+0x150]`; `B+0x98` holds its transform (row-major 3x4), `B+0x68` its
  flags, `[B+0x70]` its handle `H` (`[H+8]` is the body id). The physics sync `main+0x76c294` (a job, from
  `main+0x2331160`) copies the world's transform into `B+0x98` every frame.
- `main+0xaa9b90 (H, const row-major 3x4 *, B)` writes a new transform into the physics world's body; the sync uses it
  when the game moves a body. The world is `[[[[main+0x462e038]]+0xc8 + 8*(B+0x68 bit 5)]+0xc0]+0xe0`; its vtable
  `+0x1d0 (world, id)` returns the body's live linear velocity.

What is in the tree: patch set `teleport` (`recomp/patches-v121.txt`) turns `main+0x821500` into a call of the host
function at page `+0xf60` (`teleport.c set_transform_hook`). While a move holds the player, it calls `main+0xaa9b90`
on the game's thread with the target translation, zeroes the linear velocity, and hands the actor the moved matrix.
`TOTK_TELEPORT_HOOK=<frames>` uses it for that many frames (the default path is still the old slot writes).

Measured: a +5 m vertical hop holds, then Link falls back and lands on the ground; a 25 m hop east of Lookout Landing
at the same height moves him there but he falls through the terrain, which rises there (target inside the ground).
Still to do: release logic that detects landing (vertical speed back to 0) and retries higher when the player keeps
falling; a longer hold for far moves until the area streams in; the refusals (menu: no hook calls while paused;
mounted, climbing, cutscene, dead and shrine still need the player's state); the reason sent back to the UI; and
1.4.3, whose update is not on this machine (no 1.4.3 addresses yet). Tools: `TOTK_TELEPORT_TEST` (scripted moves),
`TOTK_TELEPORT_SHOTS` (before and after pictures), `TOTK_TELEPORT_WATCH=<hex>` or `=b<hex>` (writers of an actor or
body field, logged as executable offsets for `build/<ver>/totk.map`), `TOTK_TELEPORT_VT=1` (handle vtable dump).

Player state, second round (1.2.1, `TOTK_TELEPORT_DUMP`, one run each, not yet used by the code):

- The velocity reset through the world's vtable `+0x1d0` faults on its first call (access violation in `main+0x151ab4`
  under the actor update), so it is off unless `TOTK_TELEPORT_ZEROVEL` is set. Without it a held hop still releases
  and lands; the body leaves the hold already falling at about 10 m/s.
- Menu or pause: the player's set-transform stops being called while the pause menu is open (the hook counts the calls
  for the player actor), so "no calls for a few presents" is the pause signal.
- Dead: a 400 m drop ends in Game Over. Fields that change only then: `[[actor+0x600]+0x128]` 0 to 5,
  `[[actor+0x870]+0x230]` and `[[actor+0xab8]+0x30]` 0 to 1.
- Falling: `[[actor+0xd70]+0x420]` is 0x460 standing and gliding and 0x400 falling and dead.
- Gliding: no field of the actor or of the objects one pointer away tells it apart from standing; the state machine
  sits deeper. The paraglider sinks 3.3 m/s; a fall tops out at 60 m/s.
- No object the actor points to names its state in a string.

Debug tools added: `TOTK_TELEPORT_DUMP=<dir>` saves the actor and the objects it points to every 15 presents as
`<world ms>.bin`, the strings those objects lead to and the update count as `<world ms>.txt`, and a picture every
150 presents; `TOTK_TELEPORT_TEST` steps written `t<ms>` run at that time of the world clock.

## Settings

Section `[online]` of `totk.ini` ([totk.ini.example](../totk.ini.example)): `net_name`, `net_address`,
`net_autoconnect`, `net_key_chat`, `net_key_wheel`, `net_key_list`, `net_key_accept`, `net_key_refuse`, `net_pad_wheel`,
`net_wheel_scale`, `net_chat_fade`, `net_chat_lines`, `net_fake`, `net_fake_role`.

## Screenshots without input

`TOTK_NET_DEMO_DIR=<dir>` with `TOTK_NET_FAKE=1` walks through the screens by itself once the save has loaded (chat open
and idle, the list, toasts, the wheel's rings, the kick confirmation, a teleport hop) and saves the swapchain image of
each into `<dir>`. `TOTK_MENU_TEST` saves the F1 menu's tabs, the Online tab among them.
