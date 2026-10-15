/* Native source serial/UART/JVC vectors; no original-game acceptance claim. */
#include "twinkle_serial.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
struct Probe {
    TwinkleSerial bus;uint64_t ns=0;
    void tick(uint64_t n){ns+=n;assert(!bus.advance_to(ns));}
    void write(unsigned a,uint8_t v){assert(!bus.write(a,v));}
    uint8_t read(unsigned a){uint8_t v=0;assert(!bus.read(a,&v));return v;}
    void uart(){write(0x2fb,0x8b);write(0x2f8,12);write(0x2f9,0);write(0x2fb,0x0b);write(0x2fa,7);}
    static uint8_t sum(const uint8_t *b,unsigned n){int v=0x80;for(unsigned i=0;i<n;i++)v-=b[i]&0x7f;return v&0x7f;}
    std::vector<uint8_t> packet(unsigned a,unsigned b,unsigned c,unsigned x=0,unsigned y=0,unsigned z=0,bool valid=true){
        std::array<uint8_t,11> cmd{0xff,0xff,0x21,uint8_t(a),uint8_t(b),uint8_t(c),uint8_t(x),uint8_t(y),uint8_t(z),0,0};cmd[10]=sum(cmd.data(),10);if(!valid)cmd[10]^=1;
        for(auto v:cmd)write(0x2f8,v);
        uint64_t start=ns,first=0;std::vector<uint8_t> reply;
        while(ns-start<1400000000){tick(10000);if(read(0x2fd)&1){if(!first)first=ns;reply.push_back(read(0x2f8));}}
        if(valid){if(reply.size()!=11){TwinkleSerial::Status s{};bus.status(s);std::fprintf(stderr,"reply=%zu TX=%llu RX=%llu commands=%llu replies=%llu LSR=%02x error=%s\n",reply.size(),(unsigned long long)s.uart_tx_updates,(unsigned long long)s.uart_rx_updates,(unsigned long long)s.dvd_commands,(unsigned long long)s.dvd_response_bytes,s.uart_lsr[1],bus.error());}assert(reply.size()==11);assert(reply[0]==0xfc && reply[1]==0xff && reply[2]==0x21);assert(reply[10]==sum(reply.data(),10));assert(first-start>100000000 && first-start<120000000);}
        return reply;
    }
};
static void vectors(){
    Probe p;assert(p.read(0x2fd)==0x60);assert(p.read(0x2fe)==0xf0);assert(p.read(0x3fe)==0);
    p.write(0x2ff,0xa5);assert(p.read(0x2ff)==0xa5);assert(p.read(0)==0);
    p.uart();assert(p.read(0x2fb)==0x0b);auto r=p.packet(0x3e,0x4e,0x20);assert(r[3]==3 && r[4]==0 && r[5]==0x20);
    r=p.packet(0x3e,0x40,0x70);assert(r[3]==3);r=p.packet(0x3e,0x40,0x70);assert(r[3]==5);
    r=p.packet(0x7c,0x4c,0);assert(std::string(r.begin()+4,r.begin()+10)=="DVD PL");
    r=p.packet(0x0c,0x50,0x20,0,2,0);assert(r[3]==3);TwinkleSerial::Status s{};p.bus.status(s);assert(s.chapter==2 && s.playback==1 && s.powered && s.dvd_commands==5 && s.dvd_response_bytes==55);
    r=p.packet(0x0c,0x43,0x6d);p.bus.status(s);assert(r[3]==3 && s.playback==2);
    assert(p.packet(0x3e,0x4e,0x20,0,0,0,false).empty());
    // Source loopback uses the same serial bit timers, not an immediate echo.
    Probe loop;loop.write(0x3fb,0x83);loop.write(0x3f8,12);loop.write(0x3f9,0);loop.write(0x3fb,3);loop.write(0x3fc,0x1f);
    assert(loop.read(0x3fe)==0xfb);assert(loop.read(0x3fe)==0xf0);loop.write(0x3f8,0x69);assert(!(loop.read(0x3fd)&1));loop.tick(2000000);assert(loop.read(0x3fd)&1);assert(loop.read(0x3f8)==0x69);
    // Receiver overrun retains the last byte; LSR IRQ precedes data IRQ.
    Probe over;over.write(0x3fb,0x83);over.write(0x3f8,12);over.write(0x3f9,0);over.write(0x3fb,3);over.write(0x3fc,0x10);over.write(0x3f9,5);
    over.write(0x3f8,0x12);over.tick(2000000);over.write(0x3f8,0x34);over.tick(2000000);
    assert(over.read(0x3fa)==6);assert(over.read(0x3fd)==0x63);assert(over.read(0x3fa)==4);assert(over.read(0x3f8)==0x34);
    // Preserve the pinned source's short FIFO timeout rather than repairing it.
    Probe fifo;fifo.write(0x3fb,0x83);fifo.write(0x3f8,12);fifo.write(0x3f9,0);fifo.write(0x3fb,3);fifo.write(0x3fc,0x10);fifo.write(0x3fa,0x41);fifo.write(0x3f9,1);
    fifo.write(0x3f8,0x5a);fifo.tick(2000000);assert(fifo.read(0x3fa)==0xcc);assert(fifo.read(0x3f8)==0x5a);assert(fifo.read(0x3fa)==0xc1);
    // Configuration lock/address changes use the original Super-I/O decoder.
    Probe config;config.write(0x3f0,0x55);config.write(0x3f0,0x55);config.write(0x3f0,2);config.write(0x3f1,0xde);config.write(0x3f0,0xaa);
    config.write(0x33f,0x42);assert(config.read(0x33f)==0x42); // COM1 relocated to COM3 by CR2.
    uint8_t value=0;assert(config.bus.read(0x3f4,&value));assert(std::string(config.bus.error()).find("floppy MSR")!=std::string::npos);
    Probe locked;locked.write(0x3f0,0x55);locked.write(0x3f0,0x55);locked.write(0x3f0,1);locked.write(0x3f1,0);locked.write(0x3f0,2);locked.write(0x3f1,0xde);locked.write(0x3f0,0xaa);
    locked.write(0x3ff,0x37);assert(locked.read(0x3ff)==0x37);assert(locked.read(0x33f)==0);
    Probe bounds;bounds.write(0x3f0,0x55);bounds.write(0x3f0,0x55);bounds.write(0x3f0,16);assert(bounds.bus.write(0x3f1,0));
    Probe rewind;rewind.tick(100);assert(rewind.bus.advance_to(99));
    std::puts("Twinkle UART/FIFO/loopback, source JVC checksums/power/name/chapter/100ms replies, configuration and explicit faults PASS");
}
static void source(const std::string &path){
    Probe p;std::ifstream f(path);assert(f);std::string line;std::getline(f,line);uint64_t rows=0,reads=0;
    while(std::getline(f,line)){
        std::istringstream row(line);std::string time,op,off,val;std::getline(row,time,',');std::getline(row,op,',');std::getline(row,off,',');std::getline(row,val,',');
        uint64_t ns=std::stoull(time);unsigned address=std::stoul(off,nullptr,16),value=std::stoul(val,nullptr,16);assert(ns>=p.ns);p.tick(ns-p.ns);
        if(op=="w")p.write(address,value);else {assert(op=="r");auto actual=p.read(address);if(actual!=value){std::fprintf(stderr,"UART trace mismatch row=%llu ns=%llu off=%03x expected=%02x actual=%02x\n",(unsigned long long)rows,(unsigned long long)ns,address,value,actual);assert(false);}reads++;}rows++;
    }
    std::string root=path.substr(0,path.find_last_of('/')+1);std::ifstream final(root+"final.csv");assert(final);unsigned checks=0;
    while(std::getline(final,line)){
        std::istringstream row(line);std::string channel,reg,value;std::getline(row,channel,',');std::getline(row,reg,',');
        if(channel=="ns"){uint64_t ns=std::stoull(reg);p.tick(ns-p.ns);continue;}
        std::getline(row,value,',');unsigned port=std::stoul(channel)-1,expected=std::stoul(value);assert(port<2);TwinkleSerial::Status s{};p.bus.status(s);
        unsigned actual=reg=="lcr"?s.uart_lcr[port]:reg=="lsr"?s.uart_lsr[port]:reg=="fcr"?s.uart_fcr[port]:s.uart_divisor[port];assert(actual==expected);checks++;
    }
    assert(rows>0 && reads>0 && checks==8);TwinkleSerial::Status s{};p.bus.status(s);
    std::printf("Twinkle actual MAME UART/JVC bus replay PASS rows=%llu reads=%llu final_registers=%u commands=%llu responses=%llu\n",(unsigned long long)rows,(unsigned long long)reads,checks,(unsigned long long)s.dvd_commands,(unsigned long long)s.dvd_response_bytes);
}
int main(int argc,char **argv){if(argc==3 && std::string(argv[1])=="--source-trace")source(argv[2]);else {assert(argc==1);vectors();}}
