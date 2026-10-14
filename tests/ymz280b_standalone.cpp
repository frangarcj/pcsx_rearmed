/* SPDX-License-Identifier: BSD-3-Clause. Source register, PCM/ADPCM and IRQ vectors. */
#include "ymz280b.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
struct Sink {
    YMZ280B *chip=nullptr;
    std::vector<std::array<float,2>> samples;
    std::vector<std::pair<int,uint64_t>> edges;
    int reentry=0;
};
static void sample(void *opaque,float l,float r,uint64_t cycle) {
    auto &s=*static_cast<Sink *>(opaque);assert(cycle%192==0);s.samples.push_back({l,r});
}
static void irq(void *opaque,int state,uint64_t cycle) {
    auto &s=*static_cast<Sink *>(opaque);s.edges.emplace_back(state,cycle);
    if(s.chip) s.reentry=ymz280b_write(s.chip,0,0,cycle);
}
static YMZ280B *make(Sink &s,const std::vector<uint8_t> &rom) {
    YMZ280BConfig cfg{16934400,rom.data(),rom.size(),&s,sample,irq};YMZ280B *c=nullptr;
    assert(ymz280b_create(&cfg,&c)==0);s.chip=c;return c;
}
static void reg(YMZ280B *c,unsigned address,unsigned value,uint64_t cycle=0) {
    assert(ymz280b_write(c,0,address,cycle)==0);assert(ymz280b_write(c,1,value,cycle)==0);
}
static void setup(YMZ280B *c,unsigned mode,unsigned pan,unsigned length=32,bool loop=false) {
    reg(c,0xff,0x90);reg(c,0xfe,1);reg(c,0,255);reg(c,2,255);reg(c,3,pan);
    reg(c,0x63,length);reg(c,0x62,length/2);
    reg(c,1,0x80|(mode<<5)|(mode==1?0:1)|(loop?0x10:0));
}
static void pcm(unsigned mode,unsigned pan) {
    std::vector<uint8_t> rom(128,0x40);
    if(mode==3)for(unsigned i=0;i<rom.size();i++)rom[i]=i%2?0x12:0x34;
    Sink a,b;auto ca=make(a,rom),cb=make(b,rom);setup(ca,mode,pan);setup(cb,mode,pan);
    assert(ymz280b_sync_to_cycle(ca,16*192)==0);
    for(unsigned i=1;i<=16*192;i++)assert(ymz280b_sync_to_cycle(cb,i)==0);
    assert(a.samples==b.samples && a.samples.size()==16);
    float expected=float((mode==2?0x4000:0x1234)*255/2)/float(32768*256);
    assert(a.samples.back()[0]==(pan==15?0:expected));
    assert(a.samples.back()[1]==(pan<=1?0:expected));
    assert(ymz280b_sync_to_cycle(ca,15*192)==YMZ280B_TIME);
    assert(ymz280b_reset(ca,16*192)==0);assert(ymz280b_sync_to_cycle(ca,17*192)==0);
    assert((a.samples.back()==std::array<float,2>{0,0}));
    ymz280b_destroy(ca);ymz280b_destroy(cb);
}
static void adpcm() {
    std::vector<uint8_t> rom(128,0x17);Sink s;auto c=make(s,rom);setup(c,1,8);
    assert(ymz280b_sync_to_cycle(c,3*192)==0);
    // First high nibble=1: step127*3/8=47. Fractional output uses source 512-step interpolation.
    float expected=float(47*255/2)/float(32768*256);
    assert(s.samples[0][0]==0 && s.samples[2][0]==expected && s.samples[2][1]==expected);
    ymz280b_destroy(c);
}
static void ends_and_loops() {
    std::vector<uint8_t> rom(128,0x40);Sink s;auto c=make(s,rom);uint8_t value;
    setup(c,2,8,2);
    assert(ymz280b_read(c,1,191,&value)==0 && value==0);
    assert(ymz280b_sync_to_cycle(c,192)==0 && s.edges.back()==std::make_pair(1,uint64_t(192)));
    assert(s.reentry==YMZ280B_REENTRANT);
    YMZ280BStatus st{};ymz280b_get_status(c,&st);assert(st.status==1 && st.irq==1 && !st.playing && st.voice_ends==1);
    assert(ymz280b_read(c,1,192,&value)==0 && value==1 && s.edges.back().first==0);
    assert(ymz280b_read(c,1,192,&value)==0 && value==0);
    assert(ymz280b_reset(c,192)==0);
    // Loop repeats without end status; masking/unmasking IRQ keeps status latched.
    reg(c,0xff,0x90,192);reg(c,0,255,192);reg(c,2,255,192);reg(c,3,8,192);
    reg(c,0x63,8,192);reg(c,0x62,2,192);reg(c,1,0xd1,192);
    assert(ymz280b_sync_to_cycle(c,100*192)==0);ymz280b_get_status(c,&st);
    assert(st.status==0 && st.playing==1 && st.frames==100);
    reg(c,0xff,0,100*192);assert(ymz280b_reset(c,100*192)==0);
    // All eight status bits and IRQ-mask gating.
    reg(c,0xff,0x80,100*192);reg(c,0xfe,0xff,100*192);
    for(unsigned v=0;v<8;v++){reg(c,v*4,255,100*192);reg(c,0x63+v*4,2,100*192);reg(c,v*4+1,0xc1,100*192);}
    assert(ymz280b_sync_to_cycle(c,101*192)==0);ymz280b_get_status(c,&st);assert(st.status==0xff && !st.irq);
    reg(c,0xff,0x90,101*192);ymz280b_get_status(c,&st);assert(st.irq);
    assert(ymz280b_read(c,1,101*192,&value)==0 && value==0xff);ymz280b_destroy(c);
}
static void rom_latch() {
    std::vector<uint8_t> rom(128);for(unsigned i=0;i<128;i++)rom[i]=i^0x55;auto original=rom;
    Sink s;auto c=make(s,rom);uint8_t value;
    assert(ymz280b_read(c,0,0,&value)==0 && value==0xff);
    reg(c,0xff,0x40);reg(c,0x84,0xff);reg(c,0x85,0xff);reg(c,0x86,0xfe);
    for(unsigned index:{126,126,127,0})assert(ymz280b_read(c,0,0,&value)==0 && value==rom[index]);
    reg(c,0x87,0);assert(rom==original);
    reg(c,0x80,1);YMZ280BStatus st{};ymz280b_get_status(c,&st);assert(st.diagnostics&YMZ280B_DIAG_DSP);
    ymz280b_destroy(c);
}
int main() {
    for(unsigned mode:{2,3})for(unsigned pan:{0,1,8,15})pcm(mode,pan);
    adpcm();ends_and_loops();rom_latch();
    std::puts("YMZ280B source PCM8/LE16 scale/pan, ADPCM high nibble, loop/end IRQ/mask/ack, ROM latch/mirror, cycle slicing/reset and callback guard PASS");
}
