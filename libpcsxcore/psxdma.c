/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/*
* Handles PSX DMA functions.
*/

#include "psxdma.h"
#include "gpu.h"
#ifdef PCSX_SYSTEM12
#include "system10.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#ifdef PCSX_SYSTEM_GV
#include "systemgv.h"
#endif

void psxDma5(u32 madr, u32 bcr, u32 chcr)
{
	if (system10_active()) {
		if (!system10_dma5(madr, bcr, chcr)) {
			psxHu32ref(0x10d8) &= SWAPu32(~0x11000000u);
			DMA_INTERRUPT(5);
		}
	}
#ifdef PCSX_SYSTEM_GV
	else if (systemgv_active()) {
		/* MAME's PSX DMA device calls dma_finished immediately after the
		 * channel-5 handler registers the transfer. The GV board may keep
		 * payload pending until NCR DRQ; that payload is still gated by its
		 * real controller/target status. */
		if (systemgv_dma5(madr, bcr, chcr) == 0) {
			psxHu32ref(0x10d8) &= SWAPu32(~0x11000000u);
			DMA_INTERRUPT(5);
		}
	}
#endif
	else if (system12_active()) system12_dma5(madr, bcr, chcr);
}
#endif

#ifndef min
#define min(a, b) ((b) < (a) ? (b) : (a))
#endif
#ifndef PSXDMA_LOG
#define PSXDMA_LOG(...)
#endif

// Dma0/1 in Mdec.c
// Dma3   in CdRom.c

#ifdef PCSX_SYSTEM_GNET
#include "systemgnet.h"
#endif
int psxSourceSpuDmaActive(void)
{
#ifdef PCSX_SYSTEM_GNET
    if (systemgnet_active()) return 1;
#endif
#ifdef PCSX_SYSTEM12
    if (systemtaito_active() || systemzn_tps_active()) return 1;
#ifdef PCSX_SYSTEM_GV
    if (systemgv_active()) return 1;
#endif
#endif
    return 0;
}
/* Pinned MAME PSX DMA: channel 4 writes deliver their payload after a fixed
 * 24000 ticks at 33.8688 MHz, retain MADR, and then signal completion. */
static void source_spu_transfer(int read)
{
    u32 address = psxHu32(0x10c0) & PSX_RAM_MASK & ~3u;
    u32 bcr = psxHu32(0x10c4);
    u32 blocks = bcr >> 16;
    u32 words = (blocks ? blocks : 0x10000u) * (bcr & 0xffff);
    while (words) {
        u32 count = min(words, (PSX_RAM_SIZE - address) / 4);
        u16 *data = (u16 *)(psxRegs.ptrs.psxM + address);
        if (read) {
            SPU_readDMAMem(data, count * 2, psxRegs.cycle);
            psxCpu->Clear(address, count);
        } else SPU_writeDMAMem(data, count * 2, psxRegs.cycle);
        words -= count;
        address = 0;
    }
}

void spuInterrupt() {
	if (HW_DMA4_CHCR & SWAP32(0x01000000))
	{
        if (psxSourceSpuDmaActive() && psxHu32(0x10c8) == 0x01000201)
            source_spu_transfer(0);
		HW_DMA4_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(4);
	}
}

void psxDma4(u32 madr, u32 bcr, u32 chcr) { // SPU
    if (psxSourceSpuDmaActive()) {
        if (chcr == 0x01000201) {
            u32 delay = ((uint64_t)24000 * PSXCLK + 33868800 - 1) / 33868800;
            set_event(PSXINT_SPUDMA, delay);
            return;
        }
        if (chcr == 0x01000200) {
            source_spu_transfer(1);
            HW_DMA4_CHCR &= SWAP32(~0x01000000);
            DMA_INTERRUPT(4);
            return;
        }
    }
	u32 words, words_max = 0, words_copy;
	u16 *ptr;

	madr &= ~3;
	ptr = getDmaRam(madr, &words_max);
	if (ptr == INVALID_PTR)
		log_unhandled("bad dma4 madr %x\n", madr);

	words = words_copy = (bcr >> 16) * (bcr & 0xffff);
	if (words_copy > words_max) {
		log_unhandled("bad dma4 madr %x bcr %x\n", madr, bcr);
		words_copy = words_max;
	}

	switch (chcr) {
		case 0x01000201: //cpu to spu transfer
			PSXDMA_LOG("*** DMA4 SPU - mem2spu *** %x addr = %x size = %x\n", chcr, madr, bcr);
			if (ptr == INVALID_PTR)
				break;
			SPU_writeDMAMem(ptr, words_copy * 2, psxRegs.cycle);
			HW_DMA4_MADR = SWAPu32(madr + words_copy * 2);
			// This should be much slower, like 12+ cycles/byte, it's like
			// that because the CPU runs too fast and fifo is not emulated.
			// See also set_dma_end().
			set_event(PSXINT_SPUDMA, words * 4 * 4);
			return;

		case 0x01000200: //spu to cpu transfer
			PSXDMA_LOG("*** DMA4 SPU - spu2mem *** %x addr = %x size = %x\n", chcr, madr, bcr);
			if (ptr == INVALID_PTR)
				break;
			SPU_readDMAMem(ptr, words_copy * 2, psxRegs.cycle);
			psxCpu->Clear(madr, words_copy);

			HW_DMA4_MADR = SWAPu32(madr + words_copy * 4);
			set_event(PSXINT_SPUDMA, words * 4 * 4);
			return;

		default:
			log_unhandled("*** DMA4 SPU - unknown *** %x addr = %x size = %x\n", chcr, madr, bcr);
			break;
	}

	HW_DMA4_CHCR &= SWAP32(~0x01000000);
	DMA_INTERRUPT(4);
}

