/* SPDX-License-Identifier: BSD-3-Clause. MAME konamigv.cpp, R. Belmont/smf.
 * deb2f54fe7300e7a192690318beb860159bfd52a. See konamigv_LICENSE.txt. */
#include "btchamp_io.h"
#include "nor_flash.h"
#include "upd4701.h"
#include <stdlib.h>
#include <string.h>
struct BtChampIO {
  nor_flash_t *flash;
  Upd4701 track[2];
  uint64_t ns;
  BtChampIOStatus status;
};
BtChampIO *btchamp_io_create(uint8_t *image, size_t size) {
  BtChampIO *d=calloc(1,sizeof(*d));
  if(!d)return NULL;
  d->flash=nor_flash_create(NOR_FLASH_SHARP_LH28F400_X16,image,size);
  if(!d->flash){free(d);return NULL;}
  upd4701_init(&d->track[0]);upd4701_init(&d->track[1]);
  return d;
}
void btchamp_io_destroy(BtChampIO *d) {
  if(d){nor_flash_destroy(d->flash);free(d);}
}
int btchamp_io_advance_to(BtChampIO *d,uint64_t ns) {
  if(!d || ns<d->ns){if(d)d->status.fault=1;return -1;}
  nor_flash_advance_us(d->flash,ns/1000-d->ns/1000);d->ns=ns;
  return d->status.fault?-1:0;
}
void btchamp_io_frame(BtChampIO *d,const uint16_t track[2][2]) {
  if(!d || !track)return;
  for(unsigned p=0;p<2;p++) {
    upd4701_update(&d->track[p],track[p][0],track[p][1]);
    for(unsigned axis=0;axis<2;axis++)d->status.track[p][axis]=track[p][axis]&0xfff;
  }
  d->status.frames++;
}
static int valid(uint32_t a,unsigned w) {
  return (w==1 || w==2 || w==4) && a<=UINT32_MAX-(w-1);
}
int btchamp_io_read(BtChampIO *d,uint32_t a,unsigned w,uint32_t *value) {
  if(!d || !value || !valid(a,w))return 0;
  uint32_t out=0,old_word=UINT32_MAX;uint16_t word=0;int handled=0;
  for(unsigned lane=0;lane<w;lane++) {
    uint32_t at=a+lane;uint8_t b=0;
    if(at>=0x1f380000 && at<0x1f400000) {
      uint32_t offset=(at-0x1f380000)>>1;
      if(offset!=old_word) {
        if(nor_flash_read(d->flash,offset,&word))d->status.fault=1;
        d->status.flash_reads++;old_word=offset;
      }
      b=(uint8_t)(word>>((at&1)*8));handled=1;
    } else if(at>=0x1f680080 && at<0x1f680088) {
      /* Source player 1 owns odd byte lanes, player 2 even lanes. */
      b=upd4701_read_xy(&d->track[(at&1)?0:1],(at-0x1f680080)>>1);
      d->status.track_reads++;handled=1;
    }
    out|=(uint32_t)b<<(lane*8);
  }
  if(!handled)return 0;
  *value=out;return d->status.fault?-1:1;
}
static void control(BtChampIO *d,uint16_t data) {
  for(unsigned p=0;p<2;p++) {
    upd4701_cs(&d->track[p],(data>>1)&1);
    upd4701_reset_x(&d->track[p],!(data&1));
    upd4701_reset_y(&d->track[p],!(data&1));
  }
  d->status.control_writes++;
}
int btchamp_io_write(BtChampIO *d,uint32_t a,unsigned w,uint32_t value,uint32_t mask) {
  if(!d || !valid(a,w))return 0;
  int handled=0;uint32_t pending=UINT32_MAX;uint16_t data=0;
  for(unsigned lane=0;lane<w;lane++) {
    uint32_t at=a+lane;
    int flash=at>=0x1f380000 && at<0x1f400000;
    int ctl=at>=0x1f680088 && at<0x1f68008a;
    if(at>=0x1f6800e0 && at<0x1f6800e4){handled=1;continue;}
    if(!flash && !ctl)continue;
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
    if(pending==0x1f680088)control(d,data);
    else {
      if(nor_flash_write(d->flash,(pending-0x1f380000)>>1,data))d->status.fault=1;
      d->status.flash_writes++;
    }
  }
  return !handled?0:d->status.fault?-1:1;
}
void btchamp_io_status(const BtChampIO *d,BtChampIOStatus *status) {
  if(status){memset(status,0,sizeof(*status));if(d)*status=d->status;}
}
