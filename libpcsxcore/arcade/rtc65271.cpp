/* RTC65271 framework/timer adapter. Source methods in rtc65271_mame.inc.
 * BSD-3-Clause, Raphael Nabet and R. Belmont; see rtc65271_LICENSE.txt. */
#include "rtc65271.h"
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace rtc65271_native {
using Tick=unsigned __int128;
constexpr Tick SECOND=Tick(1000000000)*1000000000;
using offs_t=unsigned;
constexpr int ASSERT_LINE=1,CLEAR_LINE=0;
struct attotime {
    Tick value;
    static const attotime never;
    static attotime from_hz(unsigned f){return f?attotime{SECOND/f}:never;}
    static attotime from_usec(unsigned v){return {Tick(v)*1000000000000};}
    attotime operator/(unsigned n)const{return value==never.value?never:attotime{value/n};}
    attotime operator-(attotime b)const{return {value-b.value};}
    bool operator>(attotime b)const{return value>b.value;}
};
const attotime attotime::never{~Tick(0)};
struct rtc65271_device;
struct Timer {
    rtc65271_device *owner=nullptr;
    Tick due=~Tick(0),started=0,period=0;
    void adjust(attotime delay,int32_t=0,attotime repeat=attotime{0});
    attotime elapsed()const;
};
struct rtc65271_device {
    uint8_t m_regs[64]{},m_xram[4096]{},m_cur_reg=0,m_cur_xram_page=0,m_SQW_internal_state=0;
    std::array<Timer,3> timers;
    Timer *m_begin_update_timer=&timers[0],*m_end_update_timer=&timers[1],*m_SQW_timer=&timers[2];
    Tick now=0;bool busy=false;
    unsigned faults=0;
    uint64_t begins=0,ends=0,squares=0,callbacks=0;
    Rtc65271Config config{};
    uint8_t read(int,offs_t);
    void write(int,offs_t,uint8_t);
    void rtc_clock_updated(int,int,int,int,int,int,int);
    void field_interrupts();int intrq_r();
    void rtc_SQW_cb(int32_t);void rtc_begin_update_cb(int32_t);void rtc_end_update_cb(int32_t);
    void m_interrupt_cb(int state){callbacks++;if(config.irq)config.irq(config.opaque,state);}
};
void Timer::adjust(attotime delay,int32_t,attotime repeat){
    started=owner->now;period=repeat.value;
    due=delay.value==attotime::never.value?delay.value:started+delay.value;
}
attotime Timer::elapsed()const{return {owner->now-started};}
#define UPDATE_CYCLE_TIME attotime::from_usec(1984)
#include "rtc65271_mame.inc"
#undef UPDATE_CYCLE_TIME
}
struct Rtc65271:rtc65271_native::rtc65271_device{};

