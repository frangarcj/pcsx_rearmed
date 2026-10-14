/* SPDX-License-Identifier: BSD-3-Clause. Native source register/clock vectors. */
#include "ymf271.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
struct Sink { YMF271 *chip=nullptr;std::vector<std::array<float,4>> samples;std::vector<int> irq;int reentry=0; };
static void sample(void *opaque,const float out[4],uint64_t cycle) {
    auto &s=*static_cast<Sink *>(opaque);assert(cycle%384==0);
    s.samples.push_back({out[0],out[1],out[2],out[3]});
}
static void irq(void *opaque,int state,uint64_t cycle) {
    auto &s=*static_cast<Sink *>(opaque);s.irq.push_back(state);
    if(s.chip) s.reentry=ymf271_write(s.chip,0,0,cycle);
}
static void reg(YMF271 *c,unsigned port,unsigned address,unsigned value,uint64_t cycle=0) {
    assert(ymf271_write(c,port,address,cycle)==0);assert(ymf271_write(c,port+1,value,cycle)==0);
}
static YMF271 *make(Sink &s,const std::vector<uint8_t> &rom) {
    YMF271Config cfg{16934400,rom.data(),rom.size(),&s,sample,irq};YMF271 *c=nullptr;
    assert(ymf271_create(&cfg,&c)==0);s.chip=c;return c;
}
static void timer_vectors() {
    std::vector<uint8_t> rom(128);Sink s;auto c=make(s,rom);uint8_t value;
    reg(c,12,0x10,255);reg(c,12,0x11,3);reg(c,12,0x13,5);
    assert(ymf271_read(c,0,383,&value)==0 && value==0);
    assert(ymf271_read(c,0,384,&value)==0 && value==1 && s.irq.back()==1);
    assert(s.reentry==YMF271_REENTRANT);
    reg(c,12,0x13,0x15,384);assert(s.irq.back()==0);
    assert(ymf271_read(c,0,384,&value)==0 && value==0);
    // Pinned source only adjusts a timer on its enable rising edge.
    reg(c,12,0x13,0,384);assert(ymf271_read(c,0,768,&value)==0 && value==1);
    assert(ymf271_reset(c,768)==0);assert(ymf271_read(c,0,10000,&value)==0 && value==0);
    reg(c,12,0x12,255,10000);reg(c,12,0x13,10,10000);
    assert(ymf271_read(c,0,16143,&value)==0 && value==0);
    assert(ymf271_read(c,0,16144,&value)==0 && value==2 && s.irq.back()==1);
    assert(ymf271_write(c,0,0,16143)==YMF271_TIME);
    YMF271Status st{};ymf271_get_status(c,&st);assert(st.cycles==16144 && st.frames==16144/384);
    assert(st.timer_a_events==2 && st.timer_b_events==1);ymf271_destroy(c);
}
static void rom_latch_vectors() {
    std::vector<uint8_t> rom(128);for(unsigned i=0;i<rom.size();i++)rom[i]=i^0x55;
    Sink s;auto c=make(s,rom);uint8_t value;
    reg(c,12,0x14,0xfe);reg(c,12,0x15,0xff);reg(c,12,0x16,0xff);
    assert(ymf271_read(c,2,0,&value)==0 && value==0);
    assert(ymf271_read(c,2,0,&value)==0 && value==rom[127]);
    assert(ymf271_read(c,2,0,&value)==0 && value==rom[0]);
    ymf271_destroy(c);
}
static void pcm_setup(YMF271 *c,bool twelve) {
    reg(c,12,0,3); // four independent external PCM slots
    reg(c,0,0x30,1);reg(c,0,0x50,31);reg(c,0,0xb0,7);
    reg(c,0,0xa0,8);reg(c,0,0x90,0); // one decoded sample per output sample
    reg(c,0,0xd0,0x0f);reg(c,0,0xe0,0x0f);
    reg(c,8,0x30,0xff);reg(c,8,0x40,0xff);
    reg(c,8,0x90,twelve?4:0);reg(c,0,0,1);
}
static void pcm_vectors(bool twelve) {
    std::vector<uint8_t> rom(128,0x40);
    if(twelve)for(unsigned i=0;i<rom.size();i++)rom[i]=std::array<uint8_t,3>{0x12,0x34,0x56}[i%3];
    Sink a,b;auto ca=make(a,rom),cb=make(b,rom);pcm_setup(ca,twelve);pcm_setup(cb,twelve);
    assert(ymf271_sync_to_cycle(ca,20*384)==0);
    for(unsigned i=1;i<=20;i++)assert(ymf271_sync_to_cycle(cb,i*384)==0);
    if(a.samples!=b.samples || a.samples.size()!=20) {
      std::fprintf(stderr,"PCM twelve=%d counts=%zu/%zu last=%g/%g\n",twelve,a.samples.size(),b.samples.size(),a.samples.back()[0],b.samples.back()[0]);
    }
    assert(a.samples==b.samples && a.samples.size()==20);
    float expected=twelve?float(0x5640)/131072.0f:float(0x4000)/131072.0f;
    if(a.samples.back()[0]!=expected)std::fprintf(stderr,"PCM twelve=%d output=%g expected=%g\n",twelve,a.samples.back()[0],expected);
    assert(a.samples.back()[0]==expected && a.samples.back()[2]==expected);
    assert(a.samples.back()[1]==0 && a.samples.back()[3]==0);
    reg(ca,0,0,0,20*384);assert(ymf271_reset(ca,20*384)==0);
    assert(ymf271_sync_to_cycle(ca,21*384)==0 && a.samples.back()[0]==0);
    ymf271_destroy(ca);ymf271_destroy(cb);
}
static void fm_vectors() {
    std::vector<uint8_t> rom(128);Sink a,b;auto ca=make(a,rom),cb=make(b,rom);
    for(auto c:{ca,cb}) {
        for(unsigned bank=0;bank<4;bank++) {reg(c,bank*2,0x30,1);reg(c,bank*2,0x50,31);reg(c,bank*2,0xb0,0);}
        reg(c,0,0xc0,15);reg(c,0,0xa0,8);reg(c,0,0x90,0);reg(c,0,0xd0,0);reg(c,0,0xe0,0);reg(c,0,0,1);
    }
    assert(ymf271_sync_to_cycle(ca,1024*384)==0);
    for(unsigned i=1;i<=1024;i++)assert(ymf271_sync_to_cycle(cb,i*384)==0);
    assert(a.samples==b.samples);bool nonzero=false;
    for(const auto &frame:a.samples)for(float v:frame){assert(std::isfinite(v));nonzero|=v!=0;}
    assert(nonzero);YMF271Status st{};ymf271_get_status(ca,&st);assert(!st.faults && !st.diagnostics);
    ymf271_destroy(ca);ymf271_destroy(cb);
}
int main() {
    timer_vectors();rom_latch_vectors();pcm_vectors(false);pcm_vectors(true);fm_vectors();
    std::puts("YMF271 timers/IRQ/reentry, 23-bit mirrored ROM latch, 8/12-bit PCM scale, FM, batching and reset PASS");
}
