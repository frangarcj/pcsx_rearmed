#include "../libpcsxcore/arcade/m48t58.h"
#include "../libpcsxcore/arcade/adc0834.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void pulse(Adc0834 *a,int bit){adc0834_di(a,bit);adc0834_clk(a,1);adc0834_clk(a,0);}
static unsigned convert(Adc0834 *a,unsigned channel,int single){
    adc0834_cs(a,1);adc0834_cs(a,0);pulse(a,1);assert(!a->sars);
    pulse(a,single);pulse(a,channel&1);pulse(a,channel>>1);
    assert(a->sars && !a->dout);
    unsigned result=0;for(unsigned i=0;i<8;i++){pulse(a,0);result=(result<<1)|a->dout;}
    unsigned reverse=result&1;
    for(unsigned i=1;i<8;i++){pulse(a,0);reverse|=a->dout<<i;}
    assert(reverse==result);pulse(a,0);assert(!a->dout);
    adc0834_cs(a,1);assert(a->sars && a->dout);return result;
}
int main(void){
    uint8_t ram[0x2000],nv[0x2000];memset(ram,0xa5,sizeof(ram));
    struct tm t={.tm_year=124,.tm_mon=1,.tm_mday=28,.tm_wday=3,.tm_hour=23,.tm_min=59,.tm_sec=59};
    M48T58 r;assert(!m48t58_init(&r,ram,&t));assert(ram[0]==0xa5 && ram[0x1fff]==0x24);
    m48t58_advance_us(&r,999999);assert(ram[0x1ff9]==0x59);
    m48t58_advance_us(&r,1);assert(ram[0x1ffd]==0x29 && ram[0x1ffb]==0 && ram[0x1ffc]==5);
    m48t58_write(&r,0x1ff8,0x40);unsigned sec=ram[0x1ff9];m48t58_advance_us(&r,2000000);
    assert(ram[0x1ff9]==sec && r.counter[1]==2);
    m48t58_write(&r,0x1ff8,0);assert(ram[0x1ff9]==2);
    m48t58_write(&r,0x1ff8,0x80);m48t58_write(&r,0x1ff9,0x80);
    m48t58_write(&r,0x1ff8,0);m48t58_advance_us(&r,3000000);assert(r.counter[1]==0x80);
    m48t58_write(&r,0x1ff8,0x80);m48t58_write(&r,0x1ff9,0x59);
    m48t58_write(&r,0x1ffa,0x59);m48t58_write(&r,0x1ffb,0x23);
    m48t58_write(&r,0x1ffc,0x27);m48t58_write(&r,0x1ffd,0x31);
    m48t58_write(&r,0x1ffe,0x12);m48t58_write(&r,0x1fff,0x99);m48t58_write(&r,0x1ff8,0);
    m48t58_advance_us(&r,1000000);assert(ram[0x1ffd]==1 && ram[0x1ffe]==1 && ram[0x1fff]==0 && ram[0x1ffc]==0x30);
    m48t58_write(&r,0x1ffd,0xff);assert(m48t58_read(&r,0x1ffd)==0xbf);
    memcpy(nv,ram,sizeof(nv));nv[12]=0x19;nv[0x1fff]=0x88;m48t58_nvram_import(&r,nv);
    assert(ram[12]==0x19 && ram[0x1fff]==0);
    assert(m48t58_read(&r,0x2000)==0xff);m48t58_write(&r,0x2000,0);
    Adc0834 a;adc0834_init(&a);a.channel[0]=0x81;a.channel[1]=0x10;a.channel[2]=0xa5;a.channel[3]=0xff;
    for(unsigned i=0;i<4;i++)assert(convert(&a,i,1)==a.channel[i]);
    assert(convert(&a,0,0)==0x71 && convert(&a,1,0)==0 && convert(&a,2,0)==0 && convert(&a,3,0)==0x5a);
    adc0834_cs(&a,0);pulse(&a,0);pulse(&a,0);assert(a.dout && a.sars);
    adc0834_cs(&a,1);pulse(&a,1);assert(a.dout && a.sars);
    puts("System 573 M48T58 elapsed clock, leap/century rollover, R/W/ST latches, original SRAM and ADC0834 channels/differential/serial edges PASS");
    return 0;
}
