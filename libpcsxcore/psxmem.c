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
* PSX memory functions.
*/

// TODO: Implement caches & cycle penalty.

#include <stdio.h>
#include <assert.h>
#include "psxmem.h"
#include "psxmem_map.h"
#include "r3000a.h"
#include "psxhw.h"
#ifdef PCSX_SYSTEM573
#include "system573.h"
#endif
#ifdef PCSX_SYSTEM12
#include "system12.h"
#include "system11.h"
#include "system10.h"
#include "systemzn.h"
#include "systemtaito.h"
#ifdef PCSX_SYSTEM_GNET
#include "systemgnet.h"
#endif
#endif
#ifdef PCSX_SYSTEM_GV
#include "systemgv.h"
#endif
#ifdef PCSX_SYSTEM_GQ
#include "systemgq.h"
#endif
#ifdef PCSX_SYSTEM12
u32 psxRamSize = 0x200000, psxRomSize = 0x80000;
#endif
//#include "debug.h"
#define DebugCheckBP(...)

#include "lightrec/mem.h"
#include "memmap.h"
#include "compiler_features.h"

#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static void * psxMapDefault(unsigned long addr, size_t size,
			    enum psxMapTag tag, int *can_retry_addr)
{
	void *ptr;
#if !P_HAVE_MMAP
	*can_retry_addr = 0;
	ptr = calloc(1, size);
	return ptr ? ptr : MAP_FAILED;
#else
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	*can_retry_addr = 1;
	ptr = mmap((void *)(uintptr_t)addr, size,
		    PROT_READ | PROT_WRITE, flags, -1, 0);
#ifdef MADV_HUGEPAGE
	if (size >= 2*1024*1024) {
		if (ptr != MAP_FAILED && ((uintptr_t)ptr & (2*1024*1024 - 1))) {
			// try to manually realign assuming decreasing addr alloc
			munmap(ptr, size);
			addr = (uintptr_t)ptr & ~(2*1024*1024 - 1);
			ptr = mmap((void *)(uintptr_t)addr, size,
				PROT_READ | PROT_WRITE, flags, -1, 0);
		}
		if (ptr != MAP_FAILED)
			madvise(ptr, size, MADV_HUGEPAGE);
	}
#endif
	return ptr;
#endif
}

static void psxUnmapDefault(void *ptr, size_t size, enum psxMapTag tag)
{
#if !P_HAVE_MMAP
	free(ptr);
#else
	munmap(ptr, size);
#endif
}

void *(*psxMapHook)(unsigned long addr, size_t size,
		enum psxMapTag tag, int *can_retry_addr) = psxMapDefault;
void (*psxUnmapHook)(void *ptr, size_t size,
		     enum psxMapTag tag) = psxUnmapDefault;

void *psxMap(unsigned long addr, size_t size, int is_fixed,
		enum psxMapTag tag)
{
	int try_, can_retry_addr = 0;
	void *ret = MAP_FAILED;

	for (try_ = 0; try_ < 3; try_++)
	{
		if (ret != MAP_FAILED)
			psxUnmap(ret, size, tag);
		ret = psxMapHook(addr, size, tag, &can_retry_addr);
		if (ret == MAP_FAILED)
			return MAP_FAILED;

		if (addr != 0 && ret != (void *)(uintptr_t)addr) {
			SysMessage("psxMap: tried to map @%08lx, got %p\n",
				addr, ret);
			if (is_fixed) {
				psxUnmap(ret, size, tag);
				return MAP_FAILED;
			}

			if (can_retry_addr && ((addr ^ (uintptr_t)ret) & ~0xff000000l)) {
				unsigned long mask;

				// try to use similarly aligned memory instead
				// (recompiler prefers this)
				mask = try_ ? 0xffff : 0xffffff;
				addr = ((uintptr_t)ret + mask) & ~mask;
				continue;
			}
		}
		break;
	}

	return ret;
}

void psxUnmap(void *ptr, size_t size, enum psxMapTag tag)
{
	psxUnmapHook(ptr, size, tag);
}

