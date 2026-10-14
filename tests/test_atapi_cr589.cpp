#include "../libpcsxcore/arcade/atapi_cr589.h"
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
struct Media {bool present=true,reentry=false;AtapiCr589 *drive=nullptr;uint32_t reads=0;int irq=0,dmarq=0;nscsi_cd_audio_status_t audio{};float gain[2]={1,1};};
static int present(void *p){return static_cast<Media *>(p)->present;}
static uint64_t sequence(void *){return 1;}
static int read_sector(void *p,uint32_t lba,uint8_t *out){if(lba>=40)return 0;static_cast<Media *>(p)->reads++;for(unsigned i=0;i<2048;i++)out[i]=(lba*17+i)&255;return 1;}
static uint32_t track_start(void *,unsigned t){return t==0xaa?40:t==1?20:0;}
static unsigned tracks(void *){return 2;}
static unsigned track_for(void *,uint32_t lba){return lba>=20?1:0;}
static uint8_t adr(void *,unsigned t){return t==0?0x14:0x10;}
static int is_audio(void *,unsigned t){return t==1;}
static int track_info(void *,unsigned t,AtapiTrack *info){if(t>=2)return 0;*info={t==0?1u:7u,20,0,0,0};return 1;}
static int raw(void *,uint32_t lba,uint8_t *out){if(lba>=40)return 0;for(unsigned i=0;i<2352;i++)out[i]=(i+lba)&255;return 1;}
static void play(void *p,uint32_t lba,uint32_t blocks){static_cast<Media *>(p)->audio={int(blocks!=0),0,0,lba};}
static void pause(void *p,int v){static_cast<Media *>(p)->audio.paused=v;}
static void status(void *p,nscsi_cd_audio_status_t *s){*s=static_cast<Media *>(p)->audio;}
static void gain(void *p,unsigned c,float v){assert(c<2);static_cast<Media *>(p)->gain[c]=v;}
static void irq(void *p,int v){auto *m=static_cast<Media *>(p);m->irq=v;if(m->reentry && m->drive){AtapiCr589Status s{};atapi_cr589_get_status(m->drive,&s);atapi_cr589_destroy(m->drive);atapi_cr589_reset(m->drive);}}
static void drq(void *p,int v){static_cast<Media *>(p)->dmarq=v;}
static uint64_t now=0;
static void advance(AtapiCr589 *d,uint64_t ns=1000000){now+=ns;assert(!atapi_cr589_advance_to(d,now));}
static void packet(AtapiCr589 *d,std::array<uint8_t,12> cmd,unsigned limit=0xf800){
    atapi_cr589_write(d,0,4,limit&255,0xffff);atapi_cr589_write(d,0,5,limit>>8,0xffff);
    atapi_cr589_write(d,0,7,0xa0,0xffff);advance(d);
    uint16_t s=atapi_cr589_read(d,1,6,0xffff);assert(s&8);
    assert((atapi_cr589_read(d,0,2,0xffff)&3)==1);
    for(unsigned i=0;i<12;i+=2)atapi_cr589_write(d,0,0,cmd[i]|cmd[i+1]<<8,0xffff);
}
static std::vector<uint8_t> input(AtapiCr589 *d){
    std::vector<uint8_t> bytes;
    for(unsigned rounds=0;rounds<8;rounds++){
        auto s=atapi_cr589_read(d,1,6,0xffff);if(!(s&8))return bytes;
        unsigned n=atapi_cr589_read(d,0,4,0xffff)|(atapi_cr589_read(d,0,5,0xffff)<<8);
        assert(n && !(n&1) && n<=0xf800);
        for(unsigned i=0;i<n;i+=2){uint16_t v=atapi_cr589_read(d,0,0,0xffff);bytes.push_back(v);bytes.push_back(v>>8);}advance(d);
    }
    assert(false);return bytes;
}
static void output(AtapiCr589 *d,const uint8_t *data,unsigned n){assert(!(n&1));for(unsigned i=0;i<n;i+=2)atapi_cr589_write(d,0,0,data[i]|data[i+1]<<8,0xffff);advance(d);}
int main(){
    Media m;AtapiCr589Config c{};
    c.media={&m,present,sequence,read_sector,track_start,tracks,track_for,adr,is_audio,play,pause,status,gain};
    c.track_info=track_info;c.read_raw=raw;c.irq=irq;c.dmarq=drq;c.line_opaque=&m;
    assert(!atapi_cr589_create(nullptr));AtapiCr589 *d=atapi_cr589_create(&c);assert(d);m.drive=d;advance(d,10000000);
    assert(atapi_cr589_read(d,0,4,0xffff)==0x14 && atapi_cr589_read(d,0,5,0xffff)==0xeb);
    atapi_cr589_write(d,0,7,0xa1,0xffff);advance(d);auto id=input(d);assert(id.size()==512 && id[1]==0x85);
    assert(id[54]=='A' && id[55]=='M'); // little-endian ATA string words
    std::array<uint8_t,12> cmd{};cmd[0]=0x12;cmd[4]=36;packet(d,cmd);auto inquiry=input(d);
    assert(inquiry.size()==36 && !memcmp(inquiry.data()+8,"MATSHITACD-ROM CR-589   GS0N",28));
    cmd={};cmd[0]=0x28;cmd[5]=3;cmd[8]=2;packet(d,cmd,2048);auto sectors=input(d);
    assert(sectors.size()==4096 && sectors[0]==51 && sectors[2048]==68 && m.reads==2);
    // Status acknowledgement is distinct from alternate status.
    cmd={};cmd[0]=0;packet(d,cmd);assert(m.irq);atapi_cr589_read(d,1,6,0xffff);assert(m.irq);atapi_cr589_read(d,0,7,0xffff);assert(!m.irq);
    cmd={};cmd[0]=0x45;cmd[5]=20;cmd[8]=5;packet(d,cmd);assert(m.audio.active && m.audio.lba==20);
    cmd={};cmd[0]=0x4b;packet(d,cmd);assert(m.audio.paused);
    cmd[8]=1;packet(d,cmd);assert(!m.audio.paused);
    cmd={};cmd[0]=0xcc;cmd[8]=28;packet(d,cmd);output(d,inquiry.data()+8,28);
    cmd={};cmd[0]=0x12;cmd[4]=36;packet(d,cmd);auto downloading=input(d);
    assert(!memcmp(downloading.data()+8,"MATSHITA CD98Q4 DOWNLOADGS0N",28));
    cmd={};cmd[0]=0xcc;cmd[8]=28;packet(d,cmd);output(d,downloading.data()+8,28);
    cmd={};cmd[0]=0x12;cmd[4]=36;packet(d,cmd);assert(input(d)==inquiry);
    std::array<uint8_t,64> data{};for(unsigned i=32;i<64;i++)data[i]=i;
    cmd={};cmd[0]=0x3b;cmd[5]=0x10;cmd[8]=64;packet(d,cmd);output(d,data.data(),64);
    cmd[0]=0x3c;cmd[8]=32;packet(d,cmd);auto buffer=input(d);assert(buffer.size()==32 && buffer[0]==32 && buffer[31]==63);
    std::array<uint8_t,65536> nv{};assert(!atapi_cr589_nvram_export(d,nv.data(),nv.size()));assert(nv[0x10]==32);
    AtapiCr589Status s{};atapi_cr589_get_status(d,&s);assert(s.packets>=11 && !s.faults && s.sectors==2);
    atapi_cr589_reset(d);advance(d,10000000);assert(!atapi_cr589_nvram_import(d,nv.data(),nv.size()));
    // A real missing medium produces source sense failure, not a zero sector.
    m.present=false;cmd={};cmd[0]=0x28;cmd[8]=1;packet(d,cmd);atapi_cr589_get_status(d,&s);assert((s.status&1) && !(s.status&8) && s.sense_key==0 && s.sense_asc==0); // pinned CR589 bypasses the generic CD-ROM ASC override
    // Invalid buffer ranges are rejected before accessing host memory.
    cmd={};cmd[0]=0x3c;cmd[3]=1;cmd[8]=2;packet(d,cmd);atapi_cr589_get_status(d,&s);assert((s.status&1) && (s.faults&4));
    atapi_cr589_read(d,0,7,0xffff);m.reentry=true;cmd={};packet(d,cmd);atapi_cr589_get_status(d,&s);assert(s.faults&8);m.reentry=false;
    atapi_cr589_destroy(d);
    puts("Source CR589 ATAPI signature/IDENTIFY, packet budgets/chunked sectors, IRQ ack, CDDA commands, writable buffer/identity download and absent/range errors PASS");
}