#if 0
// Taken from PEOPS SOFTGPU
static inline boolean CheckForEndlessLoop(u32 laddr, u32 *lUsedAddr) {
	if (laddr == lUsedAddr[1]) return TRUE;
	if (laddr == lUsedAddr[2]) return TRUE;

	if (laddr < lUsedAddr[0]) lUsedAddr[1] = laddr;
	else lUsedAddr[2] = laddr;

	lUsedAddr[0] = laddr;

	return FALSE;
}

static u32 gpuDmaChainSize(u32 addr) {
	u32 size;
	u32 DMACommandCounter = 0;
	u32 lUsedAddr[3];

	lUsedAddr[0] = lUsedAddr[1] = lUsedAddr[2] = 0xffffff;

	// initial linked list ptr (word)
	size = 1;

	do {
		addr &= 0x1ffffc;

		if (DMACommandCounter++ > 2000000) break;
		if (CheckForEndlessLoop(addr, lUsedAddr)) break;

		// # 32-bit blocks to transfer
		size += psxMu8( addr + 3 );

		// next 32-bit pointer
		addr = psxMu32( addr & ~0x3 ) & 0xffffff;
		size += 1;
	} while (!(addr & 0x800000)); // contrary to some documentation, the end-of-linked-list marker is not actually 0xFF'FFFF
                                  // any pointer with bit 23 set will do.

	return size;
}
#endif

void psxDma2(u32 madr, u32 bcr, u32 chcr) { // GPU
	u32 *ptr, madr_next, *madr_next_p;
	u32 words, words_left, words_max, words_copy;
	s32 cycles_last_cmd = 0;
	int do_walking;
	long cycles_sum;

	madr &= ~3;
	switch (chcr) {
		case 0x01000200: // vram2mem
			PSXDMA_LOG("*** DMA2 GPU - vram2mem *** %x addr = %x size = %x\n", chcr, madr, bcr);
			ptr = getDmaRam(madr, &words_max);
			if (ptr == INVALID_PTR) {
				log_unhandled("bad dma2 madr %x\n", madr);
				break;
			}
			// BA blocks * BS words (word = 32-bits)
			words = words_copy = (bcr >> 16) * (bcr & 0xffff);
			if (words > words_max) {
				log_unhandled("bad dma2 madr %x bcr %x\n", madr, bcr);
				words_copy = words_max;
			}
			GPU_readDataMem(ptr, words_copy);
			psxCpu->Clear(madr, words_copy);

			HW_DMA2_MADR = SWAPu32(madr + words_copy * 4);

			// careful: gpu_state_change() also messes with this
			psxRegs.gpuIdleAfter = psxRegs.cycle + words / 4 + 16;
			// already 32-bit word size ((size * 4) / 4)
			set_event(PSXINT_GPUDMA, words / 4);
			return;

		case 0x01000201: // mem2vram
			PSXDMA_LOG("*** DMA 2 - GPU mem2vram *** %x addr = %x size = %x\n", chcr, madr, bcr);
			words = words_left = (bcr >> 16) * (bcr & 0xffff);
			while (words_left > 0) {
				ptr = getDmaRam(madr, &words_max);
				if (ptr == INVALID_PTR) {
					log_unhandled("bad2 dma madr %x\n", madr);
					break;
				}
				words_copy = min(words_left, words_max);
				GPU_writeDataMem(ptr, words_copy);
				words_left -= words_copy;
				madr += words_copy * 4;
			}

			HW_DMA2_MADR = SWAPu32(madr);

			// careful: gpu_state_change() also messes with this
			psxRegs.gpuIdleAfter = psxRegs.cycle + words / 4 + 16;
			// already 32-bit word size ((size * 4) / 4)
			set_event(PSXINT_GPUDMA, words / 4);
			return;

		case 0x01000401: // dma chain
			PSXDMA_LOG("*** DMA 2 - GPU dma chain *** %x addr = %x size = %x\n", chcr, madr, bcr);
			// when not emulating walking progress, end immediately
			// (some games abort the dma and read madr so break out of that logic)
			madr_next = 0xffffff;

			do_walking = Config.GpuListWalking;
			if (do_walking < 0)
				do_walking = Config.hacks.gpu_slow_list_walking;
			madr_next_p = do_walking ? &madr_next : NULL;

			cycles_sum = GPU_dmaChain((u32 *)psxRegs.ptrs.psxM, madr & PSX_RAM_MASK,
					madr_next_p, &cycles_last_cmd);

			HW_DMA2_MADR = SWAPu32(madr_next);

			// timing hack with some lame heuristics
			if (Config.gpu_timing_override && (do_walking || cycles_sum > 64)
			    && !(HW_GPU_STATUS & SWAP32(PSXGPU_DHEIGHT | PSXGPU_RGB24)))
				cycles_sum = Config.gpu_timing_override;

			psxRegs.gpuIdleAfter = psxRegs.cycle + cycles_sum + cycles_last_cmd;
			set_event(PSXINT_GPUDMA, cycles_sum);
			//printf("%u dma2cf: %6ld,%4d %08x %08x %08x %08x\n", psxRegs.cycle,
			//  cycles_sum, cycles_last_cmd, madr, bcr, chcr, HW_DMA2_MADR);
			return;

		default:
			log_unhandled("*** DMA 2 - GPU unknown *** %x addr = %x size = %x\n", chcr, madr, bcr);
			break;
	}

	HW_DMA2_CHCR &= SWAP32(~0x01000000);
	DMA_INTERRUPT(2);
}

