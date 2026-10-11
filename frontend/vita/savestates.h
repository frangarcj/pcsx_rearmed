#ifndef PCSX_VITA_SAVESTATES_H
#define PCSX_VITA_SAVESTATES_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void VitaSaveStates_Init(void);
int VitaSaveStates_GetSlot(void);
void VitaSaveStates_SetSlot(int slot);
bool VitaSaveStates_Exists(const char *game_path, int slot);
int VitaSaveStates_Save(const char *game_path, int slot);
int VitaSaveStates_Load(const char *game_path, int slot);

#ifdef __cplusplus
}
#endif

#endif
