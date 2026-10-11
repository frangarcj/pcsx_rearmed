#ifndef PCSX_VITA_SETTINGS_H
#define PCSX_VITA_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void VitaSettings_Init(void);
const char *VitaSettings_GetOption(const char *key);
int VitaSettings_SetOption(const char *key, const char *value);
int VitaSettings_Save(void);
bool VitaSettings_ConsumeDirty(void);

#ifdef __cplusplus
}
#endif

#endif
