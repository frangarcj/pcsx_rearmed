/* BSD-3-Clause. Analytical source-core ADSP2181 acceptance vectors. */
#include "adsp2181.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
struct Fixture {
 std::array<uint32_t,0x4000> pm{};std::array<uint16_t,0x4000> dm{};std::array<uint16_t,0x800> io{};
 Adsp2181 *cpu=nullptr;std::vector<uint32_t> overlay;unsigned tx=0;uint32_t tx_data=0;uint64_t tx_cycle=0;
 static uint32_t pr(void *p,uint16_t a){assert(a<0x4000);return static_cast<Fixture *>(p)->pm[a];}
 static void pw(void *p,uint16_t a,uint32_t d){assert(a<0x4000 && !(d&0xff000000));static_cast<Fixture *>(p)->pm[a]=d;}
 static uint16_t dr(void *p,uint16_t a){assert(a<0x4000);return static_cast<Fixture *>(p)->dm[a];}
 static void dw(void *p,uint16_t a,uint16_t d){assert(a<0x4000);static_cast<Fixture *>(p)->dm[a]=d;}
 static uint16_t ir(void *p,uint16_t a){auto &f=*static_cast<Fixture *>(p);assert(a<0x800);if(a==3)assert(adsp2181_irq(f.cpu,ADSP2181_IRQL0,0)==0);return f.io[a];}
 static void iw(void *p,uint16_t a,uint16_t d){assert(a<0x800);static_cast<Fixture *>(p)->io[a]=d;}
 static void ov(void *p,uint32_t v){static_cast<Fixture *>(p)->overlay.push_back(v);}
 static void sport(void *p,unsigned n,uint32_t d){
  auto &f=*static_cast<Fixture *>(p);assert(n==0);f.tx++;f.tx_data=d;
  Adsp2181Status s;adsp2181_status(f.cpu,&s);f.tx_cycle=s.cycles;
  assert(adsp2181_run(f.cpu,1)==-1 && adsp2181_reset(f.cpu)==-1 && adsp2181_idma_address(f.cpu,0)==-1);
  adsp2181_destroy(f.cpu); // destruction is refused inside the running callback
  assert(adsp2181_set_register(f.cpu,ADSP2181_REG_I0,0x123)==0);
 }
 Fixture(){Adsp2181Config c{};c.opaque=this;c.program_read=pr;c.program_write=pw;c.data_read=dr;c.data_write=dw;c.io_read=ir;c.io_write=iw;c.dmovlay=ov;c.sport_tx=sport;assert(adsp2181_create(&c,&cpu)==0);}
 ~Fixture(){adsp2181_destroy(cpu);}
 uint32_t get(unsigned r){return adsp2181_get_register(cpu,r);}
 void set(unsigned r,uint32_t v){assert(adsp2181_set_register(cpu,r,v)==0);}
 void one(uint32_t op){pm[0]=op;set(ADSP2181_REG_PC,0);assert(adsp2181_run(cpu,1)==0);}
 Adsp2181Status status(){Adsp2181Status s;adsp2181_status(cpu,&s);return s;}
};
static uint32_t imm(unsigned r,uint16_t v){return 0x400000|(uint32_t(v)<<4)|r;}
static uint32_t gimm(unsigned group,unsigned r,uint16_t v){return 0x300000|(group<<18)|((uint32_t(v)&0x3fff)<<4)|r;}
static uint32_t jump(unsigned a){return 0x18000f|(a<<4);}
static uint32_t sr(Fixture &f){return f.get(ADSP2181_REG_SR0)|(f.get(ADSP2181_REG_SR1)<<16);}
static uint32_t asr(uint32_t v,unsigned n){return n>=32?(int32_t(v)<0?UINT32_MAX:0):uint32_t(int32_t(v)>>n);}
static void test_memory_io_sport_overlay(){
 Fixture f;f.dm[0x100]=0x5678;
 f.pm[0]=imm(0,0xabcd);f.pm[1]=0x900000|(0x123<<4);f.pm[2]=0x800000|(0x100<<4)|1;
 f.pm[3]=0x018000|(0x7ff<<4)|1;f.pm[4]=0x010000|(0x7ff<<4)|4;
 f.pm[5]=0x0d0000|0xc00|(9<<4)|0;f.pm[6]=gimm(1,15,1);f.pm[7]=jump(7);
 assert(adsp2181_run(f.cpu,8)==0);
 assert(f.get(ADSP2181_REG_AX0)==0xabcd && f.get(ADSP2181_REG_AX1)==0x5678 && f.get(ADSP2181_REG_AY0)==0x5678);
 assert(f.dm[0x123]==0xabcd && f.io[0x7ff]==0x5678 && f.tx==1 && f.tx_data==uint32_t(int32_t(int16_t(0xabcd))) && f.tx_cycle==5);
 assert(f.get(ADSP2181_REG_I0)==0x123 && f.overlay.back()==1);
 assert(adsp2181_reset(f.cpu)==0 && f.overlay.back()==0 && f.get(ADSP2181_REG_AX0)==0xabcd && f.status().cycles==8);
 assert(adsp2181_set_register(f.cpu,ADSP2181_REG_PCSP,17)==-1 && adsp2181_irq(f.cpu,10,1)==-1);
}
static void test_alu_mac(){
 Fixture f;f.set(ADSP2181_REG_AX0,0x7fff);f.set(ADSP2181_REG_AY0,1);f.one(0x22000f|(3<<13));
 assert(f.get(ADSP2181_REG_AR)==0x8000 && (f.status().astat&15)==6);
 f.set(ADSP2181_REG_AX0,0xffff);f.one(0x22000f|(3<<13));assert(f.get(ADSP2181_REG_AR)==0 && (f.status().astat&15)==9);
 f.set(ADSP2181_REG_MSTAT,8);f.set(ADSP2181_REG_AX0,0x7fff);f.one(0x22000f|(3<<13));assert(f.get(ADSP2181_REG_AR)==0x7fff && (f.status().astat&4));
 f.set(ADSP2181_REG_MSTAT,0);f.one(0x22001f|(3<<13));assert(f.get(ADSP2181_REG_AR)==0x8000); // 218x constant +1
 f.set(ADSP2181_REG_MX0,0x4000);f.set(ADSP2181_REG_MY0,0x4000);f.one(0x20000f|(4<<13));
 assert(f.get(ADSP2181_REG_MR0)==0 && f.get(ADSP2181_REG_MR1)==0x2000 && f.get(ADSP2181_REG_MR2)==0);
 f.one(0x24000f|(4<<13));assert(f.get(ADSP2181_REG_MF)==0x2000);
 // Preserve upstream int32 intermediate product bit patterns, including
 // negative, unsigned and fractional overflow cases without C++ UB.
 uint32_t random=0x31415926;
 for(unsigned i=0;i<2000;i++)for(unsigned mode=0;mode<2;mode++)for(unsigned kind=4;kind<8;kind++){
  random=random*1664525+1013904223;uint16_t x=random;
  random=random*1664525+1013904223;uint16_t y=random;
  f.set(ADSP2181_REG_MSTAT,mode?0x10:0);f.set(ADSP2181_REG_MX0,x);f.set(ADSP2181_REG_MY0,y);
  int64_t sx=(kind==4 || kind==5)?int16_t(x):int64_t(x);int64_t sy=(kind==4 || kind==6)?int16_t(y):int64_t(y);
  int32_t product=uint32_t(uint64_t(sx*sy)<<(mode?0:1));
  f.one(0x20000f|(kind<<13));uint64_t expected=uint64_t(int64_t(product));
  assert(f.get(ADSP2181_REG_MR0)==(expected&65535) && f.get(ADSP2181_REG_MR1)==((expected>>16)&65535) && (f.get(ADSP2181_REG_MR2)&255)==((expected>>32)&255));
 }
 assert(!f.status().diagnostics);
}
static void test_shifts(){
 Fixture f;
 for(uint16_t v:{0,1,0x7fff,0x8000,0xffff})for(int sc:{-128,-33,-32,-31,-1,0,1,15,31,32,33,127})for(unsigned kind:{0,2,4,6,8,10}){
  f.set(ADSP2181_REG_SI,v);f.set(ADSP2181_REG_SE,uint32_t(sc));f.set(ADSP2181_REG_ASTAT,8);
  uint32_t input=(kind==0 || kind==4 || kind==8)?uint32_t(v)<<16:kind==6?uint32_t(int32_t(int16_t(v))):v;
  uint32_t expected=0;
  if(kind==0 || kind==2)expected=sc>=32 || sc<=-32?0:sc>0?input<<sc:input>>-sc;
  if(kind==4 || kind==6)expected=sc>0?(sc>=32?0:input<<sc):asr(input,unsigned(-sc));
  if(kind==8)expected=sc>0?asr((input>>1)|0x80000000,unsigned(sc-1)):sc<=-32?0:input<<-sc;
  if(kind==10)expected=sc>0?(sc>=32?0:input>>sc):sc<=-32?0:input<<-sc;
  f.one(0x0e000f|(kind<<11));assert(sr(f)==expected);
 }
 assert(!f.status().diagnostics);
}
static void test_dag_loops_stack_banks(){
 Fixture f;f.set(ADSP2181_REG_I0,0x100);f.set(ADSP2181_REG_M0,1);f.set(ADSP2181_REG_L0,3);
 f.dm[0x100]=1;f.dm[0x101]=2;f.dm[0x102]=3;f.pm[0]=f.pm[1]=f.pm[2]=0x600000;
 assert(adsp2181_run(f.cpu,3)==0 && f.get(ADSP2181_REG_AX0)==3 && f.get(ADSP2181_REG_I0)==0x100);
 f.set(ADSP2181_REG_M0,uint32_t(-1));f.one(0x600000);assert(f.get(ADSP2181_REG_I0)==0x102);
 f.set(ADSP2181_REG_L0,0);f.set(ADSP2181_REG_I0,0x100);f.set(ADSP2181_REG_M0,1);f.set(ADSP2181_REG_MSTAT,2);f.dm[0x20]=0x8888;
 f.one(0x600000);assert(f.get(ADSP2181_REG_AX0)==0x8888 && f.get(ADSP2181_REG_I0)==0x101);
 f.set(ADSP2181_REG_MSTAT,0);f.set(ADSP2181_REG_I4,0x100);f.set(ADSP2181_REG_M4,1);f.pm[0x100]=0x1234ab;
 f.one(0x500000);assert(f.get(ADSP2181_REG_AX0)==0x1234 && f.get(ADSP2181_REG_PX)==0xab);
 f.set(ADSP2181_REG_AX0,0xfff0);f.set(ADSP2181_REG_I4,0x101);f.one(0x580000);assert(f.pm[0x101]==0xfff0ab);
 f.set(ADSP2181_REG_I0,0x100);f.pm[0]=gimm(3,5,3);f.pm[1]=0x14000e|(2<<4);f.pm[2]=0x090000;f.pm[3]=jump(3);
 f.set(ADSP2181_REG_PC,0);assert(adsp2181_run(f.cpu,100)==0 && f.get(ADSP2181_REG_I0)==0x103 && f.get(ADSP2181_REG_LOOPSP)==0 && f.get(ADSP2181_REG_PCSP)==0);
 f.pm[0]=0x1c000f|(8<<4);f.pm[1]=jump(1);f.pm[8]=imm(0,0x55);f.pm[9]=0x0a000f;f.set(ADSP2181_REG_PC,0);
 assert(adsp2181_run(f.cpu,100)==0 && f.get(ADSP2181_REG_PC)==1 && f.get(ADSP2181_REG_AX0)==0x55 && f.get(ADSP2181_REG_PCSP)==0);
 f.set(ADSP2181_REG_AX0,0x1111);f.set(ADSP2181_REG_MSTAT,1);f.set(ADSP2181_REG_AX0,0x2222);f.set(ADSP2181_REG_MSTAT,0);
 assert(f.get(ADSP2181_REG_AX0)==0x1111 && f.get(ADSP2181_REG_AX0_SEC)==0x2222);
 assert(!f.status().diagnostics);
}
static void test_irq_idma_halt(){
 Fixture f;f.io[3]=0xbeef;f.pm[12]=0x010030;f.pm[13]=0x0a001f;f.set(ADSP2181_REG_PC,0x80);f.set(ADSP2181_REG_IMASK,0x80);
 assert(adsp2181_irq(f.cpu,ADSP2181_IRQL0,1)==0);assert(adsp2181_run(f.cpu,2)==0);
 assert(f.get(ADSP2181_REG_AX0)==0xbeef && f.get(ADSP2181_REG_PC)==0x80 && f.status().interrupts==1 && f.status().imask==0x80);
 f.pm[28]=imm(0,0x1234);f.pm[29]=0x0a001f;f.set(ADSP2181_REG_IMASK,8);
 assert(adsp2181_irq(f.cpu,ADSP2181_BDMA,1)==0 && adsp2181_irq(f.cpu,ADSP2181_BDMA,0)==0);assert(adsp2181_run(f.cpu,2)==0);
 assert(f.get(ADSP2181_REG_AX0)==0x1234 && f.get(ADSP2181_REG_PC)==0x80 && f.status().interrupts==2);
 assert(adsp2181_idma_address(f.cpu,0x1000)==0 && adsp2181_idma_write(f.cpu,0xabcd)==0 && adsp2181_idma_write(f.cpu,0x12ef)==0 && f.pm[0x1000]==0xabcdef);
 uint16_t v=0;assert(adsp2181_idma_address(f.cpu,0x1000)==0 && adsp2181_idma_read(f.cpu,&v)==0 && v==0xabcd && adsp2181_idma_read(f.cpu,&v)==0 && v==0xef);
 assert(adsp2181_idma_address(f.cpu,0x3fff)==0 && adsp2181_idma_write(f.cpu,0x1122)==0 && adsp2181_idma_write(f.cpu,0x33)==0 && f.pm[0x3fff]==0x112233);
 assert(adsp2181_idma_write(f.cpu,0x7788)==0 && f.dm[0]==0x7788); // source increment carries into DM mode
 unsigned pc=f.get(ADSP2181_REG_PC);uint64_t cycles=f.status().cycles;assert(adsp2181_halt(f.cpu,1)==0 && adsp2181_run(f.cpu,100)==0 && f.get(ADSP2181_REG_PC)==pc && f.status().cycles==cycles+100);
 assert(adsp2181_halt(f.cpu,0)==0 && adsp2181_reset(f.cpu)==0 && f.status().pc==0 && f.status().imask==0 && f.status().sstat==0x55 && !f.status().diagnostics);
}
static void test_exponent_divide_and_slicing(){
 Fixture a,b;
 const uint16_t input[]={0,1,0x4000,0x7fff,0x8000,0xfffe,0xffff};
 const int exp[]={-15,-14,0,0,0,-14,-15};
 for(unsigned i=0;i<7;i++){
  a.set(ADSP2181_REG_SI,input[i]);a.one(0x0e000f|(12<<11));
  assert(int16_t(a.get(ADSP2181_REG_SE))==exp[i] && ((a.status().astat&0x80)!=0)==((input[i]&0x8000)!=0));
 }
 a.set(ADSP2181_REG_AX0,0x8000);a.set(ADSP2181_REG_AY0,0x4000);a.one(0x060000);
 assert(a.get(ADSP2181_REG_AF)==0x8000 && a.get(ADSP2181_REG_AY0)==0x8001 && (a.status().astat&0x20));
 a.one(0x070000);assert(a.get(ADSP2181_REG_AF)==1 && a.get(ADSP2181_REG_AY0)==2 && (a.status().astat&0x20));
 a.set(ADSP2181_REG_ASTAT,0);a.set(ADSP2181_REG_AF,0);a.set(ADSP2181_REG_AY0,0);a.one(0x070000);
 assert(a.get(ADSP2181_REG_AF)==0 && a.get(ADSP2181_REG_AY0)==1 && !(a.status().astat&0x20));
 assert(adsp2181_reset(a.cpu)==0);
 for(unsigned i=0;i<128;i++){a.pm[i*2]=imm(0,i);a.pm[i*2+1]=0x900000|((0x100+i)<<4);}
 b.pm=a.pm;uint64_t start=a.status().cycles;
 assert(adsp2181_run(a.cpu,256)==0);for(unsigned i=0;i<256;i++)assert(adsp2181_run(b.cpu,1)==0);
 assert(a.dm==b.dm && a.get(ADSP2181_REG_AX0)==127 && b.get(ADSP2181_REG_AX0)==127 && a.status().pc==256 && b.status().pc==256 && a.status().cycles-start==b.status().cycles);
}
int main(){test_memory_io_sport_overlay();test_alu_mac();test_shifts();test_dag_loops_stack_banks();test_irq_idma_halt();test_exponent_divide_and_slicing();puts("ADSP2181 executed PM/DM/I/O, SPORT/overlay/reentry, ALU/saturation/constant, source MAC product patterns, shift/exponent/divide vectors, circular/reverse DAG/PX, loops/calls/banks, level/edge IRQ/RTI, IDMA, slicing and reset/HALT PASS");}
