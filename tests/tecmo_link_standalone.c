/* GPL-2.0-or-later. Source link CPU has ROM/RAM and an empty 8-bit port map. */
#include "tecmo_link.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    uint8_t *rom=calloc(1,0x40000);assert(rom);
    const uint8_t program[]={0xf3,0x31,0xfe,0xff,0x3e,0x5a,0x32,0,0x80,0x32,0,0,
        0x3a,0xff,0x7f,0x32,0xff,0xff,0x3a,0,0x80,0xd3,0x90,0xdb,0x91,0x32,1,0x80,0xfb,0x76};
    memcpy(rom,program,sizeof(program));rom[0x7fff]=0x81;rom[0x8000]=0x34;
    TecmoLink *a=NULL,*b=NULL;assert(tecmo_link_create(rom,0x8000,&a)==-1);
    assert(tecmo_link_create(rom,0x40000,&a)==0 && tecmo_link_create(rom,0x40000,&b)==0);
    assert(tecmo_link_sync(a,10000)==0);
    for(unsigned i=1;i<=10000;i++)assert(tecmo_link_sync(b,i)==0);
    TecmoLink *devices[]={a,b};
    for(unsigned i=0;i<2;i++) {
        TecmoLink *s=devices[i];TecmoLinkStatus st;tecmo_link_status(s,&st);
        assert(st.cycles==10000 && !st.faults && st.cpu.halt && st.cpu.iff1);
        assert(st.io_reads==1 && st.io_writes==1 && st.last_read_port==0x91 && st.last_write_port==0x90 && st.last_write_data==0x5a);
        assert(!st.cpu.interrupts && !st.cpu.nmis && !st.cpu.unsupported_operations);
        assert(tecmo_link_peek(s,0)==0xf3 && tecmo_link_peek(s,0x7fff)==0x81);
        assert(tecmo_link_peek(s,0x8000)==0x5a && tecmo_link_peek(s,0x8001)==0 && tecmo_link_peek(s,0xffff)==0x81);
        assert(tecmo_link_sync(s,9999)==-1 && tecmo_link_reset(s)==0);
        assert(tecmo_link_peek(s,0x8000)==0x5a && tecmo_link_sync(s,20000)==0);
        tecmo_link_status(s,&st);assert(st.cycles==20000 && st.io_reads==2 && st.io_writes==2 && st.cpu.halt);
    }
    TecmoLinkStatus sa,sb;tecmo_link_status(a,&sa);tecmo_link_status(b,&sb);
    assert(sa.cpu.cycles==sb.cpu.cycles && sa.cpu.cycle_debt==sb.cpu.cycle_debt && sa.cpu.instructions==sb.cpu.instructions);
    assert(!memcmp(rom,program,sizeof(program)));
    tecmo_link_destroy(a);tecmo_link_destroy(b);free(rom);
    puts("Tecmo source link Z80 ROM/RAM endpoints, ROM protection, masked unmapped I/O, no IRQ/NMI, slicing and reset PASS; cabinet transport remains unimplemented");
}
