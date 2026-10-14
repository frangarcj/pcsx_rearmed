/* GPL-2.0-or-later. Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * sony/zn.cpp cbaj_state (BSD-3-Clause, smf/R. Belmont; see sony_zn_LICENSE.txt).
 * Z80 32MHz/8, YMZ280B 16.9344MHz, two IDT7202s. No wired IRQ/NMI. */
#include "cbaj_sound.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>
struct CbajSound {
    CbajSoundConfig config{};
    Z80 *cpu=nullptr;
    YMZ280B *ymz=nullptr;
    Fifo7200 *fifo[2]{};
    std::array<uint8_t,0x8000> ram{};
    uint64_t cycles=0,cpu_origin=0,time_origin=0,ymz_writes=0;
    uint32_t faults=0;
    bool busy=false;
    ~CbajSound(){z80_destroy(cpu);ymz280b_destroy(ymz);fifo7200_destroy(fifo[0]);fifo7200_destroy(fifo[1]);}
    uint64_t oscillator_cycle(uint64_t cpu_cycle) {
        YMZ280BStatus st{};ymz280b_get_status(ymz,&st);
        if(cpu_cycle/CBAJ_SOUND_CLOCK>UINT64_MAX/YMZ280B_DEFAULT_CLOCK){faults|=1;return st.cycles;}
        uint64_t whole=cpu_cycle/CBAJ_SOUND_CLOCK*YMZ280B_DEFAULT_CLOCK;
        uint64_t part=cpu_cycle%CBAJ_SOUND_CLOCK*YMZ280B_DEFAULT_CLOCK/CBAJ_SOUND_CLOCK;
        if(part>UINT64_MAX-whole){faults|=1;return st.cycles;}
        // The source bus-cycle engine may overshoot a short budget. Preserve
        // actual I/O timestamps; later credit repays that cycle debt.
        return std::max(whole+part,st.cycles);
    }
    uint64_t io_cycle(){
        Z80Status st{};z80_get_status(cpu,&st);
        uint64_t delta=st.cycles-cpu_origin;
        if(delta>UINT64_MAX-time_origin){faults|=1;return oscillator_cycle(cycles);}
        return oscillator_cycle(time_origin+delta);
    }
    static uint8_t read(void *opaque,uint16_t address){
        auto &s=*static_cast<CbajSound *>(opaque);
        return address<0x8000?s.config.program[address]:s.ram[address-0x8000];
    }
    static void write(void *opaque,uint16_t address,uint8_t value){
        auto &s=*static_cast<CbajSound *>(opaque);if(address>=0x8000)s.ram[address-0x8000]=value;
    }
    static uint8_t io_read(void *opaque,uint16_t port){
        auto &s=*static_cast<CbajSound *>(opaque);uint8_t value=0; // source default unmapped I/O is low
        switch(port&0xff){
        case 0x84:case 0x85:
            if(ymz280b_read(s.ymz,port&1,s.io_cycle(),&value))s.faults|=2;
            return value;
        case 0x90:return (uint8_t)fifo7200_read(s.fifo[0],1);
        case 0x91:return fifo7200_ef(s.fifo[0])<<1;
        default:return value;
        }
    }
    static void io_write(void *opaque,uint16_t port,uint8_t value){
        auto &s=*static_cast<CbajSound *>(opaque);
        switch(port&0xff){
        case 0x84:case 0x85:
            s.ymz_writes++;
            if(ymz280b_write(s.ymz,port&1,value,s.io_cycle()))s.faults|=2;
            break;
        case 0x90:fifo7200_write(s.fifo[1],value);break;
        default:break;
        }
    }
    static void sample(void *opaque,float left,float right,uint64_t cycle){
        auto &s=*static_cast<CbajSound *>(opaque);
        if(s.config.sample)s.config.sample(s.config.opaque,left,right,cycle);
    }
};
extern "C" {
int cbaj_sound_create(const CbajSoundConfig *config,CbajSound **out){
    if(!out || !config || !config->program || config->program_size!=0x40000 ||
       !config->samples || config->sample_size!=0x800000)return -1;
    *out=nullptr;
    try{
        auto s=std::make_unique<CbajSound>();s->config=*config;
        Fifo7200Config fc{1024,nullptr,nullptr};
        if(fifo7200_create(&fc,&s->fifo[0]) || fifo7200_create(&fc,&s->fifo[1]))return -1;
        YMZ280BConfig yc{16934400,config->samples,config->sample_size,s.get(),CbajSound::sample,nullptr};
        if(ymz280b_create(&yc,&s->ymz))return -1;
        Z80Config zc{};zc.opaque=s.get();zc.memory_read=CbajSound::read;zc.memory_write=CbajSound::write;
        zc.io_read=CbajSound::io_read;zc.io_write=CbajSound::io_write;
        s->cpu=z80_create(&zc);if(!s->cpu)return -1;
        *out=s.release();return 0;
    }catch(const std::bad_alloc &){return -1;}
}
void cbaj_sound_destroy(CbajSound *s){if(s && !s->busy)delete s;}
int cbaj_sound_sync(CbajSound *s,uint64_t target){
    if(!s || s->busy || target<s->cycles)return -1;
    s->busy=true;
    while(target>s->cycles && !s->faults){
        // Source maximum quantum is 1/6000s; at 4MHz use <=666 T states.
        unsigned budget=(unsigned)std::min<uint64_t>(target-s->cycles,666);
        z80_run_cycles(s->cpu,budget);s->cycles+=budget;
        if(ymz280b_sync_to_cycle(s->ymz,s->oscillator_cycle(s->cycles)))s->faults|=2;
        Z80Status st{};z80_get_status(s->cpu,&st);if(st.unsupported_operations)s->faults|=4;
    }
    s->busy=false;return s->faults?-1:0;
}
int cbaj_sound_reset(CbajSound *s){
    if(!s || s->busy)return -1;
    s->busy=true;int result=ymz280b_reset(s->ymz,s->oscillator_cycle(s->cycles));
    z80_reset(s->cpu);Z80Status st{};z80_get_status(s->cpu,&st);s->cpu_origin=st.cycles;s->time_origin=s->cycles;
    fifo7200_reset(s->fifo[0]);fifo7200_reset(s->fifo[1]);s->busy=false;
    // Source RAM persists, while CPU/chip and FIFO device reset clear control state.
    return result;
}
int cbaj_sound_send(CbajSound *s,uint8_t data){if(!s || s->busy)return -1;fifo7200_write(s->fifo[0],data);return 0;}
int cbaj_sound_receive(CbajSound *s,uint8_t *data){if(!s || s->busy || !data)return -1;*data=(uint8_t)fifo7200_read(s->fifo[1],1);return 0;}
int cbaj_sound_main_status(CbajSound *s,uint8_t *data){if(!s || s->busy || !data)return -1;*data=fifo7200_ef(s->fifo[1])<<1;return 0;}
void cbaj_sound_status(const CbajSound *s,CbajSoundStatus *st){
    if(!s || !st)return;*st={};st->cpu_cycles=s->cycles;st->ymz_writes=s->ymz_writes;st->faults=s->faults;
    z80_get_status(s->cpu,&st->z80);ymz280b_get_status(s->ymz,&st->ymz);
    fifo7200_get_status(s->fifo[0],&st->fifo[0]);fifo7200_get_status(s->fifo[1],&st->fifo[1]);
}
}
