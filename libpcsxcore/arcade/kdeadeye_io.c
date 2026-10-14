/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#include "kdeadeye_io.h"
#include "nor_flash.h"
#include <stdlib.h>
#include <string.h>
struct KDeadEyeIO {
  nor_flash_t *flash;
  uint64_t ns;
  KDeadEyeIOStatus status;
};
KDeadEyeIO *kdeadeye_io_create(uint8_t *image, size_t size) {
  KDeadEyeIO *d=calloc(1,sizeof(*d));
  if(!d)return NULL;
  d->flash=nor_flash_create(NOR_FLASH_SHARP_LH28F400_X16,image,size);
  if(!d->flash){free(d);return NULL;}
  for(unsigned p=0;p<2;p++){d->status.gun[p][0]=0x100;d->status.gun[p][1]=0x77;}
  d->status.buttons=0xffff;
  return d;
}
void kdeadeye_io_destroy(KDeadEyeIO *d) {
  if(d){nor_flash_destroy(d->flash);free(d);}
}
int kdeadeye_io_advance_to(KDeadEyeIO *d,uint64_t ns) {
  if(!d || ns<d->ns){if(d)d->status.fault=1;return -1;}
  nor_flash_advance_us(d->flash,ns/1000-d->ns/1000);d->ns=ns;
  return d->status.fault?-1:0;
}
int kdeadeye_io_set_axis(KDeadEyeIO *d,unsigned p,unsigned axis,unsigned v) {
  if(!d || p>=2 || axis>=2 || v>0xffff)return -1;
  d->status.gun[p][axis]=(uint16_t)v;return 0;
}
int kdeadeye_io_trigger(KDeadEyeIO *d,unsigned p,int pressed) {
  if(!d || p>=2 || (pressed!=0 && pressed!=1))return -1;
  if(pressed)d->status.buttons&=~(1u<<p);else d->status.buttons|=1u<<p;
  return 0;
}
static int valid(uint32_t a,unsigned w) {
  return (w==1 || w==2 || w==4) && a<=UINT32_MAX-(w-1);
}
static int port(uint32_t a) {
  if(a<0x1f680080 || a>=0x1f6800c4)return -1;
  uint32_t offset=a-0x1f680080;
  return (offset&15)<4?(int)(offset>>4):-1;
}
int kdeadeye_io_read(KDeadEyeIO *d,uint32_t a,unsigned w,uint32_t *value) {
  if(!d || !value || !valid(a,w))return 0;
  uint32_t out=0,old_word=UINT32_MAX;uint16_t word=0;int handled=0;
  for(unsigned lane=0;lane<w;lane++) {
    uint32_t at=a+lane;uint8_t b=0;int p=port(at);
    if(at>=0x1f380000 && at<0x1f400000) {
      uint32_t offset=(at-0x1f380000)>>1;
      if(offset!=old_word) {
        if(nor_flash_read(d->flash,offset,&word))d->status.fault=1;
        d->status.flash_reads++;old_word=offset;
      }
      b=(uint8_t)(word>>((at&1)*8));handled=1;
    }else if(p>=0) {
      /* All five MAME ports have a 16-bit mask, in 32-bit MMIO slots. */
      unsigned shift=(at&3)*8;
      uint32_t raw=p<4?d->status.gun[p>>1][p&1]:d->status.buttons;
      b=(uint8_t)(raw>>shift);handled=1;
      if(p<4)d->status.gun_reads++;else d->status.button_reads++;
    }
    out|=(uint32_t)b<<(lane*8);
  }
  if(!handled)return 0;
  *value=out;return d->status.fault?-1:1;
}
int kdeadeye_io_write(KDeadEyeIO *d,uint32_t a,unsigned w,uint32_t value,uint32_t mask) {
  if(!d || !valid(a,w))return 0;
  int handled=0,noop=0;uint32_t pending=UINT32_MAX;uint16_t data=0;
  for(unsigned lane=0;lane<w;lane++) {
    uint32_t at=a+lane;
    if(at>=0x1f6800e0 && at<0x1f6800e4){handled=1;noop=1;continue;}
    if(at<0x1f380000 || at>=0x1f400000)continue;
    handled=1;if(!(mask&(0xffu<<(lane*8))))continue;
    uint32_t word=at&~1u;
    if(pending!=word) {
      if(pending!=UINT32_MAX) {
        if(nor_flash_write(d->flash,(pending-0x1f380000)>>1,data))d->status.fault=1;
        d->status.flash_writes++;
      }
      pending=word;data=0;
    }
    data|=(uint16_t)(((value>>(lane*8))&255)<<((at&1)*8));
  }
  if(pending!=UINT32_MAX) {
    if(nor_flash_write(d->flash,(pending-0x1f380000)>>1,data))d->status.fault=1;
    d->status.flash_writes++;
  }
  if(noop)d->status.noop_writes++;
  return !handled?0:d->status.fault?-1:1;
}
void kdeadeye_io_status(const KDeadEyeIO *d,KDeadEyeIOStatus *s) {
  if(s){memset(s,0,sizeof(*s));if(d)*s=d->status;}
}
