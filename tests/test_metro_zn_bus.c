#include "../libpcsxcore/arcade/metro_zn_bus.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t read(MetroZnBus *b,uint32_t a,unsigned w){uint32_t v;assert(metro_zn_bus_read(b,a,w,&v)==1);return v;}
static void write(MetroZnBus *b,uint32_t a,unsigned w,uint32_t v){assert(metro_zn_bus_write(b,a,w,v)==1);}
int main(void){
    uint8_t *rom=calloc(1,METRO_ZN_ROM_BYTES);assert(rom);
    for(unsigned page=0;page<11;page++)for(unsigned i=0;i<4;i++)rom[page*0x400000+i]=page*4+i;
    MetroZnBus b;assert(metro_zn_bus_init(&b,rom,METRO_ZN_ROM_BYTES-1)==-1);
    assert(!metro_zn_bus_init(&b,rom,METRO_ZN_ROM_BYTES));
    assert(b.bank==1 && read(&b,0x1f000000,4)==0x03020100 && read(&b,0x1f400000,4)==0x07060504);
    write(&b,0x1fb00000,4,0x007f000a);assert(b.bank==10 && b.command==0x7f);
    assert(read(&b,0x1f400000,4)==0x2b2a2928 && read(&b,0x1fb00004,4)==1);
    assert(read(&b,0x1fb00004,1)==1 && read(&b,0x1fb00005,1)==0);
    assert(read(&b,0x1fb00002,2)==0); /* Status is at offset 2, not 1. */
    write(&b,0x1fb00002,2,0x1c);assert(read(&b,0x1fb00004,2)==1);
    write(&b,0x1fb00002,2,0);assert(read(&b,0x1fb00004,2)==4);
    write(&b,0x1fb00003,1,0x7f);assert(b.command==0x7f00 && read(&b,0x1fb00004,2)==4);
    write(&b,0x1fb00002,1,0x1c);assert(b.command==0x1c && read(&b,0x1fb00004,2)==1);
    write(&b,0x1fb00004,4,0xfffeffff);assert(b.command==0x1c && b.bank==10);
    metro_zn_bus_reset(&b);assert(b.bank==1 && b.command==0x1c);
    write(&b,0x1fb00000,2,0xf);assert(read(&b,0x1f400000,4)==UINT32_MAX && b.unpopulated_reads==1);
    assert(metro_zn_bus_rom_span(&b,0x1f400000,4)==NULL);
    write(&b,0x1fb00001,1,0xaa);assert(b.bank==0 && read(&b,0x1f400000,4)==0x03020100);
    assert(metro_zn_bus_rom_span(&b,0x1f3ffffe,4)==NULL);
    uint32_t v=0;assert(metro_zn_bus_read(&b,0x1fbfff90,2,&v)==0); /* ATA is not on PSX bus. */
    assert(metro_zn_bus_read(&b,0x1fb00001,2,&v)==-1 && metro_zn_bus_write(&b,0x1fb00000,3,0)==-1);
    free(rom);puts("Metro ZN source ROM banks, lanes, warm reset and provisional MCU errors (not H8/ATA emulation) PASS");return 0;
}
