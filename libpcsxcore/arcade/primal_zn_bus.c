/* GPL-2.0-or-later. MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * sony/zn.cpp primrag2_state (BSD-3-Clause; see sony_zn_LICENSE.txt).
 * Preserve the distinct 16-bit task-file and paired-data-latch maps. */
#include "primal_zn_bus.h"
#include <stdlib.h>
#define WATCHDOG_NS UINT64_C(600000000)
struct PrimalZnBus { PrimalZnBusConfig config;PrimalZnBusStatus status;Vt83c461 *via; };
static void arm_watchdog(PrimalZnBus *s) {
    s->status.watchdog_deadline=s->status.now_ns>UINT64_MAX-WATCHDOG_NS?UINT64_MAX:s->status.now_ns+WATCHDOG_NS;
}
int primal_zn_bus_create(const PrimalZnBusConfig *cfg,PrimalZnBus **out) {
    if(!cfg || !cfg->ata || !out)return -1;*out=NULL;
    PrimalZnBus *s=calloc(1,sizeof(*s));if(!s)return -1;s->config=*cfg;
    if(vt83c461_create(cfg->ata,&s->via)){free(s);return -1;}
    arm_watchdog(s);*out=s;return 0;
}
void primal_zn_bus_destroy(PrimalZnBus *s){if(s){vt83c461_destroy(s->via);free(s);}}
int primal_zn_bus_sync(PrimalZnBus *s,uint64_t ns) {
    if(!s || ns<s->status.now_ns || ns>INT64_MAX)return -1;
    s->status.now_ns=ns;
    if(ata_storage_run_until(s->config.ata,ns))s->status.faults|=1;
    if(!s->status.reset_pending && ns>=s->status.watchdog_deadline){s->status.reset_pending=1;s->status.watchdog_resets++;}
    return s->status.faults?-1:s->status.reset_pending?1:0;
}
void primal_zn_bus_reset(PrimalZnBus *s){if(s){vt83c461_reset(s->via);s->status.reset_pending=0;arm_watchdog(s);}}
void primal_zn_bus_ata_irq(PrimalZnBus *s,int state) {
    if(!s)return;
    s->status.ata_irq=state!=0;
    s->status.irq10=(state && (s->status.irq_control&1))?1:0;
    if(s->config.irq10)s->config.irq10(s->config.opaque,s->status.irq10);
}
static uint16_t read16(PrimalZnBus *s,uint32_t a,uint16_t mask) {
    if(a>=0x1f7e8000 && a<0x1f7e8004)return s->status.irq_control;
    if(a>=0x1f7f4000 && a<0x1f7f5000){
        uint32_t o=(a-0x1f7f4000)/2;
        if(o==0x1f0/2){uint32_t pair=vt83c461_read_cs0(s->via,0,UINT32_MAX);s->status.data_latch=pair>>16;return pair&0xffff;}
        if(o==0x1f2/2)return s->status.data_latch;
        return 0xffff;
    }
    uint32_t o=(a-0x1f7e4000)/2,shift=16*(o&1);
    if(o>=0x30/2 && o<0x40/2)return vt83c461_config_read(s->via,(o/2)&3)>>shift;
    if(o>=0x1f0/2 && o<0x1f8/2)return vt83c461_read_cs0(s->via,(o/2)&1,(uint32_t)mask<<shift)>>shift;
    if(o>=0x3f0/2 && o<0x3f8/2)return vt83c461_read_cs1(s->via,(o/2)&1,(uint32_t)mask<<shift)>>shift;
    return 0xffff;
}
static void write16(PrimalZnBus *s,uint32_t a,uint16_t data,uint16_t mask) {
    if(a>=0x1f000000 && a<0x1f000004){s->status.watchdog_kicks++;arm_watchdog(s);return;}
    if(a>=0x1f7e8000 && a<0x1f7e8004){s->status.irq_control=(s->status.irq_control&~mask)|(data&mask);return;}
    if(a>=0x1f7f4000 && a<0x1f7f5000)return; // source log-only write, no ATA payload
    uint32_t o=(a-0x1f7e4000)/2,shift=16*(o&1);
    if(o>=0x30/2 && o<0x40/2)vt83c461_config_write(s->via,(o/2)&3,(uint32_t)data<<shift);
    else if(o>=0x1f0/2 && o<0x1f8/2)vt83c461_write_cs0(s->via,(o/2)&1,(uint32_t)data<<shift,(uint32_t)mask<<shift);
    else if(o>=0x3f0/2 && o<0x3f8/2)vt83c461_write_cs1(s->via,(o/2)&1,(uint32_t)data<<shift,(uint32_t)mask<<shift);
}
static int mapped(uint32_t a,int write) {
    return (write && a>=0x1f000000 && a<0x1f000004) || (a>=0x1f7e4000 && a<0x1f7e5000) ||
        (a>=0x1f7f4000 && a<0x1f7f5000) || (a>=0x1f7e8000 && a<0x1f7e8004);
}
static int valid(uint32_t a,unsigned w) {return (w==1 || w==2 || w==4) && !(a&(w-1)) && mapped(a+w-1,1);}
int primal_zn_bus_read(PrimalZnBus *s,uint32_t a,unsigned w,uint32_t *data) {
    if(!s || !data || !mapped(a,0))return 0;if(!valid(a,w))return -1;
    *data=0;
    if(w==1){unsigned shift=(a&1)*8;*data=(read16(s,a&~1u,0xffu<<shift)>>shift)&255;}
    else for(unsigned i=0;i<w;i+=2)*data|=(uint32_t)read16(s,a+i,0xffff)<<(i*8);
    return 1;
}
int primal_zn_bus_write(PrimalZnBus *s,uint32_t a,unsigned w,uint32_t data) {
    if(!s || !mapped(a,1))return 0;if(!valid(a,w))return -1;
    if(w==1){unsigned shift=(a&1)*8;write16(s,a&~1u,data<<shift,0xffu<<shift);}
    else for(unsigned i=0;i<w;i+=2)write16(s,a+i,data>>(i*8),0xffff);
    return 1;
}
int primal_zn_bus_dma_read(PrimalZnBus *s,uint8_t *ram,size_t bytes,uint32_t a,uint32_t words) {
    if(!s || !ram || (a&3) || (uint64_t)a+(uint64_t)words*4>bytes)return -1;
    s->status.dma_reads++;s->status.dma_read_words+=words;
    for(uint64_t i=0;i<(uint64_t)words*2;i++){
        uint16_t v=vt83c461_read_cs0(s->via,0,0xffff);ram[a+i*2]=v;ram[a+i*2+1]=v>>8;
    }
    return 0;
}
void primal_zn_bus_dma_write(PrimalZnBus *s){if(s)s->status.dma_writes++;}
void primal_zn_bus_status(const PrimalZnBus *s,PrimalZnBusStatus *out){if(s && out){*out=s->status;vt83c461_status(s->via,&out->via);}}
