/* SPDX-License-Identifier: BSD-3-Clause. Source MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * machine/timekpr.cpp/timehelp.h, Aaron Giles/smf. See system573_LICENSE.txt. */
#include "m48t58.h"
#include <string.h>
static uint8_t bcd(unsigned n){return (n/10%10)*16+n%10;}
static unsigned decimal(uint8_t n){return (n>>4)*10+(n&15);}
static int inc(uint8_t *p,unsigned mask,unsigned min,unsigned max){
    unsigned v=(*p+1)&mask;
    if((v&15)>9)v=(v&0xf0)+0x10;
    int carry=v>max;if(carry)v=min;
    *p=(*p&~mask)|(v&mask);return carry;
}
static void to_ram(M48T58 *r){memcpy(r->ram+0x1ff8,r->counter,8);}
int m48t58_init(M48T58 *r,uint8_t *ram,const struct tm *t){
    if(!r || !ram || !t)return -1;
    memset(r,0,sizeof(*r));r->ram=ram;
    r->counter[1]=bcd(t->tm_sec);r->counter[2]=bcd(t->tm_min);r->counter[3]=bcd(t->tm_hour);
    r->counter[4]=bcd(t->tm_wday+1);r->counter[5]=bcd(t->tm_mday);
    r->counter[6]=bcd(t->tm_mon+1);r->counter[7]=bcd(t->tm_year+1900);
    to_ram(r);return 0;
}
static void tick(M48T58 *r){
    uint8_t *c=r->counter;
    if((c[1]&0x80) || (c[0]&0x80))return;
    int carry=inc(c+1,0x7f,0,0x59);
    if(carry)carry=inc(c+2,0x7f,0,0x59);
    if(carry)carry=inc(c+3,0x3f,0,0x23);
    if(carry){
        static const uint8_t days[]={0x31,0x28,0x31,0x30,0x31,0x30,0x31,0x31,0x30,0x31,0x30,0x31};
        inc(c+4,7,1,7);unsigned m=decimal(c[6]),y=decimal(c[7]);
        unsigned max=m==2 && y%4==0?0x29:m>=1 && m<=12?days[m-1]:0x31;
        carry=inc(c+5,0x3f,1,max);
    }
    if(carry)carry=inc(c+6,0x1f,1,0x12);
    if(carry)carry=inc(c+7,0xff,0,0x99);
    if(carry && (c[4]&0x20))c[4]^=0x10;
    r->ticks++;
    if(!(c[0]&0x40))to_ram(r);
}
void m48t58_advance_us(M48T58 *r,uint64_t us){
    if(!r || !r->ram)return;
    uint64_t seconds=us/1000000;r->fraction_us+=us%1000000;
    seconds+=r->fraction_us/1000000;r->fraction_us%=1000000;
    while(seconds--)tick(r);
}
uint8_t m48t58_read(const M48T58 *r,unsigned a){
    if(!r || !r->ram || a>=0x2000)return 0xff;
    return a==0x1ffd?r->ram[a]&~0x40:r->ram[a];
}
void m48t58_write(M48T58 *r,unsigned a,uint8_t v){
    if(!r || !r->ram || a>=0x2000)return;
    if(a==0x1ff8){
        if((r->counter[0]&0x80) && !(v&0x80))memcpy(r->counter,r->ram+0x1ff8,8);
        if((r->counter[0]&0x40) && !(v&0x80))to_ram(r);
        r->counter[0]=v;
    }else if(a==0x1ffc)r->counter[4]=(r->counter[4]&~0x20)|(v&0x20);
    r->ram[a]=v;
}
void m48t58_nvram_import(M48T58 *r,const uint8_t *image){
    if(r && r->ram && image){memcpy(r->ram,image,0x2000);to_ram(r);}
}
