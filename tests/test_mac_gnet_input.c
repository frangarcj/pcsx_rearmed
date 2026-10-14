/* GPL-2.0-or-later. */
/* Test the production SDL handler, including a virtual mapped gamepad. */
#define main harness_original_main
#define systemgnet_set_analog gnet_test_analog
#include "../frontend/mac_harness.c"
#undef main
#include <assert.h>
static unsigned wheel=128, trigger=128, handle[2]={128,128};
static int coin, start, fire, beast, beast2, service, raizing_mode;
int gnet_test_analog(const char *name,unsigned raw)
{
    if (!strcmp(name,"p1_wheel")) wheel=raw;
    if (!strcmp(name,"p1_trigger")) trigger=raw;
    if (!strcmp(name,"p1_handle")) handle[0]=raw;
    if (!strcmp(name,"p2_handle")) handle[1]=raw;
    return 0;
}
static int digital(const char *name,int down)
{
    if (raizing_mode && name[0] == 'p') assert(!strchr(name,'_'));
    if (!strcmp(name,"p1button4")) beast=down;
    if (!strcmp(name,"p2button4")) beast2=down;
    if (!strcmp(name,"service1")) service=down;
    if (!strcmp(name,"coin1")) coin=down;
    if ((!strcmp(name,"p1_start") || !strcmp(name,"p1start"))) start=down;
    if ((!strcmp(name,"p1_button1") || !strcmp(name,"p1button1"))) fire=down;
    return 0;
}
static unsigned atlus_mix_frames;
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
    SDL_CloseGamepad(interactive_pads[0]);interactive_pads[0]=NULL;
    SDL_CloseJoystick(joy);assert(SDL_DetachVirtualJoystick(id));
    SDL_DestroyWindow(w);SDL_Quit();
    puts("G-Net SDL keyboard, mouse, mapped gamepad, raw extrema, focus release and replay isolation PASS");
}
