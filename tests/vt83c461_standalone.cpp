/* GPL-2.0-or-later. VIA/IDE controller lanes with the real ATA HDD backend. */
#include "vt83c461.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
static constexpr uint64_t rotation=1000000000ull/90;
struct Disk {std::array<uint8_t,8192> bytes{};unsigned reads=0,writes=0;bool irq=false;};
static int read_sector(void *p,uint32_t lba,void *out){auto &d=*static_cast<Disk *>(p);if(lba>=16)return 0;d.reads++;memcpy(out,d.bytes.data()+lba*512,512);return 1;}
static int write_sector(void *p,uint32_t lba,const void *in){auto &d=*static_cast<Disk *>(p);if(lba>=16)return 0;d.writes++;memcpy(d.bytes.data()+lba*512,in,512);return 1;}
static void irq(void *p,int v){static_cast<Disk *>(p)->irq=v!=0;}
static void reg_write(Vt83c461 *v,unsigned reg,uint8_t data){vt83c461_write_cs0(v,reg/4,uint32_t(data)<<((reg%4)*8),0xffu<<((reg%4)*8));}
static uint8_t reg_read(Vt83c461 *v,unsigned reg){return vt83c461_read_cs0(v,reg/4,0xffu<<((reg%4)*8))>>((reg%4)*8);}
int main(){
    Disk disk;for(unsigned i=0;i<disk.bytes.size();i++)disk.bytes[i]=(i*17+3)&255;
    ata_storage_config_t cfg{};cfg.sector_count=16;cfg.cylinders=1;cfg.heads=1;cfg.sectors_per_track=16;
    cfg.device_kind=ATA_STORAGE_DEVICE_IDE_HDD;cfg.media_opaque=&disk;cfg.read_sector=read_sector;cfg.write_sector=write_sector;cfg.line_opaque=&disk;cfg.irq=irq;
    ata_storage_t *ata=nullptr;assert(ata_storage_create(&cfg,&ata)==0);
    Vt83c461 *v=nullptr;assert(vt83c461_create(ata,&v)==0);assert(ata_storage_run_until(ata,rotation+1)==0);
    vt83c461_config_write(v,1,0x12345678);assert(vt83c461_config_read(v,1)==0x78);
    vt83c461_config_write(v,2,0x102);vt83c461_config_write(v,3,0x1234);assert(vt83c461_config_read(v,2)==2 && vt83c461_config_read(v,3)==0x34);
    vt83c461_config_write(v,2,16);vt83c461_config_write(v,3,0xab);assert(vt83c461_config_read(v,3)==0);
    vt83c461_config_write(v,2,2);assert(vt83c461_config_read(v,3)==0x34);
    vt83c461_config_write(v,3,0xff000000);assert(vt83c461_config_read(v,3)==0); // source replacement, not COMBINE_DATA
    vt83c461_config_write(v,3,0x56);
    // Different byte lanes target the source task file, one branch per access.
    reg_write(v,2,0x12);reg_write(v,3,0x34);reg_write(v,4,0x56);reg_write(v,5,0x78);reg_write(v,6,0xe0);
    assert(reg_read(v,2)==0x12 && reg_read(v,3)==0x34 && reg_read(v,4)==0x56 && reg_read(v,5)==0x78);
    assert(vt83c461_read_cs0(v,1,0xffffffff)==0x56); // full mask chooses reg4 only
    Vt83c461Status before,after;vt83c461_status(v,&before);assert(vt83c461_read_cs0(v,0,0)==0);vt83c461_write_cs0(v,0,0,0);vt83c461_status(v,&after);assert(before.cs0_reads==after.cs0_reads && before.cs0_writes==after.cs0_writes);
    reg_write(v,7,0xec);assert(ata_storage_run_until(ata,ata_storage_now(ata)+10000)==0);assert(disk.irq);
    assert(vt83c461_read_cs1(v,1,0xff0000)>>16 & 8);assert(disk.irq); // alternate status preserves IRQ
    assert(reg_read(v,7)&8);assert(!disk.irq); // status acknowledges IRQ
    const uint16_t *id=ata_storage_identify(ata);
    uint32_t pair=vt83c461_read_cs0(v,0,0xffffffff);assert(pair==(uint32_t(id[1])<<16|id[0]));
    for(unsigned i=2;i<256;i++)assert(vt83c461_read_cs0(v,0,0xffff)==id[i]);
    assert(!(ata_storage_status(ata)&8));
    reg_write(v,7,0xec);assert(ata_storage_run_until(ata,ata_storage_now(ata)+10000)==0);
    assert(vt83c461_read_cs0(v,0,0xff)==(id[0]&255)); // wired-AND mask, still consumes a word
    for(unsigned i=1;i<256;i++)assert(vt83c461_read_cs0(v,0,0xffff)==id[i]);
    auto lba=[&](uint8_t n){reg_write(v,2,1);reg_write(v,3,n);reg_write(v,4,0);reg_write(v,5,0);reg_write(v,6,0xe0);};
    lba(3);reg_write(v,7,0x20);assert(ata_storage_run_until(ata,ata_storage_now(ata)+400)==0);
    for(unsigned i=0;i<128;i++){
        uint32_t expected=0;memcpy(&expected,disk.bytes.data()+3*512+i*4,4);
        assert(vt83c461_read_cs0(v,0,0xffffffff)==expected);
    }
    lba(4);reg_write(v,7,0x30);for(unsigned i=0;i<128;i++)vt83c461_write_cs0(v,0,0x89ab0123,0xffffffff);
    assert(ata_storage_run_until(ata,ata_storage_now(ata)+3000000)==0 && disk.writes==1);
    assert(disk.bytes[4*512]==0x23 && disk.bytes[4*512+2]==0xab && disk.bytes[5*512-1]==0x89);
    // SET FEATURES survives native bus operations instead of being forced to 16 bits.
    reg_write(v,1,1);reg_write(v,7,0xef);assert(ata_storage_run_until(ata,ata_storage_now(ata)+10000)==0);
    reg_write(v,7,0xec);assert(ata_storage_run_until(ata,ata_storage_now(ata)+10000)==0);
    assert(vt83c461_read_cs0(v,0,0xffff)==(id[0]&255));assert(vt83c461_read_cs0(v,0,0xffff)==(id[0]>>8));
    vt83c461_reset(v);assert(vt83c461_config_read(v,3)==0x56);vt83c461_status(v,&after);assert(!after.diagnostics);
    assert(ata_storage_run_until(ata,ata_storage_now(ata)+rotation+1)==0);
    vt83c461_destroy(v);ata_storage_destroy(ata);
    puts("VT83C461 configuration replacement/persistence and real HDD IDE32 branch lanes, two-word data, IRQ/alternate status, PIO read/write and SET FEATURES width PASS");
}
