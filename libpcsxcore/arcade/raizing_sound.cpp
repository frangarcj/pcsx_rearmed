/* GPL-2.0-or-later. Source map: MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * src/mame/sony/zn.cpp raizing_zn_state (BSD-3-Clause, smf/Philip Bennett).
 * MC68000 @12 MHz, YMF271 @16.9344 MHz, HOLD_LINE IRQ2. */
#include "raizing_sound.h"
#include "m68000.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>

struct RaizingSound {
    RaizingSoundConfig config{};
    std::unique_ptr<M68000> cpu;
    YMF271 *ymf=nullptr;
    std::array<uint8_t,0x80000> ram{};
    uint64_t commands=0, irq_requests=0, irq_acknowledgements=0, ymf_writes=0;
    uint32_t faults=0;
    uint8_t latch=0;
    bool pending=false, irq2=false, busy=false;
    ~RaizingSound() { ymf271_destroy(ymf); }

    uint64_t chip_cycle()
    {
        uint64_t cycle=0;
        YMF271Status status{};
        ymf271_get_status(ymf,&status);
        if (!cpu->current_cycle(cycle) || cycle/RAIZING_SOUND_CLOCK > UINT64_MAX/YMF271_DEFAULT_CLOCK) {
            faults|=1; return status.cycles;
        }
        const uint64_t whole=(cycle/RAIZING_SOUND_CLOCK)*YMF271_DEFAULT_CLOCK;
        const uint64_t part=(cycle%RAIZING_SOUND_CLOCK)*YMF271_DEFAULT_CLOCK/RAIZING_SOUND_CLOCK;
        if (part > UINT64_MAX-whole) { faults|=1; return status.cycles; }
        cycle=whole+part;
        /* A partial source instruction can issue a bus access beyond its short
         * budget. Later slices repay that debt, without rewinding the device. */
        return std::max(cycle,status.cycles);
    }
    static void sample(void *opaque,const float outputs[4],uint64_t cycle)
    {
        auto &s=*static_cast<RaizingSound *>(opaque);
        if (s.config.sample) s.config.sample(s.config.opaque,outputs,cycle);
    }
    static bool read(void *opaque,M68kSpace,uint32_t address,uint16_t mask,uint16_t *value)
    {
        auto &s=*static_cast<RaizingSound *>(opaque);
        *value=0xffff;
        if (address<0x80000) *value=(s.config.program[address]<<8)|s.config.program[address+1];
        else if (address<0x100000) {
            size_t i=address-0x80000; *value=(s.ram[i]<<8)|s.ram[i+1];
        } else if (address<=0x10001e && (mask&0xff)) {
            uint8_t byte=0xff;
            if (ymf271_read(s.ymf,(address-0x100000)/2,s.chip_cycle(),&byte)) s.faults|=2;
            *value=0xff00|byte;
        } else if (address==0x180008 && (mask&0xff)) {
            *value=0xff00|s.latch; s.pending=false;
        }
        /* Source unmapped reads are open bus, not a fabricated bus error. */
        return true;
    }
    static bool write(void *opaque,M68kSpace,uint32_t address,uint16_t data,uint16_t mask)
    {
        auto &s=*static_cast<RaizingSound *>(opaque);
        if (address>=0x80000 && address<0x100000) {
            size_t i=address-0x80000;
            if (mask&0xff00) s.ram[i]=(s.ram[i]&~(mask>>8))|((data&mask)>>8);
            if (mask&0x00ff) s.ram[i+1]=(s.ram[i+1]&~mask)|(data&mask);
        } else if (address>=0x100000 && address<=0x10001e && (mask&0xff)) {
            s.ymf_writes++;
            if (ymf271_write(s.ymf,(address-0x100000)/2,data&0xff,s.chip_cycle())) s.faults|=2;
        }
        /* Source ROM writes and 100020..ffffff writes are nopw. */
        return true;
    }
};
extern "C" {
int raizing_sound_create(const RaizingSoundConfig *config,RaizingSound **out)
{
    if (!out || !config || !config->program || config->program_size!=0x100000 ||
        !config->samples || config->sample_size!=0x400000) return -1;
    *out=nullptr;
    try {
        auto s=std::make_unique<RaizingSound>();s->config=*config;
        YMF271Config yc{YMF271_DEFAULT_CLOCK,config->samples,config->sample_size,s.get(),RaizingSound::sample,nullptr};
        if (ymf271_create(&yc,&s->ymf)) return -1;
        M68000Bus bus{};bus.opaque=s.get();bus.read_word=RaizingSound::read;bus.write_word=RaizingSound::write;
        s->cpu=std::make_unique<M68000>(bus);
        s->cpu->set_irq_ack([owner=s.get()](unsigned level) {
            if (level==2) {
                owner->irq_acknowledgements++;owner->irq2=false;owner->cpu->set_irq(2,false);
            }
        });
        s->cpu->reset();*out=s.release();return 0;
    } catch (const std::bad_alloc &) {return -1;}
}
void raizing_sound_destroy(RaizingSound *s) {if(s && !s->busy) delete s;}
int raizing_sound_sync(RaizingSound *s,uint64_t target)
{
    if (!s || s->busy || target<s->cpu->total_cycles()) return -1;
    s->busy=true;
    while (target>s->cpu->total_cycles() && !s->faults) {
        unsigned n=(unsigned)std::min<uint64_t>(target-s->cpu->total_cycles(),4096);
        if (s->cpu->run(n)<0 || ymf271_sync_to_cycle(s->ymf,s->chip_cycle()) ||
            s->cpu->bus_error() || s->cpu->callback_fault()) s->faults|=4;
    }
    s->busy=false;return s->faults ? -1 : 0;
}
int raizing_sound_reset(RaizingSound *s)
{
    if (!s || s->busy) return -1;
    s->busy=true;
    int result=ymf271_reset(s->ymf,s->chip_cycle());
    s->cpu->set_irq(2,false);s->cpu->reset();s->irq2=false;s->pending=false;
    /* Source reset clears pending, preserving the generic latch value and RAM. */
    s->busy=false;return result;
}
int raizing_sound_command(RaizingSound *s,uint8_t data)
{
    if (!s || s->busy) return -1;
    s->latch=data;s->pending=true;s->commands++;return 0;
}
int raizing_sound_irq(RaizingSound *s)
{
    if (!s || s->busy) return -1;
    s->irq2=true;s->irq_requests++;s->cpu->set_irq(2,true);return 0;
}
void raizing_sound_status(const RaizingSound *s,RaizingSoundStatus *status)
{
    if (!s || !status) return;
    *status={};status->cpu_cycles=s->cpu->total_cycles();status->commands=s->commands;
    status->irq_requests=s->irq_requests;status->irq_acknowledgements=s->irq_acknowledgements;
    status->ymf_writes=s->ymf_writes;status->pc=s->cpu->pc();status->sr=s->cpu->sr();
    status->d0=s->cpu->data_register(0);status->faults=s->faults;status->latch=s->latch;status->pending=s->pending;status->irq2=s->irq2;
    ymf271_get_status(s->ymf,&status->ymf);
}
}