/*  Playstation Memory Map (from Playstation doc by Joshua Walker)
0x0000_0000-0x0000_ffff		Kernel (64K)
0x0001_0000-0x001f_ffff		User Memory (1.9 Meg)

0x1f00_0000-0x1f00_ffff		Parallel Port (64K)

0x1f80_0000-0x1f80_03ff		Scratch Pad (1024 bytes)

0x1f80_1000-0x1f80_2fff		Hardware Registers (8K)

0x1fc0_0000-0x1fc7_ffff		BIOS (512K)

0x8000_0000-0x801f_ffff		Kernel and User Memory Mirror (2 Meg) Cached
0x9fc0_0000-0x9fc7_ffff		BIOS Mirror (512K) Cached

0xa000_0000-0xa01f_ffff		Kernel and User Memory Mirror (2 Meg) Uncached
0xbfc0_0000-0xbfc7_ffff		BIOS Mirror (512K) Uncached
*/

static int psxMemInitMap(void)
{
	u8 *ptr;

	ptr = psxMap(0x80000000, (PSX_RAM_SIZE + 0x10000), 1, MAP_TAG_RAM);
	if (ptr == MAP_FAILED)
		ptr = psxMap(0x77000000, (PSX_RAM_SIZE + 0x10000), 0, MAP_TAG_RAM);
	if (ptr == MAP_FAILED) {
		SysMessage("mapping main RAM failed");
		return -1;
	}
	psxRegs.ptrs.psxM = ptr;
	psxRegs.ptrs.psxP = ptr + PSX_RAM_SIZE;

	ptr = psxMap(0x1f800000, 0x10000, 0, MAP_TAG_OTHER);
	if (ptr == MAP_FAILED) {
		SysMessage("Error allocating psxH");
		return -1;
	}
	psxRegs.ptrs.psxH = ptr;

	ptr = psxMap(0x1fc00000, PSX_ROM_SIZE, 0, MAP_TAG_OTHER);
	if (ptr == MAP_FAILED) {
		SysMessage("Error allocating psxR");
		return -1;
	}
	psxRegs.ptrs.psxR = ptr;

	return 0;
}

static void psxMemFreeMap(void)
{
	if (psxRegs.ptrs.psxM) psxUnmap(psxRegs.ptrs.psxM, (PSX_RAM_SIZE + 0x10000), MAP_TAG_RAM);
	if (psxRegs.ptrs.psxH) psxUnmap(psxRegs.ptrs.psxH, 0x10000, MAP_TAG_OTHER);
	if (psxRegs.ptrs.psxR) psxUnmap(psxRegs.ptrs.psxR, PSX_ROM_SIZE, MAP_TAG_OTHER);
	psxRegs.ptrs.psxM = psxRegs.ptrs.psxH = psxRegs.ptrs.psxR = NULL;
	psxRegs.ptrs.psxP = NULL;
}

static void lutMap(uintptr_t *lut, u8 *mem, u32 size, u32 start, u32 end)
{
	u32 i;
	assert((size  & ((1u << PSXM_SHIFT) - 1)) == 0);
	assert((start & ((1u << PSXM_SHIFT) - 1)) == 0);
	assert((end   & ((1u << PSXM_SHIFT) - 1)) == 0);

	for (i = start; i < end; i += (1u << PSXM_SHIFT))
		lut[i >> PSXM_SHIFT] = (uintptr_t)mem - (i & ~(size - 1));
}

// shouldn't this affect reads, more mirrors?
static void mapRam(int isMapped)
{
	//uintptr_t *rLUT = psxRegs.ptrs.memRLUT;
	uintptr_t *wLUT = psxRegs.ptrs.memWLUT;

	if (isMapped) {
		u8 *ram = psxRegs.ptrs.psxM;
		u32 i;
		for (i = 0; i < 0x800000; i += PSX_RAM_SIZE) {
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0x00000000u + i, PSX_RAM_SIZE + i);
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0x80000000u + i, 0x80000000u + PSX_RAM_SIZE + i);
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0xa0000000u + i, 0xa0000000u + PSX_RAM_SIZE + i);
		}
	}
	else {
		size_t len = (PSX_RAM_SIZE >> PSXM_SHIFT) * sizeof(wLUT[0]);
		//memset(rLUT + (0x00000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(rLUT + (0x80000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(rLUT + (0xa0000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		memset(wLUT + (0x00000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		memset(wLUT + (0x80000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(wLUT + (0xa0000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
	}
}

