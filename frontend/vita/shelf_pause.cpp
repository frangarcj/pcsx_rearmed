#include "shelf_pause.h"

#include <string.h>
#include <string>

#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "arcade_shelf.h"
#include "../shelf_options.h"
#include "../shelf_snapshot.h"
#include "../arcade_profile.h"
#include "rom_selector.h"
#include "savestates.h"
#include "shelf_common.h"

#include "app_root.h"
extern "C" size_t retro_serialize_size(void);

struct PauseTask {
    const char *game_path;
    bool start_in_save_states;
    int result;
};

static int state_exists(void *user, int slot)
{
    return VitaSaveStates_Exists((const char *)user, slot);
}

static void pause_task(void *arg)
{
    PauseTask *task = (PauseTask *)arg;
    const VitaShelfPresenter *presenter = VitaShelf_Presenter();
    const size_t length = strlen(task->game_path);
    ArcadeProfile profile;
    const bool prepared = arcade_profile_read(task->game_path, &profile) != 0;
    const bool arcade = prepared || (length > 4 && !strcasecmp(task->game_path + length - 4, ".zip"));
    int option_count;
    const ShelfOption *options = pcsx_shelf_options(arcade ? PCSX_OPTIONS_ARCADE : PCSX_OPTIONS_DISC, &option_count);
    /* An arcade game's frame can be its cabinet's screen on the shelf:
     * snap/<set>.png, the art name of a MAME set. */
    static const char *const arcade_actions[] = { "Use as cabinet art" };
    std::string art;
    if (arcade) {
        const char *slash = strrchr(task->game_path, '/');
        const std::string file = slash ? slash + 1 : task->game_path;
        art = std::string(APP_ROOT "/snap/") + (prepared ? profile.set : file.substr(0, file.size() - 4)) + ".png";
    }
    const ShelfMenus menus = { options, option_count, (void *)task->game_path,
        VitaShelf_GetOption, VitaShelf_SetOption, retro_serialize_size() ? 10 : 0,
        state_exists, arcade ? arcade_actions : NULL, arcade ? 1 : 0 };
    const ShelfConfig config = VitaShelf_Config(&menus);
    const std::string title = VitaShelf_GameTitle(task->game_path);

    task->result = VITA_PAUSE_RESUME;
    if (!shelf_init(&config)) {
        sceClibPrintf("pcsx-vita: pause menu init failed\n");
        shelf_shutdown();
        return;
    }
    shelf_pause_open(title.c_str(), VitaSaveStates_GetSlot(), task->start_in_save_states && menus.state_slots);
    /* A game without art gets the frame it was paused on. */
    SceIoStat stat;
    bool snapshot = arcade && sceIoGetstat(art.c_str(), &stat) < 0;
    unsigned ignored = ~0u;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();
    for (;;) {
        const ShelfInput input = VitaShelf_ReadInput(&ignored, &previous_time);
        int slot = VitaSaveStates_GetSlot();
        GLint framebuffer = 0;

        presenter->present_frame();
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (snapshot) {
            /* The game's frame, before the menu is drawn over it. */
            int rect[4];
            snapshot = false;
            presenter->frame_rect(rect);
            const int saved = shelf_snapshot_save(art.c_str(), rect[0], rect[1], rect[2], rect[3]);
            sceClibPrintf("pcsx-vita: cabinet art %s %s\n", art.c_str(), saved == 0 ? "saved" : "failed");
            shelf_toast(saved == 0 ? "Cabinet art saved" : "Could not save the cabinet art");
        }
        const int action = shelf_pause_frame(&input, &slot);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
#ifdef VITA_DEBUG_SHOT
        VitaShelf_ShotPoll();
#endif
        presenter->swap();
        if (slot != VitaSaveStates_GetSlot())
            VitaSaveStates_SetSlot(slot);
        if (action == SHELF_PAUSE_ACTION) {
            snapshot = true;
            continue;
        }
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
    const VitaShelfPresenter *presenter = VitaShelf_Presenter();
    if (!presenter)
        return VITA_PAUSE_RESUME;
    presenter->run_task(pause_task, &task);
    return task.result;
}
