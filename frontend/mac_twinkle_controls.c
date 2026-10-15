/* GPL-2.0-or-later. Keyboard/gamepad aliases are combined before each source
 * input update; turntables are wrapping counters, not centered axes. */
#include "mac_twinkle_controls.h"
#include <string.h>
#include <math.h>
static const SDL_Scancode notes[2][7]={
    {SDL_SCANCODE_A,SDL_SCANCODE_S,SDL_SCANCODE_D,SDL_SCANCODE_SPACE,SDL_SCANCODE_F,SDL_SCANCODE_G,SDL_SCANCODE_H},
    {SDL_SCANCODE_Z,SDL_SCANCODE_X,SDL_SCANCODE_C,SDL_SCANCODE_V,SDL_SCANCODE_B,SDL_SCANCODE_N,SDL_SCANCODE_M}
};
static const SDL_GamepadButton buttons[7]={SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_LEFT_STICK};
void mac_twinkle_controls_init(MacTwinkleControls *s){memset(s,0,sizeof(*s));s->last_frame=~0u;}
int mac_twinkle_controls_key(MacTwinkleControls *s,SDL_Scancode key,int pressed){
    if(!s || key<=SDL_SCANCODE_UNKNOWN || key>=SDL_SCANCODE_COUNT || (pressed!=0 && pressed!=1))return -1;
    s->key_events+=s->keys[key]!=pressed;s->keys[key]=pressed;return 0;
}
int mac_twinkle_controls_wheel(MacTwinkleControls *s,float delta){
    if(!s || !isfinite(delta))return -1;
    if(delta>64)delta=64;if(delta<-64)delta=-64;
    s->wheel_fraction+=delta*4;int steps=(int)s->wheel_fraction;s->wheel_fraction-=steps;
    s->table[0]=(uint8_t)(s->table[0]+steps);s->table_updates+=steps!=0;return 0;
}
static int pad_button(SDL_Gamepad *pad,SDL_GamepadButton b){return pad && SDL_GetGamepadButton(pad,b);}
int mac_twinkle_controls_update(MacTwinkleControls *s,uint32_t frame,int focused,
                               SDL_Gamepad *const pads[2],mac_twinkle_emit_t emit,void *opaque){
    if(!s || !emit)return -1;int error=0;char name[32];
    SDL_Gamepad *p[2]={focused && pads?pads[0]:NULL,focused && pads?pads[1]:NULL};
    int new_frame=s->last_frame!=frame;s->last_frame=frame;s->updates++;
    for(unsigned player=0;player<2;player++){
        for(unsigned n=0;n<7;n++){
            SDL_snprintf(name,sizeof(name),"p%ubutton%u",player+1,n+1);
            error|=emit(opaque,name,focused && (s->keys[notes[player][n]] || pad_button(p[player],buttons[n])));
        }
        SDL_snprintf(name,sizeof(name),"p%ustart",player+1);
        error|=emit(opaque,name,focused && (s->keys[player?SDL_SCANCODE_2:SDL_SCANCODE_1] || pad_button(p[player],SDL_GAMEPAD_BUTTON_START)));
        if(focused && new_frame){
            int delta=2*((int)s->keys[player?SDL_SCANCODE_L:SDL_SCANCODE_RIGHT]-(int)s->keys[player?SDL_SCANCODE_J:SDL_SCANCODE_LEFT]);
            delta+=2*(pad_button(p[player],SDL_GAMEPAD_BUTTON_DPAD_RIGHT)-pad_button(p[player],SDL_GAMEPAD_BUTTON_DPAD_LEFT));
            if(p[player])delta+=SDL_GetGamepadAxis(p[player],SDL_GAMEPAD_AXIS_RIGHTX)/8192;
            s->table[player]=(uint8_t)(s->table[player]+delta);s->table_updates+=delta!=0;
        }
        SDL_snprintf(name,sizeof(name),"p%utable",player+1);error|=emit(opaque,name,s->table[player]);
    }
    error|=emit(opaque,"coin1",focused && (s->keys[SDL_SCANCODE_5] || pad_button(p[0],SDL_GAMEPAD_BUTTON_BACK) || pad_button(p[1],SDL_GAMEPAD_BUTTON_BACK)));
    error|=emit(opaque,"test",focused && s->keys[SDL_SCANCODE_T]);
    error|=emit(opaque,"service",focused && s->keys[SDL_SCANCODE_9]);
    error|=emit(opaque,"vefx",focused && s->keys[SDL_SCANCODE_Q]);
    error|=emit(opaque,"effect",focused && s->keys[SDL_SCANCODE_E]);
    return error?-1:0;
}
int mac_twinkle_controls_release(MacTwinkleControls *s,mac_twinkle_emit_t emit,void *opaque){
    if(!s)return -1;memset(s->keys,0,sizeof(s->keys));s->wheel_fraction=0;
    return mac_twinkle_controls_update(s,s->last_frame,0,NULL,emit,opaque);
}
