/* GPL-2.0-or-later. Map port from pinned MAME sony/zn.cpp nbajamex_state,
 * deb2f54fe7300e7a192690318beb860159bfd52a, BSD-3-Clause smf/R. Belmont.
 * Main bank_map has unmapped-low, distinct from the outer ZN unmapped-high. */
#include "nbajam_zn_bus.h"
#include <string.h>
static int valid(uint32_t a,unsigned w){return (w==1 || w==2 || w==4) && !(a&(w-1));}
int nbajam_zn_bus_init(NbajamZnBus *b,const uint8_t *rom,size_t n,void *opaque,int (*command)(void *,uint16_t)){
 if(!b || !rom || n!=0x2000000 || !command)return -1;
 memset(b,0,sizeof(*b));b->rom=rom;b->rom_size=n;b->opaque=opaque;b->command=command;memset(b->sram,0xff,sizeof(b->sram));return 0;
}
void nbajam_zn_bus_reset(NbajamZnBus *b){if(b){b->page=0;b->bank[0]=b->bank[1]=0;}} /* Raw latches and SRAM survive. */
const uint8_t *nbajam_zn_bus_rom_span(const NbajamZnBus *b,uint32_t a,size_t n){
 if(!b || a<0x1f000000 || a>=0x1f800000 || n>0x1f800000u-a)return NULL;
 uint32_t off=a-0x1f000000;unsigned bank;
 if(off<0x200000){if(n>0x200000-off)return NULL;bank=0;}
 else {if(!b->page)return NULL;bank=1;off-=0x200000;}
 size_t start=(size_t)b->bank[bank]*0x200000+off;
 return start<=b->rom_size && n<=b->rom_size-start?b->rom+start:NULL;
}
static uint8_t read_byte(NbajamZnBus *b,uint32_t a){
 uint32_t off=a-0x1f000000;const uint8_t *p=nbajam_zn_bus_rom_span(b,a,1);if(p)return *p;
 if(!b->page && off>=0x200000 && off<0x208000){b->sram_reads++;return b->sram[off-0x200000];}
 if(b->page && off>=0x200000)b->faults|=1; /* Source ROM backing exhausted. */
 return 0;
}
int nbajam_zn_bus_read(NbajamZnBus *b,uint32_t a,unsigned w,uint32_t *v){
 if(!b || !v)return -1;
 if(!((a>=0x1f000000 && a<0x1f800000) || (a>=0x1fbfff00 && a<0x1fbfff04) || (a>=0x1fbfff08 && a<0x1fbfff0c) || (a>=0x1fbfff80 && a<0x1fbfff84)))return 0;
 if(!valid(a,w))return -1;
 if(a>=0x1f000000 && a<0x1f800000){
  if(w>0x1f800000-a)return -1;*v=0;for(unsigned i=0;i<w;i++)*v|=(uint32_t)read_byte(b,a+i)<<(8*i);return b->faults?-1:1;
 }
 if((a>=0x1fbfff08 && a<0x1fbfff0c) || (a>=0x1fbfff80 && a<0x1fbfff84)){
  *v=0;for(unsigned i=0;i<w;i++){
   uint8_t data=0xff;
   if(a+i==0x1fbfff08)data=0;
   if(a+i==0x1fbfff09)data=4;
   *v|=(uint32_t)data<<(8*i);
  }return 1;
 }
 if(a>=0x1fbfff00 && a<0x1fbfff04){*v=w==4?UINT32_MAX:(1u<<(8*w))-1;return 1;}
 return 0;
}
static void bank_write(NbajamZnBus *b,unsigned which,uint16_t v){
 b->latch[which]=v;b->bank_writes++;
 unsigned base0=((b->latch[0]>>4)&1)|((b->latch[0]&7)<<1);
 unsigned base1=((~(unsigned)b->latch[1]>>4)&1)|((b->latch[1]&7)<<1);
 if(which==0){b->page=b->latch[0]!=0;b->bank[0]=base0;if(b->page)b->bank[1]=base1;}
 else if(b->latch[0]!=0){b->page=1;b->bank[1]=base1;}
}
int nbajam_zn_bus_write(NbajamZnBus *b,uint32_t a,unsigned w,uint32_t v){
 if(!b)return -1;
 if(!((a>=0x1f000000 && a<0x1f800000) || (a>=0x1fbfff00 && a<0x1fbfff04) || (a>=0x1fbfff12 && a<0x1fbfff14) || (a>=0x1fbfff80 && a<0x1fbfff84)))return 0;
 if(!valid(a,w))return -1;
 if(a>=0x1f000000 && a<0x1f800000){
  if(w>0x1f800000-a)return -1;
  for(unsigned i=0;i<w;i++){uint32_t off=a+i-0x1f000000;if(off>=0x200000 && off<0x208000){b->sram[off-0x200000]=v>>(8*i);b->sram_writes++;}}
  return 1; /* Both map pages share SRAM writes; ROM/hole writes are ignored. */
 }
 if(a>=0x1fbfff00 && a<0x1fbfff04){
  unsigned which=(a-0x1fbfff00)/2;
  if(w==4){bank_write(b,0,v);bank_write(b,1,v>>16);}
  else bank_write(b,which,(v<<((a&1)*8))&0xffff); /* Source has no COMBINE_DATA. */
  return 1;
 }
 if(a>=0x1fbfff80 && a<0x1fbfff84){
  if(a<0x1fbfff82){b->commands++;if(b->command(b->opaque,(v<<((a&1)*8))&0xffff)){b->faults|=2;return -1;}}
  return 1;
 }
 if(a>=0x1fbfff12 && a<0x1fbfff14)return 1; /* Acclaim log-only write. */
 return 0;
}
