#include "../libpcsxcore/arcade/system573_flash.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void zi_serial(System573Flash *b,int high){
    assert(system573_flash_write(b,0x1f6a0000,1,(b->security&~16u)|(high?0:16))==1);
}
static int zi_serial_read(System573Flash *b){
    uint32_t v;assert(system573_flash_read(b,0x1f400004,4,&v)==1);return !!(v&0x4000);
}
static void zi_reset_serial(System573Flash *b){
    zi_serial(b,0);system573_flash_advance_us(b,480);zi_serial(b,1);
    system573_flash_advance_us(b,29);assert(zi_serial_read(b)==1);
    system573_flash_advance_us(b,1);assert(zi_serial_read(b)==0);
    system573_flash_advance_us(b,119);assert(zi_serial_read(b)==0);
    system573_flash_advance_us(b,1);assert(zi_serial_read(b)==1);
}
static void zi_read_serial(System573Flash *b,const uint8_t *id){
    zi_reset_serial(b);
    for(unsigned bit=0;bit<8;bit++){
        int high=(0x33>>bit)&1;
        zi_serial(b,0);system573_flash_advance_us(b,high?6:60);
        zi_serial(b,1);system573_flash_advance_us(b,high?64:10);
    }
    for(unsigned byte=0;byte<8;byte++){
        unsigned v=0;
        for(unsigned bit=0;bit<8;bit++){
            zi_serial(b,0);system573_flash_advance_us(b,6);
            zi_serial(b,1);system573_flash_advance_us(b,9);
            v|=zi_serial_read(b)<<bit;system573_flash_advance_us(b,55);
        }
        assert(v==id[7-byte]);
    }
}
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
    system573_flash_write(&b,0x1f000aaa,2,0xaaaa);
    system573_flash_write(&b,0x1f000554,2,0x5555);
    system573_flash_write(&b,0x1f000aaa,2,0xa0a0);
    system573_flash_write(&b,0x1f000200,4,0xf0f00000);
    assert(chips[0][0x100]==0 && chips[1][0x100]==0);
    assert(b.dirty_start[0]==0x100 && b.dirty_start[1]==0x100);
    assert(b.dirty_length[0]==1 && b.dirty_length[1]==1);
    system573_flash_advance_us(&b,1000000);assert(b.rtc.ticks==1);
    system573_flash_reset(&b);assert(b.control==0 && b.security==0 && b.h8_index==0 && rtc[0]==0xdd && b.rtc.ticks==1);
    assert(chips[0][0x80]==0); // soft reset does not recreate the original seed
    assert(!b.faults);
    assert(system573_flash_read(&b,0x1f000001,4,&v)==-1);
    assert(system573_flash_read(&b,0x1f700000,4,&v)==0);
    system573_flash_destroy(&b);
    /* Explicit synthetic EEPROM fixture for the X cassette/analog DDR bus. */
    uint8_t ximage[548];memset(ximage,0x5a,sizeof(ximage));
    assert(!system573_flash_init_cassette(&b,chips,rtc,response,ximage,KONAMI_X76F041,&t));
    uint8_t exported[548];assert(!konami_secure_flash_get_image(b.cassette,exported,548));
    assert(!memcmp(ximage,exported,548));
    system573_flash_write(&b,0x1f6a0000,2,0xf8);assert(!b.lamps[0] && !b.lamp_color);
    b.stage_input=0xfffffeff;
    system573_flash_read(&b,0x1f400008,4,&v);assert(v==0xfeff);
    for(unsigned i=0;i<13;i++){
        unsigned data=((0xc90>>i)&1)<<5;
        system573_flash_write(&b,0x1f640080,2,data);
        system573_flash_write(&b,0x1f640080,2,data|4);
        system573_flash_write(&b,0x1f640080,2,data);
    }
    assert(b.stage[0].init && b.stage[0].bit==0 && b.stage_mask==0xfffff9f9);
    system573_flash_read(&b,0x1f400008,4,&v);assert(v==0xf8f9);
    for(unsigned i=1;i<=22;i++){
        system573_flash_write(&b,0x1f640080,2,4);
        system573_flash_write(&b,0x1f640080,2,0);
        if(i==1)assert(b.stage_mask==0xfffffff9u);
    }
    assert(!b.stage[0].init && b.stage_mask==UINT32_MAX && b.stage_edges==35);
    system573_flash_write(&b,0x1f640088,2,0x20);
    assert(b.stage[1].data==1 && b.analog_output[1]==0x20);
    system573_flash_write(&b,0x1f640089,1,0xff);assert(b.analog_output[1]==0 && !b.stage[1].data);
    system573_flash_write(&b,0x1f640088,2,0x20);system573_flash_reset(&b);
    assert(b.analog_output[1]==0x20 && b.stage[1].data==1);
    system573_flash_read(&b,0x1f640088,4,&v);assert(v==0);
    assert(!b.faults);system573_flash_destroy(&b);
    uint8_t zimage[140]={0x5a,0x53,0,1},id[8]={0x9b,0x22,0x76,0x8a,0xfe,0x45,0x11,1};
    assert(system573_flash_init_zi(&b,chips,rtc,response,zimage,139,id,8,&t)==-1);
    assert(system573_flash_init_zi(&b,chips,rtc,response,zimage,140,id,7,&t)==-1);
    assert(system573_flash_init_zi(&b,chips,rtc,response,zimage,140,NULL,8,&t)==-1);
    assert(system573_flash_init_cassette(&b,chips,rtc,response,zimage,SYSTEM573_CASSETTE_ZI,&t)==-1);
    assert(!system573_flash_init_zi(&b,chips,rtc,response,zimage,140,id,8,&t));
    assert(b.zs01 && b.serial && !b.cassette && b.cassette_type==SYSTEM573_CASSETTE_ZI);
    assert(zi_serial_read(&b)==1);
    b.input[1]|=0x4000; /* Custom input must override even a high backing bit. */
    zi_serial(&b,0);system573_flash_advance_us(&b,479);zi_serial(&b,1);
    system573_flash_advance_us(&b,150);assert(zi_serial_read(&b)==1); // No presence on short reset.
    zi_read_serial(&b,id);
    system573_flash_write(&b,0x1f6a0000,1,0xf8);
    assert(!b.lamp_color && !b.lamps[0] && !b.lamps[1] && !b.lamps[2]);
    system573_flash_reset(&b);assert(!b.cassette_us && zi_serial_read(&b)==1);
    zi_reset_serial(&b);zi_serial(&b,0);
    system573_flash_reset(&b);assert(!b.cassette_us && zi_serial_read(&b)==1);
    zi_read_serial(&b,id); // Complete identity survives reset; no regenerated CRC.
    uint8_t zsaved[140];assert(!konami_zs01_get_image(b.zs01,zsaved,140));assert(!memcmp(zsaved,zimage,140));
    assert(!b.faults);
    system573_flash_advance_us(&b,UINT64_MAX);assert(b.faults==4); // Reject impossible timer overflow.
    system573_flash_destroy(&b);for(unsigned i=0;i<8;i++)free(chips[i]);
    puts("System 573 flash/x8 dirty lanes, H8/RTC, X/Y/ZI cassettes, timed ZI serial, analog DDR two-stage multiplexor, empty ATA/JVS and warm reset PASS");
    return 0;
}
