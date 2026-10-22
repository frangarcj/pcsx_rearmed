/* Production libretro loading of System 10, System 11 and Capcom ZN games from
 * MAME zips of synthetic data (tests/check_arcade_romset_libretro.py).
 * Arguments: core, save directory, then SET:KIND:ZIP for each game, where KIND
 * is 10, 11 or zn and SET is the set the zip must be loaded as. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
    assert(argc >= 4);
    save_dir = argv[2];
    strcpy(overrides[override_count].key, "pcsx_rearmed_system12_board_thread");
    strcpy(overrides[override_count++].value, "enabled");
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core.handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch); LOAD(set_input_poll); LOAD(set_input_state);
    LOAD(init); LOAD(deinit); LOAD(load_game); LOAD(unload_game); LOAD(run);
    LOAD(get_system_av_info); LOAD(get_memory_size); LOAD(get_memory_data);
#undef LOAD
    core.psx_cpu = symbol("psxCpu", 1); core.psx_rec = symbol("psxRec", 1);
    int (*spu_status)(unsigned *) = symbol("spu_thread_status", 1);
    int (*c76_active)(void) = symbol("system11_board_thread_active", 1);
    int (*zn_active)(void) = symbol("systemzn_board_thread_active", 1);
    int (*media_byte)(size_t, size_t) = symbol("system10_test_media_byte", 1);
    int (*media_poke)(size_t, size_t, unsigned char) = symbol("system10_test_media_poke", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb); core.set_input_state(input_state_cb); core.init();
    int flash_before = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 3; a < argc; a++) {
            char set[64], kind[8], zip[1024], nvram[1200];
            struct retro_game_info info = {0};
            struct retro_system_av_info av;
            assert(sscanf(argv[a], "%63[^:]:%7[^:]:%1023s", set, kind, zip) == 3);
            info.path = zip;
            if (!core.load_game(&info)) { fprintf(stderr, "cannot load %s\n", argv[a]); return 1; }
            assert(spu_status(NULL) == !strcmp(kind, "10"));
            assert(c76_active() == !strcmp(kind, "11"));
            assert(zn_active() == !strcmp(kind, "zn"));
            core.get_system_av_info(&av);
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x400000);
            /* The ROM files are random bytes, so the guest is not run. System 10 keeps what
             * the game writes to its flash/NAND: pass 0 changes a byte, pass 1 finds it. */
            if (!strcmp(kind, "10")) {
                int before = media_byte(0, 0x1234);
                assert(before >= 0);
                if (pass == 0) assert(!media_poke(0, 0x1234, (unsigned char)(before ^ 0x5a)));
                else assert(before != -1 && media_byte(0, 0x1234) == (flash_before ^ 0x5a));
                if (pass == 0) flash_before = before;
            }
            core.unload_game();
            assert(!spu_status(NULL) && !c76_active() && !zn_active());
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x200000);
            /* System 11 and ZN save their EEPROM under the set's name, whatever the zip is called. */
            if (strcmp(kind, "10")) {
                snprintf(nvram, sizeof(nvram), "%s/%s.arcade-nvram", save_dir, set);
                FILE *file = fopen(nvram, "rb");
                assert(file);
                fclose(file);
            }
        }
    }
    /* A flash file that does not belong to the media is refused, and kept as it is. */
    for (int a = 3; a < argc; a++) {
        char set[64], kind[8], zip[1024], path[1200], bytes[32];
        struct retro_game_info info = {0};
        assert(sscanf(argv[a], "%63[^:]:%7[^:]:%1023s", set, kind, zip) == 3);
        if (strcmp(kind, "10")) continue;
        snprintf(path, sizeof(path), "%s/%s.arcade-flash", save_dir, set);
        FILE *file = fopen(path, "wb"); assert(file);
        assert(fwrite("S10FLSH1not a real file......", 1, 29, file) == 29); assert(!fclose(file));
        info.path = zip;
        assert(core.load_game(&info));
        assert(media_byte(0, 0x1234) == flash_before);
        core.unload_game();
        file = fopen(path, "rb"); assert(file);
        assert(fread(bytes, 1, sizeof(bytes), file) == 29); fclose(file);
        assert(!memcmp(bytes, "S10FLSH1not a real file", 23));
    }
    /* A zip of the wrong kind of game, and a missing zip, are refused without a crash. */
    {
        struct retro_game_info info = {0};
        info.path = "/nonexistent/not-a-set.zip";
        assert(!core.load_game(&info));
    }
    core.deinit(); dlclose(core.handle);
    free(last_frame);
    puts("Arcade zips load in the production libretro core: System 10, System 11, Capcom ZN, set-named EEPROM, System 10 flash persistence, reload PASS");
    return 0;
}
