/* Source RTC register/timer/NVRAM vectors; not Twinkle game execution. */
#include "../libpcsxcore/arcade/rtc65271.h"
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

struct Rig {
    Rtc65271 *rtc=nullptr;
    std::vector<std::pair<uint64_t,int>> irq;
    bool reenter=false;
    static void callback(void *opaque,int state){
        auto &r=*static_cast<Rig *>(opaque);Rtc65271Status s{};rtc65271_get_status(r.rtc,&s);
        r.irq.emplace_back(s.now_ns,state);
        if(state && r.reenter){
            uint8_t untouched=0xaa;
            assert(rtc65271_read(r.rtc,0,1,&untouched)==RTC65271_BUSY && untouched==0xaa);
            assert(rtc65271_write(r.rtc,0,0,0)==RTC65271_BUSY);
            assert(rtc65271_advance_to(r.rtc,s.now_ns)==RTC65271_BUSY);
            assert(rtc65271_try_destroy(r.rtc)==RTC65271_BUSY);
        }
    }
    explicit Rig(Rtc65271Date date={2026,10,15,5,17,30,9},const uint8_t *nv=nullptr){
        Rtc65271Config c{date,nv,size_t(nv?RTC65271_NVRAM_BYTES:0),callback,this};
        assert(!rtc65271_create(&c,&rtc));
    }
    ~Rig(){assert(!rtc65271_try_destroy(rtc));}
    uint8_t raw(int bank,unsigned offset){uint8_t value=0xee;assert(!rtc65271_read(rtc,bank,offset,&value));return value;}
    void raw_write(int bank,unsigned offset,uint8_t value){assert(!rtc65271_write(rtc,bank,offset,value));}
    uint8_t read(unsigned reg){raw_write(0,0,reg);return raw(0,1);}
    void write(unsigned reg,uint8_t value){raw_write(0,0,reg);raw_write(0,1,value);}
    void to(uint64_t ns){assert(!rtc65271_advance_to(rtc,ns));}
    Rtc65271Status status(){Rtc65271Status s{};rtc65271_get_status(rtc,&s);return s;}
    std::array<uint8_t,RTC65271_NVRAM_BYTES> nvram(){std::array<uint8_t,RTC65271_NVRAM_BYTES> v{};assert(!rtc65271_nvram_export(rtc,v.data(),v.size()));return v;}
};
static std::array<uint8_t,RTC65271_NVRAM_BYTES> seed(unsigned mode=6){
    std::array<uint8_t,RTC65271_NVRAM_BYTES> v{};v[1]=0x20;v[2]=mode;return v;
}

static void registers_and_ram(){
    Rig r;assert(r.read(11)==4 && r.read(10)==0x20);
    assert(r.read(0)==9 && r.read(2)==30 && r.read(4)==0x85 && r.read(9)==26);
    assert(r.read(13)==0 && r.read(13)==0x80); // Source first VRT read sets it for the next read.
    r.write(13,0);assert(r.read(13)==0x80);
    r.write(0,0xff);assert(r.read(0)==0x7f);
    r.raw_write(0,0x3e,0xff);assert(r.raw(0,0)==63);r.raw_write(0,0x3f,0xa5);assert(r.read(63)==0xa5);
    for(unsigned p=0;p<128;p++){
        r.raw_write(1,0x20,p);
        for(unsigned i=0;i<32;i++)r.raw_write(1,i,p^i);
    }
    r.raw_write(1,63,255);assert(r.raw(1,32)==127);
    auto nv=r.nvram();
    for(unsigned p=0;p<128;p++)for(unsigned i=0;i<32;i++)assert(nv[56+p*32+i]==(p^i));
    for(unsigned reg=14;reg<64;reg++)r.write(reg,reg*3);
    nv=r.nvram();for(unsigned i=0;i<50;i++)assert(nv[6+i]==uint8_t((i+14)*3));
}

