/* GPL-2.0-or-later. Source NBA ROM/SRAM/latch map acceptance vectors. */
#include "../libpcsxcore/arcade/nbajam_zn_bus.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t last;static unsigned commands;
static int command(void *p,uint16_t v){(void)p;last=v;commands++;return 0;}
static uint32_t read_bus(NbajamZnBus *b,uint32_t a,unsigned w){uint32_t v;assert(nbajam_zn_bus_read(b,a,w,&v)==1);return v;}
static void write_bus(NbajamZnBus *b,uint32_t a,unsigned w,uint32_t v){assert(nbajam_zn_bus_write(b,a,w,v)==1);}
int main(void){
 uint8_t *rom=malloc(0x2000000);assert(rom);
 for(unsigned i=0;i<16;i++)memset(rom+0x200000*i,i,0x200000);
 NbajamZnBus b;assert(!nbajam_zn_bus_init(&b,rom,0x2000000,NULL,command));
 assert(read_bus(&b,0x1f200000,4)==UINT32_MAX && read_bus(&b,0x1f208000,4)==0 && read_bus(&b,0x1f7ffffc,4)==0);
 write_bus(&b,0x1f207ffc,4,0x12345678);assert(read_bus(&b,0x1f207ffc,4)==0x12345678);
 for(unsigned i=0;i<16;i++){
  unsigned selector=(i>>1)|((i&1)<<4);
  write_bus(&b,0x1fbfff00,2,selector);assert(b.bank[0]==i && b.page==(selector!=0));
  assert(read_bus(&b,0x1f000000,4)==i*0x01010101u && read_bus(&b,0x1f1fffff,1)==i);
  if(selector){
   for(unsigned j=0;j<16;j++){
    write_bus(&b,0x1fbfff02,2,(j>>1)|(((j&1)^1)<<4));assert(b.bank[1]==j);
    assert(read_bus(&b,0x1f200000,4)==j*0x01010101u);
    assert(nbajam_zn_bus_rom_span(&b,0x1f200000,0x200000)==rom+j*0x200000);
   }
  }
 }
 write_bus(&b,0x1fbfff02,2,0x10); // Upper-window bank zero.
 write_bus(&b,0x1f200000,4,0xaabbccdd);assert(read_bus(&b,0x1f200000,4)==0); // Read ROM, write SRAM mirror.
 write_bus(&b,0x1fbfff00,2,0);assert(read_bus(&b,0x1f200000,4)==0xaabbccdd && read_bus(&b,0x1f207ffc,4)==0x12345678);
 write_bus(&b,0x1f000000,4,0xfeedbeef);assert(read_bus(&b,0x1f000000,4)==0);
 assert(read_bus(&b,0x1fbfff08,4)==0xffff0400 && read_bus(&b,0x1fbfff09,1)==4 && read_bus(&b,0x1fbfff80,4)==UINT32_MAX);
 write_bus(&b,0x1fbfff00,2,0xffff);write_bus(&b,0x1fbfff01,1,0x12);assert(b.latch[0]==0x1200 && b.bank[0]==0);
 write_bus(&b,0x1fbfff80,2,0x5678);write_bus(&b,0x1fbfff81,1,0x12);assert(commands==2 && last==0x1200);
 write_bus(&b,0x1fbfff82,2,0x3456);assert(commands==2);
 write_bus(&b,0x1fbfff00,2,3);nbajam_zn_bus_reset(&b);assert(b.latch[0]==3 && b.page==0 && b.bank[0]==0 && b.bank[1]==0);
 write_bus(&b,0x1fbfff02,2,2);assert(b.page==1 && b.bank[0]==0 && b.bank[1]==5);
 nbajam_zn_bus_reset(&b);assert(read_bus(&b,0x1f200000,4)==0xaabbccdd);
 write_bus(&b,0x1fbfff00,2,1);write_bus(&b,0x1fbfff02,2,7);uint32_t v;
 assert(nbajam_zn_bus_read(&b,0x1f7ffffc,4,&v)==-1 && b.faults==1);
 assert(nbajam_zn_bus_read(&b,0x80000001,2,&v)==0);
 free(rom);puts("NBA source 16-entry ROM selectors, 8MiB pages, asymmetric byte SRAM, raw masked latches, ROM protection, sound constants/commands, soft reset and backing bounds PASS");
}
