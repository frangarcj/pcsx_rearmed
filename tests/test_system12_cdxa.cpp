#include "system12_cdxa.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
namespace {
uint32_t read(S12Cdxa *d, unsigned reg) {
    uint32_t value; assert(s12_cdxa_read(d, 0x1f7e0000 + reg * 2, 2, &value)); return value;
}
void write(S12Cdxa *d, unsigned reg, uint16_t value) {
    assert(s12_cdxa_write(d, 0x1f7e0000 + reg * 2, 2, value));
}
void packet(S12Cdxa *d, const std::array<uint8_t,12> &bytes) {
    write(d, 7, 0xa0); assert(read(d, 7) & 8);
    for (unsigned i=0;i<12;i+=2) write(d,0,bytes[i]|(bytes[i+1]<<8));
}
std::vector<uint8_t> drain(S12Cdxa *d, unsigned size) {
    std::vector<uint8_t> out;
    for(unsigned i=0;i<size;i+=2) { uint16_t value=read(d,0);out.push_back(value);out.push_back(value>>8); }
    assert(!(read(d,7)&8)); assert(read(d,4)==0 && read(d,5)==0);
    return out;
}
}
void test_cdxa_mode2(const char *path) {
    char error[256]{};
    assert(!s12_cdxa_create(path,"1111111111111111111111111111111111111111",nullptr,nullptr,error,sizeof(error)));
    auto *d=s12_cdxa_create(path,"0000000000000000000000000000000000000000",nullptr,nullptr,error,sizeof(error));
    assert(d);
    packet(d,{0xbe,0,0,0,0,0,0,0,1,0x10,0,0});
    assert(read(d,4)==0 && read(d,5)==8);
    auto form1=drain(d,2048);
    for(unsigned i=0;i<form1.size();i++) assert(form1[i]==uint8_t(i*7));
    /* Completion must clear the byte-count registers before the next packet,
     * otherwise a READ CD with subheaders is fragmented using the old count. */
    packet(d,{0xbe,0,0,0,0,1,0,0,1,0x50,0,0});
    assert((read(d,4)|(read(d,5)<<8))==2336);
    auto form2=drain(d,2336);
    assert(form2[2]==0x20 && form2[6]==0x20);
    for(unsigned i=0;i<2048;i++) assert(form2[i+8]==uint8_t(i*7+13));
    packet(d,{0x2b,0,0,0,0,1,0,0,0,0,0,0});
    assert(read(d,7)&0x10); /* firmware waits for Drive Seek Complete */
    packet(d,{0xff,0,0,0,0,0,0,0,0,0,0,0});
    assert(read(d,7)&1); /* unknown commands report error */
    std::vector<uint8_t> saved(s12_cdxa_state_size());
    s12_cdxa_get_state(d,saved.data()); s12_cdxa_reset(d,0); s12_cdxa_set_state(d,saved.data());
    assert(read(d,7)&1);
    s12_cdxa_destroy(d);
    puts("CDXA pinned media, READ CD Form1/Form2 fields, byte-count completion, seek, errors and state PASS");
}
