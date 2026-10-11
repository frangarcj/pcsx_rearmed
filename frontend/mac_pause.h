/* The pause menu of the interactive macOS harness: arcade-shelf over the
 * game's last frame, as on the Vita. */
#ifndef PCSX_MAC_PAUSE_H
#define PCSX_MAC_PAUSE_H

#ifdef __cplusplus
extern "C" {
#endif

enum { MAC_PAUSE_RESUME, MAC_PAUSE_RESTART, MAC_PAUSE_CLOSE };

/* Runs until the player resumes, restarts or closes; game: the header.
 * For checks without a player, script (or NULL) presses buttons, one
 * every 10 frames from frame 10 (U D L R X O: up... cross, circle), then the menu is
 * saved to shot (a PPM) and closed. */
int mac_pause_run(const char *game, const char *script, const char *shot);

#ifdef __cplusplus
}
#endif

#endif
