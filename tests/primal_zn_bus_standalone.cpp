/* GPL-2.0-or-later. Execute source board lanes with the real ATA HDD core. */
#include "primal_zn_bus.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
struct Fixture {PrimalZnBus *bus=nullptr;bool irq=false;unsigned rises=0;std::array<uint8_t,8192> disk{};};
static int read_sector(void *p,uint32_t n,void *out){auto &f=*static_cast<Fixture *>(p);if(n>=16)return 0;memcpy(out,f.disk.data()+n*512,512);return 1;}
static int write_sector(void *p,uint32_t n,const void *in){auto &f=*static_cast<Fixture *>(p);if(n>=16)return 0;memcpy(f.disk.data()+n*512,in,512);return 1;}
static void ata_irq(void *p,int s){auto &f=*static_cast<Fixture *>(p);primal_zn_bus_ata_irq(f.bus,s);}
static void irq10(void *p,int s){auto &f=*static_cast<Fixture *>(p);if(s && !f.irq)f.rises++;f.irq=s!=0;}
int main(){
    Fixture f;for(unsigned i=0;i<f.disk.size();i++)f.disk[i]=(i*7+11)&255;
    ata_storage_config_t a{};a.device_kind=ATA_STORAGE_DEVICE_IDE_HDD;a.sector_count=16;a.cylinders=1;a.heads=1;a.sectors_per_track=16;
    a.media_opaque=&f;a.read_sector=read_sector;a.write_sector=write_sector;a.line_opaque=&f;a.irq=ata_irq;
    ata_storage_t *ata=nullptr;assert(ata_storage_create(&a,&ata)==0);PrimalZnBusConfig c{ata,&f,irq10};assert(primal_zn_bus_create(&c,&f.bus)==0);
    auto write=[&](uint32_t addr,unsigned width,uint32_t data){assert(primal_zn_bus_write(f.bus,addr,width,data)==1);};
    auto read=[&](uint32_t addr,unsigned width){uint32_t data=0;assert(primal_zn_bus_read(f.bus,addr,width,&data)==1);return data;};
    auto reg=[&](unsigned n,uint8_t v){write(0x1f7e41f0+n,1,v);};
    assert(primal_zn_bus_sync(f.bus,12000000)==0);
    write(0x1f7e4038,2,2);write(0x1f7e403c,2,0x55);assert(read(0x1f7e403c,2)==0x55);
    write(0x1f7e403e,2,0xff);assert(read(0x1f7e403c,2)==0); // high half replaces the source 8-bit config with zero
    write(0x1f7e403c,2,0x66);assert(read(0x1f7e403e,2)==0);
    reg(6,0xe0);reg(7,0xec);assert(primal_zn_bus_sync(f.bus,12010000)==0);assert(!f.irq);
    write(0x1f7e8000,1,1);assert(!f.irq); // control write alone does not reevaluate source IRQ callback
    (void)read(0x1f7e41f7,1);reg(7,0xec);assert(primal_zn_bus_sync(f.bus,12020000)==0 && f.irq && f.rises==1);
    assert(read(0x1f7e43f6,1)&8);assert(f.irq); // alternate status
    assert(read(0x1f7e41f7,1)&8);assert(!f.irq);
    const uint16_t *id=ata_storage_identify(ata);
    assert(read(0x1f7f41f0,4)==(uint32_t(id[1])<<16|id[0]));
    PrimalZnBusStatus st,before;primal_zn_bus_status(f.bus,&before);
    assert(read(0x1f7f41f2,2)==id[1]);primal_zn_bus_status(f.bus,&st);assert(st.via.cs0_reads==before.via.cs0_reads);
    assert(read(0x1f7e41f0,2)==id[2]); // ordinary halfword map consumes one word
    for(unsigned i=3;i<256;i++)assert(read(0x1f7e41f0,2)==id[i]);
    reg(2,1);reg(3,3);reg(4,0);reg(5,0);reg(6,0xe0);reg(7,0x20);assert(primal_zn_bus_sync(f.bus,12020400)==0);
    std::vector<uint8_t> ram(0x800000,0x6d);assert(primal_zn_bus_dma_read(f.bus,ram.data(),ram.size(),0x400000,128)==0);
    assert(!memcmp(ram.data()+0x400000,f.disk.data()+3*512,512) && ram[0x400200]==0x6d && ram[0x3fffff]==0x6d);
    assert(primal_zn_bus_dma_read(f.bus,ram.data(),ram.size(),0x7ffffc,2)==-1);
    primal_zn_bus_status(f.bus,&before);write(0x1f7f41f0,4,0xffffffff);primal_zn_bus_dma_write(f.bus);primal_zn_bus_status(f.bus,&st);
    assert(st.via.cs0_writes==before.via.cs0_writes && st.dma_writes==1 && st.dma_read_words==128);
    assert(primal_zn_bus_sync(f.bus,500000000)==0);write(0x1f000000,4,0x12345678);
    primal_zn_bus_status(f.bus,&st);assert(st.watchdog_kicks==2 && st.watchdog_deadline==1100000000);
    assert(primal_zn_bus_sync(f.bus,1099999999)==0 && primal_zn_bus_sync(f.bus,1100000000)==1 && primal_zn_bus_sync(f.bus,1100000000)==1);
    primal_zn_bus_status(f.bus,&st);assert(st.watchdog_resets==1 && st.reset_pending);
    uint16_t latch=st.data_latch;primal_zn_bus_reset(f.bus);assert(read(0x1f7e8000,2)==1 && read(0x1f7f41f2,2)==latch && read(0x1f7e403c,2)==0x66);
    assert(primal_zn_bus_sync(f.bus,1100000000)==0 && primal_zn_bus_sync(f.bus,1099999999)==-1);
    primal_zn_bus_status(f.bus,&st);assert(!st.faults && !st.via.diagnostics && !st.reset_pending && st.watchdog_deadline==1700000000);
    primal_zn_bus_destroy(f.bus);f.bus=nullptr;ata_storage_destroy(ata);
    puts("Primal source MMIO halfword/config/latch lanes, real HDD PIO/DMA into upper 8MiB RAM, IRQ10 edge/control behavior, log-only writes, 600ms watchdog boundary/deferred reset and persistent latches PASS");
}
