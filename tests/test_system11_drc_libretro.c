/* Assert real MIPS/C76 results from the production libretro core. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>
#include <pthread.h>
static pthread_t main_thread;
static uint64_t audio_hash = UINT64_C(14695981039346656037);
static size_t checked_audio(const int16_t *data, size_t frames)
{
    assert(pthread_equal(pthread_self(), main_thread));
    for (size_t i = 0; i < frames * 2; i++) {
        audio_hash ^= (uint16_t)data[i];
        audio_hash *= UINT64_C(1099511628211);
    }
    return audio_batch_cb(data, frames);
}

int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    main_thread = pthread_self();
    const int threaded = argc == 6;
    const int drc = !strcmp(argv[4], "ari64");
    const int banked = strcmp(argv[3], "starswep") != 0;
    const int upper_latch = !strcmp(argv[3], "myangel3");
    strcpy(overrides[0].key, "pcsx_rearmed_drc");
    strcpy(overrides[0].value, drc ? "enabled" : "disabled"); override_count = 1;
    strcpy(overrides[1].key, "pcsx_rearmed_system12_board_thread");
    strcpy(overrides[1].value, threaded ? "enabled" : "disabled"); override_count++;
    save_dir = NULL;
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(core.handle);
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch); LOAD(set_input_poll); LOAD(set_input_state);
    LOAD(init); LOAD(deinit); LOAD(load_game); LOAD(unload_game); LOAD(run);
    LOAD(get_memory_data); LOAD(get_memory_size);
#undef LOAD
    core.psx_cpu = symbol("psxCpu", 1); core.psx_rec = symbol("psxRec", 1);
    void (*reset)(void) = symbol("retro_reset", 1);
    void (*report)(FILE *) = symbol("system11_report", 1);
    int (*sync)(uint32_t) = symbol("system11_sync", 1);
    int (*active)(void) = symbol("system11_board_thread_active", 1);
    int (*start)(void) = symbol("system11_board_thread_start", 1);
    void (*stop)(void) = symbol("system11_board_thread_stop", 1);
    int (*nvram)(unsigned char *, size_t) = symbol("system11_eeprom_export", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(checked_audio);
    core.set_input_poll(input_poll_cb); core.set_input_state(input_state_cb); core.init();
    struct retro_game_info info = {0}; info.path = argv[2]; assert(core.load_game(&info));
    assert(active() == threaded);
    for (unsigned pass = 0; pass < 2; pass++) {
        assert((*core.psx_cpu == core.psx_rec) == drc);
        for (unsigned frame = 0; frame < 4; frame++) core.run();
        const uint32_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
#define CHECK(offset, expected) do { \
    unsigned actual = ram[(0x300000 + (offset)) / 4]; \
    if (actual != (unsigned)(expected)) fprintf(stderr, "%s %s pass=%u offset=%x actual=%08x expected=%08x\n", argv[3], argv[4], pass, (offset), actual, (unsigned)(expected)); \
    assert(actual == (unsigned)(expected)); } while (0)
        if (banked) {
            for (unsigned window = 0; window < 8; window++) {
                CHECK(window * 16, window + 1);
                CHECK(window * 16 + 4, window + 9);
                CHECK(window * 16 + 8, window + 1);
            }
            CHECK(0xa0, 2); CHECK(0xa4, 1);
            if (upper_latch) { CHECK(0xb0, 1); CHECK(0xb4, 17); }
            CHECK(0xb8, 0x70); CHECK(0xbc, 0x71);
        }
        CHECK(0x80, 0x11223344); CHECK(0x84, 0x1122); CHECK(0x88, 0x44);
        CHECK(0x8c, 0xd15c0de0); CHECK(0x90, 0x31); CHECK(0x94, 0x42);
        CHECK(0x98, 0x3344); CHECK(0x100, 0x51a7face); CHECK(0x104, 0);
#undef CHECK
        /* More than two audio rings in one command: fences/stop must pump
         * while waiting, and no frontend callback may run on the worker. */
        assert(sync(0x06000000) == 0);
        unsigned char image[2080];
        assert(nvram(image, sizeof(image)) == 0);
        if (threaded) { stop(); assert(!active()); assert(start() == 0); }
        assert(sync(0x07000000) == 0);
        char diagnostics[2048] = {0}; FILE *file = tmpfile(); assert(file);
        report(file); rewind(file); fread(diagnostics, 1, sizeof(diagnostics) - 1, file); fclose(file);
        assert(strstr(diagnostics, "illegal=0 unsupported=0 fault=0"));
        assert(audio_frames > 200000);
        fprintf(stderr, "RESULT %s %s pass=%u frames=%llu hash=%016llx\n",
            argv[3], argv[4], pass, (unsigned long long)audio_frames,
            (unsigned long long)audio_hash);
        assert(active() == threaded);
        fprintf(stderr, "%s %s reset-pass=%u bank/shared-C76/SMC/upper-ROM/RAM signatures PASS\n%s", argv[3], argv[4], pass, diagnostics);
        reset();
        assert(active() == threaded);
    }
    /* Unload with queued work also joins before freeing devices. */
    assert(sync(0x02000000) == 0);
    core.unload_game(); assert(!active()); assert((*core.psx_cpu == core.psx_rec) == drc);
    assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x200000);
    core.deinit(); dlclose(core.handle); free(last_frame);
    return 0;
}
