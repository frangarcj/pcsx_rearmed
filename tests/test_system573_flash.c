#include "../libpcsxcore/arcade/system573_flash.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(void){
    uint8_t *chips[8],rtc[8192]={0},response[64],factory[132]={0x19,0,0xaa,0x55};
    for(unsigned i=0;i<8;i++){chips[i]=malloc(0x200000);assert(chips[i]);memset(chips[i],0x10+i,0x200000);}
    for(unsigned i=0;i<64;i++)response[i]=i;
    struct tm t={.tm_year=126,.tm_mon=9,.tm_mday=14,.tm_wday=3};System573Flash b;
    assert(!system573_flash_init(&b,chips,rtc,response,factory,&t));
    uint32_t v;assert(system573_flash_read(&b,0x1f000000,4,&v)==1 && v==0x11101110);
    assert(system573_flash_write(&b,0x1f500000,1,3)==1);
    assert(system573_flash_read(&b,0x1f000000,2,&v)==1 && v==0x1716);
    assert(system573_flash_write(&b,0x1f500001,1,0xa5)==1 && b.control==0xa503);
    assert(system573_flash_read(&b,0x1f500000,4,&v)==1 && v==0xa503);
    assert(system573_flash_write(&b,0x1f500000,1,4)==1);
    assert(system573_flash_read(&b,0x1f3ffffc,4,&v)==1 && v==UINT32_MAX);
    system573_flash_write(&b,0x1f500000,2,0);
    assert(system573_flash_read(&b,0x1f400004,4,&v)==1 && v==0x1f091007);
    system573_flash_write(&b,0x1f400001,1,1);
    assert(b.h8_index==1 && b.h8_edges==1);
    system573_flash_write(&b,0x1f400001,1,1);assert(b.h8_edges==1);
    for(unsigned i=0;i<70;i++){system573_flash_write(&b,0x1f400001,1,0);system573_flash_write(&b,0x1f400001,1,1);}
    assert(b.h8_index==63);
    system573_flash_read(&b,0x1f400004,1,&v);assert((v&0xf0)==0xf0);
    system573_flash_write(&b,0x1f620000,4,0xaabbccdd);
    assert(rtc[0]==0xdd && rtc[1]==0xbb);
    system573_flash_read(&b,0x1f620000,4,&v);assert(v==0x00bb00dd);
    system573_flash_read(&b,0x1f620001,1,&v);assert(v==0);
    system573_flash_read(&b,0x1f480000,4,&v);assert(v==UINT32_MAX);
    system573_flash_read(&b,0x1f4c000c,4,&v);assert(v==UINT32_MAX);
    /* Readback merges, but source output callbacks see only accessed lines. */
    system573_flash_write(&b,0x1f6a0000,1,0x40); // red
    system573_flash_write(&b,0x1f6a0000,1,0x48); // P1 rising latch, existing red
    assert(b.lamps[0]==4);
    system573_flash_write(&b,0x1f6a0001,1,0x55);assert(b.security==0x5548);
    /* Real empty JVS chain checksum/reset: no manufactured board reply. */
    system573_flash_write(&b,0x1f680000,2,0xffe0);
    system573_flash_write(&b,0x1f680000,2,0xf002);
    system573_flash_write(&b,0x1f680000,2,0x00f1);
    assert(b.jvs_packets==1 && b.jvs_w==0);
    system573_flash_read(&b,0x1f400008,4,&v);assert(v==0xffff);
    system573_flash_read(&b,0x1f680000,2,&v);assert(v==0xff00);
    uint8_t *view=malloc(0x400000);assert(view);system573_flash_fetch_view(&b,view);
    assert(view[0]==0x10 && view[1]==0x11 && view[0x3fffff]==0x11);free(view);
    /* Fujitsu command/ID lanes go through the actual borrowed flash engine. */
    system573_flash_write(&b,0x1f000aaa,1,0xaa);
    system573_flash_write(&b,0x1f000554,1,0x55);
    system573_flash_write(&b,0x1f000aaa,1,0x90);
    system573_flash_read(&b,0x1f000000,2,&v);assert(v==0x1104);
    system573_flash_read(&b,0x1f000002,2,&v);assert(v==0x11ad);
    system573_flash_write(&b,0x1f000000,1,0xf0);
    system573_flash_write(&b,0x1f000aaa,1,0xaa);
    system573_flash_write(&b,0x1f000554,1,0x55);
    system573_flash_write(&b,0x1f000aaa,1,0xa0);
    system573_flash_write(&b,0x1f000100,1,0);
    assert(chips[0][0x80]==0 && chips[1][0x80]==0x11);
    system573_flash_advance_us(&b,1000000);assert(b.rtc.ticks==1);
    system573_flash_reset(&b);assert(b.control==0 && b.security==0 && b.h8_index==0 && rtc[0]==0xdd && b.rtc.ticks==1);
    assert(chips[0][0x80]==0); // soft reset does not recreate the original seed
    assert(!b.faults);
    assert(system573_flash_read(&b,0x1f000001,4,&v)==-1);
    assert(system573_flash_read(&b,0x1f700000,4,&v)==0);
    system573_flash_destroy(&b);for(unsigned i=0;i<8;i++)free(chips[i]);
    puts("System 573 flash bank/x8 lanes, Fujitsu commands, H8 response pins, RTC byte lanes, cassette lamps, empty ATA/JVS and warm reset PASS");
    return 0;
}
