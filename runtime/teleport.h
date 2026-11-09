// Moves the local player: sets the position of the resident "Player" actor (the chain frame_rate.c walks for the speed
// probe). Present thread only.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Where the player is now; 0 while no player is resident (title screen, loading screen).
int teleport_player_pos(float xyz[3]);
// Queues a move to xyz (the game's world units). 0 when refused: a position outside the map or not finite, or a move
// already running. The move is carried out by teleport_frame over the next frames.
int teleport_to(const float xyz[3]);
// 1 while a queued move has not finished.
int teleport_busy(void);
// Once per presented frame, before the UI.
void teleport_frame(void);
// TOTK_TELEPORT_TEST: runs the scripted moves; a path when the frame being presented is to be saved. Present thread.
const char *teleport_test_shot(void);
#ifdef __cplusplus
}
#endif
