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

// ---- puppets: the other players' bodies (the puppets add-on, docs/online-wire.md self_state and player_state)
#define NET_ANIM_SLOTS 8
typedef struct {
    uint8_t layer;
    uint32_t anim;                  // hash of the animation's name
    float frame, rate;
} NetAnimSlot;

// The local player's body, sent to the others while in the world. pos in the game's units (metres), vel in metres a
// second, rot a quaternion x, y, z, w.
typedef struct {
    int in_world;                   // 0 on the title screen, in a loading screen or a cutscene: nothing is sent
    float pos[3], vel[3], rot[4];
    int nanim;
    NetAnimSlot anim[NET_ANIM_SLOTS];
    uint8_t gliding;
    float glide_lean;
} NetSelf;
// Call each frame with the local body; the client sends it about 20 times a second.
void net_set_self(const NetSelf *self);

typedef struct {
    uint32_t id;                    // the player's id (NetPlayer.id)
    char name[NET_NAME_MAX];
    uint8_t zelda;                  // the account plays Zelda
    float pos[3], vel[3], rot[4];   // smoothed, at a render time a little behind the sender
    float yaw;                      // radians about the up axis, from rot
    float alpha;                    // 0 to 1: fades in when the puppet appears and out before it goes
    int nanim;
    NetAnimSlot anim[NET_ANIM_SLOTS];   // the animation of the state in effect, frames advanced to the render time
    uint8_t gliding;
    float glide_lean;
} NetPuppet;
// The other players this game may show now; returns the count. Empty while offline or while the local player is not
// in the world. A player shows from its first state and goes after NET_PUPPET_STALE_MS without one (out of range,
// loading, title screen), at once when it leaves.
#define NET_PUPPET_STALE_MS 1000
int net_puppets(NetPuppet *out, int max);

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

// ---- events for the UI. Fills out with up to max events, oldest first; returns how many.
int net_poll_events(NetEvent *out, int max);

#ifdef __cplusplus
}
#endif