void gpuInterrupt() {
	if (HW_DMA2_CHCR == SWAP32(0x01000401) && !(HW_DMA2_MADR & SWAP32(0x800000)))
	{
		u32 madr_next = SWAPu32(HW_DMA2_MADR);
		s32 cycles_sum = psxRegs.gpuIdleAfter - psxRegs.cycle;
		s32 cycles_last_cmd = 0;

		do {
			cycles_sum += cycles_last_cmd;
			cycles_sum += GPU_dmaChain((u32 *)psxRegs.ptrs.psxM,
					madr_next & PSX_RAM_MASK, &madr_next, &cycles_last_cmd);
		}
		while (cycles_sum <= 0 && !(madr_next & 0x800000));
		HW_DMA2_MADR = SWAPu32(madr_next);
		psxRegs.gpuIdleAfter = psxRegs.cycle + cycles_sum + cycles_last_cmd;
		set_event(PSXINT_GPUDMA, cycles_sum);
		//printf("%u dma2cn: %6d,%4d %08x\n", psxRegs.cycle, cycles_sum,
		//  cycles_last_cmd, HW_DMA2_MADR);
		return;
	}
	if (HW_DMA2_CHCR & SWAP32(0x01000000))
	{
		HW_DMA2_CHCR &= SWAP32(~0x01000000);
		DMA_INTERRUPT(2);
	}
}

void psxAbortDma2() {
	psxRegs.gpuIdleAfter = psxRegs.cycle + 32;
}

void psxDma6(u32 madr, u32 bcr, u32 chcr) {
	u32 words, words_max;
	u32 *mem;

	PSXDMA_LOG("*** DMA6 OT *** %x addr = %x size = %x\n", chcr, madr, bcr);

	if (chcr == 0x11000002) {
		madr &= ~3;
		mem = getDmaRam(madr, &words_max);
		if (mem == INVALID_PTR) {
			log_unhandled("bad6 dma madr %x\n", madr);
			HW_DMA6_CHCR &= SWAP32(~0x11000000);
			DMA_INTERRUPT(6);
			return;
		}

		// already 32-bit size
		words = bcr;

		while (bcr-- && mem > (u32 *)psxRegs.ptrs.psxM) {
			*mem-- = SWAP32((madr - 4) & 0xffffff);
			madr -= 4;
		}
		*++mem = SWAP32(0xffffff);

		// halted
		psxRegs.cycle += words;
		set_event(PSXINT_GPUOTCDMA, 16);
		return;
	}
	else {
		// Unknown option
		log_unhandled("*** DMA6 OT - unknown *** %x addr = %x size = %x\n", chcr, madr, bcr);
	}

	HW_DMA6_CHCR &= SWAP32(~0x11000000);
	DMA_INTERRUPT(6);
}

void gpuotcInterrupt()
{
	if (HW_DMA6_CHCR & SWAP32(0x01000000))
	{
		HW_DMA6_CHCR &= SWAP32(~0x11000000);
		DMA_INTERRUPT(6);
	}
}
