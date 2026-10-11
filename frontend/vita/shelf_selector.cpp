#include "shelf_selector.h"

#include <string.h>
#include <string>

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "arcade_shelf.h"
#include "../arcade_catalog.h"
#include "../shelf_options.h"
#include "rom_selector.h"
#include "savestates.h"
#include "settings.h"

#include "app_root.h"
#define ROMS_DIR APP_ROOT "/roms"

#define MENU_W 960
#define MENU_H 544

extern "C" int NativeGpu_InitBackend(int width, int height);
extern "C" void NativeGpu_RunBackendTaskSync(void (*task)(void *), void *arg);
extern "C" void VitaCtrNativeRenderer_PresentCurrentFrame(void);
extern "C" void VitaCtrNativeRenderer_SetAspectMode(int mode);
extern "C" void NativeRenderer_SwapWindow(void);

struct ShelfTask {
    char *out_path;
    size_t out_size;
    int result;
};

struct PauseTask {
    const char *game_path;
    bool start_in_save_states;
    int result;
};

/* The title of the game launched last, for the pause menu's header. */
static std::string g_game_title, g_game_path;

/* ---- settings: settings.cfg, as the PCSX AwO menus keep them ---- */

static void apply_aspect(void)
{
    const char *value = VitaSettings_GetOption("vita_aspect_ratio");
    int mode = 0;
    if (value && !strcmp(value, "16:9_stretched"))
        mode = 1;
    else if (value && !strcmp(value, "16:9_unstretched"))
        mode = 2;
    VitaCtrNativeRenderer_SetAspectMode(mode);
}

static const char *get_option(void *, const char *key)
{
    return VitaSettings_GetOption(key);
}

static void set_option(void *, const char *key, const char *value)
{
    if (!VitaSettings_SetOption(key, value))
        return;
    VitaSettings_Save();
    if (!strcmp(key, "vita_aspect_ratio"))
        apply_aspect();
}

static int state_exists(void *user, int slot)
{
    return VitaSaveStates_Exists((const char *)user, slot);
}

static ShelfConfig shelf_config(const ShelfMenus *menus)
{
    const ShelfConfig config = { "PCSX", "ReArcade", APP_ROOT,
        "Copy MAME romset zips or PlayStation discs to " ROMS_DIR, MENU_W, MENU_H,
        arcade_catalog_styles, arcade_catalog_style_count, menus };
    return config;
}

/* A disc from the ROM selector's scan; its cover is "covers/<art>.png". */
static void add_disc(void *user, const char *path, const char *title, const char *cover_path)
{
    std::string art;
    const char *slash = strrchr(cover_path, '/');
    if (slash) {
        art = slash + 1;
        if (art.size() > 4 && !strcasecmp(art.c_str() + art.size() - 4, ".png"))
            art.resize(art.size() - 4);
    }
    ((ArcadeCatalog *)user)->add_disc(path, title, art);
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

/* The pad as the shelf takes it. Buttons held when a menu opens (from the
 * game, or the menu before) count once released. */
static ShelfInput read_input(unsigned *ignored, SceUInt64 *previous_time)
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

/* "Soul Calibur (Asia, SOC14/VER.C)" for the pause menu. */
static void remember_title(const ArcadeCatalog &catalog, const char *path)
{
    g_game_path = path;
    g_game_title.clear();
    for (int g = 0; g < catalog.count(); g++) {
        const ShelfGame &game = catalog.games()[g];
        for (int v = 0; v < game.version_count; v++) {
            const ShelfVersion &version = game.versions[v];
            if (strcmp(version.launch, path))
                continue;
            g_game_title = game.title;
            if (version.label && *version.label)
                g_game_title += std::string(" (") + version.label + ")";
            return;
        }
    }
}

static void shelf_task(void *arg)
{
    ShelfTask *task = (ShelfTask *)arg;
    ArcadeCatalog catalog;
    const ShelfMenus menus = { pcsx_shelf_options, pcsx_shelf_option_count, NULL, get_option, set_option, 0, NULL };
    const ShelfConfig config = shelf_config(&menus);

    task->result = -1;
    catalog.build(ROMS_DIR, false);
    VitaRomSelector_ListRoms(add_disc, &catalog);
    sceClibPrintf("pcsx-vita: shelf has %d games\n", catalog.count());
    if (!shelf_init(&config)) {
        sceClibPrintf("pcsx-vita: shelf init failed\n");
        shelf_shutdown();
        return;
    }
    shelf_set_games(catalog.games(), catalog.count());

    unsigned ignored = ~0u;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();
    for (;;) {
        const ShelfInput input = read_input(&ignored, &previous_time);
        glViewport(0, 0, MENU_W, MENU_H);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const int action = shelf_frame(&input, task->out_path, task->out_size);
        vglSwapBuffers(GL_FALSE);
        if (action == SHELF_LAUNCH) {
            task->result = 1;
            remember_title(catalog, task->out_path);
            break;
        }
        if (action == SHELF_QUIT) {
            task->result = 0;
            break;
        }
    }
    glFinish();
    shelf_shutdown();
}

extern "C" int VitaShelfSelector_Run(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0)
        return -1;
    out_path[0] = '\0';
    if (!NativeGpu_InitBackend(MENU_W, MENU_H))
        return -1;

    ShelfTask task = { out_path, out_size, -1 };
    NativeGpu_RunBackendTaskSync(shelf_task, &task);
    return task.result;
}

