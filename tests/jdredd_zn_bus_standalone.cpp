/* GPL-2.0-or-later. Judge direct ATA16 and source multiplexed gun vectors. */
#include "jdredd_zn_bus.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
struct Disk {std::array<uint8_t,8192> data{};unsigned reads=0,writes=0;bool irq=false;uint16_t x=0,y=0;unsigned latches=0;};
static int read_sector(void *p,uint32_t l,void *v){auto &d=*(Disk *)p;if(l>=16)return 0;memcpy(v,d.data.data()+l*512,512);d.reads++;return 1;}
static int write_sector(void *p,uint32_t l,const void *v){auto &d=*(Disk *)p;if(l>=16)return 0;memcpy(d.data.data()+l*512,v,512);d.writes++;return 1;}
static void irq(void *p,int v){((Disk *)p)->irq=v!=0;}
static void gun(void *p,uint16_t x,uint16_t y){auto &d=*(Disk *)p;d.x=x;d.y=y;d.latches++;}
static uint32_t read(JdreddZnBus &b,uint32_t a,unsigned w){uint32_t v;assert(jdredd_zn_bus_read(&b,a,w,&v)==1);return v;}
static void write(JdreddZnBus &b,uint32_t a,unsigned w,uint32_t v){assert(jdredd_zn_bus_write(&b,a,w,v)==1);}
static void reg(JdreddZnBus &b,unsigned n,uint8_t v){write(b,0x1fbfff90+2*n,1,v);}
int main(){
 Disk d;for(unsigned i=0;i<d.data.size();i++)d.data[i]=(i*17+3)&255;
 ata_storage_config_t c{};c.device_kind=ATA_STORAGE_DEVICE_IDE_HDD;c.sector_count=16;c.cylinders=1;c.heads=1;c.sectors_per_track=16;
 c.media_opaque=&d;c.read_sector=read_sector;c.write_sector=write_sector;c.line_opaque=&d;c.irq=irq;
 ata_storage_t *ata=nullptr;assert(!ata_storage_create(&c,&ata));assert(!ata_storage_run_until(ata,12000000));
 std::vector<uint8_t> rom(0x200000);rom[0]=0xab;rom.back()=0xcd;JdreddZnBus b;
 assert(!jdredd_zn_bus_init(&b,ata,rom.data(),rom.size(),&d,gun));
 assert(read(b,0x1f000000,4)==0xab && read(b,0x1f1fffff,1)==0xcd);write(b,0x1f000000,4,0x12345678);assert(read(b,0x1f000000,4)==0xab);
 reg(b,2,0x12);reg(b,3,0x34);reg(b,4,0x56);reg(b,5,0x78);reg(b,6,0xe0);
 assert(read(b,0x1fbfff94,4)==0x00340012 && read(b,0x1fbfff98,4)==0x00780056);
 write(b,0x1fbfff95,1,0xff);assert(read(b,0x1fbfff94,2)==0x100); // Source ignores mem_mask here: low data byte zero selects 256 sectors.
 reg(b,7,0xec);assert(!ata_storage_run_until(ata,ata_storage_now(ata)+10000));assert(d.irq);
 assert(read(b,0x1fbfff8c,2)&8);assert(d.irq);assert(read(b,0x1fbfff9e,2)&8);assert(!d.irq);
 const uint16_t *id=ata_storage_identify(ata);assert(read(b,0x1fbfff90,4)==id[0]); // Upper halfword reads ERROR, not a second data word.
 for(unsigned i=1;i<256;i++)assert(read(b,0x1fbfff90,2)==id[i]);
 reg(b,7,0xec);assert(!ata_storage_run_until(ata,ata_storage_now(ata)+10000));assert(read(b,0x1fbfff91,1)==(id[0]>>8));
 for(unsigned i=1;i<256;i++)assert(read(b,0x1fbfff90,2)==id[i]);
 auto lba=[&](unsigned n){reg(b,2,1);reg(b,3,n);reg(b,4,0);reg(b,5,0);reg(b,6,0xe0);};
 lba(3);reg(b,7,0x20);assert(!ata_storage_run_until(ata,ata_storage_now(ata)+400));
 for(unsigned i=0;i<256;i++){uint16_t word;memcpy(&word,d.data.data()+3*512+2*i,2);assert(read(b,0x1fbfff90,2)==word);}
 lba(4);reg(b,7,0x30);for(unsigned i=0;i<256;i++)write(b,0x1fbfff90,2,0x5678);
 assert(!ata_storage_run_until(ata,ata_storage_now(ata)+3000000) && d.writes==1 && d.data[4*512]==0x78 && d.data[5*512-1]==0x56);
 reg(b,1,1);reg(b,7,0xef);assert(!ata_storage_run_until(ata,ata_storage_now(ata)+10000));reg(b,7,0xec);assert(!ata_storage_run_until(ata,ata_storage_now(ata)+10000));
 assert(read(b,0x1fbfff90,2)==(id[0]&255) && read(b,0x1fbfff90,2)==(id[0]>>8)); // Guest-selected byte transfer mode survives.
 jdredd_zn_bus_vblank(&b,0);assert(!b.gun_mux && !b.vblanks);
 jdredd_zn_bus_gun(&b,0,0x700,0x180);jdredd_zn_bus_gun(&b,1,0x900,0x200);
 jdredd_zn_bus_vblank(&b,1);assert(b.gun_mux==1 && d.x==0x900 && d.y==0x200);
 jdredd_zn_bus_vblank(&b,1);assert(b.gun_mux==0 && d.x==0x700 && d.y==0x180);
 for(auto edge:std::array<std::array<uint16_t,2>,4>{{{0x393,0x122},{0xcb2,0x122},{0x822,0x02d},{0x822,0x217}}}){
  jdredd_zn_bus_gun(&b,1,edge[0],edge[1]);unsigned before=d.latches;jdredd_zn_bus_vblank(&b,1);
  assert(b.gun_mux==1 && d.latches==before);jdredd_zn_bus_vblank(&b,1);
 }
 jdredd_zn_bus_gun(&b,1,0x394,0x02e);jdredd_zn_bus_vblank(&b,1);assert(d.x==0x394 && d.y==0x02e);
 jdredd_zn_bus_reset(&b);assert(b.gun_mux==1 && b.gun_x[0]==0x700 && b.offscreen==4 && !b.faults);
 ata_storage_destroy(ata);puts("Judge source direct ATA16 register/halfword lanes, one-word data32, status/alternate IRQ, PIO sector read/write, SET FEATURES width, ROM protection, VBlank gun mux and strict offscreen limits PASS");
}
