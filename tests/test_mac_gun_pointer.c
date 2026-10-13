/* Exercise the real SDL event handler with a private hidden window. */
#define main harness_original_main
#define system12_analog_max pointer_test_analog_max
#define system12_set_gun_position pointer_test_position
#define system12_set_input pointer_test_input
#include "../frontend/mac_harness.c"
#undef main
#include <assert.h>
static unsigned aim_x, aim_y, aims;
static int trigger, sensor;
unsigned pointer_test_analog_max(const char *control) { return !strcmp(control,"p1gunx") ? 65535 : 0; }
int pointer_test_position(unsigned player, unsigned x, unsigned y) { assert(!player); aim_x=x;aim_y=y;aims++;return 0; }
int pointer_test_input(const char *control, int pressed) {
    if (!strcmp(control,"p1button1")) trigger=pressed;
    if (!strcmp(control,"p1button7")) sensor=pressed;
    return 0;
}
static void mouse(SDL_WindowID id, int down, unsigned button, float x, float y) {
    SDL_Event e={0};e.type=down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID=id;e.button.button=button;e.button.x=x;e.button.y=y;
    assert(SDL_PushEvent(&e));poll_interactive_input();
}
static void key(SDL_Scancode code, int down) {
    SDL_Event e={0};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.scancode=code;
    assert(SDL_PushEvent(&e));poll_interactive_input();
}
int main(void) {
    for (unsigned i=0;i<sizeof(boards)/sizeof(boards[0]);++i)
        if (boards[i].family==12) selected_board=&boards[i];
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *w=SDL_CreateWindow("PCSX gun input test",800,600,SDL_WINDOW_HIDDEN);assert(w);
    SDL_Event e;while(SDL_PollEvent(&e)) {}
    SDL_WindowID id=SDL_GetWindowID(w);
    /* A click must update aim even when there was no preceding motion. */
    mouse(id,1,SDL_BUTTON_LEFT,500,300);assert(trigger && aims && aim_x>40000 && aim_y>32000);
    key(SDL_SCANCODE_A,1);mouse(id,0,SDL_BUTTON_LEFT,500,300);assert(trigger);
    key(SDL_SCANCODE_A,0);assert(!trigger);
    mouse(id,1,SDL_BUTTON_LEFT,-10,300);assert(!trigger);
    mouse(id,1,SDL_BUTTON_RIGHT,500,300);assert(sensor);
    key(SDL_SCANCODE_F,1);mouse(id,0,SDL_BUTTON_RIGHT,500,300);assert(sensor);
    mouse(id,1,SDL_BUTTON_LEFT,400,300);assert(trigger);
    memset(&e,0,sizeof(e));e.type=SDL_EVENT_WINDOW_FOCUS_LOST;e.window.windowID=id;
    assert(SDL_PushEvent(&e));poll_interactive_input();assert(!trigger && !sensor);
    SDL_DestroyWindow(w);SDL_Quit();
    puts("Mac SDL gun click aiming, trigger/key merging, offscreen clicks, sensor and focus release PASS");
}
