#include "shelf_selector.h"

#include <stdint.h>
#include <string.h>
#include <string>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "arcade_shelf.h"
#include "../arcade_catalog.h"
#include "../shelf_options.h"
#include "rom_selector.h"
#include "shelf_common.h"

#include "app_root.h"
#define ROMS_DIR APP_ROOT "/roms"

#define MENU_W 960
#define MENU_H 544

extern "C" int NativeGpu_InitBackend(int width, int height);
extern "C" void NativeGpu_RunBackendTaskSync(void (*task)(void *), void *arg);
extern "C" void VitaCtrNativeRenderer_PresentCurrentFrame(void);
extern "C" void VitaCtrNativeRenderer_SetAspectMode(int mode);
extern "C" void NativeRenderer_SwapWindow(void);
extern "C" void VitaCtrNativeRenderer_GetFrameViewport(int rect[4]);

/* The vitaGL executable's menus: on the renderer's thread and context. */
static const VitaShelfPresenter g_vita_ctr_presenter = {
    NativeGpu_RunBackendTaskSync, VitaCtrNativeRenderer_PresentCurrentFrame,
    VitaCtrNativeRenderer_GetFrameViewport, NativeRenderer_SwapWindow, VitaCtrNativeRenderer_SetAspectMode
};

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

/* "Soul Calibur (Asia, SOC14/VER.C)" for the pause menu. */
static void remember_title(const ArcadeCatalog &catalog, const char *path)
{
    for (int g = 0; g < catalog.count(); g++) {
        const ShelfGame &game = catalog.games()[g];
        for (int v = 0; v < game.version_count; v++) {
            const ShelfVersion &version = game.versions[v];
            if (strcmp(version.launch, path))
                continue;
            std::string title = game.title;
            if (version.label && *version.label)
                title += std::string(" (") + version.label + ")";
            VitaShelf_SetGame(path, title.c_str());
            return;
        }
    }
    VitaShelf_SetGame(path, NULL);
}

/* A file the shelf lists: a MAME zip or a disc the ROM selector takes. */
static bool is_game_file(const char *name)
{
    static const char *const extensions[] = { ".zip", ".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".bin", ".exe" };
    const size_t length = strlen(name);
    for (const char *extension : extensions)
        if (length > 4 && !strcasecmp(name + length - 4, extension))
            return true;
    return false;
}

/* Names and sizes of the game files in the ROM folders. When it changes
 * (games copied over FTP...) the shelf rescans; sizes catch a copy that was
 * still running at the last scan. */
static uint64_t games_signature(void)
{
    static const char *const folders[] = { APP_ROOT, ROMS_DIR };
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char *folder : folders) {
        SceUID dir = sceIoDopen(folder);
        SceIoDirent entry;
        if (dir < 0)
            continue;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0) {
            if (!SCE_S_ISDIR(entry.d_stat.st_mode) && is_game_file(entry.d_name)) {
                for (const char *c = entry.d_name; *c; c++)
                    hash = (hash ^ (unsigned char)*c) * 0x100000001b3ULL;
                hash = (hash ^ (uint64_t)entry.d_stat.st_size) * 0x100000001b3ULL;
            }
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    return hash;
}

static void draw_shelf_frame(const ShelfInput *input, char *launch, size_t launch_size, int *action)
{
    glViewport(0, 0, MENU_W, MENU_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    *action = shelf_frame(input, launch, launch_size);
#ifdef VITA_DEBUG_SHOT
    VitaShelf_ShotPoll();
#endif
    vglSwapBuffers(GL_FALSE);
}

/* Lists the games again, with a notice on screen while it reads them. */
static void scan_games(ArcadeCatalog &catalog, uint64_t *signature, bool notice)
{
    if (notice) {
        const ShelfInput idle = { 0, 0.0f, 0.0f, 0.0f };
        char unused[8];
        int action;
        shelf_notice("Scanning games...");
        draw_shelf_frame(&idle, unused, sizeof(unused), &action);
    }
    *signature = games_signature();
    catalog.build(ROMS_DIR, false);
    VitaRomSelector_ListRoms(add_disc, &catalog);
    sceClibPrintf("pcsx-vita: shelf has %d games\n", catalog.count());
    shelf_set_games(catalog.games(), catalog.count());
    shelf_notice(NULL);
}

static void shelf_task(void *arg)
{
    ShelfTask *task = (ShelfTask *)arg;
    ArcadeCatalog catalog;
    int option_count;
    const ShelfOption *options = pcsx_shelf_options(PCSX_OPTIONS_SELECTOR, &option_count);
    const ShelfMenus menus = { options, option_count, NULL, VitaShelf_GetOption, VitaShelf_SetOption, 0, NULL, NULL, 0 };
    const ShelfConfig config = VitaShelf_Config(&menus);
    uint64_t signature = 0;

    task->result = -1;
    if (!shelf_init(&config)) {
        sceClibPrintf("pcsx-vita: shelf init failed\n");
        shelf_shutdown();
        return;
    }
    scan_games(catalog, &signature, false);

    unsigned ignored = ~0u;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();
    SceUInt64 checked = previous_time;
    for (;;) {
        const ShelfInput input = VitaShelf_ReadInput(&ignored, &previous_time);
        int action;
        draw_shelf_frame(&input, task->out_path, task->out_size, &action);
        /* Every 2 s: did the game files change? */
        if (action == SHELF_NONE && previous_time - checked >= 2000000) {
            checked = previous_time;
            if (games_signature() != signature)
                action = SHELF_RESCAN;
        }
        if (action == SHELF_RESCAN) {
            scan_games(catalog, &signature, true);
            continue;
        }
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
    VitaShelf_SetPresenter(&g_vita_ctr_presenter);

    ShelfTask task = { out_path, out_size, -1 };
    NativeGpu_RunBackendTaskSync(shelf_task, &task);
    return task.result;
}
