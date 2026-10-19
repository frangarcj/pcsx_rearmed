/* Exercise production libretro board switching with audited local media.
 * No game data is bundled. Arguments: core, System10, System11, CapcomZN. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>

static int pressed, mapped;
static int16_t arcade_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    return !port && device == RETRO_DEVICE_JOYPAD &&
        id == (mapped ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_B) ? pressed : 0;
}

int main(int argc, char **argv)
{
    const char *reports[] = {"system10_report", "system11_report", "systemzn_report"};
    const char *held[] = {"IN1=ffffffef", "inputs=ff/ff/ef/ff", "inputs=ef/ff/ff/ff/ff/ff"};
    unsigned rates[] = {66075, 88200, 24038};
    assert(argc == 5 || argc == 7);
    save_dir = NULL;
    if (argc == 7) {
        save_dir = argv[5]; mapped = !strcmp(argv[6], "mapped");
        if (mapped) {
            const char *keys[] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "select", "start"};
            for (unsigned k = 0; k < 10; k++) {
                snprintf(overrides[k].key, sizeof(overrides[k].key), "pcsx_rearmed_arcade_%s", keys[k]);
                strcpy(overrides[k].value, k == 3 ? "button1" : "none");
            }
            override_count = 10;
        }
    }
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
    size_t (*serialize_size)(void) = symbol("retro_serialize_size", 1);
    bool (*serialize)(void *, size_t) = symbol("retro_serialize", 1);
    bool (*unserialize)(const void *, size_t) = symbol("retro_unserialize", 1);
    void (*reset)(void) = symbol("retro_reset", 1);
    int (*spu_status)(unsigned *) = symbol("spu_thread_status", 1);
    int (*c76_active)(void) = symbol("system11_board_thread_active", 1);
    int (*zn_active)(void) = symbol("systemzn_board_thread_active", 1);
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
            assert(spu_status(NULL) == (i == 0));
            assert(c76_active() == (i == 1));
            assert(zn_active() == (i == 2));
            assert((*core.psx_cpu == core.psx_rec) == (i < 3));
            core.get_system_av_info(&av);
            assert((unsigned)av.timing.sample_rate == rates[i]);
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x400000);
            assert(serialize_size() == 0);
            unsigned char invalid_state = 0;
            assert(!serialize(&invalid_state, 1) && !unserialize(&invalid_state, 1));
            if (i && argc == 7 && pass) {
                int (*export_nvram)(unsigned char *, size_t) = symbol(i == 1 ? "system11_eeprom_export" : "systemzn_eeprom_export", 1);
                unsigned char actual[2080];
                assert(!export_nvram(actual, sizeof(actual)));
                for (unsigned b = 0; b < sizeof(actual); b++) assert(actual[b] == (unsigned char)(b * 17 + i));
            }
            pressed = 1; core.run();
            if (i == 0) {
                const uint32_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
                assert(ram[0x210010 / 4] == 0x51a7cafe);
                assert(ram[0x010010 / 4] == 0);
                assert(ram[0x210004 / 4] == 0x5a); /* Executed banked MIPS helper. */
            }
            FILE *file = tmpfile(); assert(file);
            report(file); rewind(file); fread(text, 1, sizeof(text) - 1, file); fclose(file);
            assert(strstr(text, held[i]));
            pressed = 0; core.run();
            file = tmpfile(); assert(file);
            memset(text, 0, sizeof(text));
            report(file); rewind(file); fread(text, 1, sizeof(text) - 1, file); fclose(file);
            assert(!strstr(text, held[i]));
            reset(); core.run();
            assert((*core.psx_cpu == core.psx_rec) == (i < 3));
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x400000);
            if (i && argc == 7) {
                int (*import_nvram)(const unsigned char *, size_t) = symbol(i == 1 ? "system11_eeprom_import" : "systemzn_eeprom_import", 1);
                unsigned char image[2080];
                for (unsigned b = 0; b < sizeof(image); b++) image[b] = (unsigned char)(b * 17 + i);
                assert(!import_nvram(image, sizeof(image)));
            }
            core.unload_game();
            assert(!spu_status(NULL) && !c76_active() && !zn_active());
            assert(*core.psx_cpu == core.psx_rec);
            assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x200000);
        }
    }
    if (argc == 7) {
        const char *sets[] = {"tekken", "sfex"};
        for (unsigned i = 1; i < 3; i++) {
            char path[1024], bytes[4] = {0};
            struct retro_game_info info = {0}; info.path = argv[i + 2];
            snprintf(path, sizeof(path), "%s/%s.arcade-nvram", save_dir, sets[i - 1]);
            FILE *file = fopen(path, "wb"); assert(file);
            assert(fwrite("BAD", 1, 3, file) == 3); assert(!fclose(file));
            assert(core.load_game(&info)); core.unload_game();
            file = fopen(path, "rb"); assert(file);
            assert(fread(bytes, 1, sizeof(bytes), file) == 3); fclose(file);
            assert(!memcmp(bytes, "BAD", 3));
        }
    }
    core.deinit(); dlclose(core.handle);
    free(last_frame);
    puts("Arcade libretro switching, reset, CPU restoration, mapped inputs, source audio rates, EEPROM persistence and savestate rejection PASS");
    return 0;
}
