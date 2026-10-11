#include "shelf_common.h"

#include <stdio.h>
#include <string.h>
#include <string>

#include <psp2/ctrl.h>

#include "arcade_shelf.h"
#include "../arcade_catalog.h"
#include "../shelf_options.h"
#include "settings.h"

#include "app_root.h"

#define MENU_W 960
#define MENU_H 544

static const VitaShelfPresenter *g_presenter;
static std::string g_game_path, g_game_title;

extern "C" void VitaShelf_SetPresenter(const VitaShelfPresenter *presenter)
{
    g_presenter = presenter;
}

extern "C" const VitaShelfPresenter *VitaShelf_Presenter(void)
{
    return g_presenter;
}

extern "C" int VitaShelf_AspectMode(void)
{
    const char *value = VitaSettings_GetOption("vita_aspect_ratio");
    if (value && !strcmp(value, "16:9_stretched"))
        return 1;
    if (value && !strcmp(value, "16:9_unstretched"))
        return 2;
    return 0;
}

extern "C" void VitaShelf_SetGame(const char *path, const char *title)
{
    g_game_path = path ? path : "";
    g_game_title = title ? title : "";
}

extern "C" void VitaShelf_GetGameTitle(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", VitaShelf_GameTitle(path).c_str());
}

std::string VitaShelf_GameTitle(const char *path)
{
    if (g_game_path == path && !g_game_title.empty())
        return g_game_title;
    const char *slash = strrchr(path, '/');
    const std::string file = slash ? slash + 1 : path;
    return file.substr(0, file.find_last_of('.'));
}

/* ---- settings: settings.cfg, as the PCSX AwO menus keep them ---- */

const char *VitaShelf_GetOption(void *, const char *key)
{
    return VitaSettings_GetOption(key);
}

void VitaShelf_SetOption(void *, const char *key, const char *value)
{
    if (!VitaSettings_SetOption(key, value))
        return;
    VitaSettings_Save();
    if (!strcmp(key, "vita_aspect_ratio") && g_presenter && g_presenter->set_aspect)
        g_presenter->set_aspect(VitaShelf_AspectMode());
}

ShelfConfig VitaShelf_Config(const ShelfMenus *menus)
{
    static const std::string about = pcsx_shelf_about(PCSX_GIT_VERSION);
    const ShelfConfig config = { "PCSX", "ReArcade", APP_ROOT,
        "Copy MAME romset zips or PlayStation discs to " APP_ROOT "/roms", MENU_W, MENU_H,
        arcade_catalog_styles, arcade_catalog_style_count, menus, about.c_str(), 0, 0 };
    return config;
}

static unsigned shelf_buttons(unsigned buttons)
{
    static const struct { unsigned vita, shelf; } map[] = {
        { SCE_CTRL_UP, SHELF_UP }, { SCE_CTRL_DOWN, SHELF_DOWN },
        { SCE_CTRL_LEFT, SHELF_LEFT }, { SCE_CTRL_RIGHT, SHELF_RIGHT },
        { SCE_CTRL_CROSS, SHELF_CROSS }, { SCE_CTRL_CIRCLE, SHELF_CIRCLE },
        { SCE_CTRL_TRIANGLE, SHELF_TRIANGLE }, { SCE_CTRL_SQUARE, SHELF_SQUARE },
        { SCE_CTRL_LTRIGGER, SHELF_L }, { SCE_CTRL_RTRIGGER, SHELF_R },
        { SCE_CTRL_START, SHELF_START }, { SCE_CTRL_SELECT, SHELF_SELECT },
    };
    unsigned out = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (buttons & map[i].vita)
            out |= map[i].shelf;
    return out;
}

static float stick_axis(unsigned char value)
{
    return ((int)value - 128) / 127.0f;
}

ShelfInput VitaShelf_ReadInput(unsigned *ignored, SceUInt64 *previous_time)
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
        sceCtrlPeekBufferPositive(0, &pad, 1);
    *ignored &= pad.buttons;

    const SceUInt64 now = sceKernelGetProcessTimeWide();
    ShelfInput input;
    input.buttons = shelf_buttons(pad.buttons & ~*ignored);
    input.stick_x = stick_axis(pad.rx);
    input.stick_y = stick_axis(pad.ry);
    input.dt = (float)(now - *previous_time) / 1000000.0f;
    *previous_time = now;
    return input;
}
