#include "../libpcsxcore/arcade/btchamp_io.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static uint32_t rd(BtChampIO *d,uint32_t a,unsigned w) {
  uint32_t v;assert(btchamp_io_read(d,a,w,&v)==1);return v;
}
static void wr(BtChampIO *d,uint32_t a,unsigned w,uint32_t v,uint32_t mask) {
  assert(btchamp_io_write(d,a,w,v,mask)==1);
}
int main(void) {
  uint8_t *image=malloc(BTCHAMP_FLASH_BYTES);assert(image);
  memset(image,255,BTCHAMP_FLASH_BYTES);
  assert(!btchamp_io_create(image,BTCHAMP_FLASH_BYTES-1));
  BtChampIO *d=btchamp_io_create(image,BTCHAMP_FLASH_BYTES);assert(d);
  assert(rd(d,0x1f380000,4)==0xffffffff);
  wr(d,0x1f380000,2,0x90,0xffff);
  assert(rd(d,0x1f380000,4)==0x00ed00b0);
  assert(rd(d,0x1f380001,1)==0 && rd(d,0x1f380002,1)==0xed);
  wr(d,0x1f380000,2,0xff,0xffff);
  wr(d,0x1f380246,2,0x40,0xffff);wr(d,0x1f380246,2,0x1234,0xffff);
  assert(image[0x246]==0x12 && image[0x247]==0x34);
  wr(d,0x1f380000,2,0xff,0xffff);assert(rd(d,0x1f380246,2)==0x1234);
  wr(d,0x1f380246,2,0x20,0xffff);wr(d,0x1f380246,2,0xd0,0xffff);
  assert(rd(d,0x1f380246,2)==0);
  assert(!btchamp_io_advance_to(d,999999999));assert(rd(d,0x1f380246,2)==0);
  assert(!btchamp_io_advance_to(d,1000000000));assert(rd(d,0x1f380246,2)==0x80);
  wr(d,0x1f380000,2,0xff,0xffff);assert(rd(d,0x1f380246,2)==0xffff);
  uint16_t track[2][2]={{0x123,0xabc},{0x567,0xdef}};
  btchamp_io_frame(d,track);
  assert(rd(d,0x1f680080,4)==0x01052367);
  assert(rd(d,0x1f680084,4)==0x0a0dbcef);
  assert(rd(d,0x1f680080,1)==0x67 && rd(d,0x1f680081,1)==0x23);
  wr(d,0x1f680088,2,1,0xffff); /* CS low latches both channels. */
  track[0][0]+=10;track[1][1]+=20;btchamp_io_frame(d,track);
  assert(rd(d,0x1f680080,4)==0x01052367 && rd(d,0x1f680084,4)==0x0a0dbcef);
  wr(d,0x1f680088,2,3,0xffff); /* Release CS. */
  assert(rd(d,0x1f680081,1)==0x2d && rd(d,0x1f680084,1)==3);
  wr(d,0x1f680088,2,2,0xffff); /* Hold both axes in reset. */
  track[0][0]+=5;track[1][1]+=5;btchamp_io_frame(d,track);
  wr(d,0x1f680088,2,3,0xffff);assert(rd(d,0x1f680080,4)==0 && rd(d,0x1f680084,4)==0);
  wr(d,0x1f680088,2,0,0); /* An entirely masked write has no effect. */
  track[0][0]+=7;btchamp_io_frame(d,track);assert(rd(d,0x1f680081,1)==7);
  wr(d,0x1f6800e0,4,0xdeadbeef,~0u); /* Source no-op. */
  uint32_t v;assert(!btchamp_io_read(d,0x1f680088,2,&v));
  assert(!btchamp_io_read(d,0xffffffff,4,&v));
  BtChampIOStatus st;btchamp_io_status(d,&st);assert(!st.fault && st.frames==4);
  assert(btchamp_io_advance_to(d,0)<0);btchamp_io_status(d,&st);assert(st.fault);
  btchamp_io_destroy(d);free(image);
  puts("Beat the Champ: Sharp ID/program/erase time, dual lane counters, latch/reset/mask and no-op PASS");
}
