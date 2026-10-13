/* The production libretro input bridge, with front-touch callbacks supplied
 * by a headless host. Uses a local Point Blank 2 zip (no ROMs bundled). */
#define main measurement_host_main
#include "../tools/libretro-host/s12_libretro_host.c"
#undef main
#include <assert.h>
static int touched, pad_trigger, mapped;
static int16_t pointer_x=-32767, pointer_y=32767;
static int16_t pointer_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port) return 0;
    if ((device&RETRO_DEVICE_MASK)==RETRO_DEVICE_POINTER) {
        if (id==RETRO_DEVICE_ID_POINTER_PRESSED) return touched;
        if (id==RETRO_DEVICE_ID_POINTER_X) return pointer_x;
        if (id==RETRO_DEVICE_ID_POINTER_Y) return pointer_y;
    }
    if ((device&RETRO_DEVICE_MASK)==RETRO_DEVICE_JOYPAD &&
        id==(mapped?RETRO_DEVICE_ID_JOYPAD_X:RETRO_DEVICE_ID_JOYPAD_B)) return pad_trigger;
    return 0;
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==4); mapped=argc==4;
    if (mapped) {
        const char *keys[]={"cross","circle","square","triangle","l1","r1","l2","r2","select","start"};
        for(unsigned i=0;i<10;i++) {
            snprintf(overrides[i].key,sizeof(overrides[i].key),"pcsx_rearmed_arcade_%s",keys[i]);
            strcpy(overrides[i].value,i==3?"button1":"none");
        }
        override_count=10;
    }
    core.handle=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!core.handle) {fprintf(stderr,"%s\n",dlerror());return 1;}
    core.set_environment=symbol("retro_set_environment",1);
    core.set_video_refresh=symbol("retro_set_video_refresh",1);
    core.set_audio_sample=symbol("retro_set_audio_sample",1);
    core.set_audio_sample_batch=symbol("retro_set_audio_sample_batch",1);
    core.set_input_poll=symbol("retro_set_input_poll",1);
    core.set_input_state=symbol("retro_set_input_state",1);
    core.init=symbol("retro_init",1);core.deinit=symbol("retro_deinit",1);
    core.load_game=symbol("retro_load_game",1);core.unload_game=symbol("retro_unload_game",1);
    core.run=symbol("retro_run",1);
    int (*read_board)(uint32_t,unsigned,uint32_t*)=symbol("system12_read",1);
    uint8_t (*jamma)(unsigned)=symbol("system12_jamma_read",1);
    core.set_environment(environment_cb);core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);core.set_input_state(pointer_input);core.init();
    struct retro_game_info info={0}; info.path=argv[2];assert(core.load_game(&info));
    touched=1;core.run();uint32_t value;
    assert(read_board(0x1f780000,2,&value)&&value==0xdc);
    assert(read_board(0x1f780004,2,&value)&&value==0x11c);
    assert(!(jamma(1)&0x10));
    touched=0;pad_trigger=1;core.run();assert(!(jamma(1)&0x10));
    pad_trigger=0;core.run();assert(jamma(1)&0x10);
    assert(read_board(0x1f780004,2,&value)&&value==0x11c); /* release holds aim */
    pointer_x=32767;pointer_y=-32767;touched=1;core.run();
    assert(read_board(0x1f780000,2,&value)&&value==0x388);
    assert(read_board(0x1f780004,2,&value)&&value==0x28);
    touched=0;core.run();assert(jamma(1)&0x10);
    core.unload_game();core.deinit();dlclose(core.handle);free(last_frame);
    printf("System12 production libretro pointer coordinates, tap/release and %s pad merging PASS\n",mapped?"remapped":"default");
}
