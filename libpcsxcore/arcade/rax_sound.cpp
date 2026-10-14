/* SPDX-License-Identifier: BSD-3-Clause.
 * Board port of MAME shared/rax.cpp (Phil Bennett) and sound/dmadac.cpp/.h
 * (Aaron Giles), deb2f54fe7300e7a192690318beb860159bfd52a.
 * See rax_sound_LICENSE.txt. No command HLE or replacement sound firmware. */
#include "rax_sound.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

enum { BDMA_INT=1,BDMA_EXT,BDMA_CTRL,BDMA_COUNT,PF_DATA,
 S1_AUTO=15,S1_CTRL=18,S0_AUTO=19,S0_DIV=21,SYS_CTRL=31 };
struct RaxSound {
 RaxSoundConfig cfg{};Adsp2181 *cpu=nullptr;
 std::array<uint32_t,0x4000> pm{};
 std::array<uint16_t,0x2000*5> banks{};
 std::array<uint16_t,0x1fe0> internal{};
 std::array<uint16_t,32> ctrl{};
 struct Dac {std::array<int16_t,32768> ring{};uint32_t in=0,out=0;bool enabled=false;} dac[2];
 RaxSoundStatus stats{};
 uint64_t clocks=0,audio_phase=0;
 uint32_t rom_bank=0,rate=48000,dma_end_address=0;
 uint16_t input=0,output=0,size=0,inc=0,base=0;
 unsigned overlay=0,data_bank=0,ireg=0;
 bool pf0=true,busy=false,in_dma=false,dma_pending=false,autobuf_pending=false;
 long double dma_due=0,autobuf_due=0,autobuf_period=0;
 std::array<float,256> audio{};std::array<uint64_t,128> sample_clocks{};size_t audio_count=0;
 ~RaxSound(){adsp2181_destroy(cpu);}
 void fault(uint32_t f){stats.faults|=f;}
 uint16_t data_read(uint16_t a) const {
  a&=0x3fff;
  if(a<0x2000)return banks[(overlay?1+data_bank:0)*0x2000+a];
  if(a<0x3fe0)return internal[a-0x2000];
  return a==0x3fe5?uint16_t(pf0):ctrl[a-0x3fe0];
 }
 void enable(unsigned ch,bool v){dac[ch].enabled=v;if(!v)dac[ch].in=dac[ch].out=0;}
 void emit(){if(audio_count && cfg.audio)cfg.audio(cfg.opaque,audio.data(),audio_count,rate,sample_clocks.data());audio_count=0;}
 void audio_tick(){
  audio_phase+=rate;
  while(audio_phase>=RAX_SOUND_CLOCK){
   audio_phase-=RAX_SOUND_CLOCK;bool nonzero=false;
   for(unsigned ch=0;ch<2;ch++){
    auto &d=dac[ch];int16_t s=0;
    if(d.out!=d.in){s=d.ring[d.out];d.out=(d.out+1)&32767;}
    else if(d.enabled)stats.dac_underruns++;
    audio[2*audio_count+ch]=float(s)*(1.0f/32767.0f);nonzero|=s!=0;
   }
   sample_clocks[audio_count]=clocks;stats.dac_frames++;stats.dac_nonzero+=nonzero;audio_count++;
   if(audio_count==audio.size()/2)emit();
  }
 }
 void dma_start(uint16_t value){
  ctrl[BDMA_COUNT]=value&0x3fff;
  if(in_dma){fault(RAX_FAULT_RECURSIVE_DMA);return;}
  if(ctrl[BDMA_CTRL]&4){fault(RAX_FAULT_DMA_WRITE);return;}
  const unsigned type=ctrl[BDMA_CTRL]&3;
  const unsigned bytes=type==0?3:type==1?2:1;
  uint32_t address=((ctrl[BDMA_CTRL]>>8)<<14)|ctrl[BDMA_EXT];
  uint64_t off=uint64_t(rom_bank)*0x400000+address;
  uint64_t length=uint64_t(ctrl[BDMA_COUNT])*bytes;
  if(off>cfg.rom_size || length>cfg.rom_size-off){fault(RAX_FAULT_ROM_BOUNDS);return;}
  in_dma=true;stats.bdma_transfers++;stats.bdma_words+=ctrl[BDMA_COUNT];
  const unsigned count=ctrl[BDMA_COUNT];
  for(unsigned i=0;i<count;i++){
   uint32_t word=cfg.rom[off++];
   if(type==0){word=(word<<8)|cfg.rom[off++];word=(word<<8)|cfg.rom[off++];}
   else if(type==1)word=(word<<8)|cfg.rom[off++];
   else if(type==2)word<<=8;
   if(type==0)pm[ctrl[BDMA_INT]&0x3fff]=word;
   else data_write(ctrl[BDMA_INT]&0x3fff,word);
   ctrl[BDMA_INT]++;address+=bytes;
   if(stats.faults)break;
  }
  in_dma=false;dma_end_address=address;dma_due=(long double)clocks+count;dma_pending=true;
 }
 void data_write(uint16_t a,uint16_t v){
  a&=0x3fff;
  if(a<0x2000){banks[(overlay?1+data_bank:0)*0x2000+a]=v;return;}
  if(a<0x3fe0){internal[a-0x2000]=v;return;}
  unsigned r=a-0x3fe0;ctrl[r]=v;
  switch(r){
   case BDMA_INT:case BDMA_EXT:ctrl[r]=v&0x3fff;break;
   case BDMA_CTRL:ctrl[r]=v&0xff0f;break;
   case BDMA_COUNT:dma_start(v);break;
   case S1_AUTO:if(!(v&2))enable(1,false);break;
   case S0_AUTO:if(!(v&2)){enable(0,false);autobuf_pending=false;}break;
   case S1_CTRL:if(((v>>4)&3)>=2)fault(RAX_FAULT_COMPANDING);break;
   default:break; // Source stores all other controls (logging only).
  }
 }
 uint16_t io_read(uint16_t a){
  if((a&0x7ff)!=3)return 0xffff;
  adsp2181_irq(cpu,ADSP2181_IRQL0,0);stats.host_reads++;return input;
 }
 void io_write(uint16_t a,uint16_t v){
  switch(a&0x7ff){case 0:data_bank=v&3;break;case 1:rom_bank=v;break;
   case 3:output=v;pf0=false;if(!stats.replies)stats.first_reply_clock=clocks;stats.replies++;break;default:break;}
 }
 void sport(unsigned which){
  if(which!=0)return;
  stats.sport_transmits++;
  if((ctrl[SYS_CTRL]&0x1000) && (ctrl[S0_AUTO]&2)){
   ireg=(ctrl[S0_AUTO]>>9)&7;unsigned mreg=((ctrl[S0_AUTO]>>7)&3)|(ireg&4);
   inc=adsp2181_get_register(cpu,ADSP2181_REG_M0+mreg);
   size=adsp2181_get_register(cpu,ADSP2181_REG_L0+ireg);
   base=uint16_t(adsp2181_get_register(cpu,ADSP2181_REG_I0+ireg)-inc);
   adsp2181_set_register(cpu,ADSP2181_REG_I0+ireg,base);
   unsigned next_rate=RAX_SOUND_CLOCK/(16u*(uint32_t(ctrl[S0_DIV])+1));
   if(next_rate!=rate){emit();rate=next_rate;audio_phase=0;}
   enable(0,true);enable(1,true);
   if(inc){
    autobuf_period=(long double)(2u*(uint32_t(ctrl[S0_DIV])+1))*size/inc;
    autobuf_due=(long double)clocks+autobuf_period;autobuf_pending=true;
   }
   return;
  }
  enable(0,false);enable(1,false);autobuf_pending=false;
 }
 void autobuffer(){
  unsigned count=size/(4u*(inc?inc:1));
  if(count>256){fault(RAX_FAULT_AUTOBUFFER);return;}
  uint32_t reg=adsp2181_get_register(cpu,ADSP2181_REG_I0+ireg);
  std::array<int16_t,256> buffer{};
  for(unsigned i=0;i<count;i++){buffer[i]=data_read(reg);reg+=inc;}
  for(unsigned ch=0;ch<2;ch++){
   auto &d=dac[ch];if(!d.enabled)continue;
   for(unsigned i=0;i<count/2;i++){
    unsigned next=(d.in+1)&32767;
    if(next==d.out){stats.dac_overruns+=(count/2)-i;break;}
    d.ring[d.in]=buffer[2*i+ch];d.in=next;
   }
  }
  if(reg>=uint32_t(base)+size)reg=base;
  adsp2181_set_register(cpu,ADSP2181_REG_I0+ireg,reg);stats.autobuffers++;
 }
 void events(){
  if(dma_pending && (long double)clocks>=dma_due){
   dma_pending=false;ctrl[BDMA_COUNT]=0;ctrl[BDMA_EXT]=dma_end_address&0x3fff;
   ctrl[BDMA_CTRL]=(ctrl[BDMA_CTRL]&0xff)|(((dma_end_address>>14)&0xff)<<8);
   stats.bdma_completions++;
   if(ctrl[BDMA_CTRL]&8){stats.bdma_resets++;adsp2181_reset(cpu);}
   else {adsp2181_irq(cpu,ADSP2181_BDMA,1);adsp2181_irq(cpu,ADSP2181_BDMA,0);}
  }
  while(autobuf_pending && !stats.faults && (long double)clocks>=autobuf_due){
   autobuffer();
   if(autobuf_period>0)autobuf_due+=autobuf_period;
   else autobuf_pending=false; // Zero period is a one-shot generic timer.
  }
 }
 static uint32_t pr(void *p,uint16_t a){return ((RaxSound *)p)->pm[a];}
 static void pw(void *p,uint16_t a,uint32_t v){((RaxSound *)p)->pm[a]=v;}
 static uint16_t dr(void *p,uint16_t a){return ((RaxSound *)p)->data_read(a);}
 static void dw(void *p,uint16_t a,uint16_t v){((RaxSound *)p)->data_write(a,v);}
 static uint16_t ir(void *p,uint16_t a){return ((RaxSound *)p)->io_read(a);}
 static void iw(void *p,uint16_t a,uint16_t v){((RaxSound *)p)->io_write(a,v);}
 static void tx(void *p,unsigned which,uint32_t){((RaxSound *)p)->sport(which);}
 static void ov(void *p,uint32_t v){auto &s=*(RaxSound *)p;if(v>1)s.fault(RAX_FAULT_OVERLAY);else s.overlay=v;}
 static void diag(void *p,uint16_t,uint32_t){((RaxSound *)p)->fault(RAX_FAULT_CPU);}
 void reset(){
  adsp2181_reset(cpu);
  for(unsigned i=0;i<32;i++)pm[i]=(uint32_t(cfg.rom[3*i])<<16)|(uint32_t(cfg.rom[3*i+1])<<8)|cfg.rom[3*i+2];
  pf0=true;rom_bank=0;size=inc=ireg=0;ctrl.fill(0);overlay=data_bank=0;
  // Generic timers with no configured period retain their existing deadlines.
  // Generic latch reset clears pending flags, preserving both latched values.
  // DMADAC has no device_reset override: its queue/rate/enabled state survives.
 }
};
extern "C" {
int rax_sound_create(const RaxSoundConfig *c,RaxSound **out){
 if(!out || !c || !c->rom || c->rom_size<96)return -1;*out=nullptr;
 auto *s=new(std::nothrow) RaxSound;if(!s)return -1;s->cfg=*c;
 Adsp2181Config cfg{};cfg.opaque=s;cfg.program_read=RaxSound::pr;cfg.program_write=RaxSound::pw;
 cfg.data_read=RaxSound::dr;cfg.data_write=RaxSound::dw;cfg.io_read=RaxSound::ir;cfg.io_write=RaxSound::iw;
 cfg.sport_tx=RaxSound::tx;cfg.dmovlay=RaxSound::ov;cfg.diagnostic=RaxSound::diag;
 if(adsp2181_create(&cfg,&s->cpu)){delete s;return -1;}s->reset();*out=s;return 0;
}
void rax_sound_destroy(RaxSound *s){if(s && !s->busy)delete s;}
int rax_sound_reset(RaxSound *s){if(!s || s->busy)return -1;s->busy=true;s->reset();s->busy=false;return 0;}
int rax_sound_run(RaxSound *s,uint64_t clocks){
 if(!s || s->busy || s->stats.faults || clocks>UINT64_MAX-s->clocks)return -1;
 s->busy=true;s->events();
 for(uint64_t i=0;i<clocks && !s->stats.faults;i++){
  adsp2181_run(s->cpu,1);s->clocks++;s->audio_tick();s->events();
 }
 s->emit();s->busy=false;return s->stats.faults?-1:0;
}
int rax_sound_host_write(RaxSound *s,uint16_t v){if(!s || s->busy)return -1;s->input=v;s->stats.host_writes++;adsp2181_irq(s->cpu,ADSP2181_IRQL0,1);return 0;}
int rax_sound_host_read(RaxSound *s,uint16_t *v){if(!s || !v || s->busy)return -1;s->pf0=true;*v=s->output;return 0;}
void rax_sound_status(const RaxSound *s,RaxSoundStatus *v){
 if(!s || !v)return;*v=s->stats;adsp2181_status(s->cpu,&v->cpu);
 v->rom_bank=s->rom_bank;v->sample_rate=s->rate;v->input_latch=s->input;v->output_latch=s->output;
 v->bdma_count=s->ctrl[BDMA_COUNT];v->buffer_size=s->size;v->buffer_increment=s->inc;v->buffer_base=s->base;
 v->pf0=s->pf0;v->overlay=s->overlay;v->data_bank=s->data_bank;v->dma_pending=s->dma_pending;
 for(unsigned i=0;i<2;i++)v->dac_enabled[i]=s->dac[i].enabled;
}
uint32_t rax_sound_program_read(const RaxSound *s,uint16_t a){return s?s->pm[a&0x3fff]:0xffffff;}
int rax_sound_program_write(RaxSound *s,uint16_t a,uint32_t v){if(!s || s->busy)return -1;s->pm[a&0x3fff]=v&0xffffff;return 0;}
uint16_t rax_sound_data_read(const RaxSound *s,uint16_t a){return s?s->data_read(a):0xffff;}
int rax_sound_data_write(RaxSound *s,uint16_t a,uint16_t v){if(!s || s->busy)return -1;s->busy=true;s->data_write(a,v);s->busy=false;return s->stats.faults?-1:0;}
int rax_sound_io_read(RaxSound *s,uint16_t a,uint16_t *v){if(!s || !v || s->busy)return -1;*v=s->io_read(a);return 0;}
int rax_sound_io_write(RaxSound *s,uint16_t a,uint16_t v){if(!s || s->busy)return -1;s->io_write(a,v);return 0;}
uint32_t rax_sound_cpu_register(const RaxSound *s,unsigned r){return s?adsp2181_get_register(s->cpu,r):0;}
int rax_sound_set_cpu_register(RaxSound *s,unsigned r,uint32_t v){return s && !s->busy?adsp2181_set_register(s->cpu,r,v):-1;}
}
