/* Source framework adaptation, BSD-3-Clause; see twinkle_serial_LICENSE.txt. */
#include "twinkle_serial.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
namespace twinkle_serial_native {
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using offs_t=uint32_t;
using Tick=unsigned __int128;
constexpr Tick SECOND=Tick(1000000000)*1000000000;
constexpr Tick NEVER=~Tick(0);
struct Fault:std::runtime_error{using std::runtime_error::runtime_error;};
template<class T> unsigned BIT(T v,unsigned bit,unsigned width=1){return (uint64_t(v)>>bit)&((uint64_t(1)<<width)-1);}
struct attotime {
    Tick value=0;static const attotime never;
    static attotime from_hz(uint64_t n){return n?attotime{SECOND/n}:never;}
    static attotime from_msec(unsigned n){return {Tick(n)*SECOND/1000};}
    bool is_never() const{return value==NEVER;}
    attotime operator*(uint64_t n)const{return is_never()?never:attotime{value*n};}
    attotime operator/(uint64_t n)const{if(!n)throw Fault("zero timer divisor");return is_never()?never:attotime{value/n};}
};
const attotime attotime::never{NEVER};
struct Scheduler;
struct emu_timer {
    Scheduler &owner;std::function<void(int32_t)> cb;Tick due=NEVER,period=0;
    uint64_t revision=0,order=0;int32_t param=0;
    void adjust(attotime delay,int32_t arg=0,attotime repeat=attotime{0});
};
struct Scheduler {
    Tick now=0;uint64_t order=0;bool running=false;
    std::vector<std::unique_ptr<emu_timer>> timers;
    emu_timer *timer_alloc(std::function<void(int32_t)> cb){
        auto t=std::make_unique<emu_timer>(emu_timer{*this,std::move(cb)});auto *ptr=t.get();timers.push_back(std::move(t));return ptr;
    }
    bool side_effects_disabled()const{return false;}
    void advance(uint64_t ns){
        Tick target=Tick(ns)*1000000000;if(target<now || running)throw Fault("serial clock rewind/reentry");running=true;
        try {for(unsigned count=0;;count++){
            if(count>1000000)throw Fault("serial event loop overflow");emu_timer *next=nullptr;
            for(auto &t:timers)if(t->due!=NEVER && (!next || t->due<next->due || (t->due==next->due && t->order<next->order)))next=t.get();
            if(!next || next->due>target)break;
            now=next->due;auto revision=next->revision;Tick period=next->period;next->due=NEVER;next->cb(next->param);
            if(next->revision==revision && period && period!=NEVER)next->adjust(attotime{period},next->param,attotime{period});
        }now=target;running=false;}catch(...){running=false;throw;}
    }
};
void emu_timer::adjust(attotime delay,int32_t arg,attotime repeat){
    revision++;order=++owner.order;period=repeat.value;param=arg;due=delay.is_never()?NEVER:owner.now+delay.value;
}
struct Line {
    std::function<void(int)> cb;Line()=default;template<class T> explicit Line(T&){}
    void operator()(int v){if(cb)cb(v);}
    struct Binding {Line &line;void set(std::function<void(int)> f){line.cb=std::move(f);}};
    Binding bind(){return {*this};}
    void set(std::function<void(int)> f){cb=std::move(f);}
};
struct device_t {
    Scheduler &scheduler;u32 frequency;
    explicit device_t(Scheduler &s,u32 clock=0):scheduler(s),frequency(clock){}
    virtual ~device_t()=default;virtual void device_start(){}virtual void device_reset(){}
    Scheduler &machine(){return scheduler;}u32 clock()const{return frequency;}
    void set_unscaled_clock(u32 n){frequency=n;}
    template<class T> emu_timer *timer_alloc(void(T::*method)(int32_t),T *object){return scheduler.timer_alloc([=](int32_t p){(object->*method)(p);});}
};
struct device_interface {
    device_t &owner;device_interface(device_t &d,const char*):owner(d){}virtual ~device_interface()=default;
    device_t &device(){return owner;}virtual void interface_pre_start(){}virtual void interface_post_start(){}
};
#define ATTR_COLD
#define TIMER_CALLBACK_MEMBER(name) void name([[maybe_unused]] int32_t param)
#define FUNC(name) &name
#define NAME(name) name
#define save_item(...) ((void)0)
#define LOG(...) ((void)0)
#define LOGMASKED(...) ((void)0)
#define LOGCONFIG(...) ((void)0)
#define LOGCMD(...) ((void)0)
using devcb_write_line=Line;
#include "twinkle_serial_declarations.inc"
ins8250_uart_device::ins8250_uart_device(Scheduler &s,u32 clock):device_t(s,clock),device_serial_interface(static_cast<device_t &>(*this)),m_device_type(dev_type::NS16550),m_regs{},m_int_pending(0),m_txd(1),m_rxd(1),m_dcd(1),m_dsr(1),m_ri(1),m_cts(1){}
ns16550_device::ns16550_device(Scheduler &s,u32 clock):ins8250_uart_device(s,clock),m_rintlvl(0),m_rfifo{},m_efifo{},m_tfifo{},m_rhead(0),m_rtail(0),m_rnum(0),m_thead(0),m_ttail(0),m_timeout(nullptr){}
struct upd765_family_device {enum class mode_t {AT,PS2,M30};};
struct UnimplementedFloppy {
    void set_mode(upd765_family_device::mode_t){}
    uint8_t sra_r(){throw Fault("unsupported Super-I/O floppy SRA");}
    uint8_t srb_r(){throw Fault("unsupported Super-I/O floppy SRB");}
    uint8_t dor_r(){throw Fault("unsupported Super-I/O floppy DOR");}
    uint8_t tdr_r(){throw Fault("unsupported Super-I/O floppy TDR");}
    uint8_t msr_r(){throw Fault("unsupported Super-I/O floppy MSR");}
    uint8_t fifo_r(){throw Fault("unsupported Super-I/O floppy FIFO");}
    uint8_t dir_r(){throw Fault("unsupported Super-I/O floppy DIR");}
    void dor_w(uint8_t){throw Fault("unsupported Super-I/O floppy DOR");}
    void tdr_w(uint8_t){throw Fault("unsupported Super-I/O floppy TDR");}
    void dsr_w(uint8_t){throw Fault("unsupported Super-I/O floppy DSR");}
    void fifo_w(uint8_t){throw Fault("unsupported Super-I/O floppy FIFO");}
    void ccr_w(uint8_t){throw Fault("unsupported Super-I/O floppy CCR");}
};
struct UnimplementedLpt {
    uint8_t read(unsigned){throw Fault("unsupported Super-I/O parallel read");}
    void write(unsigned,uint8_t){throw Fault("unsupported Super-I/O parallel write");}
};
struct fdc37c665gt_device:device_t {
    enum OperatingMode {Run,Configuration}mode=Run;
    enum LogicalDevice {FDC,IDE,Parallel,Serial1,Serial2,LogicalDeviceEnd};
    int config_key_step=0,config_index=0;bool enabled_logical[LogicalDeviceEnd]{};
    int device_addresses[LogicalDeviceEnd]{},com_addresses[4]{};
    upd765_family_device::mode_t m_floppy_mode=upd765_family_device::mode_t::AT;
    uint8_t configuration_registers[16]{};
    Line m_fintr_callback,m_fdrq_callback,m_pintr1_callback,m_irq3_callback,m_irq4_callback;
    Line m_txd1_callback,m_ndtr1_callback,m_nrts1_callback,m_txd2_callback,m_ndtr2_callback,m_nrts2_callback;
    UnimplementedFloppy floppy;UnimplementedLpt lpt;UnimplementedFloppy *m_fdc=&floppy;UnimplementedLpt *m_lpt=&lpt;
    std::array<ns16550_device*,2> m_serial;
    fdc37c665gt_device(Scheduler&s,ns16550_device&a,ns16550_device&b):device_t(s,24000000),m_serial{&a,&b}{}
    void device_start()override;uint8_t read(offs_t);void write(offs_t,uint8_t);void write_configuration_register(int,int);
    void irq_floppy_w(int);void irq_parallel_w(int);void irq_serial1_w(int);void txd_serial1_w(int);void dtr_serial1_w(int);void rts_serial1_w(int);
    void irq_serial2_w(int);void txd_serial2_w(int);void dtr_serial2_w(int);void rts_serial2_w(int);
    void rxd1_w(int);void ndcd1_w(int);void ndsr1_w(int);void nri1_w(int);void ncts1_w(int);
    void rxd2_w(int);void ndcd2_w(int);void ndsr2_w(int);void nri2_w(int);void ncts2_w(int);
};
struct jvc_xvd701_device:device_t,device_serial_interface {
    enum jvc_xvd701_media_type:uint32_t {JVC_MEDIA_VCD,JVC_MEDIA_DVD};
    enum jvc_xvd701_playback_status:uint32_t {STATUS_STOP,STATUS_PLAYING,STATUS_PAUSE};
    jvc_xvd701_media_type m_media_type=JVC_MEDIA_VCD;
    unsigned char m_command[11]{},m_response[11]{};int m_response_index=0;emu_timer *m_timer_response=nullptr;
    jvc_xvd701_playback_status m_playback_status=STATUS_STOP;unsigned char m_jlip_id=0;bool m_is_powered=false;int m_chapter=0;
    enum:unsigned char{STATUS_UNKNOWN_COMMAND=1,STATUS_OK=3,STATUS_ERROR=5};const unsigned char NO_RESPONSE[6]={};
    Line output_rxd,output_dcd,output_dsr,output_ri,output_cts;
    explicit jvc_xvd701_device(Scheduler&s):device_t(s),device_serial_interface(static_cast<device_t &>(*this)){}
    void device_start()override;void device_reset()override;void tra_callback()override;void tra_complete()override;void rcv_complete()override;
    void send_response(int32_t);unsigned char sum(unsigned char*,int);void create_packet(unsigned char,const unsigned char[6]);bool seek_chapter(int);
};
#include "twinkle_serial_mame.inc"
struct Uart:ns16550_device {
    using ns16550_device::ns16550_device;
    void start(){interface_pre_start();device_start();interface_post_start();}void reset(){device_reset();}
    uint8_t lcr()const{return m_regs.lcr;}uint8_t lsr()const{return m_regs.lsr;}uint8_t fcr()const{return m_regs.fcr;}uint16_t divisor()const{return m_regs.dl;}
};
struct Dvd:jvc_xvd701_device {
    uint64_t commands=0,response_bytes=0;
    using jvc_xvd701_device::jvc_xvd701_device;
    void start(){interface_pre_start();device_start();interface_post_start();}void reset(){device_reset();}
    void rcv_complete()override{jvc_xvd701_device::rcv_complete();if(m_command[0]==0xff && m_command[1]==0xff && m_command[10]==sum(m_command,10))commands++;}
    void tra_complete()override{response_bytes++;jvc_xvd701_device::tra_complete();}
};
}
using namespace twinkle_serial_native;
struct TwinkleSerial::Impl {
    Scheduler scheduler;Uart uart0{scheduler,24000000/13},uart1{scheduler,24000000/13};
    fdc37c665gt_device fdc{scheduler,uart0,uart1};Dvd dvd{scheduler};
    uint64_t reads=0,writes=0,tx_bits=0,rx_bits=0;unsigned faults=0;std::string error;
    Impl(){
        uart0.start();uart1.start();fdc.device_start();
        uart1.out_tx_callback().set([this](int v){tx_bits++;dvd.rx_w(v);});
        dvd.output_rxd.set([this](int v){rx_bits++;uart1.rx_w(v);});
        dvd.output_dcd.set([this](int v){uart1.dcd_w(v);});dvd.output_dsr.set([this](int v){uart1.dsr_w(v);});
        dvd.output_ri.set([this](int v){uart1.ri_w(v);});dvd.output_cts.set([this](int v){uart1.cts_w(v);});
        dvd.start();uart0.reset();uart1.reset();dvd.reset();
    }
    template<class F> int call(F f){if(faults)return -1;try{f();return 0;}catch(const std::exception &e){faults=1;error=e.what();return -1;}}
};
TwinkleSerial::TwinkleSerial():p(std::make_unique<Impl>()){}
TwinkleSerial::~TwinkleSerial()=default;
int TwinkleSerial::advance_to(uint64_t ns){return p->call([&]{p->scheduler.advance(ns);});}
int TwinkleSerial::read(unsigned off,uint8_t *v){if(!v || off>=0x400)return -1;return p->call([&]{p->reads++;*v=p->fdc.read(off);});}
int TwinkleSerial::write(unsigned off,uint8_t v){if(off>=0x400)return -1;return p->call([&]{p->writes++;p->fdc.write(off,v);});}
const char *TwinkleSerial::error()const{return p->error.c_str();}
void TwinkleSerial::status(Status &s)const{
    s={};s.now_ns=uint64_t(p->scheduler.now/1000000000);s.reads=p->reads;s.writes=p->writes;s.uart_tx_updates=p->tx_bits;s.uart_rx_updates=p->rx_bits;
    s.dvd_commands=p->dvd.commands;s.dvd_response_bytes=p->dvd.response_bytes;s.faults=p->faults;s.chapter=p->dvd.m_chapter;s.playback=p->dvd.m_playback_status;s.powered=p->dvd.m_is_powered;
    const Uart *u[2]={&p->uart0,&p->uart1};for(unsigned i=0;i<2;i++){s.uart_lcr[i]=u[i]->lcr();s.uart_lsr[i]=u[i]->lsr();s.uart_fcr[i]=u[i]->fcr();s.uart_divisor[i]=u[i]->divisor();}
}
