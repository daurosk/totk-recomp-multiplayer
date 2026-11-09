// The offline fake of net/net_client.h (net_stub.c) under names of its own. net_client.c hands every call to these
// while TOTK_NET_FAKE=1, so the whole online UI can be tried without a server. net_stub.c defines NET_FAKE_IMPL before
// it includes this header: the macros below then rename its definitions of the net_client.h functions to net_fake_*.
#pragma once
#include "net/net_client.h"

#ifdef __cplusplus
extern "C" {
#endif

int net_fake_enabled(void);
int net_fake_connect(const NetConfig *cfg);
void net_fake_disconnect(void);
NetState net_fake_state(void);
uint32_t net_fake_self_id(void);
const char *net_fake_status_text(void);
void net_fake_tick(void);
int net_fake_players(NetPlayer *out, int max);
int net_fake_player_by_id(uint32_t id, NetPlayer *out);
int net_fake_player_by_name(const char *name, NetPlayer *out);
int net_fake_teams(NetTeam *out, int max);
void net_fake_set_local(const char *area, const float pos[3], const char *character);
int net_fake_chat_count(void);
int net_fake_chat_get(int index, NetChatLine *out);
int net_fake_chat_send(NetChannel channel, const char *whisper_to, const char *text);
uint32_t net_fake_chat_serial(void);
int net_fake_teleport_request(uint32_t player);
void net_fake_teleport_answer(uint32_t request, int accept);
int net_fake_team_create(const char *name);
int net_fake_team_invite(uint32_t player);
void net_fake_team_answer(uint32_t invite, int accept);
void net_fake_team_leave(void);
int net_fake_host_action(NetHostAction action, uint32_t target, const char *text);
int net_fake_poll_events(NetEvent *out, int max);

#ifdef __cplusplus
}
#endif

#ifdef NET_FAKE_IMPL
#define net_enabled net_fake_enabled
#define net_connect net_fake_connect
#define net_disconnect net_fake_disconnect
#define net_state net_fake_state
#define net_self_id net_fake_self_id
#define net_status_text net_fake_status_text
#define net_tick net_fake_tick
#define net_players net_fake_players
#define net_player_by_id net_fake_player_by_id
#define net_player_by_name net_fake_player_by_name
#define net_teams net_fake_teams
#define net_set_local net_fake_set_local
#define net_chat_count net_fake_chat_count
#define net_chat_get net_fake_chat_get
#define net_chat_send net_fake_chat_send
#define net_chat_serial net_fake_chat_serial
#define net_teleport_request net_fake_teleport_request
#define net_teleport_answer net_fake_teleport_answer
#define net_team_create net_fake_team_create
#define net_team_invite net_fake_team_invite
#define net_team_answer net_fake_team_answer
#define net_team_leave net_fake_team_leave
#define net_host_action net_fake_host_action
#define net_poll_events net_fake_poll_events
#endif
