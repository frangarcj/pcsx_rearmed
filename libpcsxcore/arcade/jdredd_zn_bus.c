/* GPL-2.0-or-later. Source MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * sony/zn.cpp (BSD-3-Clause, smf/R. Belmont), jdredd_state; abstract ATA
 * interface wired-AND behavior (BSD-3-Clause, smf). Direct 16-bit handlers
 * on a 32-bit little-endian bus: each halfword targets its own ATA register. */
#include "jdredd_zn_bus.h"
#include <string.h>
int jdredd_zn_bus_init(JdreddZnBus *b,ata_storage_t *ata,const uint8_t *rom,size_t bytes,void *opaque,void (*gun)(void *,uint16_t,uint16_t)){
 if(!b || !ata || !rom || bytes!=0x200000 || !gun)return -1;
 memset(b,0,sizeof(*b));b->ata=ata;b->rom=rom;b->rom_size=bytes;b->opaque=opaque;b->lightgun=gun;
 for(unsigned i=0;i<2;i++){b->gun_x[i]=0x822;b->gun_y[i]=0x122;}return 0;
}
void jdredd_zn_bus_reset(JdreddZnBus *b){if(b)ata_storage_reset(b->ata);}
int jdredd_zn_bus_gun(JdreddZnBus *b,unsigned p,uint16_t x,uint16_t y){if(!b || p>1)return -1;b->gun_x[p]=x;b->gun_y[p]=y;return 0;}
void jdredd_zn_bus_vblank(JdreddZnBus *b,int state){
 if(!b || !state)return;b->vblanks++;b->gun_mux=!b->gun_mux;
 uint16_t x=b->gun_x[b->gun_mux],y=b->gun_y[b->gun_mux];
 if(x>0x393 && x<0xcb2 && y>0x02d && y<0x217){b->lightgun(b->opaque,x,y);b->gun_latches++;}
 else b->offscreen++; /* Source retains the previous GPU latch until GP1 0Dh. */
}
static int mapped(uint32_t a){return (a>=0x1f000000 && a<0x1f200000) || (a>=0x1fbfff80 && a<0x1fbfffa0);}
static int valid(uint32_t a,unsigned w){return (w==1 || w==2 || w==4) && !(a&(w-1));}
int jdredd_zn_bus_read(JdreddZnBus *b,uint32_t a,unsigned w,uint32_t *v){
 if(!b || !v)return -1;if(!mapped(a))return 0;if(!valid(a,w))return -1;
 *v=0;
 if(a<0x1f200000){if(w>0x1f200000-a)return -1;for(unsigned i=0;i<w;i++)*v|=(uint32_t)b->rom[a-0x1f000000+i]<<(8*i);return 1;}
 int cs0=a>=0x1fbfff90;uint32_t base=cs0?0x1fbfff90:0x1fbfff80;
 if(w>base+16-a)return -1;
 for(unsigned i=0;i<w;){
  unsigned lane=(a+i)&1,n=w-i;if(n>2-lane)n=2-lane;
  uint16_t mask=n==2?0xffff:0xffu<<(8*lane);unsigned reg=(a+i-base)/2;
  uint16_t data=cs0?ata_storage_command_r(b->ata,reg,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE):ata_storage_control_r(b->ata,reg,mask);
  data&=mask;b->reads[cs0?0:1]++;*v|=(uint32_t)(data>>(8*lane))<<(8*i);i+=n;
 }
 return 1;
}
int jdredd_zn_bus_write(JdreddZnBus *b,uint32_t a,unsigned w,uint32_t v){
 if(!b)return -1;
 if((a>=0x1fbfff00 && a<0x1fbfff04) || (a>=0x1fbfff12 && a<0x1fbfff14))return 1; // Parent source log-only handlers.
 if(!mapped(a))return 0;if(!valid(a,w))return -1;
 if(a<0x1f200000)return 1;
 int cs0=a>=0x1fbfff90;uint32_t base=cs0?0x1fbfff90:0x1fbfff80;
 if(w>base+16-a)return -1;
 for(unsigned i=0;i<w;){
  unsigned lane=(a+i)&1,n=w-i;if(n>2-lane)n=2-lane;
  uint16_t mask=n==2?0xffff:0xffu<<(8*lane),data=(v>>(8*i))<<(8*lane);unsigned reg=(a+i-base)/2;
  if(cs0)ata_storage_command_w(b->ata,reg,data,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE);
  else ata_storage_control_w(b->ata,reg,data,mask);
  b->writes[cs0?0:1]++;i+=n;
 }
 return 1;
}
