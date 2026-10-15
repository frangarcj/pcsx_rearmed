/* BSD-3-Clause; pin vectors for the source-derived M24C02 model. */
#include "m24c02.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sstream>
struct Pins {
    std::array<uint8_t,256> data{};M24c02 chip{data};
    void set(int c,int d){chip.write_sda(d);chip.write_scl(c);}
    void start(){set(0,1);set(1,1);set(1,0);set(0,0);}
    void stop(){set(0,0);set(1,0);set(1,1);}
    int put(unsigned v){for(int i=7;i>=0;i--){int d=(v>>i)&1;set(0,d);set(1,d);set(0,d);}set(0,1);set(1,1);int ack=chip.read_sda();set(0,1);return ack;}
    unsigned get(bool ack){unsigned v=0;for(unsigned i=0;i<8;i++){set(1,1);v=(v<<1)|chip.read_sda();set(0,1);}set(0,!ack);set(1,!ack);set(0,!ack);return v;}
    void seek(unsigned a){start();assert(!put(0xa0));assert(!put(a));start();assert(!put(0xa1));}
};
int main(int argc,char **argv){
    Pins p;assert(p.chip.read_sda()==0);p.set(0,1);assert(p.chip.read_sda()==1);
    for(unsigned i=0;i<256;i++)p.data[i]=i^0x5a;
    p.seek(0xfe);assert(p.get(true)==(0xfe^0x5a));assert(p.get(true)==(0xff^0x5a));assert(p.get(false)==0x5a);p.stop();
    p.start();assert(p.put(0xa2)==1);p.stop(); // Wrong tied-low device address.
    auto original=p.data;p.start();assert(!p.put(0xa0));assert(!p.put(13));
    for(unsigned i=0;i<20;i++)assert(!p.put(0x80+i));
    assert(p.data==original);p.stop(); // Source commits the buffered page only at STOP.
    for(unsigned i=0;i<16;i++){unsigned expected=0x80+(i<4?i+16:i);assert(p.data[(13+i)&15]==expected);}
    for(unsigned i=16;i<256;i++)assert(p.data[i]==original[i]);
    p.chip.write_wc(1);original=p.data;p.start();assert(!p.put(0xa0));assert(!p.put(70));assert(p.put(0x37));p.stop();assert(p.data==original);p.chip.write_wc(0);
    auto saved=p.data;M24c02 cold(saved);assert(cold.read_sda()==0);assert(saved==p.data);
    if(argc==3 && !std::strcmp(argv[1],"--source-trace")){
        std::ifstream f(argv[2]);assert(f);Pins q;uint32_t output=0;uint64_t rows=0,reads=0;
        // Trace starts before source CPU execution, with original security image.
        for(unsigned i=0;i<256;i++)q.data[i]=0; // Replaced by the trace's initial image below.
        std::string root=argv[2];root=root.substr(0,root.find_last_of('/')+1);
        std::ifstream image(root+"initial.bin",std::ios::binary);image.read(reinterpret_cast<char*>(q.data.data()),256);assert(image.gcount()==256);
        std::string line;std::getline(f,line);
        while(std::getline(f,line)){
            std::istringstream row(line);std::string op,d,m;std::getline(row,op,',');std::getline(row,d,',');std::getline(row,m,',');
            unsigned value=std::stoul(d,nullptr,16),mask=std::stoul(m,nullptr,16);
            if(op=="w") {output=(output&~mask)|(value&mask);q.set((output>>4)&1,(output>>3)&1);}
            else {assert(op=="r");assert((((q.chip.read_sda()<<12)^value)&mask)==0);reads++;}
            rows++;
        }
        std::array<uint8_t,256> final{};std::ifstream finish(root+"final.bin",std::ios::binary);finish.read(reinterpret_cast<char*>(final.data()),256);assert(finish.gcount()==256 && final==q.data);
        assert(reads>0);std::printf("M24C02 actual MAME transactions replay PASS rows=%llu reads=%llu\n",(unsigned long long)rows,(unsigned long long)reads);
    }else assert(argc==1);
    std::puts("M24C02 random/sequential reads, wrap, wrong device, page STOP commit and write protection PASS");
}
