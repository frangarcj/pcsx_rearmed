/* Real SPU worker: compare sample hashes with serial synthesis, no game ROMs. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>
#include <pthread.h>
static pthread_t owner;
static uint64_t sample_hash = UINT64_C(14695981039346656037), nonzero;
static size_t checked_audio(const int16_t *samples, size_t frames)
{
    assert(pthread_equal(owner, pthread_self()));
    for (size_t i = 0; i < frames * 2; i++) {
        sample_hash ^= (uint16_t)samples[i]; sample_hash *= UINT64_C(1099511628211);
        nonzero += samples[i] != 0;
    }
    return audio_batch_cb(samples, frames);
}
int main(int argc, char **argv)
{
    assert(argc == 4);
    const int threaded = atoi(argv[3]);
    owner = pthread_self();
    strcpy(overrides[0].key, "pcsx_rearmed_system12_board_thread");
    strcpy(overrides[0].value, threaded ? "enabled" : "disabled"); override_count = 1;
    save_dir = NULL;
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(core.handle);
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch); LOAD(set_input_poll); LOAD(set_input_state);
    LOAD(init); LOAD(deinit); LOAD(load_game); LOAD(unload_game); LOAD(run);
#undef LOAD
    void (*write_reg)(unsigned long, unsigned short, unsigned) = symbol("SPUwriteRegister", 1);
    void (*dma_write)(unsigned short *, int, unsigned) = symbol("SPUwriteDMAMem", 1);
    void (*dma_read)(unsigned short *, int, unsigned) = symbol("SPUreadDMAMem", 1);
    void (*render)(unsigned, unsigned) = symbol("SPUasync", 1);
    unsigned (*end_cycle)(void) = symbol("dfsound_output_end_cycle", 1);
    int (*status)(unsigned *) = symbol("spu_thread_status", 1);
    void (*reset)(void) = symbol("retro_reset", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(checked_audio);
    core.set_input_poll(input_poll_cb); core.set_input_state(input_state_cb); core.init();
    struct retro_game_info info = {0}; info.path = argv[2]; assert(core.load_game(&info));
    assert(status(NULL) == threaded);
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned cycle = end_cycle(), jobs = 0;
        unsigned short block[8] = {0x0704, 0x1234, 0x5678, 0x9abc, 0xdef1, 0x4321, 0x8765, 0xcba9};
#define REG(reg, val) write_reg(0x1f801000u + (reg), (val), cycle)
        REG(0xda6, 0x200); dma_write(block, 8, cycle);
        REG(0xd80, 0x3fff); REG(0xd82, 0x3fff); REG(0xdaa, 0xc000);
        for (unsigned ch = 0; ch < 4; ch++) {
            REG(0xc00 + ch*16, 0x2800); REG(0xc02 + ch*16, 0x2000);
            REG(0xc04 + ch*16, 0x800 + ch*0x400); REG(0xc06 + ch*16, 0x200);
            REG(0xc08 + ch*16, 0x00ff); REG(0xc0a + ch*16, 0x1fc0);
            REG(0xc0e + ch*16, 0x200);
        }
        REG(0xd88, 15);
        for (unsigned step = 0; step < 180; step++) {
            cycle += 256 * 768; render(cycle, 1);
            if (step == 12) { REG(0xc00, 0x1800); REG(0xd80, 0x3000); }
            if (step == 24) { REG(0xc04, 0x1a00); }
            if (step == 36) { REG(0xd94, 4); } /* noise */
            if (step == 48) { REG(0xd94, 0); REG(0xd90, 2); } /* pitch modulation */
            if (step == 52) { REG(0xd94, 1); } /* noise as modulation source */
            if (step == 56) { REG(0xd94, 0); }
            if (step == 60) {
                REG(0xd90, 0); REG(0xda2, 0x8000);
                /* Nonzero reverb feedback, input and output coefficients. */
                for (unsigned reg = 0; reg < 32; reg++) {
                    unsigned value = reg < 2 ? 4 : reg < 10 || reg >= 30 ? 0x4000 : 0x20 + reg*2;
                    REG(0xdc0 + reg*2, value);
                }
                REG(0xd84, 0x2000); REG(0xd86, 0x2000);
                REG(0xd98, 15); REG(0xdaa, 0xc080); REG(0xd88, 15);
            }
            if (step == 80) { /* DMA must fence the worker before replacing its samples. */
                REG(0xda6, 0x200); block[1] ^= 0xffff; dma_write(block, 8, cycle);
                unsigned short copy[8]; REG(0xda6, 0x200); dma_read(copy, 8, cycle);
                assert(!memcmp(copy, block, sizeof(block)));
            }
            if (step == 100) { REG(0xdaa, 0xc084); } /* CD reverb serial fallback */
            if (step == 110) { REG(0xdaa, 0xc080); }
            if (step == 140) { REG(0xd8c, 8); }
        }
        assert(status(&jobs) == threaded);
        assert(!threaded || jobs > 50);
        fprintf(stderr, "SPU pass=%u worker=%d jobs=%u\n", pass, threaded, jobs);
        /* Reset, then unload, each with another queued audio block. */
        cycle += 256 * 768; render(cycle, 0);
        if (!pass) { reset(); assert(status(NULL) == threaded); }
#undef REG
    }
    core.unload_game(); assert(!status(NULL));
    assert(nonzero > 10000);
    fprintf(stderr, "RESULT frames=%llu hash=%016llx nonzero=%llu\n",
        (unsigned long long)audio_frames, (unsigned long long)sample_hash,
        (unsigned long long)nonzero);
    core.deinit(); dlclose(core.handle); free(last_frame);
    return 0;
}
