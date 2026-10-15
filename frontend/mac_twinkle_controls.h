/* GPL-2.0-or-later. Mac cabinet input adapter for the native Twinkle profile. */
#ifndef PCSX_MAC_TWINKLE_CONTROLS_H
#define PCSX_MAC_TWINKLE_CONTROLS_H
#include <SDL3/SDL.h>
#include <stdint.h>
typedef struct MacTwinkleControls {
    uint8_t keys[SDL_SCANCODE_COUNT],table[2];
    uint32_t last_frame;
    float wheel_fraction;
    uint64_t key_events,table_updates,updates;
} MacTwinkleControls;
typedef int (*mac_twinkle_emit_t)(void *,const char *,int);
void mac_twinkle_controls_init(MacTwinkleControls *);
int mac_twinkle_controls_key(MacTwinkleControls *,SDL_Scancode,int pressed);
int mac_twinkle_controls_wheel(MacTwinkleControls *,float delta);
int mac_twinkle_controls_update(MacTwinkleControls *,uint32_t frame,int focused,
                               SDL_Gamepad *const pads[2],mac_twinkle_emit_t,void *);
int mac_twinkle_controls_release(MacTwinkleControls *,mac_twinkle_emit_t,void *);
#endif
