/* GPL-2.0-or-later. */
/* Test the production SDL handler, including a virtual mapped gamepad. */
#define main harness_original_main
#define systemgnet_set_analog gnet_test_analog
#define systemzn_set_gun_position judge_test_gun
#define systemgq_set_gun_position gq_test_gun
#define systemgv_has_trackball gv_test_has_trackball
#define systemgv_trackball_delta gv_test_track_delta
#define systemgv_has_sensors gv_test_has_sensors
#define systemgv_sensor_values gv_test_sensor_values
#define systemgv_set_analog gv_test_sensor_set
#define systemgv_set_input gv_test_sensor_input
#include "../frontend/mac_harness.c"
#undef main
#include <assert.h>
static int gv_dx,gv_dy,gv_sensor_mode;static unsigned gv_heart=31,gv_gsr,gv_pulses;
int gv_test_has_sensors(void){return gv_sensor_mode && selected_board && selected_board->family==102;}
int gv_test_sensor_values(unsigned *h,unsigned *g){if(!gv_test_has_sensors())return -1;*h=gv_heart;*g=gv_gsr;return 0;}
int gv_test_sensor_set(const char *name,unsigned value){if(!strcmp(name,"heartbeat")){assert(value<=101);gv_heart=value;}else{assert(!strcmp(name,"gsr") && value<=128);gv_gsr=value;}return 0;}
int gv_test_sensor_input(const char *name,int down){assert(!strcmp(name,"heartbeat_pulse"));if(down)gv_pulses++;return 0;}

