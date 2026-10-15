/* GPL-2.0-or-later. Native host clock/map plumbing for pinned MAME Twinkle.
 * SCSI bus adapter pattern follows the BSD-3-Clause Konami GV port (Olivier
 * Galibert); actual controller/target models retain their original licenses. */
#include "systemtwinkle.h"
extern "C" {
#include "r3000a.h"
#include "psxmem.h"
#include "psxhw.h"
#include "system12.h"
#include "system11.h"
#include "system10.h"
#include "systemzn.h"
#include "systemtaito.h"
#include "systemgv.h"
#include "systemgnet.h"
#include "systemgq.h"
#include "system573.h"
#include "gpu_vram.h"
}
#include "arcade/twinkle_board.h"
#include "arcade/rtc65271.h"
#include "arcade/m24c02.h"
#include "arcade/psx_sio.h"
#include "arcade/ncr53cf96.h"
#include "arcade/nscsi_cd.h"
#include "arcade/chd_cd.h"
#include "arcade/chd_hd.h"
#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr const char *ini=
 "format=pcsx-aow-twinkle-v1\nmame_set=bmiidx2\nmame_parent=gq863\nmachine=twinklei\n"
 "ram_bytes=4194304\nvram_bytes=2097152\nbios=bios.bin\nsoundcpu=soundcpu.bin\nsecurity=security.bin\n"
 "program_cd=gc985a01.chd\nvideo_cd=gc985a04.chd\nsound_hdd=985hdda01.chd\n";
