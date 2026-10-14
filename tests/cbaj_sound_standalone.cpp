/* GPL-2.0-or-later. Execute the real Z80 through CBaj ROM/RAM/I/O and both FIFOs. */
#include "cbaj_sound.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
struct Sink { CbajSound *sound=nullptr;std::vector<std::array<float,2>> samples;bool tried=false; };
static void sample(void *opaque,float l,float r,uint64_t cycle){
    auto &s=*static_cast<Sink *>(opaque);assert(cycle%192==0);s.samples.push_back({l,r});
    if(s.sound && !s.tried){s.tried=true;uint8_t value;assert(cbaj_sound_sync(s.sound,1)==-1);assert(cbaj_sound_send(s.sound,1)==-1);assert(cbaj_sound_receive(s.sound,&value)==-1);assert(cbaj_sound_reset(s.sound)==-1);}
}
int main(){
    std::vector<uint8_t> program(0x40000),rom(0x800000,0x40);size_t pc=0;
    auto emit=[&](std::initializer_list<uint8_t> bytes){for(auto b:bytes)program[pc++]=b;};
    auto reg=[&](unsigned address,unsigned value){emit({0x3e,(uint8_t)address,0xd3,0x84,0x3e,(uint8_t)value,0xd3,0x85});};
    emit({0xf3,0x31,0xfe,0xff}); // DI, SP in source RAM
    reg(0xff,0x80);reg(0,0xff);reg(2,255);reg(3,8);reg(0x63,255);reg(1,0xc1);
    auto loop=pc;emit({0xdb,0x91,0xe6,2,0x28,0xfa,0xdb,0x90,0x32,0x00,0x80,0x3a,0x00,0x80,0xee,0x5a,0xd3,0x90,0xc3,(uint8_t)loop,(uint8_t)(loop>>8)});
    auto original=program;Sink a,b;CbajSound *sa=nullptr,*sb=nullptr;
    CbajSoundConfig cfg{program.data(),rom.data(),program.size(),rom.size(),&a,sample};
    assert(cbaj_sound_create(&cfg,&sa)==0);a.sound=sa;cfg.opaque=&b;assert(cbaj_sound_create(&cfg,&sb)==0);b.sound=sb;
    for(auto sound:{sa,sb})for(uint8_t v:{0x12,0x34,0xa5})assert(cbaj_sound_send(sound,v)==0);
    assert(cbaj_sound_sync(sa,10000)==0);
    for(unsigned t=1;t<=10000;t++)assert(cbaj_sound_sync(sb,t)==0);
    CbajSoundStatus st{};cbaj_sound_status(sa,&st);
    assert(st.cpu_cycles==10000 && !st.faults && st.ymz_writes==12 && !st.ymz.diagnostics);
    assert(st.fifo[0].reads==3 && st.fifo[0].used==0 && st.fifo[1].writes==3 && st.fifo[1].used==3);
    assert(st.z80.interrupts==0 && st.z80.nmis==0 && st.z80.unsupported_operations==0);
    assert(a.samples==b.samples && a.samples.size()==(10000ull*16934400/4000000)/192);
    assert(a.samples.back()[0]==float(0x4000*255/2)/float(32768*256));
    for(auto sound:{sa,sb}){
        uint8_t value;assert(cbaj_sound_main_status(sound,&value)==0 && value==2);
        for(uint8_t expected:{0x48,0x6e,0xff})assert(cbaj_sound_receive(sound,&value)==0 && value==expected);
        assert(cbaj_sound_main_status(sound,&value)==0 && value==0);
        assert(cbaj_sound_receive(sound,&value)==0 && value==0xff); // source underflow value, not a fabricated reply
        assert(cbaj_sound_sync(sound,9999)==-1);assert(cbaj_sound_send(sound,0x77)==0);
        assert(cbaj_sound_reset(sound)==0);cbaj_sound_status(sound,&st);
        assert(st.fifo[0].used==0 && st.fifo[1].used==0 && !st.ymz.playing && st.fifo[1].underflow==1);
        assert(cbaj_sound_sync(sound,20000)==0);assert(cbaj_sound_send(sound,0x81)==0);
        assert(cbaj_sound_sync(sound,21000)==0);assert(cbaj_sound_receive(sound,&value)==0 && value==0xdb);
    }
    assert(program==original && a.samples==b.samples);
    cbaj_sound_destroy(sa);cbaj_sound_destroy(sb);
    puts("CBaj real Z80 RAM/8-bit I/O, YMZ PCM, dual FIFO order/active-low status, no IRQ, slicing, debt/reset and callback guards PASS");
}
