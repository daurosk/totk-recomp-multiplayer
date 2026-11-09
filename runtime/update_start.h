// The update check at start (update_start.c).
#pragma once

// Windows: runs totk-update.exe --check beside totk.exe and waits for it; when the player chose Update (or an update
// is running) the game exits here. Elsewhere, and in a folder without the updater (a build outside the setup), nothing.
void update_at_start(void);
