#include "shelf_selector.h"

#include <string.h>
#include <string>

#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "arcade_shelf.h"
#include "../arcade_catalog.h"
#include "rom_selector.h"

#define APP_ROOT "ux0:data/pcsx-rearmed"
#define ROMS_DIR APP_ROOT "/roms"

#define MENU_W 960
#define MENU_H 544

extern "C" int NativeGpu_InitBackend(int width, int height);
extern "C" void NativeGpu_RunBackendTaskSync(void (*task)(void *), void *arg);

struct ShelfTask {
    char *out_path;
    size_t out_size;
    int result;
};

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

static void shelf_task(void *arg)
{
    ShelfTask *task = (ShelfTask *)arg;
    ArcadeCatalog catalog;
    const ShelfConfig config = { "PCSX", "ReArcade", APP_ROOT,
        "Copy MAME romset zips or PlayStation discs to " ROMS_DIR, MENU_W, MENU_H,
        arcade_catalog_styles, arcade_catalog_style_count };

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

    /* Buttons still held from the game or the pause menu wait for release. */
    unsigned ignored = ~0u;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
            sceCtrlPeekBufferPositive(0, &pad, 1);
        ignored &= pad.buttons;

        const SceUInt64 now = sceKernelGetProcessTimeWide();
        ShelfInput input;
        input.buttons = shelf_buttons(pad.buttons & ~ignored);
        input.stick_x = stick_axis(pad.rx);
        input.stick_y = stick_axis(pad.ry);
        input.dt = (float)(now - previous_time) / 1000000.0f;
        previous_time = now;

        glViewport(0, 0, MENU_W, MENU_H);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const int action = shelf_frame(&input, task->out_path, task->out_size);
        vglSwapBuffers(GL_FALSE);
        if (action == SHELF_LAUNCH) {
            task->result = 1;
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