int gv_test_has_trackball(void){return selected_board && selected_board->family==102;}
int gv_test_track_delta(int x,int y){gv_dx+=x;gv_dy+=y;return 0;}
static unsigned gq_x[3],gq_y[3];
int gq_test_gun(unsigned p,unsigned x,unsigned y){assert(p<3);gq_x[p]=x;gq_y[p]=y;return 0;}
static unsigned judge_x[2],judge_y[2];
int judge_test_gun(unsigned p,unsigned x,unsigned y){assert(p<2);judge_x[p]=x;judge_y[p]=y;return 0;}
static unsigned wheel=128, trigger=128, handle[2]={128,128};
static int coin, start, fire, beast, beast2, service, raizing_mode, fifth, sixth;
int gnet_test_analog(const char *name,unsigned raw)
{
    if (!strcmp(name,"p1_wheel")) wheel=raw;
    if (!strcmp(name,"p1_trigger")) trigger=raw;
    if (!strcmp(name,"p1_handle")) handle[0]=raw;
    if (!strcmp(name,"p2_handle")) handle[1]=raw;
    return 0;
}
static int nba_coin[4],nba_start[4],nba_fourth[4];
static int digital(const char *name,int down)
{
    if(selected_board && selected_board->family==111){
        if(!strncmp(name,"coin",4) && name[4]>='1' && name[4]<='4')nba_coin[name[4]-'1']=down;
        if(name[0]=='p' && name[1]>='1' && name[1]<='4'){
            if(!strcmp(name+2,"start"))nba_start[name[1]-'1']=down;
            if(!strcmp(name+2,"button4"))nba_fourth[name[1]-'1']=down;
        }
    }
    if (raizing_mode && name[0] == 'p') assert(!strchr(name,'_'));
    if (!strcmp(name,"p1button4")) beast=down;
    if (!strcmp(name,"p1button5")) fifth=down;
    if (!strcmp(name,"p1button6")) sixth=down;
    if (!strcmp(name,"p2button4")) beast2=down;
    if (!strcmp(name,"service1")) service=down;
    if (!strcmp(name,"coin1")) coin=down;
    if ((!strcmp(name,"p1_start") || !strcmp(name,"p1start"))) start=down;
    if ((!strcmp(name,"p1_button1") || !strcmp(name,"p1button1"))) fire=down;
    return 0;
}
static unsigned nba_mix_frames;
static unsigned atlus_mix_frames;
static unsigned cbaj_mix_frames;
static void nba_mix_output(void *opaque,const int16_t *pcm,unsigned n)
{
    (void)opaque;for(unsigned i=0;i<n;i++){assert(pcm[2*i]==13926);assert(pcm[2*i+1]==10650);}nba_mix_frames+=n;
}
static void cbaj_mix_output(void *opaque,const int16_t *pcm,unsigned frames)
{
    (void)opaque;
    for(unsigned i=0;i<frames;i++) {assert(pcm[i*2]==8601);assert(pcm[i*2+1]==0);}
    cbaj_mix_frames+=frames;
}
static void atlus_mix_output(void *opaque,const int16_t *pcm,unsigned frames)
{
    (void)opaque;
    for(unsigned i=0;i<frames;i++) {assert(pcm[i*2]==5734);assert(pcm[i*2+1]==2867);}
    atlus_mix_frames+=frames;
}
static void key(SDL_Scancode code,int down)
{
    SDL_Event e={0}; e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.scancode=code;
    assert(SDL_PushEvent(&e));poll_interactive_input();
}
int main(void)
{
    selected_board=NULL;
    assert(console_input("p1cross",1)==0 && in_keystate[0]==0x4000);
    assert(console_input("p1start",1)==0 && in_keystate[0]==0x4008);
    assert(console_input("p2circle",1)==0 && in_keystate[1]==0x2000);
    assert(console_input("p1cross",0)==0 && in_keystate[0]==8);
    assert(console_input("p1button1",1)<0 && console_input("coin1",1)<0);
    assert(console_input("p3cross",1)<0 && console_input("p1cross",2)<0);
    harness_input_event pad_events[]={{0,"p1start",0},{1,"p2circle",0},{2,"p1square",1}};
    size_t pad_next=0;
    assert(!apply_input_events(pad_events,3,&pad_next,1) && pad_next==2);
    assert(!in_keystate[0] && !in_keystate[1]);
    assert(!apply_input_events(pad_events,3,&pad_next,2) && in_keystate[0]==0x8000);
    memset(in_keystate,0,sizeof(in_keystate));
    puts("Console replay uses PlayStation SIO pad bits and rejects arcade controls PASS");
    HarnessBoard board={0};board.family=103;board.set_input=digital;selected_board=&board;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    SDL_Window *w=SDL_CreateWindow("PCSX G-Net input test",800,600,SDL_WINDOW_HIDDEN);assert(w);
    SDL_Event e; while(SDL_PollEvent(&e)) {}
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);assert(coin && start);
    key(SDL_SCANCODE_RIGHT,1);key(SDL_SCANCODE_A,1);assert(wheel==255 && trigger==255 && fire);
    key(SDL_SCANCODE_RIGHT,0);key(SDL_SCANCODE_A,0);assert(wheel==128 && trigger==128 && !fire);
    key(SDL_SCANCODE_LEFT,1);key(SDL_SCANCODE_S,1);key(SDL_SCANCODE_L,1);
    assert(wheel==0 && trigger==0 && handle[0]==1 && handle[1]==255);
    release_interactive_input();assert(!coin && !start && wheel==128 && trigger==128);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_MOTION;e.motion.windowID=SDL_GetWindowID(w);
    e.motion.x=799;e.motion.xrel=-20;assert(SDL_PushEvent(&e));poll_interactive_input();
    assert(wheel==255 && handle[0]==1);
    frame_counter++;poll_interactive_input();assert(handle[0]==128);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_LEFT;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(trigger==255 && !fire);
    key(SDL_SCANCODE_A,1);e.type=SDL_EVENT_MOUSE_BUTTON_UP;assert(SDL_PushEvent(&e));
    poll_interactive_input();assert(trigger==255);key(SDL_SCANCODE_A,0);assert(trigger==128);
    release_interactive_input();
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="PCSX validation gamepad";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);
    SDL_Joystick *joy=SDL_OpenJoystick(id);assert(joy);
    refresh_interactive_gamepads();assert(interactive_pads[0]);
    assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,-32768));
    assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_START,true));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();
    fprintf(stderr,"Virtual pad: wheel=%u handle=%u trigger=%u start=%d axes=%d/%d/%d\n",
        wheel,handle[0],trigger,start,
        SDL_GetGamepadAxis(interactive_pads[0],SDL_GAMEPAD_AXIS_LEFTX),
        SDL_GetGamepadAxis(interactive_pads[0],SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
        SDL_GetGamepadAxis(interactive_pads[0],SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    assert(wheel==0 && handle[0]==1 && trigger==255 && start);
    interactive_replay=1;wheel=71;trigger=99;
    key(SDL_SCANCODE_RIGHT,1);assert(wheel==71 && trigger==99);interactive_replay=0;
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(wheel==128 && trigger==128 && !start);
    board.family=104; interactive_focused=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);assert(coin && fire && start);
    key(SDL_SCANCODE_5,0);key(SDL_SCANCODE_A,0);assert(!coin && !fire);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(fire);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!coin && !fire && !start);
    puts("Tecmo TPS SDL keyboard/gamepad controls and focus release PASS");
    board.family=105; interactive_focused=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);assert(coin && fire && start);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!coin && !fire && !start);
    puts("FX1Z SDL input translation and focus release PASS");
    board.family=106; interactive_focused=1; raizing_mode=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);
    key(SDL_SCANCODE_SPACE,1);key(SDL_SCANCODE_V,1);key(SDL_SCANCODE_9,1);
    assert(coin && fire && start && beast && beast2 && service);
    release_interactive_input();assert(!coin && !fire && !start && !beast && !beast2 && !service);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_NORTH,true));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(beast);
    interactive_replay=1;beast=0;key(SDL_SCANCODE_SPACE,1);assert(!beast);interactive_replay=0;
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!beast && !beast2);
    puts("Raizing SDL exact input names, fourth buttons, service, gamepad, focus and replay PASS");
    board.family=107; interactive_focused=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);key(SDL_SCANCODE_9,1);
    assert(coin && start && fire && service);
    release_interactive_input();assert(!coin && !start && !fire && !service);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_BACK,true));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(coin && fire);
    interactive_replay=1;coin=fire=0;key(SDL_SCANCODE_A,1);assert(!coin && !fire);interactive_replay=0;
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!coin && !start && !fire && !service);
    puts("Visco SDL exact input names, coin/start/fire/service, gamepad, focus and replay PASS");
    board.family=108; interactive_focused=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);key(SDL_SCANCODE_9,1);
    assert(coin && start && fire && service);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!coin && !start && !fire && !service);
    arcade_mix_init_rates(&interactive_mix,33868800,16934400,192,atlus_mix_output,NULL);
    interactive_volume=100;
    const int16_t source_spu[2]={16384,-16384};
    for(unsigned i=0;i<512;i++) {
        interactive_spu_pcm((void *)source_spu,4);
        interactive_zoom_pcm(NULL,.25f,.5f,i*384);
        interactive_zoom_pcm(NULL,.25f,.5f,i*384+192);
    }
    assert(atlus_mix_frames==512 && !interactive_mix.error && !interactive_mix.clipped);
    puts("Atlus SDL controls/focus and production SPU0.175/YMZ0.35 gain/rate routing PASS");
    board.family=109; interactive_focused=1;
    key(SDL_SCANCODE_5,1);key(SDL_SCANCODE_1,1);key(SDL_SCANCODE_A,1);key(SDL_SCANCODE_9,1);
    assert(coin && start && fire && service);
    release_interactive_input();assert(!coin && !start && !fire && !service);
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(coin && start && fire);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!coin && !start && !fire && !service);
    arcade_mix_init_rates(&interactive_mix,33868800,16934400,192,cbaj_mix_output,NULL);
    for(unsigned i=0;i<512;i++) {
        interactive_spu_pcm((void *)source_spu,4);
        interactive_zoom_pcm(NULL,.25f,.5f,i*384);
        interactive_zoom_pcm(NULL,.25f,.5f,i*384+192);
    }
    assert(cbaj_mix_frames==512 && !interactive_mix.error && !interactive_mix.clipped);
    puts("CBaj SDL keyboard/gamepad/focus and production SPU0.35/YMZ0.35 gain/rate routing PASS");
    board.family=110;interactive_focused=1;
    key(SDL_SCANCODE_F,1);key(SDL_SCANCODE_H,1);assert(fifth && sixth);
    release_interactive_input();assert(!fifth && !sixth);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(fifth && sixth && fire && start);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!fifth && !sixth && !fire && !start);
    puts("Primal SDL six-button keyboard/gamepad shoulder mapping and focus release PASS");
    board.family=111;interactive_focused=1;
    key(SDL_SCANCODE_7,1);key(SDL_SCANCODE_8,1);key(SDL_SCANCODE_3,1);key(SDL_SCANCODE_4,1);
    key(SDL_SCANCODE_KP_9,1);key(SDL_SCANCODE_F8,1);
    assert(nba_coin[2] && nba_coin[3] && nba_start[2] && nba_start[3] && nba_fourth[2] && nba_fourth[3]);
    release_interactive_input();assert(!nba_coin[2] && !nba_coin[3] && !nba_start[2] && !nba_start[3] && !nba_fourth[2] && !nba_fourth[3]);
    SDL_JoystickID extra_id[3];SDL_Joystick *extra[3];
    for(unsigned i=0;i<3;i++){
        extra_id[i]=SDL_AttachVirtualJoystick(&desc);assert(extra_id[i]);extra[i]=SDL_OpenJoystick(extra_id[i]);assert(extra[i]);
        assert(SDL_SetJoystickVirtualButton(extra[i],SDL_GAMEPAD_BUTTON_START,true));
        assert(SDL_SetJoystickVirtualButton(extra[i],SDL_GAMEPAD_BUTTON_BACK,true));
        assert(SDL_SetJoystickVirtualButton(extra[i],SDL_GAMEPAD_BUTTON_NORTH,true));
    }
    SDL_UpdateJoysticks();refresh_interactive_gamepads();frame_counter++;poll_interactive_input();
    for(unsigned i=1;i<4;i++)assert(interactive_pads[i] && nba_coin[i] && nba_start[i] && nba_fourth[i]);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;assert(SDL_PushEvent(&e));poll_interactive_input();
    for(unsigned i=0;i<4;i++)assert(!nba_coin[i] && !nba_start[i] && !nba_fourth[i]);
    for(unsigned i=0;i<3;i++){
        SDL_CloseGamepad(interactive_pads[i+1]);interactive_pads[i+1]=NULL;SDL_CloseJoystick(extra[i]);assert(SDL_DetachVirtualJoystick(extra_id[i]));
    }
    arcade_mix_init_rates(&interactive_mix,33868800,16670000,1,nba_mix_output,NULL);
    unsigned ni=0,di=0;const float pair[2]={.25f,.5f};
    while(ni<1024 || di<734){
        uint64_t tick=((uint64_t)di*16670000u+31570)/31571;
        if(ni<1024 && (di==734 || (uint64_t)ni*16670000u<=tick*44100u)){
            interactive_spu_pcm((void *)source_spu,4);ni++;
        }else {interactive_rax_pcm(NULL,pair,1,31571,&tick);di++;}
    }
    assert(nba_mix_frames==1024 && !interactive_mix.error && !interactive_mix.clipped);
    puts("NBA SDL four-player coins/start/buttons, four mapped gamepads, focus release and production SPU0.35/RAX1 timed stereo routing PASS");
    board.family=112;interactive_focused=1;
    assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,0));assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTY,0));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();
    assert(judge_x[0]==0x822 && judge_y[0]==0x122);
    SDL_CloseGamepad(interactive_pads[0]);interactive_pads[0]=NULL;
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_MOTION;e.motion.windowID=SDL_GetWindowID(w);e.motion.x=400;e.motion.y=300;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(judge_x[0]>=0x820 && judge_x[0]<=0x825 && judge_y[0]>=0x121 && judge_y[0]<=0x123);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.windowID=SDL_GetWindowID(w);e.button.x=-10;e.button.y=300;e.button.button=SDL_BUTTON_LEFT;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(fire && judge_x[0]==0x392 && judge_y[0]==0x02c);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;assert(SDL_PushEvent(&e));poll_interactive_input();assert(!fire && !interactive_mouse_trigger);
    puts("Judge SDL mouse/gamepad raw aim, offscreen trigger, viewport scaling and focus release PASS");
    board.family=113;interactive_focused=1;
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_MOTION;e.motion.windowID=SDL_GetWindowID(w);e.motion.x=400;e.motion.y=300;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(gq_x[0]>=0x11c && gq_x[0]<=0x11f && gq_y[0]>=0x77 && gq_y[0]<=0x79);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.windowID=SDL_GetWindowID(w);e.button.x=-10;e.button.y=300;e.button.button=SDL_BUTTON_LEFT;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(fire && gq_x[0]==0 && gq_y[0]==255);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;assert(SDL_PushEvent(&e));poll_interactive_input();assert(!fire && !interactive_mouse_trigger);
    interactive_focused=1;refresh_interactive_gamepads();
    assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTX,32767));assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_LEFTY,-32768));
    SDL_UpdateJoysticks();frame_counter++;poll_interactive_input();assert(gq_x[0]==0x1bc && gq_y[0]==0);
    puts("GQ SDL source 9-bit gun X, mouse/gamepad extrema, offscreen trigger and focus release PASS");
    board.family=102;interactive_focused=1;interactive_gv_track_frame=~0u;
    frame_counter++;poll_interactive_input();assert(gv_dx==-63 && gv_dy==-64);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_MOTION;e.motion.windowID=SDL_GetWindowID(w);e.motion.xrel=3.5f;e.motion.yrel=-2.5f;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(gv_dx==-66 && gv_dy==-66);
    e.motion.xrel=.5f;e.motion.yrel=-.5f;assert(SDL_PushEvent(&e));poll_interactive_input();assert(gv_dx==-67 && gv_dy==-67);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_LEFT;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(fire);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;assert(SDL_PushEvent(&e));poll_interactive_input();assert(!fire && !interactive_mouse_trigger);
    SDL_AudioSpec gv_spec={SDL_AUDIO_S16,2,44100};
    assert(SDL_InitSubSystem(SDL_INIT_AUDIO));interactive_audio=SDL_CreateAudioStream(&gv_spec,&gv_spec);assert(interactive_audio);
    interactive_volume=100;int16_t gv_source[600],gv_output[600];
    for(unsigned n=0;n<300;n++){gv_source[2*n]=4000;gv_source[2*n+1]=-12000;}
    interactive_zn_spu_pcm(gv_source,sizeof(gv_source));assert(SDL_FlushAudioStream(interactive_audio));
    assert(SDL_GetAudioStreamData(interactive_audio,gv_output,sizeof(gv_output))==sizeof(gv_output));
    for(unsigned n=0;n<300;n++)assert(gv_output[2*n]==-9000 && gv_output[2*n+1]==3000);
    assert(!interactive_audio_error);SDL_DestroyAudioStream(interactive_audio);interactive_audio=NULL;
    puts("GV SDL fractional reversed-X trackball, frame-sampled gamepad, held mouse/focus release and source swapped SPU0.75 stereo PASS");
    gv_sensor_mode=1;interactive_focused=1;
    key(SDL_SCANCODE_EQUALS,1);key(SDL_SCANCODE_EQUALS,0);assert(gv_heart==41);
    key(SDL_SCANCODE_MINUS,1);key(SDL_SCANCODE_MINUS,0);assert(gv_heart==31);
    key(SDL_SCANCODE_PAGEUP,1);key(SDL_SCANCODE_PAGEUP,0);assert(gv_gsr==2);
    key(SDL_SCANCODE_PAGEDOWN,1);key(SDL_SCANCODE_PAGEDOWN,0);assert(gv_gsr==0);
    key(SDL_SCANCODE_H,1);key(SDL_SCANCODE_H,0);assert(gv_heart==0 && gv_pulses==1);
    gv_sensor_mode=0;puts("Tokimeki production SDL simulated/direct pulse and GSR controls PASS");
    refresh_interactive_gamepads();
    SDL_CloseGamepad(interactive_pads[0]);interactive_pads[0]=NULL;
    SDL_CloseJoystick(joy);assert(SDL_DetachVirtualJoystick(id));
    SDL_DestroyWindow(w);SDL_Quit();
    puts("G-Net SDL keyboard, mouse, mapped gamepad, raw extrema, focus release and replay isolation PASS");
}
