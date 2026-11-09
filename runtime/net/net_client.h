// The client side of online play, as the in-game UI (ui/online_ui.cpp) and the teleport (teleport.c) use it. This header
// is the whole contract: net_client.c implements it over the wire protocol (docs/online-wire.md), and with
// TOTK_NET_FAKE=1 hands every call to net_stub.c, which plays three to five invented players offline.
//
// Threading: every function is called from the present thread, the thread that draws the UI. An implementation that
// talks to a server on its own thread guards its state itself; the arrays handed out are copies.
//
// Ids: a player id is a small non-zero number the server hands out and stays valid until that player leaves. 0 means
// "nobody" (no team, no target). Text is UTF-8 and always NUL-terminated.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_NAME_MAX 32
#define NET_AREA_MAX 40
#define NET_TEXT_MAX 256
#define NET_MAX_PLAYERS 64
#define NET_MAX_TEAMS 16

typedef enum { NET_ROLE_PLAYER = 0, NET_ROLE_MOD = 1, NET_ROLE_HOST = 2 } NetRole;
typedef enum { NET_OFFLINE = 0, NET_CONNECTING = 1, NET_ONLINE = 2 } NetState;

typedef struct {
    uint32_t id;
    char name[NET_NAME_MAX];
    char character[NET_NAME_MAX];   // the player's character as a name ("Link", "Zelda")
    char area[NET_AREA_MAX];        // where they are, as a place name or the map tile
    float pos[3];                   // world position in the game's units
    uint32_t team;                  // team id, 0 none
    NetRole role;
    uint32_t ping_ms;
    uint8_t muted;                  // the host muted them: their chat does not show
} NetPlayer;

typedef struct {
    uint32_t id;
    char name[NET_NAME_MAX];
    uint32_t leader;                // player id
} NetTeam;

typedef enum { NET_CHAT_ALL = 0, NET_CHAT_TEAM = 1, NET_CHAT_WHISPER = 2, NET_CHAT_SYSTEM = 3 } NetChannel;

typedef struct {
    uint64_t time_ms;               // wall clock, milliseconds since 1970
    NetChannel channel;
    uint32_t from;                  // sender's id, 0 for system lines
    char from_name[NET_NAME_MAX];
    char to_name[NET_NAME_MAX];     // whispers: the recipient's name (the sender's own line shows it too)
    char text[NET_TEXT_MAX];
} NetChatLine;

typedef enum {
    NET_EV_PLAYER_JOINED = 1,       // player: id and name
    NET_EV_PLAYER_LEFT,             // player
    NET_EV_TEAM_INVITE,             // player: who invites, request: the invite id, text: the team's name
    NET_EV_TELEPORT_REQUEST,        // player: who asks to come to you, request: the request id
    NET_EV_TELEPORT_ANSWER,         // player: who answered your request, accepted: 1 or 0
    NET_EV_TELEPORT_GO,             // your request was accepted: move to pos
    NET_EV_SUMMON,                  // the host summons everyone: move to pos
    NET_EV_KICKED,                  // you were removed; text: the reason
    NET_EV_MUTED,                   // you were muted (accepted 1) or unmuted (0)
    NET_EV_ROLE_CHANGED,            // your role changed; accepted: 1 granted, 0 taken
    NET_EV_SERVER_MESSAGE,          // text: the host's message (it also arrives as a system chat line)
    NET_EV_STATE                    // the connection state changed (net_state); text: why, when it failed or ended
} NetEventType;

typedef struct {
    NetEventType type;
    uint32_t player;
    uint32_t request;
    uint8_t accepted;
    float pos[3];
    char name[NET_NAME_MAX];
    char text[NET_TEXT_MAX];
} NetEvent;

typedef enum {
    NET_HOST_SUMMON_ALL = 1,        // everyone moves to the host
    NET_HOST_KICK,                  // target; text: reason (may be empty)
    NET_HOST_MUTE,                  // target; text "1" mute, "0" unmute
    NET_HOST_BAN,                   // target; text: reason
    NET_HOST_MESSAGE,               // text goes to everyone as a system line
    NET_HOST_GRANT_MOD,             // target becomes a moderator
    NET_HOST_REVOKE_MOD             // target stops being one
} NetHostAction;

typedef struct {
    char name[NET_NAME_MAX];        // the name the player goes by
    char address[96];               // "host:port"
    char token[64];                 // optional join secret: the room's password or a one-time claim code
} NetConfig;

// ---- connection
// Whether online features exist at all: the UI draws nothing while this is 0. The stub says 1 with TOTK_NET_FAKE=1; the
// real client says 1 once the player set a server address (net_address in totk.ini or the Online tab).
int net_enabled(void);
int net_connect(const NetConfig *cfg);   // 1 when the attempt started; progress arrives as NET_EV_STATE
void net_disconnect(void);
NetState net_state(void);
uint32_t net_self_id(void);              // 0 while not online
// Why the last connection failed or ended, as a sentence for the player ("The server is full."); "" when it did not.
// NET_EV_STATE carries the same text in its text field when the state changed for that reason.
const char *net_status_text(void);
// Called once per presented frame, before the UI reads anything: pumps sockets, timers, the fake traffic.
void net_tick(void);

// ---- who is here
int net_players(NetPlayer *out, int max);       // everyone including the local player; returns the count
int net_player_by_id(uint32_t id, NetPlayer *out);   // 1 when found
int net_player_by_name(const char *name, NetPlayer *out);   // case-insensitive; 1 when found
int net_teams(NetTeam *out, int max);
// The local player's own state, sent to the others: where they are. Call each frame; the client rate-limits.
void net_set_local(const char *area, const float pos[3], const char *character);