static bool valid_date(const Rtc65271Date &d){
    if(d.year<1 || d.year>9999 || d.month<1 || d.month>12 || d.weekday<1 || d.weekday>7 ||
       d.hour<0 || d.hour>23 || d.minute<0 || d.minute>59 || d.second<0 || d.second>59)return false;
    static constexpr unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned n=days[d.month-1]+(d.month==2 && d.year%4==0 && (d.year%100!=0 || d.year%400==0));
    return d.day>=1 && unsigned(d.day)<=n;
}
extern "C" int rtc65271_create(const Rtc65271Config *cfg,Rtc65271 **out){
    using namespace rtc65271_native;
    if(out)*out=nullptr;
    if(!out || !cfg || !valid_date(cfg->date) ||
       (cfg->nvram?cfg->nvram_size!=RTC65271_NVRAM_BYTES:cfg->nvram_size!=0) ||
       (cfg->nvram && cfg->nvram[0]!=0))return RTC65271_ARGUMENT;
    auto *r=new(std::nothrow) Rtc65271;if(!r)return RTC65271_FAULT;
    r->config=*cfg;for(auto &timer:r->timers)timer.owner=r;
    // Source nvram_default with no optional default region: zero RAM, binary mode.
    r->m_regs[reg_B]=reg_B_DM;
    if(cfg->nvram){
        const uint8_t *v=cfg->nvram;
        r->m_regs[reg_A]=v[1]&reg_A_DV;
        r->m_regs[reg_B]=v[2]&(reg_B_SET|reg_B_DM|reg_B_24h|reg_B_DSE);
        r->m_regs[reg_alarm_second]=v[3];r->m_regs[reg_alarm_minute]=v[4];r->m_regs[reg_alarm_hour]=v[5];
        std::memcpy(r->m_regs+14,v+6,50);std::memcpy(r->m_xram,v+56,4096);r->m_regs[reg_D]|=reg_D_VRT;
    }
    auto &d=cfg->date;r->rtc_clock_updated(d.year,d.month,d.day,d.weekday,d.hour,d.minute,d.second);
    r->m_begin_update_timer->adjust({SECOND},0,{SECOND});
    *out=r;return RTC65271_OK;
}
extern "C" int rtc65271_try_destroy(Rtc65271 *r){
    if(!r)return RTC65271_ARGUMENT;
    if(r->busy){r->faults|=1;return RTC65271_BUSY;}delete r;return RTC65271_OK;
}
extern "C" int rtc65271_advance_to(Rtc65271 *r,uint64_t ns){
    using namespace rtc65271_native;
    if(!r)return RTC65271_ARGUMENT;
    if(r->busy){r->faults|=1;return RTC65271_BUSY;}
    if(r->faults)return RTC65271_FAULT;
    Tick target=Tick(ns)*1000000000;
    if(target<r->now){r->faults|=2;return RTC65271_FAULT;}
    r->busy=true;
    while(!r->faults){
        unsigned next=0;for(unsigned i=1;i<3;i++)if(r->timers[i].due<r->timers[next].due)next=i;
        auto &timer=r->timers[next];if(timer.due>target)break;
        r->now=timer.due;timer.started=r->now;timer.due=timer.period?r->now+timer.period:~Tick(0);
        if(next==0){r->begins++;r->rtc_begin_update_cb(0);}
        else if(next==1){r->ends++;r->rtc_end_update_cb(0);}
        else {r->squares++;r->rtc_SQW_cb(0);}
    }
    if(!r->faults)r->now=target;r->busy=false;
    return r->faults?RTC65271_FAULT:RTC65271_OK;
}
extern "C" int rtc65271_read(Rtc65271 *r,int xram,unsigned offset,uint8_t *value){
    if(!r || !value || (xram!=0 && xram!=1) || offset>=64)return RTC65271_ARGUMENT;
    if(r->busy){r->faults|=1;return RTC65271_BUSY;}if(r->faults)return RTC65271_FAULT;
    r->busy=true;*value=r->read(xram,offset);r->busy=false;return r->faults?RTC65271_FAULT:RTC65271_OK;
}
extern "C" int rtc65271_write(Rtc65271 *r,int xram,unsigned offset,uint8_t value){
    if(!r || (xram!=0 && xram!=1) || offset>=64)return RTC65271_ARGUMENT;
    if(r->busy){r->faults|=1;return RTC65271_BUSY;}if(r->faults)return RTC65271_FAULT;
    r->busy=true;r->write(xram,offset,value);r->busy=false;return r->faults?RTC65271_FAULT:RTC65271_OK;
}
extern "C" int rtc65271_nvram_export(const Rtc65271 *r,uint8_t *v,size_t size){
    using namespace rtc65271_native;
    if(!r || !v || size!=RTC65271_NVRAM_BYTES)return RTC65271_ARGUMENT;
    v[0]=0;v[1]=r->m_regs[reg_A]&(reg_A_DV|reg_A_RS);
    v[2]=r->m_regs[reg_B]&(reg_B_SET|reg_B_DM|reg_B_24h|reg_B_DSE);
    v[3]=r->m_regs[reg_alarm_second];v[4]=r->m_regs[reg_alarm_minute];v[5]=r->m_regs[reg_alarm_hour];
    std::memcpy(v+6,r->m_regs+14,50);std::memcpy(v+56,r->m_xram,4096);return RTC65271_OK;
}
extern "C" void rtc65271_get_status(const Rtc65271 *r,Rtc65271Status *s){
    if(!r || !s)return;*s={};s->now_ns=uint64_t(r->now/1000000000);
    s->begin_updates=r->begins;s->end_updates=r->ends;s->square_edges=r->squares;s->irq_callbacks=r->callbacks;
    std::memcpy(s->regs,r->m_regs,64);s->register_index=r->m_cur_reg;s->xram_page=r->m_cur_xram_page;
    s->square_state=r->m_SQW_internal_state;s->irq=!!(r->m_regs[12]&0x80);s->faults=r->faults;
}
