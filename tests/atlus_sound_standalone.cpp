/* GPL-2.0-or-later. Execute Atlus source sound bus and the two IRQ level routes. */
#include "atlus_sound.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
struct Sink { AtlusSound *sound=nullptr;std::vector<std::array<float,2>> samples;bool tried=false; };
static void sample(void *opaque,float l,float r,uint64_t cycle) {
    auto &s=*static_cast<Sink *>(opaque);assert(cycle%192==0);s.samples.push_back({l,r});
    if(s.sound && !s.tried){s.tried=true;assert(atlus_sound_sync(s.sound,1)==-1);assert(atlus_sound_command(s.sound,1)==-1);assert(atlus_sound_reset(s.sound)==-1);}
}
int main() {
    std::vector<uint8_t> program(0x40000),rom(0x800000,0x40);size_t pos=0;
    auto word=[&](unsigned v){program[pos++]=v>>8;program[pos++]=v;};
    auto lng=[&](unsigned v){word(v>>16);word(v);};
    lng(0x710000);lng(0x100);pos=26*4;lng(0x300);pos=27*4;lng(0x320);pos=0x100;
    auto byte=[&](unsigned address,unsigned value){word(0x13fc);word(value);lng(address);};
    auto reg=[&](unsigned address,unsigned value){byte(0x200001,address);byte(0x200003,value);};
    word(0x33fc);word(0x1234);lng(0x700000);byte(0x700000,0xbe);byte(0x700001,0xef);
    word(0x3039);lng(0x700000);
    byte(0x200000,0xff); // ignored high YMF/YMZ bus lane
    reg(0xff,0x90);reg(0xfe,1);reg(0,255);reg(2,255);reg(3,8);reg(0x63,32);reg(1,0xc1);
    word(0x4e72);word(0x2000);word(0x60fa);
    pos=0x300;word(0x3239);lng(0x200002);word(0x4e73); // IRQ2 status read releases chip IRQ
    pos=0x320;word(0x33fc);word(0xbeef);lng(0x100000); // source sound_unk write does not release IRQ3
    word(0x3039);lng(0x100000);word(0x4e73); // latch read, not IACK, releases IRQ3
    const auto original=program;Sink a,b;AtlusSound *sa=nullptr,*sb=nullptr;
    AtlusSoundConfig cfg{program.data(),rom.data(),program.size(),rom.size(),&a,sample};
    assert(atlus_sound_create(&cfg,&sa)==0);a.sound=sa;cfg.opaque=&b;
    assert(atlus_sound_create(&cfg,&sb)==0);b.sound=sb;
    assert(atlus_sound_sync(sa,5000)==0);
    for(unsigned i=1;i<=5000;i++)assert(atlus_sound_sync(sb,i)==0);
    AtlusSoundStatus st{};atlus_sound_status(sa,&st);
    assert(st.d0==0xbeef && !st.faults && st.ymz_writes==14 && st.irq2_ack==1 && !st.irq2);
    assert(st.ymz.frames==(5000ull*16934400/10000000)/192 && st.ymz.voice_ends==1 && !st.ymz.diagnostics);
    assert(a.samples==b.samples);
    bool nonzero=false;for(auto frame:a.samples)nonzero|=frame[0]!=0;assert(nonzero);
    for(auto sound:{sa,sb}) {
        assert(atlus_sound_command(sound,0x1234)==0);atlus_sound_status(sound,&st);
        assert(st.latch==0x1234 && st.pending);
        assert(atlus_sound_sync(sound,6000)==0);atlus_sound_status(sound,&st);
        assert(st.d0==0x1234 && st.commands==1 && st.latch_reads==1 && st.irq3_ack==1 && !st.pending);
        assert(atlus_sound_sync(sound,5999)==-1);
        assert(atlus_sound_reset(sound)==0);assert(atlus_sound_sync(sound,11000)==0);
        atlus_sound_status(sound,&st);assert(st.d0==0xbeef && st.cpu_cycles==11000 && st.latch==0x1234 && !st.pending && !st.faults);
    }
    assert(program==original && a.samples==b.samples);
    atlus_sound_destroy(sa);atlus_sound_destroy(sb);
    std::puts("Atlus MC68000 source RAM/YMZ lanes, PCM/end IRQ2, 16-bit latch IRQ3/read ack, source no-op write, slicing/reset and callback guards PASS");
}
