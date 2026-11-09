# The actors add-on on the server: what was built and what the lab showed

The server side of actor sync for release 1. What both ends do is [online-wire.md](online-wire.md) section 9.1; this
page records how the rules were settled, what the replay lab measures and what is left.

| Part | Where |
|---|---|
| Class table, shared by game and server | `online/proto/classes.txt`, `ow_classes.c` (`ow_class_find`, `ow_name_hash`, `ow_herd_cell`), `ow_classes_data.h` from `tools/gen_classes.py` |
| Owner rules, pure | `server/sv_owner.c`, numbers in `sv_owner.h` |
| Records, interest, relays, holds, groups, followers, spawns, take-once | `server/sv_actors.c` |
| Room wiring: grant, dispatch, own frame budget, area, position, hold, leave | `server/sv_room.c` |
| Replay lab | `tests/actor_lab.c`, `tests/actor_lab.py`, `tests/actor_cases/*.txt` |
| End to end through the real server | `tests/actor_server_check.py`, with `tools/net_bot.c` (`repeat`, list fields) |

The wire's layout is unchanged: version 1, the same fingerprint. Section 9.1 adds the rules only.

## Where the rules come from

The old port's rules (its SyncFix S1 stage and the M5 to M8 design) were read for behaviour and written anew in C:
rule 1 is S1's orphans, rule 2 its "owner far" with a margin, rules 3 and 4 its "close" hold, rule 5 its still
things, rule 7 its fight lock, rule 8 its parts, held objects and quest actors, rule 11 its contraption clusters. The
numbers are S1's: 120 m, 5 m, 10 m, 5 s, 20 m, 20 s, 1 m in 5 s, 160 m, 6 s, 60 m, 12 m, 6 m.

Where this design differs from S1, and why:

| Change | Why |
|---|---|
| "Clearly closer" is ours: at most half the owner's distance and 15 m nearer | S1 sat in front of a closed policy whose margin was not known; the forest-walk case (20 to 22 m against 10 to 15 m) must not move |
| Rule 5 holds rule 3 for every class, and rule 2 for things that do not act by themselves | The first draft gave 62 owner changes a minute in the roam case, mostly static boxes handed to whoever was nearest. Enemies keep rule 2: one standing still may be still only because its far owner no longer runs it |
| An owner silent for 2 s gives the actor up (rule 9) | Rule 5 leans on games releasing what they cannot run; silence makes that hold when a game does not say so |
| A game that just released or unloaded an actor does not hold rule 1 back | Found by the release case: the releaser counted as "a nearer game that may still offer" and the hand-over waited 3 s |
| One epoch counter for the room | Found by the reconnect case: a record dropped and made again restarted at epoch 1, and a game that remembered the old epoch dropped everything about it |
| Placed ids are read in the sender's scene | A shrine's map ids are not the field's (the old port's p60 section 6) |
| Actor frames have their own budget (2000 at once, 400 a second) | The room cuts off at 200 frames at once; loading a busy place offers hundreds of actors |

## What the lab measures

25 cases, each a bot timeline on a virtual clock (50 ms ticks, games every 250 ms), all passing. The ones from the old
port's logs and rounds:

| Case | From | Result |
|---|---|---|
| 01 far-both-far | camp 2026-11-02 19:03, 71 to 108 changes a minute | 0 changes in 115 s after the first owners |
| 03 orphans-loading | S1 orphans after a loading screen | stays with the loading owner until the other player is within 120 m, then 1 change |
| 06 closer-forest-walk | gate run 46f2ee15, a change every 12 s | 0 changes in 123 s |
| 07 fruit-passing | Item_Fruit_A#472, 11 changes a minute | 2.5 a minute |
| 10 fight-lock | 2026-11-03 fight lock | held while hit every 3 s, moves once 10 s after |
| 12 boss-and-parts | S1 parts (Stalnox) | boss held to 160 m, parts follow in the same second |
| 13 held-rock | camp capture, a thrown rock taken 3 m out | stays with the thrower's owner |
| 14 contraption | a vehicle with no glue, steered as copies | one grab takes all three parts |

The brief's bar, under 5 owner changes a minute in steady play, counted as the old port's rounds counted them (changes
by rules 2 to 4, "flips"; actors coming into range and owners leaving or unloading are counted apart):

| Case | Flips a minute | All changes a minute |
|---|---|---|
| 23 steady-together: four players exploring together, 113 actors, 10 minutes | 0.89 | 22.1 (first owners on coming into range, hand-overs to nobody on leaving it) |
| 24 steady-apart: two pairs 600 m apart | 1.44 | 6.7 |
| 25 roam-stress: four players each roaming one village alone | 13.4 | 15.7, mostly owner far, and due |

## Not done, for later

- `herd` (the wildlife leader rule) is accepted and ignored: no leader is named yet.
- The talk claim for townspeople has no wire value (`OW_HOLD_*` has none for talking).
- Rule 8's quest actors keep their owner but are not tied to the game that runs the quest step.
- Followers and clusters scan all records once a second (fine for hundreds of shared actors, quadratic in the
  thousands).
- No game round: every rule is checked on the lab and with bots only. The client side is r1-actors' work.
