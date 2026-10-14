/* GPL-2.0-or-later. Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * sony/zn.cpp atlus_zn_state (BSD-3-Clause, smf/R. Belmont).
 * MC68000 10MHz, 16-bit pending latch IRQ3, YMZ280B level IRQ2. */
#include "atlus_sound.h"
#include "m68000.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>
struct AtlusSound {
    AtlusSoundConfig config{};
    std::unique_ptr<M68000> cpu;
    YMZ280B *ymz=nullptr;
    std::array<uint8_t,0x10000> ram{};
    uint64_t commands=0,latch_reads=0,irq3_ack=0,irq2_ack=0,ymz_writes=0;
    uint32_t faults=0;
    uint16_t latch=0;
    bool pending=false,irq2=false,busy=false;
    ~AtlusSound() {ymz280b_destroy(ymz);}
    uint64_t chip_cycle() {
        uint64_t cycle=0;YMZ280BStatus st{};ymz280b_get_status(ymz,&st);
        if(!cpu->current_cycle(cycle) || cycle/ATLUS_SOUND_CLOCK>UINT64_MAX/YMZ280B_DEFAULT_CLOCK) {
            faults|=1;return st.cycles;
        }
        uint64_t whole=cycle/ATLUS_SOUND_CLOCK*YMZ280B_DEFAULT_CLOCK;
        uint64_t part=cycle%ATLUS_SOUND_CLOCK*YMZ280B_DEFAULT_CLOCK/ATLUS_SOUND_CLOCK;
        if(part>UINT64_MAX-whole){faults|=1;return st.cycles;}
        // Partial source instructions may issue accesses beyond their budget.
        return std::max(whole+part,st.cycles);
    }
    static void sample(void *opaque,float l,float r,uint64_t cycle) {
        auto &s=*static_cast<AtlusSound *>(opaque);
        if(s.config.sample)s.config.sample(s.config.opaque,l,r,cycle);
    }
    static void irq(void *opaque,int asserted,uint64_t) {
        auto &s=*static_cast<AtlusSound *>(opaque);s.irq2=asserted;
        if(s.cpu)s.cpu->set_irq(2,asserted);
    }
    static bool read(void *opaque,M68kSpace,uint32_t address,uint16_t mask,uint16_t *value) {
        auto &s=*static_cast<AtlusSound *>(opaque);*value=0xffff;
        if(address<0x40000) *value=(s.config.program[address]<<8)|s.config.program[address+1];
        else if(address==0x100000) {
            *value=s.latch;s.pending=false;s.latch_reads++;s.cpu->set_irq(3,false);
        } else if(address>=0x200000 && address<=0x200002 && (mask&0xff)) {
            uint8_t byte=0xff;
            if(ymz280b_read(s.ymz,(address-0x200000)/2,s.chip_cycle(),&byte))s.faults|=2;
            *value=0xff00|byte;
        } else if(address>=0x700000 && address<=0x70fffe) {
            size_t i=address-0x700000;*value=(s.ram[i]<<8)|s.ram[i+1];
        }
        return true;
    }
    static bool write(void *opaque,M68kSpace,uint32_t address,uint16_t data,uint16_t mask) {
        auto &s=*static_cast<AtlusSound *>(opaque);
        if(address>=0x700000 && address<=0x70fffe) {
            size_t i=address-0x700000;
            if(mask&0xff00)s.ram[i]=(s.ram[i]&~(mask>>8))|((data&mask)>>8);
            if(mask&0xff)s.ram[i+1]=(s.ram[i+1]&~mask)|(data&mask);
        } else if(address>=0x200000 && address<=0x200002 && (mask&0xff)) {
            s.ymz_writes++;
            if(ymz280b_write(s.ymz,(address-0x200000)/2,data&0xff,s.chip_cycle()))s.faults|=2;
        }
        // Source sound_unk_w(100000) only logs; it is not an IRQ acknowledge.
        return true;
    }
};
extern "C" {
int atlus_sound_create(const AtlusSoundConfig *config,AtlusSound **out) {
    if(!out || !config || !config->program || config->program_size!=0x40000 ||
       !config->samples || config->sample_size!=0x800000) return -1;
    *out=nullptr;
    try {
        auto s=std::make_unique<AtlusSound>();s->config=*config;
        YMZ280BConfig yc{YMZ280B_DEFAULT_CLOCK,config->samples,config->sample_size,s.get(),AtlusSound::sample,AtlusSound::irq};
        if(ymz280b_create(&yc,&s->ymz))return -1;
        M68000Bus bus{};bus.opaque=s.get();bus.read_word=AtlusSound::read;bus.write_word=AtlusSound::write;
        s->cpu=std::make_unique<M68000>(bus);
        s->cpu->set_irq_ack([owner=s.get()](unsigned level){
            if(level==3)owner->irq3_ack++;if(level==2)owner->irq2_ack++;
            // Source uses level lines. Only latch/status reads release them.
        });
        s->cpu->reset();*out=s.release();return 0;
    }catch(const std::bad_alloc &){return -1;}
}
void atlus_sound_destroy(AtlusSound *s){if(s && !s->busy)delete s;}
int atlus_sound_sync(AtlusSound *s,uint64_t target){
    if(!s || s->busy || target<s->cpu->total_cycles())return -1;
    s->busy=true;
    while(target>s->cpu->total_cycles() && !s->faults){
        YMZ280BStatus st{};ymz280b_get_status(s->ymz,&st);
        if(st.frames>=UINT64_MAX/192){s->faults|=1;break;}
        uint64_t next=(st.frames+1)*192;
        // Service the wired YMZ IRQ before the next CPU execution, at the
        // exact next sample's CPU-clock boundary, rounded upward once.
        uint64_t whole=next/YMZ280B_DEFAULT_CLOCK*ATLUS_SOUND_CLOCK;
        uint64_t part=(next%YMZ280B_DEFAULT_CLOCK*ATLUS_SOUND_CLOCK+YMZ280B_DEFAULT_CLOCK-1)/YMZ280B_DEFAULT_CLOCK;
        uint64_t deadline=whole+part;
        uint64_t budget=deadline>s->cpu->total_cycles()?deadline-s->cpu->total_cycles():1;
        unsigned n=(unsigned)std::min<uint64_t>({target-s->cpu->total_cycles(),budget,4096});
        if(s->cpu->run(n)<0 || ymz280b_sync_to_cycle(s->ymz,s->chip_cycle()) ||
           s->cpu->bus_error() || s->cpu->callback_fault())s->faults|=4;
    }
    s->busy=false;return s->faults?-1:0;
}
int atlus_sound_reset(AtlusSound *s){
    if(!s || s->busy)return -1;
    s->busy=true;int result=ymz280b_reset(s->ymz,s->chip_cycle());
    s->cpu->set_irq(3,false);s->cpu->set_irq(2,false);s->cpu->reset();s->pending=false;s->irq2=false;
    // Source latch reset clears pending while retaining value; RAM persists.
    s->busy=false;return result;
}
int atlus_sound_command(AtlusSound *s,uint16_t data){
    if(!s || s->busy)return -1;
    s->latch=data;s->pending=true;s->commands++;s->cpu->set_irq(3,true);return 0;
}
void atlus_sound_status(const AtlusSound *s,AtlusSoundStatus *st){
    if(!s || !st)return;*st={};st->cpu_cycles=s->cpu->total_cycles();st->pc=s->cpu->pc();st->d0=s->cpu->data_register(0);
    st->commands=s->commands;st->latch_reads=s->latch_reads;st->irq3_ack=s->irq3_ack;st->irq2_ack=s->irq2_ack;
    st->ymz_writes=s->ymz_writes;st->faults=s->faults;st->sr=s->cpu->sr();st->latch=s->latch;st->pending=s->pending;st->irq2=s->irq2;
    ymz280b_get_status(s->ymz,&st->ymz);
}
}
