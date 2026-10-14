/* SPDX-License-Identifier: BSD-3-Clause. MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * konami/ksys573.cpp/k573cass.cpp. See system573_LICENSE.txt. */
#include "system573_flash.h"
#include <string.h>
int system573_flash_init(System573Flash *b,uint8_t *flash[8],uint8_t *rtc,
                         const uint8_t *response,const uint8_t *cassette,const struct tm *local){
    if(!b || !flash || !rtc || !response || !cassette || !local)return -1;
    memset(b,0,sizeof(*b));b->h8_response=response;
    for(unsigned i=0;i<8;i++)if(!(b->flash[i]=nor_flash_create(NOR_FLASH_FUJITSU_MBM29F016A,flash[i],0x200000)))goto bad;
    b->cassette=konami_secure_flash_create(KONAMI_X76F100,cassette,132);if(!b->cassette)goto bad;
    if(m48t58_init(&b->rtc,rtc,local))goto bad;
    adc0834_init(&b->adc);
    b->input[0]=UINT32_MAX;b->input[1]=0x13001007;b->input[2]=0xffff;b->input[3]=0x0f000f00;
    return 0;
bad:system573_flash_destroy(b);return -1;
}
void system573_flash_destroy(System573Flash *b){
    if(!b)return;for(unsigned i=0;i<8;i++)nor_flash_destroy(b->flash[i]);
    konami_secure_flash_destroy(b->cassette);memset(b,0,sizeof(*b));
}
void system573_flash_reset(System573Flash *b){
    b->control=b->security=0;b->h8_index=b->h8_clk=0;b->jvs_r=b->jvs_w=0;memset(b->jvs,0,sizeof(b->jvs));
    konami_secure_flash_reset(b->cassette);
}
void system573_flash_advance_us(System573Flash *b,uint64_t us){
    m48t58_advance_us(&b->rtc,us);for(unsigned i=0;i<8;i++)nor_flash_advance_us(b->flash[i],us);
}
void system573_flash_fetch_view(System573Flash *b,uint8_t *window){
    unsigned bank=b->control&63;if(bank>=4){memset(window,0xff,0x400000);return;}
    const uint8_t *lo=nor_flash_image(b->flash[bank*2]),*hi=nor_flash_image(b->flash[bank*2+1]);
    for(unsigned i=0;i<0x200000;i++){window[2*i]=lo[i];window[2*i+1]=hi[i];}
}
static int aperture(uint32_t a){
    return (a>=0x1f000000 && a<0x1f400000) || (a>=0x1f400000 && a<0x1f400010) ||
        (a>=0x1f480000 && a<0x1f480010) || (a>=0x1f4c0000 && a<0x1f4c0010) ||
        (a>=0x1f500000 && a<0x1f500002) || (a>=0x1f560000 && a<0x1f560002) ||
        (a>=0x1f5c0000 && a<0x1f5c0004) || (a>=0x1f600000 && a<0x1f600004) ||
        (a>=0x1f620000 && a<0x1f624000) || (a>=0x1f680000 && a<0x1f680002) ||
        (a>=0x1f6a0000 && a<0x1f6a0002);
}
static uint32_t in1(System573Flash *b){
    return b->input[1]|((b->h8_response[b->h8_index]&15u)<<4)|
        ((uint32_t)b->adc.dout<<16)|((uint32_t)konami_secure_flash_read_sda(b->cassette)<<18)|
        0x0c080000; /* no cards (CD1 high), no JVS device (sense high). */
}
static void out0(System573Flash *b,uint32_t v,uint32_t mask){
    if(mask&1)adc0834_di(&b->adc,v&1);
    if(mask&2)adc0834_cs(&b->adc,(v>>1)&1);
    if(mask&4)adc0834_clk(&b->adc,(v>>2)&1);
    if(mask&0x100){int clk=(v>>8)&1;if(clk!=b->h8_clk && clk){if(b->h8_index<63)b->h8_index++;b->h8_edges++;}b->h8_clk=clk;}
}
static void security(System573Flash *b,uint16_t v,uint16_t mask){
    b->security=(b->security&~mask)|(v&mask);
    if(mask&1)konami_secure_flash_sda(b->cassette,v&1);
    if(mask&2)konami_secure_flash_scl(b->cassette,(v>>1)&1);
    if(mask&4)konami_secure_flash_cs(b->cassette,(v>>2)&1);
    if(mask&8)konami_secure_flash_rst(b->cassette,(v>>3)&1);
    uint8_t colors=0;
    /* Output latches update per source output line, in increasing bit order. */
    const unsigned strobes[]={8,2,1};
    for(unsigned bit=0;bit<8;bit++)if(mask&(1u<<bit)){
        unsigned line=1u<<bit;int high=!!(v&line);
        for(unsigned p=0;p<3;p++)if(line==strobes[p]){
            if(high && !(b->strobe&line))b->lamps[p]=b->lamp_color;
            b->strobe=(b->strobe&~line)|(high?line:0);
        }
        if(bit>=4){colors=1u<<(bit-4);b->lamp_color=(b->lamp_color&~colors)|(high?colors:0);}
    }
}
static uint16_t jvs_read(System573Flash *b){
    if(b->jvs_r+2>sizeof(b->jvs)){b->faults|=1;return 0;}
    unsigned p=b->jvs_r;b->jvs_r+=2;return b->jvs[p]|b->jvs[p+1]<<8;
}
static void jvs_write(System573Flash *b,uint16_t v){
    if(b->jvs_w+2>sizeof(b->jvs)){b->faults|=1;b->jvs_w=0;return;}
    b->jvs[b->jvs_w++]=v;b->jvs[b->jvs_w++]=v>>8;
    if(b->jvs[0]!=0xe0){b->jvs_w=0;return;}
    unsigned size=b->jvs[2]+3;
    if(b->jvs_w<5 || !b->jvs[1] || b->jvs_w<size)return;
    uint8_t sum=0;for(unsigned i=1;i<size-1;i++)sum+=b->jvs[i];
    if(sum==b->jvs[size-1]){b->jvs_packets++;b->jvs_w=b->jvs_r=0;b->jvs[0]=0;}
    /* Empty source JVS chain returns no packet. */
}
int system573_flash_read(System573Flash *b,uint32_t a,unsigned w,uint32_t *v){
    if(!b || !v)return -1;if(!aperture(a))return 0;
    if((w!=1 && w!=2 && w!=4) || (a&(w-1)))return -1;
    *v=0;uint32_t base=a&~3u,raw=0;
    if(a<0x1f400000){
        unsigned bank=b->control&63;
        for(unsigned i=0;i<w;i++){
            uint16_t byte=0xff;unsigned off=a+i-0x1f000000;
            if(bank<4 && nor_flash_read(b->flash[bank*2+(off&1)],off/2,&byte))b->faults|=2;
            *v|=(uint32_t)(byte&255)<<(8*i);b->flash_reads++;
        }
        return 1;
    }
    if(base>=0x1f620000 && base<0x1f624000){
        for(unsigned i=0;i<w;i++)if(!((a+i)&1)){
            *v|=(uint32_t)m48t58_read(&b->rtc,(a+i-0x1f620000)/2)<<(8*i);b->rtc_reads++;
        }
        return 1;
    }
    switch(base){
    case 0x1f400000:raw=b->input[0];break;
    case 0x1f400004:raw=in1(b);break;
    case 0x1f400008:raw=b->input[2]&0xffff;break;
    case 0x1f40000c:raw=b->input[3];break;
    case 0x1f500000:raw=b->control;break;
    case 0x1f6a0000:raw=b->security;break;
    case 0x1f680000:raw=jvs_read(b);break;
    default:
        if((base>=0x1f480000 && base<0x1f480010) || (base>=0x1f4c0000 && base<0x1f4c0010)){
            /* Source ATA interface starts at mem_mask and ANDs present drives.
             * With no devices connected, every requested lane stays high. */
            raw=UINT32_MAX;b->empty_ata_reads++;
        }
    }
    *v=raw>>(8*(a&3));if(w<4)*v&=(1u<<(8*w))-1;return 1;
}
int system573_flash_write(System573Flash *b,uint32_t a,unsigned w,uint32_t v){
    if(!b)return -1;if(!aperture(a))return 0;
    if((w!=1 && w!=2 && w!=4) || (a&(w-1)))return -1;
    if(a<0x1f400000){
        unsigned bank=b->control&63;
        if(bank<4)for(unsigned i=0;i<w;i++){
            unsigned off=a+i-0x1f000000;
            int r=nor_flash_write(b->flash[bank*2+(off&1)],off/2,(v>>(i*8))&255);
            if(r!=NOR_FLASH_OK && r!=NOR_FLASH_ERR_BUSY && r!=NOR_FLASH_ERR_SEQUENCE)b->faults|=2;
            b->flash_writes++;
        }
        return 1;
    }
    if(a>=0x1f620000 && a<0x1f624000){
        for(unsigned i=0;i<w;i++)if(!((a+i)&1)){m48t58_write(&b->rtc,(a+i-0x1f620000)/2,v>>(8*i));b->rtc_writes++;}
        return 1;
    }
    uint32_t base=a&~3u,mask=w==4?UINT32_MAX:((1u<<(8*w))-1)<<(8*(a&3));v<<=8*(a&3);
    switch(base){
    case 0x1f400000:out0(b,v,mask);break;
    case 0x1f500000:b->control=(b->control&~mask)|(v&mask);break;
    case 0x1f6a0000:security(b,v,mask);break;
    case 0x1f680000:jvs_write(b,v);break;
    }
        return 1;
}
