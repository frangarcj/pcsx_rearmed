#ifndef PCSX_VITA_SHELF_PAUSE_H
#define PCSX_VITA_SHELF_PAUSE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The arcade-shelf pause menu over the game, through the presenter
 * (shelf_common.h). As VitaPauseMenu_Run: VITA_PAUSE_* (rom_selector.h).
 * The save-state slot chosen is kept with VitaSaveStates_SetSlot. */
int VitaShelfPause_Run(const char *game_path, int start_in_save_states);

#ifdef __cplusplus
}
#endif

#endif
