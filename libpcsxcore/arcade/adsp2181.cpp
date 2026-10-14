/* SPDX-License-Identifier: BSD-3-Clause. MAME ADSP2181 standalone host API.
 * Source core: Aaron Giles; see adsp2181_LICENSE.txt and generated includes. */
#include "adsp2181.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <new>
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LSB_FIRST
#endif
using u32=uint32_t;
template<typename T> static constexpr uint32_t BIT(T value,unsigned shift,unsigned bits=1) {
 return (uint64_t(value)>>shift)&(bits==64?UINT64_MAX:((uint64_t(1)<<bits)-1));
}
namespace util { static constexpr int32_t sext(uint32_t v,unsigned bits){uint32_t s=1u<<(bits-1);return int32_t(((v&((1u<<bits)-1))^s)-s);} }
static constexpr int32_t adsp_asr32(int32_t v,unsigned shift){return shift>=32?(v<0?-1:0):v>>shift;}
enum { CLEAR_LINE=0 };
#include "adsp2181_internal.h"
struct CallbackScope {Adsp2181 &s;explicit CallbackScope(Adsp2181 &v):s(v){s.callback_depth++;}~CallbackScope(){s.callback_depth--;}};
uint64_t Adsp2181::now() const {
 if(!running || !budget)return cycles;
 uint32_t remaining=m_icount>0?std::min<uint32_t>(m_icount,budget):0;
 return cycles+budget-remaining;
}
inline uint16_t Adsp2181::data_read(uint32_t addr){CallbackScope g(*this);return config.data_read(config.opaque,addr&0x3fff);}
inline void Adsp2181::data_write(uint32_t addr,uint16_t data){CallbackScope g(*this);config.data_write(config.opaque,addr&0x3fff,data);}
inline uint32_t Adsp2181::program_read(uint32_t addr){CallbackScope g(*this);return config.program_read(config.opaque,addr&0x3fff)&0xffffff;}
inline void Adsp2181::program_write(uint32_t addr,uint32_t data){CallbackScope g(*this);config.program_write(config.opaque,addr&0x3fff,data&0xffffff);}
inline uint32_t Adsp2181::opcode_read(){return program_read(m_pc);}
inline uint16_t Adsp2181::io_read(uint32_t addr){CallbackScope g(*this);return config.io_read?config.io_read(config.opaque,addr&0x7ff):0xffff;}
inline void Adsp2181::io_write(uint32_t addr,uint16_t data){CallbackScope g(*this);if(config.io_write)config.io_write(config.opaque,addr&0x7ff,data);}
uint32_t Adsp2181::m_sport_rx_cb(unsigned n){CallbackScope g(*this);return config.sport_rx?config.sport_rx(config.opaque,n):0;}
void Adsp2181::m_sport_tx_cb(unsigned n,uint32_t data,uint32_t mask){(void)mask;CallbackScope g(*this);if(config.sport_tx)config.sport_tx(config.opaque,n,data);}
void Adsp2181::m_dmovlay_cb(uint32_t v){CallbackScope g(*this);if(config.dmovlay)config.dmovlay(config.opaque,v);}
void Adsp2181::m_timer_fired_cb(int v){CallbackScope g(*this);if(config.timer_enable)config.timer_enable(config.opaque,v);}
void Adsp2181::logerror(const char *,...){diagnostics|=1;diagnostic_count++;CallbackScope g(*this);if(config.diagnostic)config.diagnostic(config.opaque,m_ppc,1);}
#include "adsp2181_core.inc"
extern "C" {
int adsp2181_create(const Adsp2181Config *cfg,Adsp2181 **out){
 if(!out || !cfg || !cfg->program_read || !cfg->program_write || !cfg->data_read || !cfg->data_write)return -1;
 *out=nullptr;try{*out=new Adsp2181(*cfg);return 0;}catch(const std::bad_alloc &){return -1;}
}
void adsp2181_destroy(Adsp2181 *s){if(s && !s->running && !s->callback_depth)delete s;}
int adsp2181_reset(Adsp2181 *s){if(!s || s->running || s->callback_depth)return -1;s->running=true;s->budget=0;s->device_reset();s->running=false;return 0;}
int adsp2181_run(Adsp2181 *s,uint64_t cycles){
 if(!s || s->running || s->callback_depth || cycles>UINT64_MAX-s->cycles)return -1;
 s->running=true;
 while(cycles){unsigned n=std::min<uint64_t>(cycles,INT_MAX);s->budget=n;s->m_icount=n;s->execute_run();s->cycles+=n;cycles-=n;}
 s->budget=0;s->running=false;return 0;
}
int adsp2181_irq(Adsp2181 *s,unsigned line,int state){if(!s || line>=10)return -1;s->execute_set_input(line,state!=0);return 0;}
int adsp2181_halt(Adsp2181 *s,int state){if(!s)return -1;s->halted=state!=0;return 0;}
void adsp2181_status(const Adsp2181 *s,Adsp2181Status *v){
 if(!s || !v)return;*v={};v->cycles=s->now();v->instructions=s->instructions;v->interrupts=s->interrupts;v->diagnostic_count=s->diagnostic_count;v->diagnostics=s->diagnostics;
 v->pc=s->m_pc;v->previous_pc=s->m_ppc;v->imask=s->m_imask;v->icntl=s->m_icntl;v->astat=s->m_astat;v->mstat=s->m_mstat;v->sstat=s->m_sstat;v->idle=s->m_idle;v->halt=s->halted;
}
#include "adsp2181_register_api.inc"
int adsp2181_idma_address(Adsp2181 *s,uint16_t a){if(!s || s->running || s->callback_depth)return -1;s->idma_addr_w(a);return 0;}
int adsp2181_idma_read(Adsp2181 *s,uint16_t *v){if(!s || !v || s->running || s->callback_depth)return -1;*v=s->idma_data_r();return 0;}
int adsp2181_idma_write(Adsp2181 *s,uint16_t v){if(!s || s->running || s->callback_depth)return -1;s->idma_data_w(v);return 0;}
}
