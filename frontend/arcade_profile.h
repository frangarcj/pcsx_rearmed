/* Shared profile identification for the shelf, pause menu and libretro.
 * Identification only: the board loader still validates every field/media. */
#ifndef PCSX_ARCADE_PROFILE_H
#define PCSX_ARCADE_PROFILE_H
#include <stdio.h>
#include <string.h>

typedef enum {
    ARCADE_PROFILE_NONE, ARCADE_PROFILE_SYSTEM12, ARCADE_PROFILE_SYSTEM10,
    ARCADE_PROFILE_SYSTEM11, ARCADE_PROFILE_CAPCOM_ZN, ARCADE_PROFILE_GNET
} ArcadeProfileKind;
typedef struct { ArcadeProfileKind kind; char set[64]; } ArcadeProfile;

static inline int arcade_profile_read(const char *directory, ArcadeProfile *profile)
{
    char path[1024], line[1024];
    FILE *file;
    int ini = 1, have_format = 0, have_set = 0;
    memset(profile, 0, sizeof(*profile));
    if (!directory || snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path))
        return 0;
    file = fopen(path, "r");
    if (!file) {
        ini = 0;
        if (snprintf(path, sizeof(path), "%s/board.cfg", directory) >= (int)sizeof(path)) return 0;
        file = fopen(path, "r");
        if (!file) return 0;
        profile->kind = ARCADE_PROFILE_SYSTEM12;
    }
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') goto invalid;
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "format=", 7)) {
            if (have_format++) goto invalid;
            if (!strcmp(line + 7, "pcsx-aow-system10-v1")) profile->kind = ARCADE_PROFILE_SYSTEM10;
            else if (!strcmp(line + 7, "pcsx-aow-system11-v1")) profile->kind = ARCADE_PROFILE_SYSTEM11;
            else if (!strcmp(line + 7, "pcsx-aow-capcom-zn-v1")) profile->kind = ARCADE_PROFILE_CAPCOM_ZN;
            else if (!strcmp(line + 7, "pcsx-aow-gnet-v1")) profile->kind = ARCADE_PROFILE_GNET;
            else goto invalid;
        } else if (!strncmp(line, "mame_set=", 9)) {
            const char *name = line + 9;
            size_t n = strlen(name), i;
            if (have_set++ || !n || n >= sizeof(profile->set)) goto invalid;
            for (i = 0; i < n; i++)
                if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '_')) goto invalid;
            memcpy(profile->set, name, n + 1);
        }
    }
    if (ferror(file) || !have_set || (ini && !have_format)) goto invalid;
    fclose(file);
    return 1;
invalid:
    fclose(file);
    memset(profile, 0, sizeof(*profile));
    return 0;
}
#endif