static void updates_and_calendar(){
    auto nv=seed();Rig r({2000,2,28,2,23,59,59},nv.data());r.write(11,0x16);
    r.to(999999999);assert(r.read(0)==59 && !(r.read(10)&0x80));
    r.to(1000000000);assert(r.read(10)&0x80);assert(r.read(0)==59);
    r.to(1001983999);assert(r.read(0)==59);r.to(1001984000);
    assert(r.read(0)==0 && r.read(2)==0 && r.read(4)==0);
    assert(r.read(6)==3 && r.read(7)==29 && r.read(8)==2 && r.read(9)==0);
    assert(r.status().irq==1 && r.read(12)==0xb0 && !r.status().irq && r.read(12)==0);
    assert(r.status().begin_updates==1 && r.status().end_updates==1);
    Rig cold;auto s=cold.status();cold.to(2001984000);assert(cold.read(0)==s.regs[0]);
    assert(cold.read(10)==0x20 && cold.status().begin_updates==2 && !cold.status().end_updates);
    auto bcd=seed(2);Rig year({2099,12,31,5,23,59,59},bcd.data());year.to(1001984000);
    assert(year.read(9)==0xa0 && year.read(8)==1 && year.read(7)==1); // Literal source BCD 99 -> A0.
    Rig binary_year({2099,12,31,5,23,59,59},nv.data());binary_year.to(1001984000);
    assert(binary_year.read(9)==0 && binary_year.read(8)==1 && binary_year.read(7)==1);
    // Preserve source's BCD leap-year test on raw year bits (0x12 & 3 != 0).
    Rig source_2012({2012,2,28,3,23,59,59},bcd.data());source_2012.to(1001984000);
    assert(source_2012.read(7)==1 && source_2012.read(8)==3);
    Rig source_2024({2024,2,28,4,23,59,59},bcd.data());source_2024.to(1001984000);
    assert(source_2024.read(7)==0x29 && source_2024.read(8)==2);
    Rig pm({2026,1,1,5,11,59,59});pm.write(10,0x20);pm.to(1001984000);
    assert(pm.read(4)==0x8c); // Literal source 12-hour transition, not host reformatted time.
}

static void set_abort_and_alarm(){
    auto nv=seed();Rig r({2000,1,1,7,0,0,0},nv.data());
    r.write(1,0xc0);r.write(3,0xff);r.write(5,0xc1);r.write(11,0x36);
    r.to(1000000000);assert(r.read(10)&0x80);
    r.write(11,0xb6);assert(r.read(11)==0xa6 && !(r.read(10)&0x80));
    r.to(1001984000);assert(r.read(0)==0 && r.read(12)==0);
    r.write(11,0x36);r.to(2001984000);assert(r.read(0)==1);
    assert(r.read(12)==0xb0 && !r.status().irq);
    r.write(10,0x60);r.to(4001984000);assert(r.read(0)==1 && r.read(10)==0x20);
}

static void square_phase_and_slices(){
    Rig r;r.write(11,0x44);r.write(10,0x23);
    r.to(61035);assert(!r.status().square_edges);r.to(61036);assert(r.status().square_edges==1);
    r.to(122070);assert(r.status().square_edges==1);r.to(122071);
    assert(r.status().square_edges==2 && r.status().irq==1 && r.read(12)==0xc0);
    r.write(10,0x23);r.to(183105);assert(r.status().square_edges==2);r.to(183106);assert(r.status().square_edges==3);
    r.write(10,0x20);assert(!r.status().square_state);auto n=r.status().square_edges;r.to(999999999);assert(r.status().square_edges==n);
    Rig coarse,fine;coarse.write(10,0x23);fine.write(10,0x23);coarse.to(2000000000);
    for(uint64_t t=0;t<2000000000;){t=std::min(uint64_t(2000000000),t+133337);fine.to(t);}
    auto a=coarse.status(),b=fine.status();assert(a.square_edges==32768 && a.square_edges==b.square_edges);
    assert(a.begin_updates==b.begin_updates && a.end_updates==b.end_updates && coarse.irq==fine.irq);
    assert(!std::memcmp(a.regs,b.regs,64));
    Rig phase;phase.to(10000);phase.write(10,0x23);phase.to(61036);assert(phase.status().square_edges==1);
    Rig pending;pending.write(10,0x23);pending.to(122071);assert(!pending.status().irq && pending.status().regs[12]==0x40);
    pending.write(11,0x44);assert(!pending.status().irq); // Source B write does not reevaluate a pending flag.
    pending.to(244141);assert(pending.status().irq && pending.read(12)==0xc0);
}

static void persistence_and_faults(){
    Rig r;r.write(10,0x2f);r.write(11,0xff);r.write(1,0xc0);r.write(3,15);r.write(5,0xc0);r.write(42,0xa5);
    r.raw_write(1,32,17);r.raw_write(1,6,0x5a);auto nv=r.nvram();assert(nv[1]==0x2f && nv[2]==0x87);
    Rig imported({2026,10,15,5,17,30,9},nv.data());auto saved=imported.nvram();
    assert(saved[1]==0x20 && saved[2]==0x87 && imported.read(13)==0x80 && imported.read(42)==0xa5);
    imported.raw_write(1,32,17);assert(imported.raw(1,6)==0x5a);
    auto s=imported.status();assert(s.register_index==42 && !s.square_edges);
    Rtc65271Config bad{};bad.date={2026,2,30,1,0,0,0};Rtc65271 *out=reinterpret_cast<Rtc65271 *>(1);
    assert(rtc65271_create(&bad,&out)==RTC65271_ARGUMENT && !out);
    bad.date={2026,1,1,5,0,0,0};bad.nvram=nv.data();bad.nvram_size=nv.size()-1;
    assert(rtc65271_create(&bad,&out)==RTC65271_ARGUMENT && !out);bad.nvram_size=nv.size();nv[0]=1;
    assert(rtc65271_create(&bad,&out)==RTC65271_ARGUMENT && !out);
    Rig rewind;rewind.to(1);assert(rtc65271_advance_to(rewind.rtc,0)==RTC65271_FAULT && rewind.status().faults==2);
    auto binary=seed();Rig invalid({2026,1,1,5,23,59,59},binary.data());invalid.write(8,0);
    assert(rtc65271_advance_to(invalid.rtc,1001984000)==RTC65271_FAULT && invalid.status().faults==4);
    auto bcd=seed(2);Rig invalid_bcd({2026,1,1,5,23,59,59},bcd.data());invalid_bcd.write(8,0x99);
    assert(rtc65271_advance_to(invalid_bcd.rtc,1001984000)==RTC65271_FAULT && invalid_bcd.status().faults==4);
    Rig callback;callback.reenter=true;callback.write(11,0x44);callback.write(10,0x23);
    assert(rtc65271_advance_to(callback.rtc,1000000)==RTC65271_FAULT && callback.status().faults==1);
}