int psxMemInit(void)
{
	size_t table_size = 1ul << (32 - PSXM_SHIFT);
	uintptr_t *memRLUT;
	uintptr_t *memWLUT;
	unsigned int i;
	int ret;

	if (LIGHTREC_CUSTOM_MAP)
		ret = lightrec_init_mmap();
	else
		ret = psxMemInitMap();
	if (ret) {
		if (LIGHTREC_CUSTOM_MAP)
			SysMessage("lightrec_init_mmap failed");
		psxMemShutdown();
		return -1;
	}

	if (DISABLE_MEM_LUTS)
		return 0;

	memRLUT = malloc(table_size * sizeof(memRLUT[0]) * 2);
	if (memRLUT == NULL) {
		SysMessage("Error allocating psxMem LUTs");
		psxMemShutdown();
		return -1;
	}

	assert((s8)INVALID_PTR_VAL == INVALID_PTR_VAL);
	memset(memRLUT, INVALID_PTR_VAL, table_size * sizeof(memRLUT[0]) * 2);
	memWLUT = memRLUT + table_size;
	psxRegs.ptrs.memRLUT = memRLUT;
	psxRegs.ptrs.memWLUT = memWLUT;

	// ram
	for (i = 0; i < 0x800000; i += PSX_RAM_SIZE) {
		u8 *ram = psxRegs.ptrs.psxM;
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0x00000000u + i, PSX_RAM_SIZE + i);
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0x80000000u + i, 0x80000000u + PSX_RAM_SIZE + i);
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0xa0000000u + i, 0xa0000000u + PSX_RAM_SIZE + i);
	}
	mapRam(1);

	// bios
	for (i = 0; i < 0x400000; i += PSX_ROM_SIZE) {
		lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0x1fc00000u + i, 0x1fc00000u + PSX_ROM_SIZE + i);
		lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0x9fc00000u + i, 0x9fc00000u + PSX_ROM_SIZE + i);
		lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0xbfc00000u + i, 0xbfc00000u + PSX_ROM_SIZE + i);
	}

	// Don't allow writes to PIO Expansion region (psxP) to take effect.
	// NOTE: Not sure if this is needed to fix any games but seems wise,
	//       seeing as some games do read from PIO as part of copy-protection
	//       check. (See fix in psxMemReset() regarding psxP region reads).
	//memWLUT[0x1f000000 >> PSXM_SHIFT] = INVALID_PTR_VAL;

	return 0;
}

