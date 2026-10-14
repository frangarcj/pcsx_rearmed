#include "../libpcsxcore/arcade/kdeadeye_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t read(KDeadEyeIO *d,uint32_t a,unsigned w) {
  uint32_t v=0xdeadbeef;assert(kdeadeye_io_read(d,a,w,&v)==1);return v;
}
static void write(KDeadEyeIO *d,uint32_t a,unsigned w,uint32_t v,uint32_t mask) {
  assert(kdeadeye_io_write(d,a,w,v,mask)==1);
}
int main(void) {
  const uint32_t f=0x1f380000;uint32_t v=0xfeedbeef;
  uint8_t *image=malloc(KDEADEYE_FLASH_BYTES);assert(image);
  memset(image,0xff,KDEADEYE_FLASH_BYTES);image[0]=0x12;image[1]=0x34;
  assert(!kdeadeye_io_create(image,KDEADEYE_FLASH_BYTES-1));
  KDeadEyeIO *d=kdeadeye_io_create(image,KDEADEYE_FLASH_BYTES);assert(d);
  assert(read(d,f,2)==0x1234);assert(read(d,f,4)==0xffff1234);
  assert(read(d,f,1)==0x34 && read(d,f+1,1)==0x12);
  for(unsigned p=0;p<2;p++) {
    uint32_t x=0x1f680080+p*0x20,y=x+0x10;
    assert(read(d,x,4)==0x100 && read(d,y,4)==0x77);
    assert(kdeadeye_io_set_axis(d,p,0,0xabcd)==0);
    assert(kdeadeye_io_set_axis(d,p,1,0xffff)==0);
    assert(read(d,x,4)==0xabcd && read(d,x,1)==0xcd && read(d,x+1,1)==0xab);
    assert(read(d,x+2,2)==0 && read(d,y,4)==0xffff);
    assert(kdeadeye_io_set_axis(d,p,0,0)==0 && read(d,x,4)==0);
    assert(kdeadeye_io_set_axis(d,p,1,65536)==-1 && read(d,y,4)==65535);
  }
  assert(kdeadeye_io_set_axis(d,2,0,0)==-1 && kdeadeye_io_set_axis(d,0,2,0)==-1);
  assert(read(d,0x1f6800c0,4)==0xffff);
  assert(kdeadeye_io_trigger(d,0,1)==0 && read(d,0x1f6800c0,4)==0xfffe);
  assert(kdeadeye_io_trigger(d,1,1)==0 && read(d,0x1f6800c0,2)==0xfffc);
  assert(kdeadeye_io_trigger(d,0,0)==0 && read(d,0x1f6800c0,4)==0xfffd);
  assert(kdeadeye_io_trigger(d,1,0)==0 && read(d,0x1f6800c0,4)==0xffff);
  assert(kdeadeye_io_trigger(d,2,1)==-1 && kdeadeye_io_trigger(d,0,-1)==-1);
  assert(kdeadeye_io_read(d,0x1f680084,4,&v)==0 && v==0xfeedbeef);
  assert(kdeadeye_io_read(d,0x1f6800d0,4,&v)==0);
  assert(kdeadeye_io_read(d,UINT32_MAX,4,&v)==0);
  assert(kdeadeye_io_read(d,f,3,&v)==0 && kdeadeye_io_read(d,f,2,NULL)==0);
  assert(kdeadeye_io_write(d,0x1f680080,4,0,~0u)==0);
  assert(kdeadeye_io_write(d,0x1f6800d0,4,0,~0u)==0);
  write(d,0x1f6800e0,4,0xdeadbeef,~0u);
  assert(kdeadeye_io_read(d,0x1f6800e0,4,&v)==0);
  write(d,f,2,0x90,0xffff);assert(read(d,f,2)==0xb0 && read(d,f+2,2)==0xed);
  write(d,f,2,0xff,0xffff);
  write(d,f+0x100,2,0x40,0xffff);write(d,f+0x100,2,0x5678,0xffff);
  assert(read(d,f,2)==0x80);write(d,f,2,0xff,0xffff);
  assert(read(d,f+0x100,2)==0x5678 && image[0x100]==0x56 && image[0x101]==0x78);
  /* Zero mask is inert; commands are selected from the low byte. */
  write(d,f,4,0x00900090,0);assert(read(d,f,4)==0xffff1234);
  write(d,f,1,0x90,0xff);assert(read(d,f,4)==0x00ed00b0);
  write(d,f,1,0xff,0xff);
  write(d,f+0x10000,2,0x20,0xffff);write(d,f+0x10000,2,0xd0,0xffff);
  assert(read(d,f,2)==0 && kdeadeye_io_advance_to(d,999999999)==0 && read(d,f,2)==0);
  assert(kdeadeye_io_advance_to(d,1000000000)==0 && read(d,f,2)==0x80);
  write(d,f,2,0xff,0xffff);assert(read(d,f+0x10000,2)==0xffff && read(d,f,2)==0x1234);
  KDeadEyeIOStatus st;kdeadeye_io_status(d,&st);
  assert(!st.fault && st.noop_writes==1 && st.flash_reads && st.flash_writes && st.gun_reads && st.button_reads);
  assert(kdeadeye_io_advance_to(d,999999999)==-1);kdeadeye_io_status(d,&st);assert(st.fault);
  kdeadeye_io_destroy(d);
  d=kdeadeye_io_create(image,KDEADEYE_FLASH_BYTES);assert(d);
  assert(kdeadeye_io_write(d,f+1,1,0x90,0xff)==-1); /* Unsupported low command 00. */
  kdeadeye_io_status(d,&st);assert(st.fault);
  kdeadeye_io_destroy(d);free(image);puts("Dead Eye: source gun ports, triggers, lanes, Sharp flash and timing PASS");return 0;
}