static std::vector<uint8_t> read_file(const std::string &path){
    std::ifstream f(path,std::ios::binary);assert(f);return {(std::istreambuf_iterator<char>(f)),{}};
}
static void original_mame_trace(const std::string &root){
    auto initial=read_file(root+"/initial-regs.bin");assert(initial.size()==64 && initial[10]==0 && initial[11]==4);
    auto xram=read_file(root+"/initial-xram.bin");assert(xram.size()==4096);
    for(auto b:xram)assert(!b);
    unsigned long long ns;unsigned reg,page,sqw;
    auto initial_status=read_file(root+"/initial-status.csv");initial_status.push_back(0);
    assert(std::sscanf(reinterpret_cast<const char *>(initial_status.data()),"%llu,%u,%u,%u",&ns,&reg,&page,&sqw)==4);
    assert(ns==0 && reg==0 && page==0 && sqw==0);
    const int hour=(initial[4]&0x7f)+((initial[4]&0x80)?12:0);
    Rtc65271Date date{2000+initial[9],initial[8],initial[7],initial[6],hour,initial[2],initial[0]};
    Rig r(date);
    auto status=r.status();assert(!std::memcmp(status.regs,initial.data(),64));
    std::ifstream input(root+"/rtc-transactions.csv");assert(input);std::string line;
    assert(std::getline(input,line) && line=="ns,operation,xram,offset,value");
    unsigned reads=0,writes=0;
    while(std::getline(input,line)){
        char op;unsigned bank,offset,value;
        assert(std::sscanf(line.c_str(),"%llu,%c,%u,%u,%x",&ns,&op,&bank,&offset,&value)==5);
        r.to(ns);
        if(op=='w'){r.raw_write(bank,offset,value);writes++;}
        else {
            assert(op=='r');auto actual=r.raw(bank,offset);reads++;
            if(actual!=value)std::fprintf(stderr,"MAME RTC mismatch row=%u time=%llu bank=%u offset=%u actual=%02x expected=%02x\n",reads+writes,ns,bank,offset,actual,value);
            assert(actual==value);
        }
    }
    auto final_status=read_file(root+"/final-status.csv");final_status.push_back(0);
    assert(std::sscanf(reinterpret_cast<const char *>(final_status.data()),"%llu,%u,%u,%u",&ns,&reg,&page,&sqw)==4);
    r.to(ns);status=r.status();auto final_regs=read_file(root+"/final-regs.bin");
    assert(final_regs.size()==64 && !std::memcmp(status.regs,final_regs.data(),64));
    assert(status.register_index==reg && status.xram_page==page && status.square_state==sqw && !status.faults);
    auto final_xram=read_file(root+"/final-xram.bin");auto nv=r.nvram();
    assert(final_xram.size()==4096 && !std::memcmp(nv.data()+56,final_xram.data(),4096));
    auto mame_nv=read_file(root+"/trace-nv/bmiidx2/rtc");
    assert(mame_nv.size()==nv.size() && !std::memcmp(mame_nv.data(),nv.data(),nv.size()));
    Rig warm(date,nv.data());assert(warm.nvram()==nv);
    assert(!warm.status().register_index && !warm.status().xram_page && warm.read(13)==0x80);
    assert(reads>100 && writes>100);
    std::printf("Original IIDX2 MAME RTC trace: reads=%u writes=%u final_time_ns=%llu all responses/registers/XRAM/source NVRAM identical PASS\n",reads,writes,ns);
}

int main(int argc,char **argv){
    if(argc!=1 && !(argc==3 && !std::strcmp(argv[1],"--mame-trace")))return 2;
    registers_and_ram();updates_and_calendar();set_abort_and_alarm();square_phase_and_slices();persistence_and_faults();
    if(argc==3)original_mame_trace(argv[2]);
    std::puts("RTC65271 source registers/XRAM, UIP/calendar quirks, alarms/IRQ/SQW phase, NVRAM and bounded faults PASS");}