// ---- chat
// The newest `count` lines are n lines ending at the newest; index 0 is the oldest kept (at most NET_CHAT_KEEP).
#define NET_CHAT_KEEP 200
int net_chat_count(void);
int net_chat_get(int index, NetChatLine *out);
// channel NET_CHAT_WHISPER needs whisper_to (a player name); the other channels ignore it. Returns 1 when sent.
int net_chat_send(NetChannel channel, const char *whisper_to, const char *text);
// Increases with every chat line that arrives: the UI compares it to know when to wake the chat box.
uint32_t net_chat_serial(void);

// ---- teleport
int net_teleport_request(uint32_t player);       // ask to be taken to that player
void net_teleport_answer(uint32_t request, int accept);

// ---- teams
int net_team_create(const char *name);
int net_team_invite(uint32_t player);
void net_team_answer(uint32_t invite, int accept);
void net_team_leave(void);

// ---- host and moderators (the server checks the role again)
int net_host_action(NetHostAction action, uint32_t target, const char *text);

// ---- the shared economy (feature switch net_economy in totk.ini, off by default; the server must have it too)
// Rupees belong to the account and the server decides them: the game asks for a change and takes the answer. A Light of
// Blessing or a Korok seed one player finds counts for every player's ledger; what a player spends is their own. One
// player, the leader (the first to have joined), sets the clock and the Blood Moon for everyone. All of these are 0 or
// do nothing while net_economy_on() is 0. The serial of each answer rises with every update from the server.
typedef enum { NET_WALLET_SPEND = 0, NET_WALLET_EARN = 1, NET_WALLET_TRADE = 2, NET_WALLET_STAKE = 3 } NetWalletCause;
typedef struct {
    uint32_t lights_found, lights_spent, seeds_found, seeds_spent;
} NetLedger;
typedef struct {
    uint32_t day;
    float hour;                     // 0 to 24
    uint8_t blood_moon_due;
    uint8_t is_leader;              // this game sets the clock
} NetSky;
int net_economy_on(void);        // the switch is on and the server granted the economy and the sky clock
// The rupees the server last said the account has. 0 until it said. *serial changes whenever they did.
int net_wallet_get(uint32_t *rupees, uint32_t *serial);
// Asks to change the rupees (delta may be negative). The answer arrives in net_wallet_get, possibly cut down.
int net_wallet_ask(int32_t delta, NetWalletCause cause);
int net_ledger_get(NetLedger *out, uint32_t *serial);
int net_ledger_find(int korok_seed, uint32_t key);   // this game found one, named by its GameData key hash
int net_ledger_spend(int korok_seed, uint32_t count);
// The leader's clock, as the server last relayed it: 0 until the leader sent one. The leader's own game gets no clock
// back, only is_leader; it sends its own with net_sky_send.
int net_sky_get(NetSky *out, uint32_t *serial);
int net_sky_is_leader(void);
int net_sky_send(uint32_t day, float hour, int blood_moon_due);   // only the leader's is passed on

// ---- shared loot (feature switch net_loot in totk.ini, off by default; the server needs the actors add-on)
// A drop is made once, by the game that owns what made it (an enemy it runs, a chest it opened, an object it broke):
// that game calls net_loot_drop, and every other game in range gets a NET_LOOT_SPAWNED event to make the same thing.
// A drop is its own record and can be taken once: the game whose player picks it up calls net_loot_take and puts
// nothing in the pouch until NET_LOOT_TAKEN says yours; every other game removes its copy. A fused drop (a weapon with
// a material fused to it) is two drops, the weapon and the fuse part, each with its own id and the weapon as the
// part's parent (docs/online-wire.md, 9.1); the other games get two NET_LOOT_SPAWNED events and take either by its id.
typedef struct {
    uint64_t origin, serial;        // the actor's id; a drop's origin is the account that made it
} NetActorId;
typedef struct {
    NetActorId parent;              // what made it (zero origin and serial for none)
    char name[64];                  // the game's actor name of the drop
    float pos[3];
    float vel[3];                   // a drop that is thrown or falls starts with this
    char fuse_name[64];             // net_loot_drop only: the material fused to it, made as a second drop; "" for none
} NetLootDrop;
typedef enum { NET_LOOT_SPAWNED = 1, NET_LOOT_TAKEN = 2 } NetLootType;
typedef struct {
    NetLootType type;
    NetActorId id;
    uint64_t who;                   // SPAWNED: the account that made it; TAKEN: the account that took it
    uint8_t yours;                  // TAKEN: this game's player took it, so it belongs in this game's pouch
    NetLootDrop drop;               // SPAWNED
} NetLootEvent;
int net_loot_on(void);           // the switch is on and the server granted the actors add-on
// Tells the others about a drop this game made. Fills *id (when given) with the id to take it by and, for a fused
// drop, *fuse_id (when given) with the fuse part's. 1 when sent.
int net_loot_drop(const NetLootDrop *drop, NetActorId *id, NetActorId *fuse_id);
int net_loot_take(NetActorId id);
int net_loot_poll(NetLootEvent *out, int max);   // oldest first; returns how many

// ---- events for the UI. Fills out with up to max events, oldest first; returns how many.
int net_poll_events(NetEvent *out, int max);

#ifdef __cplusplus
}
#endif
