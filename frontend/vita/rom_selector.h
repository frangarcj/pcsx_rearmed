#ifndef PCSX_VITA_ROM_SELECTOR_H
#define PCSX_VITA_ROM_SELECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int VitaRomSelector_Run(char *out_path, size_t out_size);

/* The games the selector shows, sorted by title: path, file title and the
 * cover each resolves to (a path in the covers folder, or ""). */
typedef void (*VitaRomCallback)(void *user, const char *path, const char *title, const char *cover_path);
void VitaRomSelector_ListRoms(VitaRomCallback callback, void *user);

enum VitaPauseMenuResult {
    VITA_PAUSE_RESUME = 0,
    VITA_PAUSE_RESTART = 1,
    VITA_PAUSE_CLOSE_GAME = 2,
    VITA_PAUSE_SAVE_STATE = 3,
    VITA_PAUSE_LOAD_STATE = 4,
};

int VitaPauseMenu_Run(const char *game_path, int start_in_save_states);

#ifdef __cplusplus
}
#endif

#endif
