/* Instruction-driven Capcom ZN tests using the production libretro core. */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>
#include <pthread.h>
#undef assert
#define assert(expr) do { if (!(expr)) { fprintf(stderr,"check failed line=%u: %s\n",__LINE__,#expr); fflush(stderr); _Exit(2); } } while (0)
static pthread_t owner;
static int pressed;
static uint64_t sample_hash = UINT64_C(14695981039346656037), nonzero;
static size_t checked_audio(const int16_t *data, size_t frames)
{
    assert(pthread_equal(owner, pthread_self()));
    for (size_t i = 0; i < frames * 2; i++) {
        sample_hash ^= (uint16_t)data[i]; sample_hash *= UINT64_C(1099511628211);
        nonzero += data[i] != 0;
    }
    return audio_batch_cb(data, frames);
}
static int16_t buttons(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    return !port && device == RETRO_DEVICE_JOYPAD && pressed &&
        (id == RETRO_DEVICE_ID_JOYPAD_B || id == RETRO_DEVICE_ID_JOYPAD_X);
}
int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    owner = pthread_self();
    const int threaded = argc == 6, drc = !strcmp(argv[4], "ari64");
    const unsigned banks = !strcmp(argv[3], "sfex") ? 8 : 11;
    strcpy(overrides[0].key, "pcsx_rearmed_drc"); strcpy(overrides[0].value, drc ? "enabled" : "disabled");
    strcpy(overrides[1].key, "pcsx_rearmed_system12_board_thread");
    strcpy(overrides[1].value, threaded ? "enabled" : "disabled"); override_count = 2;
    save_dir = NULL;
    core.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(core.handle);
#define LOAD(name) core.name = symbol("retro_" #name, 1)
    LOAD(set_environment); LOAD(set_video_refresh); LOAD(set_audio_sample); LOAD(set_audio_sample_batch);
    LOAD(set_input_poll); LOAD(set_input_state); LOAD(init); LOAD(deinit); LOAD(load_game);
    LOAD(unload_game); LOAD(run); LOAD(get_memory_data); LOAD(get_memory_size);
#undef LOAD
    core.psx_cpu = symbol("psxCpu", 1); core.psx_rec = symbol("psxRec", 1);
    void (*reset)(void) = symbol("retro_reset", 1);
    void (*report)(FILE *) = symbol("systemzn_report", 1);
    int (*sync)(uint32_t) = symbol("systemzn_sync", 1);
    int (*active)(void) = symbol("systemzn_board_thread_active", 1);
    void (*stop)(void) = symbol("systemzn_board_thread_stop", 1);
    int (*start)(void) = symbol("systemzn_board_thread_start", 1);
    core.set_environment(environment_cb); core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb); core.set_audio_sample_batch(checked_audio);
    core.set_input_poll(input_poll_cb); core.set_input_state(buttons); core.init();
    struct retro_game_info info = {0}; info.path = argv[2]; assert(core.load_game(&info));
    for (unsigned pass = 0; pass < 2; pass++) {
        assert((*core.psx_cpu == core.psx_rec) == drc && active() == threaded);
        pressed = 1; for (unsigned f = 0; f < 4; f++) core.run();
        const uint32_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
#define CHECK(offset, expected) do { unsigned actual=ram[(0x300000+(offset))/4]; \
    if(actual!=(unsigned)(expected)) fprintf(stderr,"%s %s pass=%u offset=%x actual=%08x expected=%08x\n",argv[3],argv[4],pass,(offset),actual,(unsigned)(expected)); \
    assert(actual==(unsigned)(expected)); } while(0)
        CHECK(0, 0xef); CHECK(4, banks == 8 ? 0xfffffffe : 0xfffffffd);
        CHECK(0x100, 0x31); CHECK(0x104, 8); CHECK(0x108, 0x11223344); CHECK(0x10c, 0x001f001f);
        CHECK(0x110, 0x51a7cafe); CHECK(0x114, 0); CHECK(0x118, 0x1f); CHECK(0x11c, 0x5a);
        assert(ram[(0x300000+0x120)/4]&0x80); CHECK(0x124, 0);
        for(unsigned bank=0;bank<banks;bank++) CHECK(0x180+bank*4,bank+1);
        CHECK(0x1c0,1); CHECK(0x1c4,2); CHECK(0x1c8,1);
        CHECK(0x1d0,0x66); CHECK(0x1d4,0x67); CHECK(0x1d8,0x4e); CHECK(0x1dc,0x72);
        CHECK(0x1e0,0x9f000000); CHECK(0x300,0x31); CHECK(0x304,0x42);
        pressed=0;core.run();core.run();CHECK(0,0xff);CHECK(4,0xffffffff);
#undef CHECK
        /* More than one audio ring: the main waiter must keep pumping. */
        assert(sync(0x09000000) == 0);
        char text[4096]={0}; FILE *file=tmpfile();assert(file);
        report(file);rewind(file);fread(text,1,sizeof(text)-1,file);fclose(file);
        assert(strstr(text,"faults=0")); assert(strstr(text,"Z80ram[f010/f020]=40/"));
        assert(nonzero>100 && audio_frames>70000);
        fprintf(stderr,"RESULT %s %s pass=%u frames=%llu hash=%016llx\n",argv[3],argv[4],pass,
            (unsigned long long)audio_frames,(unsigned long long)sample_hash);
        fprintf(stderr,"%s",text);
        if(threaded){stop();assert(!active());assert(start()==0);}
        assert(sync(0x0a000000)==0);reset();assert(active()==threaded);
    }
    assert(sync(0x02000000)==0);core.unload_game();assert(!active());
    assert((*core.psx_cpu == core.psx_rec)==drc && core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM)==0x200000);
    core.deinit();dlclose(core.handle);free(last_frame);return 0;
}
