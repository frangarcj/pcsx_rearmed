/* SPDX-License-Identifier: BSD-3-Clause. Copyright smf (upstream gives no year).
 * Source MAME deb2f54fe7300e7a192690318beb860159bfd52a:
 * devices/machine/vt83c461.cpp and idectrl.cpp, framework-free C port.
 * Keep source branch priority: a multi-lane task-file access is not a loop
 * through adjacent ATA registers. Only CS0 data offset zero consumes two words. */
#include "vt83c461.h"
#include <stdlib.h>
struct Vt83c461 { ata_storage_t *master; Vt83c461Status status; };
int vt83c461_create(ata_storage_t *master,Vt83c461 **out) {
    if(!master || !out)return -1;*out=NULL;
    Vt83c461 *s=calloc(1,sizeof(*s));if(!s)return -1;s->master=master;*out=s;return 0;
}
void vt83c461_destroy(Vt83c461 *s) { free(s); }
void vt83c461_reset(Vt83c461 *s) { if(s)ata_storage_reset(s->master); }
uint32_t vt83c461_config_read(Vt83c461 *s,uint32_t offset) {
    if(!s)return 0;s->status.config_reads++;
    switch(offset) {
    case 1:return s->status.unknown;
    case 2:return s->status.selected;
    case 3:return s->status.selected<16?s->status.config[s->status.selected]:0;
    default:s->status.diagnostics|=1;return 0;
    }
}
void vt83c461_config_write(Vt83c461 *s,uint32_t offset,uint32_t data) {
    if(!s)return;s->status.config_writes++;
    switch(offset) {
    case 1:s->status.unknown=data;break;
    case 2:s->status.selected=data;break;
    case 3:if(s->status.selected<16)s->status.config[s->status.selected]=data;break;
    default:s->status.diagnostics|=1;break;
    }
}
static uint16_t cs_read(Vt83c461 *s,int cs,uint32_t reg,uint16_t mask) {
    /* abstract_ata_interface starts its wired-AND bus result at mem_mask. */
    if(cs){s->status.cs1_reads++;return ata_storage_control_r(s->master,reg,mask)&mask;}
    s->status.cs0_reads++;return ata_storage_command_r(s->master,reg,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE)&mask;
}
static void cs_write(Vt83c461 *s,int cs,uint32_t reg,uint16_t data,uint16_t mask) {
    if(cs){s->status.cs1_writes++;ata_storage_control_w(s->master,reg,data,mask);}
    else{s->status.cs0_writes++;ata_storage_command_w(s->master,reg,data,mask,ATA_STORAGE_SOURCE_TRANSFER_MODE);}
}
static uint32_t read_cs(Vt83c461 *s,int cs,uint32_t offset,uint32_t mask) {
    if(!s)return 0;
    if(mask&0xffff){
        if(mask&0xff){
            uint32_t data=cs_read(s,cs,offset*4,(uint16_t)mask);
            if(!cs && offset==0 && (mask&0xffff0000))
                data|=(uint32_t)cs_read(s,cs,offset*4,(uint16_t)(mask>>16))<<16;
            return data;
        }
        return (uint32_t)cs_read(s,cs,offset*4+1,(uint16_t)(mask>>8))<<8;
    }
    if(mask&0xffff0000){
        if(mask&0xff0000)return (uint32_t)cs_read(s,cs,offset*4+2,(uint16_t)(mask>>16))<<16;
        return (uint32_t)cs_read(s,cs,offset*4+3,(uint16_t)(mask>>24))<<24;
    }
    return 0;
}
static void write_cs(Vt83c461 *s,int cs,uint32_t offset,uint32_t data,uint32_t mask) {
    if(!s)return;
    if(mask&0xffff){
        if(mask&0xff){
            cs_write(s,cs,offset*4,(uint16_t)data,(uint16_t)mask);
            if(!cs && offset==0 && (mask&0xffff0000))
                cs_write(s,cs,offset*4,(uint16_t)(data>>16),(uint16_t)(mask>>16));
        }else cs_write(s,cs,offset*4+1,(uint16_t)(data>>8),(uint16_t)(mask>>8));
    }else if(mask&0xffff0000){
        if(mask&0xff0000)cs_write(s,cs,offset*4+2,(uint16_t)(data>>16),(uint16_t)(mask>>16));
        else cs_write(s,cs,offset*4+3,(uint16_t)(data>>24),(uint16_t)(mask>>24));
    }
}
uint32_t vt83c461_read_cs0(Vt83c461 *s,uint32_t o,uint32_t m){return read_cs(s,0,o,m);}
void vt83c461_write_cs0(Vt83c461 *s,uint32_t o,uint32_t d,uint32_t m){write_cs(s,0,o,d,m);}
uint32_t vt83c461_read_cs1(Vt83c461 *s,uint32_t o,uint32_t m){return read_cs(s,1,o,m);}
void vt83c461_write_cs1(Vt83c461 *s,uint32_t o,uint32_t d,uint32_t m){write_cs(s,1,o,d,m);}
void vt83c461_status(const Vt83c461 *s,Vt83c461Status *out){if(s && out)*out=s->status;}
