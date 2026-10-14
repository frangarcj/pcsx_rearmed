/* BSD-3-Clause. Source-map acceptance and optional authentic firmware probe. */
#include "rax_sound.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
struct Fixture {
 std::vector<uint8_t> rom=std::vector<uint8_t>(0x800000);
 std::vector<float> samples;RaxSound *s=nullptr;unsigned rate=0;
 static void audio(void *p,const float *v,size_t n,uint32_t rate,const uint64_t *ticks){
  for(size_t i=1;i<n;i++)assert(ticks[i]>ticks[i-1]);
  auto &f=*(Fixture *)p;f.rate=rate;f.samples.insert(f.samples.end(),v,v+2*n);
  assert(rax_sound_run(f.s,1)==-1 && rax_sound_reset(f.s)==-1 && rax_sound_host_write(f.s,0)==-1);
  assert(rax_sound_program_write(f.s,0,0)==-1 && rax_sound_data_write(f.s,0,0)==-1 && rax_sound_io_write(f.s,0,0)==-1);
  assert(rax_sound_set_cpu_register(f.s,ADSP2181_REG_PC,0)==-1);
  rax_sound_destroy(f.s); // Must be refused during callback.
 }
 void word(unsigned a,uint32_t v){rom[3*a]=v>>16;rom[3*a+1]=v>>8;rom[3*a+2]=v;}
 Fixture(){word(0,0x18000f);}
 void create(){RaxSoundConfig c{};c.rom=rom.data();c.rom_size=rom.size();c.opaque=this;c.audio=audio;assert(rax_sound_create(&c,&s)==0);}
 ~Fixture(){rax_sound_destroy(s);}
 void dw(unsigned a,uint16_t v){assert(rax_sound_data_write(s,a,v)==0);}
 void iw(unsigned a,uint16_t v){assert(rax_sound_io_write(s,a,v)==0);}
 uint16_t ir(unsigned a){uint16_t v;assert(rax_sound_io_read(s,a,&v)==0);return v;}
 void reg(unsigned r,unsigned v){assert(rax_sound_set_cpu_register(s,r,v)==0);}
 void run(uint64_t n){assert(rax_sound_run(s,n)==0);}
 RaxSoundStatus status(){RaxSoundStatus v;rax_sound_status(s,&v);return v;}
 void dma(unsigned type,unsigned page,unsigned ext,unsigned dst,unsigned count){dw(0x3fe1,dst);dw(0x3fe2,ext);dw(0x3fe3,(page<<8)|type);dw(0x3fe4,count);}
};
static void maps_and_reset(){
 Fixture f;f.word(31,0xa1b2c3);f.word(32,0x123456);f.create();
 assert(rax_sound_program_read(f.s,31)==0xa1b2c3 && rax_sound_program_read(f.s,32)==0);
 assert(rax_sound_program_write(f.s,32,0xffabcdef)==0);f.dw(0x100,0x1111);f.dw(0x2000,0xaaaa);f.dw(0x3fdf,0xbbbb);
 f.reg(ADSP2181_REG_DMOVLAY,1);
 for(unsigned bank=0;bank<4;bank++){f.iw(0,bank);assert(rax_sound_data_read(f.s,0x100)==0);f.dw(0x100,0x2000+bank);}
 for(unsigned bank=0;bank<4;bank++){f.iw(0,bank);assert(rax_sound_data_read(f.s,0x100)==0x2000+bank);}
 f.iw(0,7);assert(f.status().data_bank==3 && f.ir(0)==0xffff && f.ir(0x7ff)==0xffff);
 f.iw(1,0x1234);assert(f.status().rom_bank==0x1234);
 f.iw(3,0x4567);assert(!f.status().pf0);uint16_t reply;assert(rax_sound_host_read(f.s,&reply)==0 && reply==0x4567 && f.status().pf0);
 assert(rax_sound_host_write(f.s,0xbeef)==0 && rax_sound_host_write(f.s,0x4321)==0 && f.ir(3)==0x4321);
 f.dw(0x3fe5,0);assert(rax_sound_data_read(f.s,0x3fe5)==1);
 assert(rax_sound_reset(f.s)==0);assert(rax_sound_program_read(f.s,32)==0xabcdef && rax_sound_data_read(f.s,0x100)==0x1111);
 assert(rax_sound_data_read(f.s,0x2000)==0xaaaa && rax_sound_data_read(f.s,0x3fdf)==0xbbbb);
 assert(f.status().rom_bank==0 && f.status().overlay==0 && f.ir(3)==0x4321);
 assert(rax_sound_host_read(f.s,&reply)==0 && reply==0x4567);
}
static void bdma_vectors(){
 for(unsigned type=0;type<4;type++){
  Fixture f;f.rom[0x400100]=0x12;f.rom[0x400101]=0x34;f.rom[0x400102]=0x56;f.rom[0x400103]=0x78;f.create();f.iw(1,1);
  f.dma(type,0,0x100,0x100,2);
  assert(type==0?rax_sound_program_read(f.s,0x100)==0x123456:rax_sound_data_read(f.s,0x100)==(type==1?0x1234:type==2?0x1200:0x12));
  assert(f.status().bdma_count==2 && f.status().dma_pending);f.run(1);assert(f.status().bdma_count==2);f.run(1);
  assert(f.status().bdma_completions==1 && f.status().bdma_count==0 && rax_sound_data_read(f.s,0x3fe1)==0x102);
  assert(rax_sound_data_read(f.s,0x3fe2)==0x100+2*(type==0?3:type==1?2:1));
 }
 Fixture f;f.rom[0x3fffff]=0xab;f.rom[0x400000]=0xcd;f.create();f.dma(1,255,0x3fff,0x3fff,1);
 assert(rax_sound_data_read(f.s,0x3fff)==0xabcd && rax_sound_data_read(f.s,0x3fe1)==0x4000);f.run(1);
 assert(rax_sound_data_read(f.s,0x3fe2)==1 && rax_sound_data_read(f.s,0x3fe3)==1);
 Fixture irq;irq.create();assert(rax_sound_program_write(irq.s,28,0x400550)==0 && rax_sound_program_write(irq.s,29,0x0a001f)==0);
 irq.reg(ADSP2181_REG_IMASK,8);irq.dma(3,0,0x100,0x100,1);irq.run(3);
 assert(irq.status().cpu.interrupts==1 && rax_sound_cpu_register(irq.s,ADSP2181_REG_AX0)==0x55);
 Fixture reset;reset.rom[0x100]=0x18;reset.rom[0x101]=0x00;reset.rom[0x102]=0x0f;reset.create();
 reset.reg(ADSP2181_REG_AX0,0x9988);reset.reg(ADSP2181_REG_PC,0x100);
 reset.dma(8,0,0x100,0,1);reset.run(1);assert(reset.status().bdma_resets==1 && reset.status().cpu.pc==0);
 assert(rax_sound_cpu_register(reset.s,ADSP2181_REG_AX0)==0x9988 && reset.status().cpu.cycles==1);
 // Soft reset must preserve generic-timer deadlines and board RAM.
 Fixture pending;pending.create();pending.dma(3,0,0x100,0x100,5);pending.run(2);assert(rax_sound_reset(pending.s)==0);
 assert(pending.status().dma_pending);pending.run(3);assert(pending.status().bdma_completions==1);
}
static void host_irq(){
 Fixture f;f.create();assert(rax_sound_program_write(f.s,12,0x010030)==0 && rax_sound_program_write(f.s,13,0x018030)==0 && rax_sound_program_write(f.s,14,0x0a001f)==0);
 f.reg(ADSP2181_REG_PC,0x80);f.reg(ADSP2181_REG_IMASK,0x80);assert(rax_sound_host_write(f.s,0x1234)==0);f.run(3);
 assert(f.status().cpu.interrupts==1 && f.status().host_reads==1 && f.status().replies==1 && f.status().output_latch==0x1234 && !f.status().pf0);
 f.run(5);assert(f.status().cpu.interrupts==1);
}
static void autobuffer(){
 Fixture f;f.create();f.dw(0x100,32767);f.dw(0x101,0x8000);f.dw(0x102,0x1234);f.dw(0x103,0xedcc);
 f.reg(ADSP2181_REG_I0,0x101);f.reg(ADSP2181_REG_M0,1);f.reg(ADSP2181_REG_L0,16);
 f.dw(0x3fff,0x1000);f.dw(0x3ff3,2);f.dw(0x3ff5,19);
 assert(rax_sound_program_write(f.s,0,0x0d0c90)==0 && rax_sound_program_write(f.s,1,0x18001f)==0);
 f.run(640);assert(f.status().sample_rate==52093 && f.status().buffer_base==0x100 && f.status().autobuffers==1);
 assert(rax_sound_cpu_register(f.s,ADSP2181_REG_I0)==0x104 && f.samples.size()==2 && f.samples[0]==0);
 f.run(321);assert(f.samples.size()==6 && f.samples[2]==1.0f && f.samples[3]==float(-32768)*(1.0f/32767.0f));
 assert(f.samples[4]==float(0x1234)*(1.0f/32767.0f) && f.status().dac_nonzero==2);
 f.dw(0x3fef,0);assert(f.status().dac_enabled[0] && !f.status().dac_enabled[1]);
 f.run(320);assert(f.status().autobuffers==2 && rax_sound_cpu_register(f.s,ADSP2181_REG_I0)==0x108);
 f.dw(0x3ff3,0);f.run(640);assert(f.status().autobuffers==2 && !f.status().dac_enabled[0]);
}
static void faults(){
 Fixture a;a.create();a.iw(1,2);assert(rax_sound_data_write(a.s,0x3fe4,1)==-1 && a.status().faults==RAX_FAULT_ROM_BOUNDS);
 Fixture b;b.create();b.dw(0x3fe3,4);assert(rax_sound_data_write(b.s,0x3fe4,1)==-1 && b.status().faults==RAX_FAULT_DMA_WRITE);
 Fixture c;c.create();c.reg(ADSP2181_REG_DMOVLAY,2);assert(c.status().faults==RAX_FAULT_OVERLAY);
 Fixture d;d.create();assert(rax_sound_data_write(d.s,0x3ff2,0x30)==-1 && d.status().faults==RAX_FAULT_COMPANDING);
}
static void timer_edges(){
 for(unsigned size:{0,4,2048}){
  Fixture f;f.create();f.reg(ADSP2181_REG_I0,0x103);f.reg(ADSP2181_REG_M0,size==4?3:1);f.reg(ADSP2181_REG_L0,size);
  f.dw(0x3fff,0x1000);f.dw(0x3ff3,2);
  assert(rax_sound_program_write(f.s,0,0x0d0c90)==0 && rax_sound_program_write(f.s,1,0x18001f)==0);
  if(!size){f.run(1);assert(f.status().autobuffers==1);f.run(16);assert(f.status().autobuffers==1);}
  else if(size==4){f.run(2);assert(f.status().autobuffers==0);f.run(1);assert(f.status().autobuffers==1);f.run(5);assert(f.status().autobuffers==3);}
  else {assert(rax_sound_run(f.s,4096)==-1 && f.status().faults==RAX_FAULT_AUTOBUFFER);}
 }
}
static int firmware(const char *path,const char *commands,unsigned seconds){
 Fixture f;FILE *in=fopen(path,"rb");if(!in){perror(path);return 1;}
 if(fread(f.rom.data(),1,f.rom.size(),in)!=f.rom.size() || fgetc(in)!=EOF){fclose(in);return 1;}fclose(in);f.create();
 std::vector<std::pair<uint64_t,uint16_t>> events;
 if(commands){
  FILE *csv=fopen(commands,"r");if(!csv){perror(commands);return 1;}
  unsigned long long clock;unsigned word;char line[128];
  while(fgets(line,sizeof(line),csv)){
   char extra;int fields=sscanf(line,"%llu,%x %c",&clock,&word,&extra);
   if(fields!=2 || word>65535 || (!events.empty() && clock<events.back().first)){fclose(csv);fputs("invalid command CSV: DSP_clock,hex_word\n",stderr);return 1;}
   events.emplace_back(clock,word);
  }
  fclose(csv);
 }
 size_t event=0;uint64_t elapsed=0;
 for(unsigned slice=0;slice<10*seconds;slice++){
  uint64_t end=uint64_t(slice+1)*(RAX_SOUND_CLOCK/10);int r=0;
  while(event<events.size() && events[event].first<=end){
   if(events[event].first>elapsed){r=rax_sound_run(f.s,events[event].first-elapsed);elapsed=events[event].first;if(r)break;}
   assert(rax_sound_host_write(f.s,events[event++].second)==0);
  }
  if(!r){r=rax_sound_run(f.s,end-elapsed);elapsed=end;}
  auto s=f.status();
  printf("RAX t=%.1f pc=%04x cycles=%llu insn=%llu irq=%llu fault=%x dma=%llu/%llu reset=%llu host=%llu replies=%llu out=%04x pf=%u sport=%llu buffers=%llu rate=%u size=%u inc=%u nonzero=%llu underrun=%llu overrun=%llu\n",
   double(slice+1)/10,s.cpu.pc,(unsigned long long)s.cpu.cycles,(unsigned long long)s.cpu.instructions,(unsigned long long)s.cpu.interrupts,s.faults,
   (unsigned long long)s.bdma_completions,(unsigned long long)s.bdma_words,(unsigned long long)s.bdma_resets,(unsigned long long)s.host_reads,(unsigned long long)s.replies,s.output_latch,s.pf0,
   (unsigned long long)s.sport_transmits,(unsigned long long)s.autobuffers,s.sample_rate,s.buffer_size,s.buffer_increment,(unsigned long long)s.dac_nonzero,(unsigned long long)s.dac_underruns,(unsigned long long)s.dac_overruns);
  fflush(stdout);if(r)return 1;
 }
 auto s=f.status();
 printf("RAX_BOOT first_reply_clock=%llu AUTO=%04x DIV=%04x SYS=%04x IMASK=%04x commands=%zu/%zu\n",(unsigned long long)s.first_reply_clock,rax_sound_data_read(f.s,0x3ff3),rax_sound_data_read(f.s,0x3ff5),rax_sound_data_read(f.s,0x3fff),s.cpu.imask,event,events.size());
 return event==events.size()?0:1;
}
int main(int argc,char **argv){
 if(argc>=3 && !strcmp(argv[1],"--firmware")){
  const char *commands=nullptr;unsigned seconds=2;
  for(int i=3;i<argc;i++){
   if(!strcmp(argv[i],"--commands") && i+1<argc)commands=argv[++i];
   else if(!strcmp(argv[i],"--seconds") && i+1<argc){if(sscanf(argv[++i],"%u",&seconds)!=1 || seconds>60)return 1;}
   else return 1;
  }
  return firmware(argv[2],commands,seconds);
 }
 maps_and_reset();bdma_vectors();host_irq();autobuffer();faults();timer_edges();
 puts("RAX source bootstrap/maps/overlay, host IRQL0/PF0, four BDMA types/bank crossing/deadlines/IRQ/reset, SPORT stereo DAC-rate samples and unsupported-operation faults PASS");
}