constexpr uint8_t nv_magic[16]={'T','W','I','N','K','N','V','1','B','M','I','I','2',0,0,0};
constexpr size_t nv_bytes=32+RTC65271_NVRAM_BYTES+256;
struct Board {
    bool active=false,fixture=false,memory_initialized=false,scheduling=false;
    unsigned faults=0;uint32_t last_cycle=0,old_ram=0,old_rom=0;
    uint64_t cycles=0,now_ns=0,reads=0,writes=0,dma_requests=0,dma_remaining=0,dma_bytes=0;
    uint32_t dma_address=0;bool dma_write=false,drq=false;
    std::vector<uint8_t> bios,soundrom,security_factory;
    std::array<uint8_t,256> security{};
    std::unique_ptr<M24c02> eeprom;
    std::vector<uint16_t> wave;
    std::unique_ptr<TwinkleBoard> sound;
    Rtc65271 *rtc=nullptr;Rtc65271Date date{};
    psx_sio_t *sio[2]{};
    ChdCd *cd=nullptr,*video=nullptr;arcade_chd_hd_t *hdd=nullptr;
    NscsiBus *bus=nullptr;Ncr53cf96 *ncr=nullptr;NscsiCdrom *target=nullptr;
    uint32_t input[7]={0xff,0,0,0,0,0,0xffff};
    uint8_t io_index=0,outputs[256]{};
    uint16_t main_led=0,key_led=0,serial_latch=0,mixer_serial=0;
    unsigned serial_bits=0,serial_shift=0,mixer_bits=0,mixer_shift=0;
    uint64_t rf_frames=0,rf_nonzero=0,hdd_reads=0,hdd_failures=0,watchdog_kicks=0;
    uint64_t watchdog_due=40642560; // Source LTC1232 timeout: 1.2 s at PSXCLK.
    unsigned rf_peak=0;
} b;
std::vector<uint8_t> bytes(const std::string &path){std::ifstream f(path,std::ios::binary);if(!f)return {};return {(std::istreambuf_iterator<char>(f)),{}};}
bool sha(const std::vector<uint8_t> &data,const char *expected){
    uint8_t digest[20];char hex[41];CC_SHA1(data.data(),CC_LONG(data.size()),digest);
    for(unsigned i=0;i<20;i++)std::snprintf(hex+i*2,3,"%02x",digest[i]);return !std::strcmp(hex,expected);
}
void put_le32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
uint32_t get_le32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=uint32_t(p[i])<<(i*8);return v;}
void fault(const char *what,uint32_t address=0){
    if(!b.faults)std::fprintf(stderr,"SystemTwinkle: %s at %08x PC=%08x cycle=%u; execution stopped\n",what,address,psxRegs.pc,psxRegs.cycle);
    b.faults|=1;
}
void irq10(void *,int state){if(state && psxRegs.ptrs.psxH)psxHu32ref(0x1070)|=SWAPu32(1u<<10);}
void sio_irq(void *opaque,int state){if(state && psxRegs.ptrs.psxH)psxHu32ref(0x1070)|=SWAPu32(1u<<(7+uintptr_t(opaque)));}
void drq(void *,int state){b.drq=state!=0;}
void ctrl(void *,uint32_t value,uint32_t mask){nscsi_bus_drive_ctrl(b.bus,7,value,mask);}
void data(void *,uint32_t value){nscsi_bus_drive_data(b.bus,7,value);}
void wait_ctrl(void *,uint32_t value,uint32_t mask){nscsi_bus_wait_ctrl(b.bus,7,value,mask);}
void changed(void *,uint32_t c,uint32_t d){if(b.ncr)ncr53cf96_bus_changed(b.ncr,c,d);}
bool flush(){if(!nscsi_bus_flush(b.bus)){fault("SCSI bus failure");return false;}return true;}
bool dma_pump(){
    while(b.drq && b.dma_remaining && !b.faults){
        if(!psxRegs.ptrs.psxM || b.dma_address>=0x400000){fault("SCSI DMA outside RAM",b.dma_address);return false;}
        if(b.dma_write)ncr53cf96_dma_write(b.ncr,psxRegs.ptrs.psxM[b.dma_address]);
        else psxRegs.ptrs.psxM[b.dma_address]=ncr53cf96_dma_read(b.ncr);
        b.dma_address++;b.dma_remaining--;b.dma_bytes++;if(!flush())return false;
    }return !b.faults;
}
uint64_t next_scsi(){
    ncr53cf96_status_t n{};nscsi_cdrom_status_t c{};ncr53cf96_get_status(b.ncr,&n);nscsi_cdrom_get_status(b.target,&c);
    uint64_t due=UINT64_MAX;
    if(n.timer_deadline!=UINT64_MAX)due=(n.timer_deadline/16000000)*1000000000+
        ((n.timer_deadline%16000000)*1000000000+15999999)/16000000;
    return std::min(due,c.target.timer_deadline_ns);
}
bool scsi_to(uint64_t target){
    if(b.scheduling || target<b.now_ns){fault("SCSI clock/reentry failure");return false;}b.scheduling=true;
    unsigned events=0;
    for(;;){
        uint64_t due=next_scsi(),at=std::min(target,due);
        if(++events>1000000 || at<b.now_ns){fault("SCSI scheduler failure");break;}
        b.now_ns=at;
        ncr53cf96_advance_to(b.ncr,(at/1000000000)*16000000+(at%1000000000)*16000000/1000000000);
        nscsi_cdrom_advance_to(b.target,at);
        if(!chd_cd_advance_to(b.cd,at) || !flush() || !dma_pump()){fault("SCSI/media advance failure");break;}
        if(at==target && next_scsi()>target)break;
    }
    ncr53cf96_status_t n{};nscsi_cdrom_status_t c{};
    ncr53cf96_get_status(b.ncr,&n);nscsi_cdrom_get_status(b.target,&c);
    if(n.faults || c.faults || c.target.faults)fault("SCSI controller/target fault");
    b.scheduling=false;return !b.faults;
}
int hdd_read(void *,uint32_t lba,void *out){b.hdd_reads++;int ok=!arcade_chd_hd_read(b.hdd,lba,static_cast<uint8_t *>(out),512);b.hdd_failures+=!ok;return ok;}
int hdd_write(void *,uint32_t,const void *){return 0;}
void rf_sample(void *,const int32_t *v,uint64_t){
    b.rf_frames++;b.rf_nonzero+=v[0]!=0 || v[1]!=0;
    for(unsigned i=0;i<2;i++){uint64_t a=v[i]<0?-int64_t(v[i]):v[i];if(a>b.rf_peak)b.rf_peak=unsigned(std::min(a,uint64_t(UINT32_MAX)));}
}
Rtc65271Config rtc_config(const uint8_t *nv=nullptr){Rtc65271Config c{};c.date=b.date;c.nvram=nv;c.nvram_size=nv?RTC65271_NVRAM_BYTES:0;return c;}
int load(const char *directory,bool fixture){
    if(b.active || !directory || system12_active() || system11_active() || system10_active() || systemzn_active() ||
       systemtaito_active() || systemgv_active() || systemgnet_active() || systemgq_active() || system573_active())return -1;
    std::string root=directory;auto config=bytes(root+"/board.ini");
    if(std::string(config.begin(),config.end())!=ini){std::fprintf(stderr,"SystemTwinkle: unreviewed/incomplete board.ini\n");return -1;}
    b.bios=bytes(root+"/bios.bin");b.soundrom=bytes(root+"/soundcpu.bin");b.security_factory=bytes(root+"/security.bin");
    if(b.bios.size()!=0x80000 || b.soundrom.size()!=0x80000 || b.security_factory.size()!=256 ||
       (!fixture && (!sha(b.bios,"3599b40a5872eab3a00d345287635355fcb25a71") ||
                     !sha(b.soundrom,"23e0017c501465f50c5ccc0a2ecce9c18f5f712e") ||
                     !sha(b.security_factory,"f094a12c9a56d351667746a765804c94cb3f96bb")))){
        std::fprintf(stderr,"SystemTwinkle: original chip size/SHA1 mismatch\n");systemtwinkle_unload();return -1;
    }
    std::copy(b.security_factory.begin(),b.security_factory.end(),b.security.begin());char message[256];
    b.cd=chd_cd_open((root+"/gc985a01.chd").c_str(),nullptr,message,sizeof(message));
    b.video=chd_cd_open((root+"/gc985a04.chd").c_str(),nullptr,message,sizeof(message));
    b.hdd=arcade_chd_hd_open((root+"/985hdda01.chd").c_str(),nullptr,message,sizeof(message));
    arcade_chd_hd_info_t hd{};if(b.hdd)arcade_chd_hd_get_info(b.hdd,&hd);
    uint8_t expected[20]={0xa5,0xb9,0xec,0x9a,0x5a,0xfa,0x38,0xf3,0x6a,0xf5,0x29,0xd3,0xae,0xa5,0x35,0x5e,0xa7,0xd0,0x22,0xca};
    if(!b.cd || !b.video || !b.hdd || hd.sector_bytes!=512 || hd.cylinders>65535 || hd.heads>255 || hd.sectors_per_track>255 ||
       (!fixture && (!chd_cd_verify_sha1(b.cd,"0b783f11317f64552ebf3323459139529e7f315f") ||
                     !chd_cd_verify_sha1(b.video,"4306417f61eb1ea92894d288cdb7c385eb4610f2") ||std::memcmp(hd.sha1,expected,20)))){
        std::fprintf(stderr,"SystemTwinkle: original program/video/HDD identity or metadata mismatch\n");systemtwinkle_unload();return -1;
    }
    b.eeprom=std::make_unique<M24c02>(b.security);
    b.wave.resize(TwinkleBoard::WAVE_RAM_BYTES/2);ata_storage_config_t ata{};
    ata.sector_count=hd.sector_count;ata.cylinders=hd.cylinders;ata.heads=hd.heads;ata.sectors_per_track=hd.sectors_per_track;
    ata.read_sector=hdd_read;ata.write_sector=hdd_write;
    std::array<uint8_t,512> identify{};size_t size=0;
    auto id=arcade_chd_hd_read_metadata(b.hdd,ARCADE_CHD_HD_METADATA_ATA_IDENTIFY,identify.data(),identify.size(),&size);
    if(id==ARCADE_CHD_HD_METADATA_OK && size==512){ata.identify_data=identify.data();ata.identify_size=size;}
    else if(id!=ARCADE_CHD_HD_METADATA_MISSING){systemtwinkle_unload();return -1;}
    TwinkleBoard::Config sound{};sound.program_rom=b.soundrom.data();sound.program_rom_size=b.soundrom.size();
    sound.wave_ram=b.wave.data();sound.wave_ram_bytes=b.wave.size()*2;sound.hdd=&ata;sound.rf5c400.sample=rf_sample;
    b.sound=std::make_unique<TwinkleBoard>(sound);
    std::time_t host=std::time(nullptr);std::tm local{};localtime_r(&host,&local);
    b.date={local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_wday+1,local.tm_hour,local.tm_min,local.tm_sec};
    auto rtc=rtc_config();nscsi_cd_media_ops_t media{};b.bus=nscsi_bus_create();
    nscsi_bus_ops_t bus{};bus.changed=changed;
    bool ok=b.sound->valid() && !rtc65271_create(&rtc,&b.rtc) && b.bus && nscsi_bus_attach(b.bus,7,&bus);
    ncr53cf96_bus_ops_t ncr{};ncr.drive_ctrl=ctrl;ncr.drive_data=data;ncr.wait_ctrl=wait_ctrl;ncr.irq=irq10;ncr.drq=drq;
    if(ok)b.ncr=ncr53cf96_create(16000000,&ncr);
    nscsi_cdrom_config_t cd{};cd.bus=b.bus;cd.scsi_id=4;cd.block_size=2048;
    if(b.ncr && chd_cd_get_media_ops(b.cd,&media)){cd.media=media;b.target=nscsi_cdrom_create(&cd);}
    for(unsigned i=0;i<2 && b.target;i++){psx_sio_callbacks_t cb{};cb.opaque=reinterpret_cast<void *>(uintptr_t(i));cb.irq=sio_irq;
        if(psx_sio_create(i?PSX_SIO1:PSX_SIO0,&cb,&b.sio[i]))break;}
    if(!b.ncr || !b.target || !b.sio[0] || !b.sio[1] || !flush() || gpu_set_model(2) || gpu_set_vram_height(1024)){
        systemtwinkle_unload();return -1;
    }
    b.old_ram=psxRamSize;b.old_rom=psxRomSize;psxRamSize=0x400000;psxRomSize=0x80000;
    b.active=true;b.fixture=fixture;b.last_cycle=psxRegs.cycle;Config.HLE=FALSE;
    std::fprintf(stderr,"SystemTwinkle bmiidx2: %s; FDC/UART/DVD not yet integrated; unresolved accesses fault explicitly\n",fixture?"GENERATED MIPS/SOUND FIXTURE, not original game evidence":"three original chip SHA1s and all three source CHD identities matched");
    return 0;
}
bool range(uint32_t a,unsigned w,uint32_t first,uint32_t last){return a<=last && uint64_t(a)+w>first;}
int access(uint32_t address,unsigned width,uint32_t *value,uint32_t mask,bool write){
    if(!b.active || !value || (width!=1 && width!=2 && width!=4))return 0;
    uint32_t a=address&0x1fffffff;
    if(a>=0x1f801040 && a+width<=0x1f801060){
        if(systemtwinkle_sync(psxRegs.cycle)){if(!write)*value=0;return -1;}unsigned p=(a>=0x1f801050),shift=(a&3)*8;
        uint32_t bits=width==4?0xffffffffu:((1u<<(width*8))-1),raw=0;
        int rc=write?psx_sio_write(b.sio[p],(a>>2)&3,*value<<shift,mask<<shift):psx_sio_read(b.sio[p],(a>>2)&3,bits<<shift,&raw);
        if(!write)*value=(raw>>shift)&bits;if(rc)fault("Sony serial register failure",a);return rc?-1:1;
    }
    if(!range(a,width,0x1f000000,0x1f2b00ff))return 0;
    if(!write)*value=0;
    if(systemtwinkle_sync(psxRegs.cycle))return -1;
    if(write)b.writes++;else b.reads++;
    for(unsigned lane=0;lane<width;lane++) {
        uint32_t p=a+lane;
        if(p>=0x1f210000 && p<=0x1f2107ff && !(p&1) &&
           (!write || (mask&(255u<<(lane*8))))) {
            fault("unsupported FDC37C665GT/UART access",p);return -1;
        }
    }

    uint32_t result=0;
    for(unsigned lane=0;lane<width;lane++){
        uint32_t p=a+lane;uint8_t byte=0,v=uint8_t(*value>>(lane*8));if(write && !(mask&(255u<<(lane*8))))continue;
        if(p>=0x1f000000 && p<0x1f000800 && !(p&1)){
            if(write){if(b.sound->main_dpram_write((p-0x1f000000)/2,v))fault("sound mailbox write",p);}
            else if(b.sound->main_dpram_read((p-0x1f000000)/2,&byte))fault("sound mailbox read",p);
        }else if(p>=0x1f200000 && p<0x1f200020 && !(p&1)){
            if(write)ncr53cf96_write(b.ncr,(p-0x1f200000)/2,v);else byte=ncr53cf96_read(b.ncr,(p-0x1f200000)/2);
            if(!flush() || !dma_pump())break;
        }else if(p>=0x1f290000 && p<0x1f2a0080 && !(p&1) &&
                 (p<0x1f290080 || p>=0x1f2a0000)){
            unsigned bank=p>=0x1f2a0000,offset=(p-(bank?0x1f2a0000:0x1f290000))/2;
            int rc=write?rtc65271_write(b.rtc,bank,offset,v):rtc65271_read(b.rtc,bank,offset,&byte);
            if(rc)fault("RTC register failure",p);
        }else if(p==0x1f270000 && write){
            // MAME ioport fields are ordered by bit mask: SDA before SCL.
            b.eeprom->write_sda((v>>3)&1);b.eeprom->write_scl((v>>4)&1);
        }else if(p==0x1f280001 && !write)byte=b.eeprom->read_sda()<<4;
        else if(p>=0x1f220000 && p<=0x1f220003 && !(p&1) && write){
            if(p&2)b.io_index=v;else b.outputs[b.io_index]=v;
        }else if(p>=0x1f220004 && p<=0x1f220007 && !(p&1) && !write){
            if(!(p&2) && b.io_index>=7 && b.io_index<=0x2f && (b.io_index&7)==7)byte=b.input[(b.io_index-7)/8];
        }else if(p>=0x1f240000 && p<=0x1f240003 && !write)byte=b.input[6]>>((p-0x1f240000)*8);
        else if(p==0x1f218000 && write){b.watchdog_kicks++;b.watchdog_due=b.cycles+40642560;}
        // Other source-unmapped lanes/read neighbours return source zero.
        result|=uint32_t(byte)<<(lane*8);
    }
    uint32_t latch=a&~1u;
    if(write && (latch==0x1f230000 || latch==0x1f250000 || latch==0x1f260000)){
        uint16_t v=(*value&mask)<<((a&1)*8);if(latch==0x1f230000)b.main_led=(~v)&0x1ff;
        else if(latch==0x1f250000)b.key_led=v;
        else {unsigned clock=(v>>5)&1,cs=(v>>6)&1;
            if(!cs && (b.serial_latch&0x40)){b.serial_bits=0;b.serial_shift=0;}
            if(clock && !(b.serial_latch&0x20) && b.serial_bits<8){b.serial_shift=(b.serial_shift<<1)|((v>>4)&1);b.serial_bits++;}b.serial_latch=v;}
    }
    if(write && (a&~1u)==0x1f2b0020){
        uint16_t v=(*value&mask)<<((a&1)*8);
        if(!(v&4) && (b.mixer_serial&4)){b.mixer_bits=0;b.mixer_shift=0;}
        if((v&1) && !(b.mixer_serial&1) && b.mixer_bits<8){
            b.mixer_shift=(b.mixer_shift<<1)|((v>>1)&1);
            if(++b.mixer_bits==8){b.mixer_bits=0;b.mixer_shift=0;}
        }b.mixer_serial=v;
    }
    if(!write)*value=result;return b.faults?-1:1;
}
}
extern "C" int systemtwinkle_load(const char *p){return load(p,false);}
extern "C" int systemtwinkle_test_load(const char *p){return load(p,true);}
extern "C" int systemtwinkle_active(void){return b.active;}
extern "C" void systemtwinkle_unload(void){
    b.sound.reset();if(b.rtc)rtc65271_try_destroy(b.rtc);if(b.target)nscsi_cdrom_destroy(b.target);
    ncr53cf96_destroy(b.ncr);nscsi_bus_destroy(b.bus);chd_cd_close(b.cd);chd_cd_close(b.video);arcade_chd_hd_close(b.hdd);
    for(auto *s:b.sio)psx_sio_destroy(s);if(b.active){psxRamSize=b.old_ram;psxRomSize=b.old_rom;}b=Board{};
}
extern "C" void systemtwinkle_mem_reset(void){
    if(!b.active || !psxRegs.ptrs.psxM)return;
    std::memcpy(psxRegs.ptrs.psxR,b.bios.data(),b.bios.size());if(!b.memory_initialized)std::memset(psxRegs.ptrs.psxM,0,0x400000);
    std::memset(psxRegs.ptrs.psxP,0,0x10000);b.memory_initialized=true;
}
extern "C" void systemtwinkle_reset(void){
    if(!b.active)return;
    if(!psxArcadeSourceTimingActive() || PSXCLK!=33868800 || Config.cycle_multiplier!=200){fault("source PSX timing profile required");return;}
    b.last_cycle=psxRegs.cycle;b.dma_remaining=0;b.watchdog_due=b.cycles+40642560;b.sound->reset();ncr53cf96_reset(b.ncr);nscsi_cdrom_reset(b.target);flush();
}
extern "C" int systemtwinkle_sync(uint32_t cycle){
    if(!b.active)return 0;if(b.faults)return -1;
    if(cycle<b.last_cycle && b.last_cycle-cycle<0x80000000){fault("CPU clock rewind");return -1;}
    b.cycles+=uint32_t(cycle-b.last_cycle);b.last_cycle=cycle;
    if(b.cycles>=b.watchdog_due){fault("watchdog expired; native whole-machine reset sequencing is pending");return -1;}
    uint64_t ns=(b.cycles/PSXCLK)*1000000000+(b.cycles%PSXCLK)*1000000000/PSXCLK;
    uint64_t sound=(b.cycles/1323)*625+(b.cycles%1323)*625/1323;
    while(b.sound->cpu_cycles()<sound){
        uint32_t step=uint32_t(std::min(sound-b.sound->cpu_cycles(),uint64_t(16000)));
        if(b.sound->run(step)!=step || b.sound->cpu_bus_error() || b.sound->callback_fault()){
            fault("sound CPU/device failure",b.sound->last_bus_error_address());return -1;}
    }
    for(auto *s:b.sio)if(psx_sio_advance_to_cycle(s,b.cycles)){fault("Sony serial clock failure");return -1;}
    if(rtc65271_advance_to(b.rtc,ns) || !scsi_to(ns)){fault("device clock failure");return -1;}
    return b.faults?-1:0;
}
extern "C" int systemtwinkle_read(uint32_t a,unsigned w,uint32_t *v){return access(a,w,v,0,false);}
extern "C" int systemtwinkle_write(uint32_t a,unsigned w,uint32_t v,uint32_t m){return access(a,w,&v,m,true);}
extern "C" int systemtwinkle_dma5(uint32_t address,uint32_t count,uint32_t control){
    if(!b.active || systemtwinkle_sync(psxRegs.cycle))return -1;
    uint64_t words=0;bool write=false;
    if(control==0x01000000)words=count;
    else if(control==0x01000200 || control==0x01000201){words=uint64_t(count&65535)*(count>>16?count>>16:65536);write=control&1;}
    else if((control&0xffbffeff)==0x11000000)words=uint64_t(count&65535)*std::max(count>>16,1u);
    else if(control==0x11050100 || control==0x11150100){words=count;write=true;}
    else {fault("unsupported source SCSI DMA mode",control);return -1;}
    address&=0x1fffffcu;
    if(b.dma_remaining || address>0x400000 || words>(0x400000-address)/4){fault("invalid SCSI DMA range",address);return -1;}
    b.dma_requests++;b.dma_address=address;b.dma_remaining=words*4;b.dma_write=write;
    return dma_pump()?0:-1;
}
extern "C" unsigned systemtwinkle_analog_max(const char *name){
    if(!name)return 0;if(!std::strcmp(name,"p1table") || !std::strcmp(name,"p2table"))return 255;
    if(!std::strncmp(name,"volume",6) && name[6]>='1' && name[6]<='5' && !name[7])return 15;return 0;
}
extern "C" int systemtwinkle_set_input(const char *name,int value){
    if(!b.active || !name)return -1;unsigned limit=systemtwinkle_analog_max(name);
    if(limit){if(value<0 || unsigned(value)>limit)return -1;
        if(!std::strcmp(name,"p1table"))b.input[2]=value;else if(!std::strcmp(name,"p2table"))b.input[1]=value;
        else {unsigned n=name[6]-'1',slot=3+n/2,shift=(n&1)*4;b.input[slot]=(b.input[slot]&~(15u<<shift))|(value<<shift);}return 0;}
    if(value!=0 && value!=1)return -1;unsigned slot=0,mask=0;
    struct Button{const char *name;unsigned mask;};
    static const Button front[]={{"p1start",1},{"p2start",2},{"vefx",4},{"effect",8},{"test",16},{"service",32}};
    for(auto &button:front)if(!std::strcmp(name,button.name))mask=button.mask;
    if(!std::strcmp(name,"coin1")){slot=6;mask=0x4000;}
    if(!std::strncmp(name,"p1button",8) && name[8]>='1' && name[8]<='7' && !name[9]){slot=6;mask=1u<<(name[8]-'1');}
    if(!std::strncmp(name,"p2button",8) && name[8]>='1' && name[8]<='7' && !name[9]){slot=6;mask=1u<<(name[8]-'1'+7);}
    if(!mask)return -1;if(value)b.input[slot]&=~mask;else b.input[slot]|=mask;return 0;
}
extern "C" int systemtwinkle_selftest(void){
    if(!b.active || psxRamSize!=0x400000 || gpu_get_vram_height()!=1024)return -1;
    std::fprintf(stderr,"SystemTwinkle native geometry/selftest PASS; incomplete peripherals remain explicit faults\n");return 0;
}
extern "C" size_t systemtwinkle_nvram_size(void){return nv_bytes;}
extern "C" int systemtwinkle_nvram_export(unsigned char *out,size_t size){
    if(!b.active || !out || size!=nv_bytes)return -1;std::memset(out,0,size);std::memcpy(out,nv_magic,16);
    uint32_t length=nv_bytes-32;put_le32(out+16,length);
    if(rtc65271_nvram_export(b.rtc,out+32,RTC65271_NVRAM_BYTES))return -1;
    std::memcpy(out+32+RTC65271_NVRAM_BYTES,b.security.data(),256);
    uint32_t crc=crc32(0,out+32,length);put_le32(out+20,crc);return 0;
}
extern "C" int systemtwinkle_nvram_import(const unsigned char *in,size_t size){
    if(!b.active || b.cycles || !in || size!=nv_bytes || std::memcmp(in,nv_magic,16))return -1;
    uint32_t length=get_le32(in+16),crc=get_le32(in+20);
    for(unsigned i=24;i<32;i++)if(in[i])return -1;
    if(length!=nv_bytes-32 || crc!=crc32(0,in+32,length))return -1;
    auto cfg=rtc_config(in+32);Rtc65271 *rtc=nullptr;if(rtc65271_create(&cfg,&rtc))return -1;
    rtc65271_try_destroy(b.rtc);b.rtc=rtc;std::memcpy(b.security.data(),in+32+RTC65271_NVRAM_BYTES,256);return 0;
}
extern "C" void systemtwinkle_report(FILE *out){
    if(!b.active || !out)return;ncr53cf96_status_t n{};nscsi_cdrom_status_t c{};Rtc65271Status r{};
    ncr53cf96_get_status(b.ncr,&n);nscsi_cdrom_get_status(b.target,&c);rtc65271_get_status(b.rtc,&r);
    std::fprintf(out,"SystemTwinkle: profile=bmiidx2 media=%s CPU=CXD8530CQ RAM=4194304 VRAM=2097152 PC=%08x cycles=%llu faults=%u MMIO=%llu/%llu\n",b.fixture?"SYNTHETIC_FIXTURE":"SOURCE_SHA_VERIFIED",psxRegs.pc,(unsigned long long)b.cycles,b.faults,(unsigned long long)b.reads,(unsigned long long)b.writes);
    std::fprintf(out,"Twinkle sound: PC=%06x cycles=%llu bus_error=%d HDD_reads=%llu failures=%llu ATA=%02x DMA_words=%llu RF_frames=%llu nonzero=%llu peak=%u\n",b.sound->cpu_pc(),(unsigned long long)b.sound->cpu_cycles(),b.sound->cpu_bus_error(),(unsigned long long)b.hdd_reads,(unsigned long long)b.hdd_failures,b.sound->ata_status(),(unsigned long long)b.sound->ata_dma_words(),(unsigned long long)b.rf_frames,(unsigned long long)b.rf_nonzero,b.rf_peak);
    std::fprintf(out,"Twinkle SCSI: requests=%llu bytes=%llu pending=%llu NCR_faults=%u target_faults=%u RTC_faults=%u RTC_page=%u watchdog_kicks=%llu\n",(unsigned long long)b.dma_requests,(unsigned long long)b.dma_bytes,(unsigned long long)b.dma_remaining,n.faults,c.target.faults,r.faults,r.xram_page,(unsigned long long)b.watchdog_kicks);
}