void psxMemReset() {
#ifdef PCSX_SYSTEM573
	if (system573_active()) { system573_mem_reset(); if (!DISABLE_MEM_LUTS) mapRam(1); Config.HLE=FALSE; return; }
#endif
#ifdef PCSX_SYSTEM12
	if (systemtaito_active()) {
		systemtaito_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
#ifdef PCSX_SYSTEM_GNET
	if (systemgnet_active()) {
		systemgnet_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
#endif
#ifdef PCSX_SYSTEM_GV
	if (systemgv_active()) {
		systemgv_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
#endif
#ifdef PCSX_SYSTEM_GQ
	if (systemgq_active()) {
		systemgq_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
#endif
	if (systemzn_active()) {
		systemzn_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
	if (system10_active()) {
		system10_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
	if (system11_active()) {
		system11_mem_reset();
		if (!DISABLE_MEM_LUTS) mapRam(1);
		Config.HLE = FALSE;
		return;
	}
	if (system12_active()) {
		system12_mem_reset();
		if (!DISABLE_MEM_LUTS)
			mapRam(1);
		Config.HLE = FALSE;
		return;
	}
#endif
	size_t count = sizeof(Config.Bios) / sizeof(Config.Bios[0]);
	size_t i, r = Config.PsxRegion, rret;
	FILE *f = NULL;
	char bios[1024];

	// done by the BIOS [10000...sp) before loading the exe from cd
	//memset(psxRegs.ptrs.psxM, 0, 0x00200000);

	memset(psxRegs.ptrs.psxP, 0xff, 0x00010000);

	if (!DISABLE_MEM_LUTS)
		mapRam(1);

	Config.HLE = TRUE;

	if (r >= count || !Config.Bios[r][0])
		r = 0;

	// prefer wrong region bios to HLE
	for (i = 0; i < count; i++, r++) {
		if (r >= count)
			r = 0;
		if (!Config.Bios[r][0] || strcmp(Config.Bios[r], "HLE") == 0)
			continue;
		snprintf(bios, sizeof(bios), "%s/%s", Config.BiosDir, Config.Bios[r]);
		f = fopen(bios, "rb");
		if (f == NULL) {
			SysMessage("Could not open BIOS: \"%s\"\n", bios);
			continue;
		}
		rret = fread(psxRegs.ptrs.psxR, 1, 0x80000, f);
		fclose(f);
		if (rret == 0x80000) {
			SysMessage("Loaded BIOS \"%s\".\n", bios);
			Config.HLE = FALSE;
			break;
		}
		SysMessage("BIOS \"%s\" is of wrong size, skipping.\n", bios);
	}
	if (Config.HLE)
		memset(psxRegs.ptrs.psxR, 0, 0x80000);
}

void psxMemShutdown() {
	if (LIGHTREC_CUSTOM_MAP)
		lightrec_free_mmap();
	else
		psxMemFreeMap();

	free(psxRegs.ptrs.memRLUT);
	psxRegs.ptrs.memRLUT = NULL;
	psxRegs.ptrs.memWLUT = NULL;
}

void psxMemOnIsolate(int enable)
{
	if (!DISABLE_MEM_LUTS) {
		mapRam(!enable);
	}

	psxCpu->Notify(enable ? R3000ACPU_NOTIFY_CACHE_ISOLATED
			: R3000ACPU_NOTIFY_CACHE_UNISOLATED, NULL);
}

u8 psxMemRead8(psxRegisters *regs, u32 mem) {
#ifdef PCSX_SYSTEM12
	u32 board_value;
	if (
#ifdef PCSX_SYSTEM573
	    system573_read(mem, 1, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_read(mem, 1, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_read(mem, 1, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_read(mem, 1, &board_value) ||
#endif
	    systemtaito_read(mem, 1, &board_value) || systemzn_read(mem, 1, &board_value) || system10_read(mem, 1, &board_value) || system11_read(mem, 1, &board_value) || system12_read(mem, 1, &board_value))
		return board_value;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return *p;

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return regs->ptrs.psxH[mem & 0x3ff];
		else
			return psxHwRead8(regs, mem);
	}
	return 0xFF;
}

u16 psxMemRead16(psxRegisters *regs, u32 mem) {
#ifdef PCSX_SYSTEM12
	u32 board_value;
	if (
#ifdef PCSX_SYSTEM573
	    system573_read(mem, 2, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_read(mem, 2, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_read(mem, 2, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_read(mem, 2, &board_value) ||
#endif
	    systemtaito_read(mem, 2, &board_value) || systemzn_read(mem, 2, &board_value) || system10_read(mem, 2, &board_value) || system11_read(mem, 2, &board_value) || system12_read(mem, 2, &board_value))
		return board_value;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return SWAPu16(*(u16 *)p);

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return SWAPu16(*(u16 *)(regs->ptrs.psxH + (mem & 0x3fe)));
		else
			return psxHwRead16(regs, mem);
	}
	return 0xFFFF;
}

u32 psxMemRead32(psxRegisters *regs, u32 mem) {
#ifdef PCSX_SYSTEM12
	u32 board_value;
	if (
#ifdef PCSX_SYSTEM573
	    system573_read(mem, 4, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_read(mem, 4, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_read(mem, 4, &board_value) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_read(mem, 4, &board_value) ||
#endif
	    systemtaito_read(mem, 4, &board_value) || systemzn_read(mem, 4, &board_value) || system10_read(mem, 4, &board_value) || system11_read(mem, 4, &board_value) || system12_read(mem, 4, &board_value))
		return board_value;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return SWAPu32(*(u32 *)p);

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return SWAPu32(*(u32 *)(regs->ptrs.psxH + (mem & 0x3fc)));
		else
			return psxHwRead32(regs, mem);
	}
	if (mem == 0xfffe0130)
		return regs->biuReg;
	return 0xFFFFFFFF;
}

void psxMemWrite8(psxRegisters *regs, u32 mem, u32 value) {
#ifdef PCSX_SYSTEM12
	if (
#ifdef PCSX_SYSTEM573
	    system573_write(mem, 1, value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_write(mem, 1, value, 0xff) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_write(mem, 1, value, 0xff) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_write(mem, 1, value) ||
#endif
	    systemtaito_write(mem, 1, value) || systemzn_write(mem, 1, value) || system10_write(mem, 1, value, 0xff) || system11_write(mem, 1, value) || system12_write(mem, 1, value))
		return;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 1))) {
		*p = value;
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			regs->ptrs.psxH[mem & 0x3ff] = value;
		else
			psxHwWrite8(regs, mem, value);
		return;
	}
	log_unhandled("unhandled w8  %08x %08x @%08x\n", mem, value, regs->pc);
}

void psxMemWrite16(psxRegisters *regs, u32 mem, u32 value) {
#ifdef PCSX_SYSTEM12
	if (
#ifdef PCSX_SYSTEM573
	    system573_write(mem, 2, value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_write(mem, 2, value, 0xffff) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_write(mem, 2, value, 0xffff) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_write(mem, 2, value) ||
#endif
	    systemtaito_write(mem, 2, value) || systemzn_write(mem, 2, value) || system10_write(mem, 2, value, 0xffff) || system11_write(mem, 2, value) || system12_write(mem, 2, value))
		return;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 1))) {
		*(u16 *)p = SWAPu16(value);
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			*(u16 *)(regs->ptrs.psxH + (mem & 0x3fe)) = SWAPu16(value);
		else
			psxHwWrite16(regs, mem, value);
		return;
	}
	log_unhandled("unhandled w16 %08x %08x @%08x\n", mem, value, regs->pc);
}

void psxMemWrite32(psxRegisters *regs, u32 mem, u32 value) {
#ifdef PCSX_SYSTEM12
	if (
#ifdef PCSX_SYSTEM573
	    system573_write(mem, 4, value) ||
#endif
#ifdef PCSX_SYSTEM_GV
	    systemgv_write(mem, 4, value, 0xffffffffu) ||
#endif
#ifdef PCSX_SYSTEM_GQ
	    systemgq_write(mem, 4, value, 0xffffffffu) ||
#endif
#ifdef PCSX_SYSTEM_GNET
	    systemgnet_write(mem, 4, value) ||
#endif
	    systemtaito_write(mem, 4, value) || systemzn_write(mem, 4, value) || system10_write(mem, 4, value, 0xffffffffu) || system11_write(mem, 4, value) || system12_write(mem, 4, value))
		return;
#endif
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 1))) {
		*(u32 *)p = SWAPu32(value);
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			*(u32 *)(regs->ptrs.psxH + (mem & 0x3fc)) = SWAPu32(value);
		else
			psxHwWrite32(regs, mem, value);
		return;
	}
	if (mem == 0xfffe0130) {
		regs->biuReg = value;
		return;
	}
	if (!(regs->CP0.n.SR & ((1u << 16))))
		log_unhandled("unhandled w32 %08x %08x @%08x\n", mem, value, regs->pc);
}
