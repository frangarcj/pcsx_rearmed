#include "../frontend/mac_twinkle_controls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct Inputs {int key[2][7],start[2],table[2],coin,test,service,vefx,effect;} Inputs;
static int emit(void *opaque,const char *name,int value){
    Inputs *v=opaque;unsigned p,n;
    if(sscanf(name,"p%ubutton%u",&p,&n)==2){assert(p>=1 && p<=2 && n>=1 && n<=7);v->key[p-1][n-1]=value;return 0;}
    if(!strcmp(name,"p1start"))v->start[0]=value;else if(!strcmp(name,"p2start"))v->start[1]=value;
    else if(!strcmp(name,"p1table"))v->table[0]=value;else if(!strcmp(name,"p2table"))v->table[1]=value;
    else if(!strcmp(name,"coin1"))v->coin=value;else if(!strcmp(name,"test"))v->test=value;
    else if(!strcmp(name,"service"))v->service=value;else if(!strcmp(name,"vefx"))v->vefx=value;
    else if(!strcmp(name,"effect"))v->effect=value;else assert(!"unknown emitted control");return 0;
}
int main(void){
    MacTwinkleControls s;Inputs v={0};mac_twinkle_controls_init(&s);
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_F,1));assert(!mac_twinkle_controls_update(&s,0,1,NULL,emit,&v));
    assert(v.key[0][4] && !v.key[0][6]); // F is one key, unlike the generic board map.
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_G,1));assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_M,1));
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_RIGHT,1));assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_J,1));
    assert(!mac_twinkle_controls_update(&s,1,1,NULL,emit,&v));assert(v.key[0][5] && v.key[1][6] && v.table[0]==2 && v.table[1]==254);
    assert(!mac_twinkle_controls_update(&s,1,1,NULL,emit,&v));assert(v.table[0]==2 && v.table[1]==254); // One delta per video frame.
    assert(!mac_twinkle_controls_wheel(&s,.125f));assert(!mac_twinkle_controls_wheel(&s,.125f));
    assert(!mac_twinkle_controls_release(&s,emit,&v));assert(!v.key[0][4] && !v.key[0][5] && !v.key[1][6] && v.table[0]==3 && v.table[1]==254);
    assert(!mac_twinkle_controls_update(&s,2,0,NULL,emit,&v));assert(v.table[0]==3 && v.table[1]==254);
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_5,1));assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_T,1));
    assert(!mac_twinkle_controls_update(&s,3,1,NULL,emit,&v));assert(v.coin && v.test);
    assert(!mac_twinkle_controls_release(&s,emit,&v));assert(!v.coin && !v.test);
    assert(mac_twinkle_controls_key(&s,SDL_SCANCODE_COUNT,1)==-1);
    // Use a real SDL virtual gamepad to exercise alias merging and axis deltas.
    assert(SDL_Init(SDL_INIT_GAMEPAD));SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;desc.name="Twinkle input test";
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);SDL_Gamepad *pad=SDL_OpenGamepad(id);assert(pad);
    SDL_Joystick *joy=SDL_GetGamepadJoystick(pad);SDL_Gamepad *pads[2]={pad,NULL};
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,true));assert(SDL_SetJoystickVirtualAxis(joy,SDL_GAMEPAD_AXIS_RIGHTX,32767));SDL_UpdateGamepads();
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_A,1));assert(!mac_twinkle_controls_update(&s,4,1,pads,emit,&v));assert(v.key[0][0] && v.table[0]==6);
    assert(!mac_twinkle_controls_key(&s,SDL_SCANCODE_A,0));assert(!mac_twinkle_controls_update(&s,4,1,pads,emit,&v));assert(v.key[0][0] && v.table[0]==6);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_SOUTH,false));assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_LEFT_STICK,true));SDL_UpdateGamepads();
    assert(!mac_twinkle_controls_update(&s,5,1,pads,emit,&v));assert(!v.key[0][0] && v.key[0][6] && !v.coin);
    assert(SDL_SetJoystickVirtualButton(joy,SDL_GAMEPAD_BUTTON_BACK,true));SDL_UpdateGamepads();
    assert(!mac_twinkle_controls_update(&s,6,1,pads,emit,&v));assert(v.coin);
    assert(!mac_twinkle_controls_update(&s,7,0,pads,emit,&v));assert(!v.coin && !v.key[0][6]);
    SDL_CloseGamepad(pad);assert(SDL_DetachVirtualJoystick(id));SDL_Quit();
    puts("Twinkle unique keyboard keys, fractional wheel, frame-bound wrapping tables, focus release and real SDL virtual-gamepad aliases PASS");
}