static void pause_task(void *arg)
{
    PauseTask *task = (PauseTask *)arg;
    /* System 12 boards cannot be saved (retro_serialize refuses them):
     * MAME sets get no save-state page. */
    const size_t length = strlen(task->game_path);
    const bool arcade = length > 4 && !strcasecmp(task->game_path + length - 4, ".zip");
    const ShelfMenus menus = { pcsx_shelf_options, pcsx_shelf_option_count, (void *)task->game_path,
        get_option, set_option, arcade ? 0 : 10, state_exists };
    const ShelfConfig config = shelf_config(&menus);
    std::string title = g_game_path == task->game_path ? g_game_title : std::string();
    if (title.empty()) {
        const char *slash = strrchr(task->game_path, '/');
        title = slash ? slash + 1 : task->game_path;
        title = title.substr(0, title.find_last_of('.'));
    }

    task->result = VITA_PAUSE_RESUME;
    if (!shelf_init(&config)) {
        sceClibPrintf("pcsx-vita: pause menu init failed\n");
        shelf_shutdown();
        return;
    }
    shelf_pause_open(title.c_str(), VitaSaveStates_GetSlot(), task->start_in_save_states);
    unsigned ignored = ~0u;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();
    for (;;) {
        const ShelfInput input = read_input(&ignored, &previous_time);
        int slot = VitaSaveStates_GetSlot();
        GLint framebuffer = 0;

        VitaCtrNativeRenderer_PresentCurrentFrame();
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        const int action = shelf_pause_frame(&input, &slot);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
        NativeRenderer_SwapWindow();
        if (slot != VitaSaveStates_GetSlot())
            VitaSaveStates_SetSlot(slot);
        if (action == SHELF_PAUSE_NONE) {
            sceKernelDelayThread(16000);
            continue;
        }
        static const int results[] = { VITA_PAUSE_RESUME, VITA_PAUSE_RESUME, VITA_PAUSE_RESTART,
            VITA_PAUSE_CLOSE_GAME, VITA_PAUSE_SAVE_STATE, VITA_PAUSE_LOAD_STATE };
        task->result = results[action];
        break;
    }
    glFinish();
    shelf_shutdown();
}

extern "C" int VitaShelfPause_Run(const char *game_path, int start_in_save_states)
{
    PauseTask task = { game_path, start_in_save_states != 0, VITA_PAUSE_RESUME };
    NativeGpu_RunBackendTaskSync(pause_task, &task);
    return task.result;
}
