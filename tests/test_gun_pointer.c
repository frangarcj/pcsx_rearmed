#include "../frontend/gun_pointer.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    unsigned x, y;
    const int vita[4] = {117, 0, 725, 544};
    assert(gun_pointer_position(117, 0, vita, &x, &y) && x == 0 && y == 0);
    assert(gun_pointer_position(841, 543, vita, &x, &y) && x == 65535 && y == 65535);
    assert(!gun_pointer_position(116, 100, vita, &x, &y) && x == 0);
    assert(!gun_pointer_position(842, 100, vita, &x, &y) && x == 65535);
    const int retina[4] = {200, 100, 1600, 1200};
    assert(gun_pointer_position(999.5f, 699.5f, retina, &x, &y));
    assert(x == 32767 && y == 32767);
    const int wide[4] = {0, 0, 960, 544};
    assert(gun_pointer_position(0, 543, wide, &x, &y) && x == 0 && y == 65535);
    assert(gun_pointer_from_retro(-32768) == 0);
    for (unsigned p=0; p<=65535; p++) {
        unsigned back=gun_pointer_from_retro(gun_pointer_to_retro(p));
        assert(back <= p && p-back <= 2);
    }
    const int invalid[4] = {0,0,0,0};
    assert(!gun_pointer_position(1,1,invalid,&x,&y));
    puts("Gun pointer viewport edges, letterboxing, Retina/widescreen and libretro coordinate roundtrips PASS");
}
