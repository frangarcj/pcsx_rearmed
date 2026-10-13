/* Exercise production libretro board switching with audited local media.
 * No game data is bundled. Arguments: core, System10, System11, CapcomZN. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>

static int pressed;
static int16_t arcade_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    return !port && device == RETRO_DEVICE_JOYPAD &&
        id == RETRO_DEVICE_ID_JOYPAD_B ? pressed : 0;
}

int main(int argc, char **argv)
{
    const char *reports[] = {"system10_report", "system11_report", "systemzn_report"};
    const char *held[] = {"IN1=ffffffef", "inputs=ff/ff/ef/ff", "inputs=ef/ff/ff/ff/ff/ff"};
    unsigned rates[] = {66075, 88200, 24038};
    assert(argc == 5);
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core.handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch); LOAD(set_input_poll); LOAD(set_input_state);
    LOAD(init); LOAD(deinit); LOAD(load_game); LOAD(unload_game); LOAD(run);
    LOAD(get_system_av_info); LOAD(get_memory_size);
#undef LOAD
    core.psx_cpu = symbol("psxCpu", 1); core.psx_rec = symbol("psxRec", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb); core.set_input_state(arcade_input); core.init();
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < 3; i++) {
            struct retro_game_info info = {0};
            struct retro_system_av_info av;
            void (*report)(FILE *) = symbol(reports[i], 1);
            char text[4096] = {0};
            info.path = argv[i + 2];
            assert(core.load_game(&info));
            assert(*core.psx_cpu != core.psx_rec);
            core.get_system_av_info(&av);
            assert((unsigned)av.timing.sample_rate == rates[i]);
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x400000);
            pressed = 1; core.run();
            FILE *file = tmpfile(); assert(file);
            report(file); rewind(file); fread(text, 1, sizeof(text) - 1, file); fclose(file);
            assert(strstr(text, held[i]));
            pressed = 0; core.run();
            file = tmpfile(); assert(file);
            memset(text, 0, sizeof(text));
            report(file); rewind(file); fread(text, 1, sizeof(text) - 1, file); fclose(file);
            assert(!strstr(text, held[i]));
            core.unload_game();
            assert(*core.psx_cpu == core.psx_rec);
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x200000);
        }
    }
    core.deinit(); dlclose(core.handle);
    free(last_frame);
    puts("Arcade libretro load/unload, CPU restoration, input and source audio rates PASS");
    return 0;
}
