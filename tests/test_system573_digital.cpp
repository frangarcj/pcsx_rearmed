// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/system573_digital.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static const uint8_t fixture_id[8] = {0x9b,0x22,0x76,0x8a,0xfe,0x45,0x11,1};
struct Rig {
    System573Digital *chip;
    std::vector<unsigned> outputs;
    bool reenter = false;
    explicit Rig(int solo=0,const uint8_t *id=fixture_id) : chip(system573_digital_create(id,8,solo,output,this)) { assert(chip); }
    ~Rig() { system573_digital_destroy(chip); }
    static void output(void *opaque,unsigned line,int level) {
        auto &r = *static_cast<Rig *>(opaque); r.outputs.push_back(line*2+unsigned(level));
        if (r.reenter) {
            assert(system573_digital_write16(r.chip,0x90,5)==-1);
            system573_digital_destroy(r.chip);
        }
    }
    System573DigitalStatus status() const { System573DigitalStatus s{};system573_digital_get_status(chip,&s);return s; }
    void w(unsigned off,uint16_t value) { assert(system573_digital_write16(chip,off,value)==1); }
    uint16_t r(unsigned off) { uint16_t v=0;assert(system573_digital_read16(chip,off,&v)==1);return v; }
    void at(uint64_t ns) { assert(system573_digital_advance_to(chip,ns)==0); }
    void us(unsigned delta) { at(status().now_ns+uint64_t(delta)*1000); }
    void address(unsigned off,uint32_t addr) { w(off,uint16_t(addr>>16));w(off+2,uint16_t(addr)); }
    void load(const std::vector<uint16_t> &data,uint32_t base) { address(0xb0,base);for(auto v:data)w(0xb4,v); }
    void configure(uint32_t start,uint32_t end,uint16_t k1,uint16_t k2,uint8_t k3) {
        address(0xa0,start);address(0xa4,end);w(0xa8,k1);w(0xea,k2);w(0xec,k3);
    }
    void render(float *pcm,unsigned frames) { assert(system573_digital_render(chip,pcm,frames)==0); }
    void pins(int scl,int sda) { w(0xac,(scl?0x2000:0)|(sda?0x1000:0)); }
    void start() { pins(0,1);pins(1,1);pins(1,0);pins(0,0); }
    void stop() { pins(0,0);pins(1,0);pins(1,1); }
    void send(uint8_t v) {
        for(int bit=7;bit>=0;bit--) { pins(0,(v>>bit)&1);pins(1,(v>>bit)&1); }
        pins(0,1);pins(1,1);pins(0,1);
    }
    void mute(bool enabled) { start();send(0x3a);send(0x68);send(0x9a);send(enabled?0xa1:0xa0);send(0);send(0);stop(); }
    void serial_readback(const uint8_t *id) {
        assert(r(0xee)==0x1000);
        w(0xee,0x1000);us(480);w(0xee,0);us(29);assert(r(0xee)==0x1000);
        us(1);assert(r(0xee)==0);us(119);assert(r(0xee)==0);us(1);assert(r(0xee)==0x1000);
        for(unsigned b=0;b<8;b++) { bool high=(0x33>>b)&1;w(0xee,0x1000);us(high?6:60);w(0xee,0);us(high?64:10); }
        for(unsigned b=0;b<8;b++) {
            unsigned value=0;
            for(unsigned bit=0;bit<8;bit++) { w(0xee,0x1000);us(6);w(0xee,0);us(9);value|=(r(0xee)>>12)<<bit;us(55); }
            assert(value==id[7-b]);
        }
    }
};
static void registers_and_ram() {
    assert(!system573_digital_create(nullptr,8,0,nullptr,nullptr));
    assert(!system573_digital_create(fixture_id,7,0,nullptr,nullptr));
    assert(!system573_digital_create(fixture_id,8,2,nullptr,nullptr));
    Rig d;
    assert(d.r(0x00)==0 && d.r(0x02)==1 && d.r(0x04)==0 && d.r(0x06)==0);
    assert(d.r(0x0a)==0 && d.r(0x80)==0x1234 && d.r(0xf6)==0xb000);
    assert(d.r(0xac)==0x3000 && d.r(0xaa)==0 && d.r(0xae)==0);
    uint16_t v=0x5555;assert(system573_digital_read16(d.chip,0x12,&v)==0 && v==0x5555);
    assert(system573_digital_write16(d.chip,0x92,1)==0);
    assert(system573_digital_read16(d.chip,0x101,&v)==-1 && system573_digital_write16(d.chip,1,0)==-1);
    assert(system573_digital_read16(d.chip,0,nullptr)==-1);
    d.load({0x11aa,0x22bb},0x1fffffe);assert(d.status().ram_write_address==0x2000002);
    d.address(0xb6,0x1fffffe);assert(d.r(0xb4)==0x11aa && d.r(0xb4)==0x22bb);
    d.w(0x90,0x25);assert(d.status().network_id==0x25);
    d.w(0xc0,0xbeef);assert(d.r(0xc0)==0 && d.r(0xc2)==0 && d.r(0xc4)==0);
    d.w(0xe0,0x2000);assert(d.outputs==std::vector<unsigned>({15})); // Bank 1 physical bit1 -> logical line7.
    d.w(0xe0,0x2000);assert(d.outputs.size()==1);
    d.w(0xe0,0x4000);assert(d.outputs==std::vector<unsigned>({15,11,14}));
    d.w(0xf8,0xffff);assert(d.r(0xf6)==0xb000); // Source assumes fixed uploaded firmware.
    d.serial_readback(fixture_id);
    const auto time=d.status().now_ns;system573_digital_reset(d.chip);
    assert(d.status().now_ns==time && !d.status().ram_read_address && !d.status().network_id);
    assert(d.r(0xb4)==0x22bb); // Reset preserves shared RAM backing.
    assert(!d.status().faults);
}
static void gating_and_boundaries() {
    Rig d;d.load({0xabcd,0x1234},0);d.configure(0,2,0,0,0);
    float pcm[4]={1,1,1,1};d.w(0xae,0x4000);d.render(pcm,1);
    assert(pcm[0]==0 && d.status().bytes_sent==0 && d.r(0xae)==0x1000); // Streaming flag alone is not MP3 enable.
    d.w(0xae,0xe000);d.render(pcm,1);
    auto s=d.status();assert(s.words_decrypted==1 && s.bytes_sent==1 && s.current==2 && s.pending_bytes==1);
    assert(s.decoder.buffered_bytes==1 && !s.frame_counter && !s.decoder.decoded_frames && d.r(0xae)==0);
    d.render(pcm,2);assert(d.status().bytes_sent==1 && pcm[0]==0 && pcm[3]==0);
    d.configure(0,4,0,0,0);assert(d.status().current==0 && !d.status().pending_bytes && !d.status().decoder.buffered_bytes);
    d.render(pcm,1);assert(d.status().bytes_sent==4); // Lifetime diagnostic: one earlier plus three new source bytes.
    assert(d.status().current==4 && d.status().pending_bytes==1);
    d.w(0xae,0x8000);d.render(pcm,1);assert(d.status().bytes_sent==4);
    d.w(0xae,0);assert(d.r(0xa8)==0);
    d.configure(4,2,0,0,0);d.w(0xae,0xe000);d.render(pcm,1);assert(d.status().bytes_sent==4);
    assert(!d.status().faults && !d.status().decoder.decoded_frames);
    Rig out;out.configure(SYSTEM573_DIGITAL_RAM_BYTES,SYSTEM573_DIGITAL_RAM_BYTES+2,0,0,0);out.w(0xae,0xe000);
    assert(system573_digital_render(out.chip,pcm,1)==-1);
    assert(out.status().faults==SYSTEM573_DIGITAL_FAULT_RAM && out.status().bytes_sent==0 && !out.status().decoder.buffered_bytes);
}
static void clocks_and_reentry() {
    Rig d;d.at(1000000000);assert(d.r(0xcc)==0); // Common counter waits for an actual MPEG frame sync.
    assert(system573_digital_advance_to(d.chip,999999999)==-1);
    assert(d.status().now_ns==1000000000 && d.status().faults==SYSTEM573_DIGITAL_FAULT_TIME);
    Rig solo(1);solo.at(2000000000);assert(solo.r(0xca)==0);assert(solo.r(0xcc)==(88200&65535));assert(solo.r(0xca)==1);
    solo.w(0xec,0xff);assert(solo.status().crypto3==0);solo.w(0xcc,0);assert(solo.r(0xcc)==0);
    solo.at(2001000000);assert(solo.r(0xce)==44);assert(solo.r(0xce)==44);assert(solo.r(0xcc)==44);
    // Solo recomputes absolute elapsed time, so unlike the common variant
    // repeated difference reads retain the same value at the same timestamp.
    solo.at(100000000000000ull);
    const uint32_t wrapped=uint32_t(44100ull*99998);
    assert(solo.r(0xcc)==(wrapped&65535));assert(solo.r(0xca)==(wrapped>>16));
    Rig bad;bad.reenter=true;assert(system573_digital_write16(bad.chip,0xe2,0x1000)==-1);
    assert(bad.status().faults==SYSTEM573_DIGITAL_FAULT_REENTRY && !bad.status().network_id);
}
struct Cipher {
    uint16_t a=0xc3d2,b=0xab89;uint8_t c=0xe7;bool solo=false;
    static uint16_t permute(uint16_t value,const std::array<unsigned,16> &positions) {
        uint16_t result=0;for(auto bit:positions)result=uint16_t((result<<1)|((value>>bit)&1));return result;
    }
    uint16_t encode(uint16_t plain) {
        const uint16_t key=solo?a:permute(a^b,{{15,13,14,12,11,10,9,7,8,6,5,4,3,1,2,0}});
        uint16_t data=plain;
        if(!solo)data^=permute(c,{{7,0,6,1,5,2,4,3,3,4,2,5,1,6,0,7}});
        data^=key&0x5555;
        for(unsigned pair=0;pair<8;pair++)if(key&(2u<<(pair*2))) {
            const auto low=(data>>(pair*2))&1,high=(data>>(pair*2+1))&1;
            data=uint16_t((data&~(3u<<(pair*2)))|(low<<(pair*2+1))|(high<<(pair*2)));
        }
        if(solo)a=uint16_t((a<<1)|(a>>15));
        else { if(((a>>14)^(a>>15))&1)b=uint16_t((b<<1)|(b>>15));a=uint16_t((a&0x8000)|((a<<1)&0x7ffe)|((a>>14)&1));c++; }
        return data;
    }
};
static std::vector<uint16_t> encrypt(const std::vector<uint8_t> &plain,int solo) {
    Cipher cipher;cipher.solo=solo;
    std::vector<uint16_t> result;
    for(size_t i=0;i<plain.size()+2;i+=2) {
        const uint16_t word=uint16_t((i<plain.size()?plain[i]:0)*256+(i+1<plain.size()?plain[i+1]:0));
        result.push_back(cipher.encode(word));
    }
    return result;
}
static std::vector<float> playback(Rig &r,unsigned frames) {
    float initial[2];r.render(initial,1);assert(initial[0]==0 && initial[1]==0);
    std::vector<float> pcm(frames*2);
    uint64_t remainder=0;
    for(unsigned i=0;i<frames;i+=56) {
        const auto n=std::min(56u,frames-i);
        const auto rate=r.status().decoder.sample_rate;
        const uint64_t numerator=uint64_t(n)*1000000000+remainder;
        r.at(r.status().now_ns+numerator/rate);remainder=numerator%rate;
        r.render(pcm.data()+i*2,n);
    }
    return pcm;
}
static std::vector<float> direct_mas(const std::vector<uint8_t> &bytes) {
    bool demand=false;
    auto callback=[](void *p,int state){*static_cast<bool *>(p)=state!=0;};
    auto *mas=mas3507d_create(nullptr,callback,&demand);assert(mas);
    float initial[2];assert(mas3507d_render(mas,initial,1)==0);
    std::vector<float> pcm(88200*2);size_t cursor=0;
    for(unsigned i=0;i<88200;i+=56) {
        while(demand && cursor<bytes.size())assert(mas3507d_write_byte(mas,bytes[cursor++])==0);
        assert(mas3507d_render(mas,pcm.data()+i*2,std::min(56u,88200-i))==0);
    }
    assert(cursor==bytes.size());mas3507d_destroy(mas);return pcm;
}
static void mpeg_fixture(const char *path) {
    FILE *f=std::fopen(path,"rb");assert(f);std::vector<uint8_t> bytes;int value;
    while((value=std::fgetc(f))!=EOF)bytes.push_back(uint8_t(value));std::fclose(f);
    assert(!bytes.empty() && bytes.size()<1000000);
    std::vector<float> reference;
    for(int solo=0;solo<=1;solo++) {
        const uint16_t key2=solo?0:0xab89;const uint8_t key3=solo?0:0xe7;
        auto encoded=encrypt(bytes,solo);Rig r(solo);r.load(encoded,0x1000);
        r.configure(0x1000,0x1000+uint32_t(encoded.size()*2),0xc3d2,key2,key3);r.w(0xae,0xe000);
        auto pcm=playback(r,88200);auto s=r.status();
        assert(!s.faults && s.decoder.decoded_frames>3 && s.frame_counter==s.decoder.decoded_frames);
        assert(s.bytes_sent==encoded.size()*2-1 && s.words_decrypted==encoded.size() && s.pending_bytes==1);
        auto delivered=bytes;delivered.resize(encoded.size()*2-1,0);
        uint64_t hash=0xcbf29ce484222325ull;
        for(auto v:delivered)hash=(hash^v)*0x100000001b3ull;
        assert(s.stream_hash==hash); // Exact plaintext bytes including source end/range behaviour.
        assert(pcm==direct_mas(delivered));
        assert(std::any_of(pcm.begin(),pcm.end(),[](float v){return v!=0;}));
        if(!solo) {
            const auto low=r.r(0xcc),high=r.r(0xca);
            r.at(r.status().now_ns+1000000);assert(r.r(0xce)==44);assert(r.r(0xce)==0);
            assert(r.r(0xcc)==low && r.r(0xca)==high);
        }
        if(solo)assert(pcm==reference);else reference=pcm;
        // Reprogramming keys/range restarts decoder and cipher at the same data.
        r.configure(0x1000,0x1000+uint32_t(encoded.size()*2),0xc3d2,key2,key3);
        assert(playback(r,88200)==pcm);
        r.w(0xae,0x6000);r.configure(0x1000,0x1000+uint32_t(encoded.size()*2),0xc3d2,key2,key3);
        assert(playback(r,88200)==pcm && !r.status().frame_counter && r.status().decoder.decoded_frames==s.decoder.decoded_frames);
        r.w(0xae,0xe000);
        r.mute(true);r.configure(0x1000,0x1000+uint32_t(encoded.size()*2),0xc3d2,key2,key3);
        auto muted=playback(r,88200);assert(std::all_of(muted.begin(),muted.end(),[](float v){return v==0;}));
        assert(r.status().frame_counter==s.frame_counter);
        Rig wrong(solo);wrong.load(encoded,0x1000);
        wrong.configure(0x1000,0x1000+uint32_t(encoded.size()*2),0xc3d3,key2,key3);wrong.w(0xae,0xe000);
        auto rejected=playback(wrong,88200);
        assert(std::all_of(rejected.begin(),rejected.end(),[](float v){return v==0;}));
        assert(!wrong.status().frame_counter && !wrong.status().decoder.decoded_frames);
        assert(!(wrong.status().mpeg_status&0x4000));
        std::printf("Digital %s supplied MPEG frames=%u bytes=%llu FNV64=%016llx rate=%u restart/mute/PCM PASS\n",solo?"Solo":"common",s.frame_counter,(unsigned long long)s.bytes_sent,(unsigned long long)s.stream_hash,s.decoder.sample_rate);
    }
    f=std::fopen((std::string(path)+".digital.f32").c_str(),"wb");assert(f);
    assert(std::fwrite(reference.data(),sizeof(float),reference.size(),f)==reference.size());std::fclose(f);
}
int main(int argc,char **argv) {
    registers_and_ram();gating_and_boundaries();clocks_and_reentry();
    for(int i=1;i<argc;i++) {
        if(!std::strcmp(argv[i],"--serial")) {
            assert(++i<argc);uint8_t id[8];FILE *f=std::fopen(argv[i],"rb");assert(f);
            assert(std::fread(id,1,8,f)==8 && std::fgetc(f)==EOF);std::fclose(f);
            Rig r(0,id);r.serial_readback(id);std::puts("Digital supplied board serial through source ID register PASS");
        }else mpeg_fixture(argv[i]);
    }
    std::puts("System573 digital source registers/RAM/outputs/serial, FPGA gating/end byte, clocks and bounded faults PASS");
}
