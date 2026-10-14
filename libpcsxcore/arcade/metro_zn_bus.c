/* SPDX-License-Identifier: BSD-3-Clause. MAME sony/zn.cpp, smf/R. Belmont.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See sony_zn_LICENSE.txt. */
#include "metro_zn_bus.h"
#include <string.h>
int metro_zn_bus_init(MetroZnBus *b,const uint8_t *rom,size_t size) {
    if(!b || !rom || size!=METRO_ZN_ROM_BYTES)return -1;
    memset(b,0,sizeof(*b));b->rom=rom;b->rom_size=size;b->bank=1;return 0;
}
void metro_zn_bus_reset(MetroZnBus *b){if(b)b->bank=1;}
const uint8_t *metro_zn_bus_rom_span(const MetroZnBus *b,uint32_t a,size_t size) {
    if(!b || !b->rom || !size || a<0x1f000000 || a>=0x1f800000)return NULL;
    uint32_t window=a>=0x1f400000?0x1f400000:0x1f000000;
    size_t offset=(a-window)+(window==0x1f400000?(size_t)b->bank*0x400000:0);
    if(size>0x400000-(a-window) || offset>b->rom_size || size>b->rom_size-offset)return NULL;
    return b->rom+offset;
}
static int valid(uint32_t a,unsigned w){return (w==1 || w==2 || w==4) && !(a&(w-1)) && a<=UINT32_MAX-(w-1);}
int metro_zn_bus_read(MetroZnBus *b,uint32_t a,unsigned w,uint32_t *v) {
    if(!b || !v)return -1;
    int rom=a>=0x1f000000 && a<0x1f800000;
    int mcu=a>=0x1fb00000 && a<0x1fb00008;
    if(!rom && !mcu)return 0;if(!valid(a,w))return -1;
    *v=0;
    if(rom){
        const uint8_t *p=metro_zn_bus_rom_span(b,a,w);
        if(!p){b->unpopulated_reads++;*v=w==4?UINT32_MAX:(1u<<(w*8))-1;return 1;}
        for(unsigned i=0;i<w;i++)*v|=(uint32_t)p[i]<<(8*i);return 1;
    }
    if(w>0x1fb00008-a)return -1;
    for(unsigned i=0;i<w;){
        unsigned off=(a+i-0x1fb00000)/2,lane=(a+i)&1,n=w-i;if(n>2-lane)n=2-lane;
        uint16_t data=0;
        /* Implementation offset 2 is status, despite the old source comment
         * calling it port 1. Keep the exact provisional failure behavior. */
        if(off==2){data=(b->command==0x7f || b->command==0x1c)?1:4;b->provisional_status_reads++;}
        unsigned mask=n==2?0xffff:0xff;
        *v|=(uint32_t)((data>>(8*lane))&mask)<<(8*i);b->reads[off]++;i+=n;
    }
    return 1;
}
int metro_zn_bus_write(MetroZnBus *b,uint32_t a,unsigned w,uint32_t v) {
    if(!b)return -1;if(a<0x1fb00000 || a>=0x1fb00008)return 0;
    if(!valid(a,w) || w>0x1fb00008-a)return -1;
    for(unsigned i=0;i<w;){
        unsigned off=(a+i-0x1fb00000)/2,lane=(a+i)&1,n=w-i;if(n>2-lane)n=2-lane;
        unsigned mask=n==2?0xffff:0xff;uint16_t data=((v>>(8*i))&mask)<<(8*lane);
        /* The pinned handlers ignore mem_mask: byte writes do not merge with
         * an earlier value. The other two ports only log, with no watchdog. */
        if(off==0){b->bank=data&15;b->bank_writes++;}
        else if(off==1)b->command=data;
        b->writes[off]++;i+=n;
    }
    return 1;
}
