/* GPL-2.0-or-later. Execute source sound bus/IRQ/PCM vectors on the real MC68000. */
#include "raizing_sound.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
struct Sink { RaizingSound *sound=nullptr;std::vector<std::array<float,4>> samples; bool tried=false; };
static void sample(void *opaque,const float out[4],uint64_t cycle) {
    auto &sink=*static_cast<Sink *>(opaque); assert(cycle%384==0);
    sink.samples.push_back({out[0],out[1],out[2],out[3]});
    if (sink.sound && !sink.tried) {
        sink.tried=true; assert(raizing_sound_sync(sink.sound,1)==-1);
        assert(raizing_sound_reset(sink.sound)==-1);
        assert(raizing_sound_command(sink.sound,1)==-1);
        assert(raizing_sound_irq(sink.sound)==-1);
    }
}
int main() {
    std::vector<uint8_t> program(0x100000),rom(0x400000,0x40);size_t pos=0;
    auto word=[&](unsigned v){program[pos++]=v>>8;program[pos++]=v;};
    auto lng=[&](unsigned v){word(v>>16);word(v);};
    lng(0x100000);lng(0x100);pos=26*4;lng(0x300);pos=0x100;
    auto byte=[&](unsigned address,unsigned value){word(0x13fc);word(value);lng(address);};
    auto reg=[&](unsigned port,unsigned address,unsigned value){byte(0x100001+port*2,address);byte(0x100003+port*2,value);};
    // RAM word, high byte, low byte: exercise the big-endian mask lanes.
    word(0x33fc);word(0x1234);lng(0x80000);byte(0x80000,0xbe);byte(0x80001,0xef);
    word(0x3039);lng(0x80000); // move.w RAM,D0 = beef
    // A high-lane YMF access is ignored by umask16(00ff).
    byte(0x100018,0xff);
    reg(12,0,3);reg(0,0x30,1);reg(0,0x50,31);reg(0,0xb0,7);
    reg(0,0xa0,8);reg(0,0x90,0);reg(0,0xd0,0x0f);reg(0,0xe0,0x0f);
    reg(8,0x30,0xff);reg(8,0x40,0xff);reg(8,0x90,0);reg(0,0,1);
    size_t stop=pos;word(0x4e72);word(0x2000);word(0x60fa); // STOP, then branch back after IRQ
    pos=0x300;word(0x1039);lng(0x180009);word(0x4e73); // IRQ2 reads latch into D0, RTE
    const auto original=program;
    Sink a,b;RaizingSound *sa=nullptr,*sb=nullptr;
    RaizingSoundConfig cfg{program.data(),rom.data(),program.size(),rom.size(),&a,sample};
    assert(raizing_sound_create(&cfg,&sa)==0);a.sound=sa;cfg.opaque=&b;
    assert(raizing_sound_create(&cfg,&sb)==0);b.sound=sb;
    assert(raizing_sound_sync(sa,5000)==0);
    for(unsigned i=1;i<=5000;i++) assert(raizing_sound_sync(sb,i)==0);
    RaizingSoundStatus st{};raizing_sound_status(sa,&st);
    assert(st.d0==0xbeef && st.pc>=stop && !st.faults && st.ymf_writes==24);
    assert(st.ymf.frames==(5000ull*16934400/12000000)/384);
    assert(a.samples==b.samples && a.samples.back()[0]==0.125f && a.samples.back()[1]==0);
    assert(!st.ymf.faults && !st.ymf.diagnostics);
    // A command does not itself trigger an interrupt. Reading the latch clears
    // pending, while the CPU's autovector acknowledgement releases HOLD_LINE.
    for(auto sound:{sa,sb}) {
        assert(raizing_sound_command(sound,0x42)==0);
        assert(raizing_sound_sync(sound,5100)==0);
        raizing_sound_status(sound,&st);assert(st.d0==0xbeef && st.pending && !st.irq2);
        assert(raizing_sound_irq(sound)==0);assert(raizing_sound_sync(sound,6000)==0);
        raizing_sound_status(sound,&st);
        assert((st.d0&255)==0x42 && !st.pending && !st.irq2 && st.irq_acknowledgements==1 && !st.faults);
        assert(raizing_sound_sync(sound,5999)==-1);
        assert(raizing_sound_reset(sound)==0);assert(raizing_sound_sync(sound,11000)==0);
        raizing_sound_status(sound,&st);assert(st.cpu_cycles==11000 && st.latch==0x42 && !st.pending && !st.faults);
    }
    assert(a.samples==b.samples && program==original);
    raizing_sound_destroy(sa);raizing_sound_destroy(sb);
    std::puts("Raizing MC68000 source map: RAM lanes, YMF low-byte lanes/PCM, latch, IRQ2 HOLD/autovector, clock slicing, callback guards and reset PASS");
}
