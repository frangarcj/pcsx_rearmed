/* Real production core with generated MIPS; asserts results, not CPU labels. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>

int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    const int threaded = argc == 6;
    const int drc = !strcmp(argv[4], "ari64");
    const int memm = !strcmp(argv[3], "MEMM");
    strcpy(overrides[0].key, "pcsx_rearmed_drc");
    strcpy(overrides[0].value, drc ? "enabled" : "disabled");
    strcpy(overrides[1].key, "pcsx_rearmed_system12_board_thread");
    strcpy(overrides[1].value, threaded ? "enabled" : "disabled");
    override_count = 2;
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(core.handle);
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch); LOAD(set_input_poll); LOAD(set_input_state);
    LOAD(init); LOAD(deinit); LOAD(load_game); LOAD(unload_game); LOAD(run);
    LOAD(get_memory_data); LOAD(get_memory_size);
#undef LOAD
    core.psx_cpu = symbol("psxCpu", 1); core.psx_rec = symbol("psxRec", 1);
    void (*reset)(void) = symbol("retro_reset", 1);
    void (*report)(FILE *) = symbol("system10_report", 1);
    int (*spu_status)(unsigned *) = symbol("spu_thread_status", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb); core.set_input_state(input_state_cb); core.init();
    struct retro_game_info info = {0}; info.path = argv[2];
    assert(core.load_game(&info));
    assert(spu_status(NULL) == threaded);
    for (unsigned pass = 0; pass < 2; pass++) {
        assert((*core.psx_cpu == core.psx_rec) == drc);
        core.run(); core.run();
        report(stderr);
        const uint32_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        assert(core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0x400000);
        assert(ram[0x210010 / 4] == 0x51a7cafe && ram[0x10010 / 4] == 0);
        const unsigned expected_m[] = {0x5a, 0x6b, 0x5a, 0x5a, 0x7c};
        const unsigned expected_n[] = {0x11, 0x22, 0x33, 0x44};
        for (unsigned i = 0; i < (memm ? 5u : 4u); i++) {
            const unsigned actual = ram[(0x210080 + i * 4) / 4];
            const unsigned expected = memm ? expected_m[i] : expected_n[i];
            fprintf(stderr, "%s %s pass=%u signature[%u]=%08x expected=%08x\n", argv[3], argv[4], pass, i, actual, expected);
            assert(actual == expected);
        }
        assert(ram[0x2100a0 / 4] == 5 && ram[0x2100a4 / 4] == 0xce && ram[0x2100a8 / 4] == 0x50);
        reset();
        assert(spu_status(NULL) == threaded);
    }
    core.unload_game(); assert(!spu_status(NULL)); core.deinit(); dlclose(core.handle); free(last_frame);
    puts("System10 DRC bank/alias/flash, shared-RAM SMC, NAND DMA SMC, upper RAM, SIO and reset signatures PASS");
    return 0;
}
