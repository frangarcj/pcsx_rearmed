/***************************************************************************
                            dma.c  -  description
                             -------------------
    begin                : Wed May 15 2002
    copyright            : (C) 2002 by Pete Bernert
    email                : BlackDove@addcom.de
 ***************************************************************************/
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version. See also the license.txt file for *
 *   additional informations.                                              *
 *                                                                         *
 ***************************************************************************/

#include "stdafx.h"

#define _IN_DMA

#include "externals.h"
#include "registers.h"

#ifdef PCSX_SYSTEM573
extern int system573_active(void);
#endif
#ifdef PCSX_SYSTEM_GV
extern int systemgv_active(void);
#endif

static void set_dma_end(int iSize, unsigned int cycles)
{
 // this must be > psxdma.c dma irq
 // Road Rash also wants a considerable delay, maybe because of fifo?
 cycles += iSize * 20;  // maybe
 cycles |= 1;           // indicates dma is active
 spu.cycles_dma_end = cycles;
}

////////////////////////////////////////////////////////////////////////
// READ DMA (many values)
////////////////////////////////////////////////////////////////////////

void CALLBACK SPUreadDMAMem(unsigned short *pusPSXMem, int iSize,
 unsigned int cycles)
{
 unsigned int addr = spu.spuAddr, irq_addr = regAreaGet(H_SPUirqAddr) << 3;
 int i, irq_after;

 do_samples_if_needed(cycles, 1, 2);
 irq_after = (irq_addr - addr) & 0x7ffff;

 for(i = 0; i < iSize; i++)
 {
  *pusPSXMem++ = *(unsigned short *)(spu.spuMemC + addr);
  addr += 2;
  addr &= 0x7fffe;
 }
 if ((spu.spuCtrl & CTRL_IRQ) && irq_after < iSize * 2) {
  log_unhandled("rdma spu irq: %x/%x-%x\n", irq_addr, spu.spuAddr, addr);
  do_irq_io(irq_after);
 }
 spu.spuAddr = addr;
 set_dma_end(iSize, cycles);
}

////////////////////////////////////////////////////////////////////////
// WRITE DMA (many values)
////////////////////////////////////////////////////////////////////////

void CALLBACK SPUwriteDMAMem(unsigned short *pusPSXMem, int iSize,
 unsigned int cycles)
{
 unsigned int addr = spu.spuAddr, irq_addr = regAreaGet(H_SPUirqAddr) << 3;
 int i, irq_after;
 
 unsigned int render_cycle = cycles + iSize*2 * 4;
#ifdef PCSX_SYSTEM573
 // The source System573 DMA payload has already arrived at its timer edge.
 // MAME spu_device::start_dma copies now; the console size-based speculative
 // advance can otherwise reset the sample clock into the future on a large DMA.
 if (system573_active()) render_cycle = cycles;
#endif
#ifdef PCSX_SYSTEM_GV
 // GV uses the same source PSX SPU DMA copy at the current timer edge.
 if (systemgv_active()) render_cycle = cycles;
#endif
 do_samples_if_needed(render_cycle, 1, 2);
 irq_after = (irq_addr - addr) & 0x7ffff;
 spu.bMemDirty = 1;

 if (addr + iSize*2 < 0x80000)
 {
  memcpy(spu.spuMemC + addr, pusPSXMem, iSize*2);
  addr += iSize*2;
 }
 else
 {
  for (i = 0; i < iSize; i++)
  {
   *(unsigned short *)(spu.spuMemC + addr) = *pusPSXMem++;
   addr += 2;
   addr &= 0x7fffe;
  }
 }
 if ((spu.spuCtrl & CTRL_IRQ) && irq_after < iSize * 2) {
  log_unhandled("%u wdma spu irq: %x/%x-%x (%u)\n",
    cycles, irq_addr, spu.spuAddr, addr, irq_after);
  // this should be consistent with psxdma.c timing
  // might also need more delay like in set_dma_end()
  do_irq_io(irq_after * 4);
 }
 for (i = 0; i < MAXCHAN; i++) {
  size_t ediff, p = spu.s_chan[i].pCurr - spu.spuMemC;
  if (spu.s_chan[i].ADSRX.State == ADSR_RELEASE && !spu.s_chan[i].ADSRX.EnvelopeVol)
   continue;
  ediff = addr - p;
  if (spu.spuAddr < p && p < spu.spuAddr + iSize * 2) {
   log_unhandled("%u spu ch%02d play %zx dma %x-%x (%zd)\n",
     cycles, i, p, spu.spuAddr, addr, ediff);
   //exit(1);
  }
  // a hack for the super annoying timing issues in The Emperor's New Groove
  // (which is a game bug, but tends to trigger more here)
  if (ediff <= 0x20u) {
   spu.s_chan[i].pCurr += ediff;
   break;
  }
 }
 spu.spuAddr = addr;
 set_dma_end(iSize, cycles);
}

////////////////////////////////////////////////////////////////////////
// vim:shiftwidth=1:expandtab
