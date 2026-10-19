/*
 * (C) Gražvydas "notaz" Ignotas, 2010-2011
 *
 * This work is licensed under the terms of GNU GPL version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../psxhw.h"
#include "../cdrom.h"
#include "../mdec.h"
#include "../gpu.h"
#include "../psxmem_map.h"
#include "../psxmem.h"
#include "emu_if.h"
#include "pcsxmem.h"
#ifdef PCSX_SYSTEM12
#include "../system10.h"
#include "../systemzn.h"
#endif

#ifdef __thumb__
#error the dynarec is incompatible with Thumb functions,
#error please add -marm to compile flags
#endif

//#define memprintf printf
#define memprintf(...)

static uintptr_t *mem_readtab;
static uintptr_t *mem_writetab;
static uintptr_t mem_iortab[(1+2+4) * 0x1000 / 4];
static uintptr_t mem_iowtab[(1+2+4) * 0x1000 / 4];
static uintptr_t mem_ffrtab[(1+2+4) * 0x1000 / 4];
static uintptr_t mem_ffwtab[(1+2+4) * 0x1000 / 4];
//static uintptr_t mem_unmrtab[(1+2+4) * 0x1000 / 4];
static uintptr_t mem_unmwtab[(1+2+4) * 0x1000 / 4];
static uintptr_t board_iortab[(1+2+4) * 0x1000 / 4];
static uintptr_t board_iowtab[(1+2+4) * 0x1000 / 4];
static void map_ram_window(uintptr_t *table, u32 start, void *ram);

/* Read handlers receive the effective guest address in their first argument.
 * Unlike the write trampoline, the read trampoline does not update the global
 * `address`; using that global here makes every dynamic board read consult the
 * last store's address instead. */
static u32 board_read8(u32 addr)  { return psxMemRead8(&psxRegs, addr); }
static u32 board_read16(u32 addr) { return psxMemRead16(&psxRegs, addr); }
static u32 board_read32(u32 addr) { return psxMemRead32(&psxRegs, addr); }
static void board_write8(u32 value)  { psxMemWrite8(&psxRegs, address, value); }
static void board_write16(u32 value) { psxMemWrite16(&psxRegs, address, value); }
static void board_write32(u32 value) { psxMemWrite32(&psxRegs, address, value); }

static void map_item_(uintptr_t *out, uintptr_t hv, uintptr_t flag)
{
	if (hv & 1) {
		SysPrintf("FATAL: %zx has LSB set\n", hv);
		abort();
	}
	*out = (hv >> 1) | (flag << (sizeof(hv) * 8 - 1));
}

#define map_item(out_, h_, flag_) \
	map_item_(out_, (uintptr_t)(h_), flag_)

// size must be power of 2, at least 4k
#define map_l1_mem(tab, i, addr, size, base) \
	map_item_(&tab[((u32)(addr) >> 12) + i], \
		 (uintptr_t)(base) - (u32)((addr) + ((i << 12) & ~(size - 1))), 0)

#define IOMEM32(a) (((a) & 0xfff) / 4)
#define IOMEM16(a) (0x1000/4 + (((a) & 0xfff) / 2))
#define IOMEM8(a)  (0x1000/4 + 0x1000/2 + ((a) & 0xfff))

u32 zero_mem[0x1000/4];
static u32 ffff_mem[0x1000/4];

static u32 read_mem_dummy(u32 addr)
{
	// use 'addr' and not 'address', yes the api is weird...
	memprintf("unmapped r %08x @%08x %u\n", addr, psxRegs.pc, psxRegs.cycle);
	return 0xffffffff;
}

static void write_mem_dummy(u32 data)
{
	if (!(psxRegs.CP0.n.SR & (1 << 16)))
		memprintf("unmapped w %08x, %08x @%08x %u\n",
			  address, data, psxRegs.pc, psxRegs.cycle);
}

static void write_mem_ro(u32 data)
{
	memprintf("ro w %08x, %08x @%08x %u\n",
		  address, data, psxRegs.pc, psxRegs.cycle);
}

/* IO handlers */
static u32 io_read_sio16()
{
	return sioRead8();
}

static u32 io_read_sio32()
{
	return sioRead8();
}

static void io_write_sio16(u32 value)
{
	sioWrite8(value);
}

#if !defined(DRC_DBG) && defined(__arm__)

/* The asm m1 readers assume console clocks: rcnt1 divides by the NTSC line
 * period and rcnt0 approximates /5. Under arcade source timing (System 12 at
 * 50 MHz) the line period differs, so use the exact C readers like the other
 * backends do. */
static void map_rcnt_rcount0(u32 mode)
{
	if ((mode & 0x001) || ((mode & 0x100) && psxArcadeSourceTimingActive())) { // sync mode, or m1 with source timing
		map_item(&mem_iortab[IOMEM32(0x1100)], psxRcntRcount0, 1);
		map_item(&mem_iortab[IOMEM16(0x1100)], psxRcntRcount0, 1);
	}
	else if (mode & 0x100) { // pixel clock
		map_item(&mem_iortab[IOMEM32(0x1100)], rcnt0_read_count_m1, 1);
		map_item(&mem_iortab[IOMEM16(0x1100)], rcnt0_read_count_m1, 1);
	}
	else {
		map_item(&mem_iortab[IOMEM32(0x1100)], rcnt0_read_count_m0, 1);
		map_item(&mem_iortab[IOMEM16(0x1100)], rcnt0_read_count_m0, 1);
	}
}

static void map_rcnt_rcount1(u32 mode)
{
	if ((mode & 0x001) || ((mode & 0x100) && psxArcadeSourceTimingActive())) { // sync mode, or m1 with source timing
		map_item(&mem_iortab[IOMEM32(0x1110)], psxRcntRcount1, 1);
		map_item(&mem_iortab[IOMEM16(0x1110)], psxRcntRcount1, 1);
	}
	else if (mode & 0x100) { // hcnt
		map_item(&mem_iortab[IOMEM32(0x1110)], rcnt1_read_count_m1, 1);
		map_item(&mem_iortab[IOMEM16(0x1110)], rcnt1_read_count_m1, 1);
	}
	else {
		map_item(&mem_iortab[IOMEM32(0x1110)], rcnt1_read_count_m0, 1);
		map_item(&mem_iortab[IOMEM16(0x1110)], rcnt1_read_count_m0, 1);
	}
}

static void map_rcnt_rcount2(u32 mode)
{
	if ((mode & 7) == 1 || (mode & 7) == 7) { // sync mode
		map_item(&mem_iortab[IOMEM32(0x1120)], &psxRegs.ptrs.psxH[0x1000], 0);
		map_item(&mem_iortab[IOMEM16(0x1120)], &psxRegs.ptrs.psxH[0x1000], 0);
	}
	else if (mode & 0x200) { // clk/8
		map_item(&mem_iortab[IOMEM32(0x1120)], rcnt2_read_count_m1, 1);
		map_item(&mem_iortab[IOMEM16(0x1120)], rcnt2_read_count_m1, 1);
	}
	else {
		map_item(&mem_iortab[IOMEM32(0x1120)], rcnt2_read_count_m0, 1);
		map_item(&mem_iortab[IOMEM16(0x1120)], rcnt2_read_count_m0, 1);
	}
}

#else
#define map_rcnt_rcount0(mode)
#define map_rcnt_rcount1(mode)
#define map_rcnt_rcount2(mode)
#endif

#define make_rcnt_funcs(i) \
static u32 io_rcnt_read8_count##i##odd()  { return psxRcntRcount##i() >> 8; } \
static u32 io_rcnt_read_mode##i()         { return psxRcntRmode(i); } \
static u32 io_rcnt_read8_mode##i##odd()   { return psxRcntRmode(i) >> 8; } \
static u32 io_rcnt_read_target##i()       { return psxRcntRtarget(i); } \
static u32 io_rcnt_read8_target##i##odd() { return psxRcntRtarget(i) >> 8; } \
static void io_rcnt_write_count##i(u32 val)  { psxRcntWcount(i, val & 0xffff); } \
static void io_rcnt_write_mode##i(u32 val)   { psxRcntWmode(i, val); map_rcnt_rcount##i(val); } \
static void io_rcnt_write_target##i(u32 val) { psxRcntWtarget(i, val & 0xffff); }

make_rcnt_funcs(0)
make_rcnt_funcs(1)
make_rcnt_funcs(2)

static u32 io_spu_read8_even(u32 addr)
{
	return SPU_readRegister(addr, psxRegs.cycle) & 0xff;
}

static u32 io_spu_read8_odd(u32 addr)
{
	return SPU_readRegister(addr, psxRegs.cycle) >> 8;
}

static u32 io_spu_read16(u32 addr)
{
	return SPU_readRegister(addr, psxRegs.cycle);
}

static u32 io_spu_read32(u32 addr)
{
	u32 ret;
	ret  = SPU_readRegister(addr, psxRegs.cycle);
	ret |= SPU_readRegister(addr + 2, psxRegs.cycle) << 16;
	return ret;
}

static void io_spu_write8_even(u32 value)
{
	SPU_writeRegister(address, value, psxRegs.cycle);
}

static void io_spu_write8_odd(u32 value)
{
}

static void io_spu_write16(u32 value)
{
	// meh
	SPU_writeRegister(address, value, psxRegs.cycle);
}

static void io_spu_write32(u32 value)
{
	SPUwriteRegister wfunc = SPU_writeRegister;
	u32 a = address;

	wfunc(a, value & 0xffff, psxRegs.cycle);
	wfunc(a + 2, value >> 16, psxRegs.cycle);
}

void new_dyna_pcsx_mem_isolate(int enable)
{
	u32 i;
	u32 ram_size = PSX_RAM_SIZE;
	u32 mapped_span = ram_size > 0x800000u ? ram_size : 0x800000u;

	// note: apparently 0xa0000000 uncached access still works,
	// at least read does for sure, so assume write does too
	memprintf("mem isolate %d\n", enable);
	if (enable) {
		for (i = 0; i < (mapped_span >> 12); i++) {
			map_item(&mem_writetab[0x80000|i], mem_unmwtab, 1);
			map_item(&mem_writetab[0x00000|i], mem_unmwtab, 1);
			//map_item(&mem_writetab[0xa0000|i], mem_unmwtab, 1);
		}
	}
	else {
		void *ram = psxRegs.ptrs.psxM;
		map_ram_window(mem_writetab, 0x80000000u, ram);
		map_ram_window(mem_writetab, 0x00000000u, ram);
		map_ram_window(mem_writetab, 0xa0000000u, ram);
	}
}

static u32 read_biu(u32 addr)
{
	if (addr != 0xfffe0130)
		return read_mem_dummy(addr);

	memprintf("read_biu  %08x @%08x %u\n",
		psxRegs.biuReg, psxRegs.pc, psxRegs.cycle);
	return psxRegs.biuReg;
}

static void write_biu(u32 value)
{
	if (address != 0xfffe0130) {
		write_mem_dummy(value);
		return;
	}

	memprintf("write_biu %08x @%08x %u\n", value, psxRegs.pc, psxRegs.cycle);
	psxRegs.biuReg = value;
}

/*  scph7001 (pc = 8003de60, v1 = 1f8010f0):
  lhu     $t9, 0($v1)
  li      $at, 0xFFF0FFFF
  and     $t0, $t9, $at
  lui     $at, 8
  or      $t1, $t0, $at
  sh      $t1, 0($v1)
*/
#define make_forcew32_func(addr) \
static void io_write_force32_##addr(u32 value) \
{ \
	psxHu32ref(0x##addr) = SWAPu32(value); \
}
make_forcew32_func(1014)
make_forcew32_func(1060)
make_forcew32_func(1080)
make_forcew32_func(1090)
make_forcew32_func(10a0)
make_forcew32_func(10b0)
make_forcew32_func(10c0)
make_forcew32_func(10e0)

void new_dyna_pcsx_mem_load_state(void)
{
	map_rcnt_rcount0(rcnts[0].mode);
	map_rcnt_rcount1(rcnts[1].mode);
	map_rcnt_rcount2(rcnts[2].mode);
}

int pcsxmem_is_handler_dynamic(unsigned int addr)
{
	if (new_dynarec_arcade_memory_enabled()) {
		u32 physical = addr & 0x1fffffffu;
		if ((physical >= 0x1f000000u && physical < 0x1f800000u) ||
		    (physical >= 0x1fa00000u && physical < 0x1fc00000u))
			return 1;
	}
	if ((addr & 0xfffff000) != 0x1f801000)
		return 0;

	addr &= 0xffff;
	return addr == 0x1100 || addr == 0x1110 || addr == 0x1120;
}

static void map_board_window(u32 start, u32 end)
{
	static const u32 segment[] = { 0, 0x80000000u, 0xa0000000u };
	u32 base;
	for (base = 0; base < sizeof(segment) / sizeof(segment[0]); base++) {
		u32 address = segment[base] + start;
		u32 stop = segment[base] + end;
		for (; address < stop; address += 1u << 12) {
			map_item(&mem_readtab[address >> 12], board_iortab, 1);
			map_item(&mem_writetab[address >> 12], board_iowtab, 1);
		}
	}
}

static void map_board_handlers(void)
{
	u32 i;
	if (!new_dynarec_arcade_memory_enabled())
		return;
	for (i = 0; i < 0x1000; i += 4) {
		map_item(&board_iortab[IOMEM32(i)], board_read32, 1);
		map_item(&board_iowtab[IOMEM32(i)], board_write32, 1);
	}
	for (i = 0; i < 0x1000; i += 2) {
		map_item(&board_iortab[IOMEM16(i)], board_read16, 1);
		map_item(&board_iowtab[IOMEM16(i)], board_write16, 1);
	}
	for (i = 0; i < 0x1000; i++) {
		map_item(&board_iortab[IOMEM8(i)], board_read8, 1);
		map_item(&board_iowtab[IOMEM8(i)], board_write8, 1);
	}
	map_board_window(0x1f000000u, 0x1f800000u);
	map_board_window(0x1fa00000u, 0x1fc00000u);
}

static void map_ram_window(uintptr_t *table, u32 start, void *ram)
{
	u32 address;
	u32 ram_size = PSX_RAM_SIZE;
	u32 mapped_span = ram_size > 0x800000u ? ram_size : 0x800000u;
	for (address = 0; address < mapped_span; address += 1u << 12) {
		u32 page = address >> 12;
		map_l1_mem(table, page, start, ram_size, ram);
	}
}

void new_dyna_pcsx_mem_init(void)
{
	int i;

	memset(ffff_mem, 0xff, sizeof(ffff_mem));

	// have to map these further to keep tcache close to .text
	mem_readtab = psxMap(0x08000000, 0x200000 * sizeof(mem_readtab[0]), 0, MAP_TAG_LUTS);
	if (mem_readtab == NULL) {
		SysPrintf("failed to map mem tables\n");
		exit(1);
	}
	mem_writetab = mem_readtab + 0x100000;

	// 1st level lookup:
	//   0: direct mem
	//   1: use 2nd lookup
	// 2nd level lookup:
	//   0: direct mem variable
	//   1: memhandler

	// default/unmapped memhandlers
	for (i = 0; i < 0x100000; i++) {
		//map_item(&mem_readtab[i], mem_unmrtab, 1);
		map_l1_mem(mem_readtab, i, 0, 0x1000, ffff_mem);
		map_item(&mem_writetab[i], mem_unmwtab, 1);
	}

	// RAM aliases cover console mirrors or the full larger arcade RAM window.
	map_ram_window(mem_readtab, 0x80000000u, psxRegs.ptrs.psxM);
	map_ram_window(mem_readtab, 0x00000000u, psxRegs.ptrs.psxM);
	map_ram_window(mem_readtab, 0xa0000000u, psxRegs.ptrs.psxM);
	new_dyna_pcsx_mem_isolate(0);

	// BIOS and its mirrors
	for (i = 0; i < (0x400000 >> 12); i++) {
		map_l1_mem(mem_readtab, i, 0x1fc00000, PSX_ROM_SIZE, psxRegs.ptrs.psxR);
		map_l1_mem(mem_readtab, i, 0x9fc00000, PSX_ROM_SIZE, psxRegs.ptrs.psxR);
		map_l1_mem(mem_readtab, i, 0xbfc00000, PSX_ROM_SIZE, psxRegs.ptrs.psxR);
	}
	map_board_handlers();

	// scratchpad
	map_l1_mem(mem_readtab, 0, 0x1f800000, 0x1000, psxRegs.ptrs.psxH);
	map_l1_mem(mem_readtab, 0, 0x9f800000, 0x1000, psxRegs.ptrs.psxH);
	map_l1_mem(mem_writetab, 0, 0x1f800000, 0x1000, psxRegs.ptrs.psxH);
	map_l1_mem(mem_writetab, 0, 0x9f800000, 0x1000, psxRegs.ptrs.psxH);

	// I/O
	map_item(&mem_readtab[0x1f801000u >> 12], mem_iortab, 1);
	map_item(&mem_readtab[0x9f801000u >> 12], mem_iortab, 1);
	map_item(&mem_readtab[0xbf801000u >> 12], mem_iortab, 1);
	map_item(&mem_writetab[0x1f801000u >> 12], mem_iowtab, 1);
	map_item(&mem_writetab[0x9f801000u >> 12], mem_iowtab, 1);
	map_item(&mem_writetab[0xbf801000u >> 12], mem_iowtab, 1);

	// L2
	// unmapped tables
	for (i = 0; i < (1+2+4) * 0x1000 / 4; i++)
		map_item(&mem_unmwtab[i], write_mem_dummy, 1);

	// fill IO tables
	for (i = 0; i < 0x1000/4; i++) {
		map_item(&mem_iortab[i], &psxRegs.ptrs.psxH[0x1000], 0);
		map_item(&mem_iowtab[i], &psxRegs.ptrs.psxH[0x1000], 0);
	}
	for (; i < 0x1000/4 + 0x1000/2; i++) {
		map_item(&mem_iortab[i], &psxRegs.ptrs.psxH[0x1000], 0);
		map_item(&mem_iowtab[i], &psxRegs.ptrs.psxH[0x1000], 0);
	}
	for (; i < 0x1000/4 + 0x1000/2 + 0x1000; i++) {
		map_item(&mem_iortab[i], &psxRegs.ptrs.psxH[0x1000], 0);
		map_item(&mem_iowtab[i], &psxRegs.ptrs.psxH[0x1000], 0);
	}

	map_item(&mem_iortab[IOMEM32(0x1040)], io_read_sio32, 1);
	map_item(&mem_iortab[IOMEM32(0x1100)], psxRcntRcount0, 1);
	map_item(&mem_iortab[IOMEM32(0x1104)], io_rcnt_read_mode0, 1);
	map_item(&mem_iortab[IOMEM32(0x1108)], io_rcnt_read_target0, 1);
	map_item(&mem_iortab[IOMEM32(0x1110)], psxRcntRcount1, 1);
	map_item(&mem_iortab[IOMEM32(0x1114)], io_rcnt_read_mode1, 1);
	map_item(&mem_iortab[IOMEM32(0x1118)], io_rcnt_read_target1, 1);
	map_item(&mem_iortab[IOMEM32(0x1120)], psxRcntRcount2, 1);
	map_item(&mem_iortab[IOMEM32(0x1124)], io_rcnt_read_mode2, 1);
	map_item(&mem_iortab[IOMEM32(0x1128)], io_rcnt_read_target2, 1);
//	map_item(&mem_iortab[IOMEM32(0x1810)], GPU_readData, 1);
	map_item(&mem_iortab[IOMEM32(0x1814)], psxHwReadGpuSR, 1);
	map_item(&mem_iortab[IOMEM32(0x1820)], mdecRead0, 1);
	map_item(&mem_iortab[IOMEM32(0x1824)], mdecRead1, 1);

	map_item(&mem_iortab[IOMEM16(0x1040)], io_read_sio16, 1);
	map_item(&mem_iortab[IOMEM16(0x1100)], psxRcntRcount0, 1);
	map_item(&mem_iortab[IOMEM16(0x1104)], io_rcnt_read_mode0, 1);
	map_item(&mem_iortab[IOMEM16(0x1108)], io_rcnt_read_target0, 1);
	map_item(&mem_iortab[IOMEM16(0x1110)], psxRcntRcount1, 1);
	map_item(&mem_iortab[IOMEM16(0x1114)], io_rcnt_read_mode1, 1);
	map_item(&mem_iortab[IOMEM16(0x1118)], io_rcnt_read_target1, 1);
	map_item(&mem_iortab[IOMEM16(0x1120)], psxRcntRcount2, 1);
	map_item(&mem_iortab[IOMEM16(0x1124)], io_rcnt_read_mode2, 1);
	map_item(&mem_iortab[IOMEM16(0x1128)], io_rcnt_read_target2, 1);

	map_item(&mem_iortab[IOMEM8(0x1040)], sioRead8, 1);
	map_item(&mem_iortab[IOMEM8(0x1800)], cdrRead0, 1);
	map_item(&mem_iortab[IOMEM8(0x1801)], cdrRead1, 1);
	map_item(&mem_iortab[IOMEM8(0x1802)], cdrRead2, 1);
	map_item(&mem_iortab[IOMEM8(0x1803)], cdrRead3, 1);
	// 16bit regs (rarely used)
	map_item(&mem_iortab[IOMEM8(0x1100)], psxRcntRcount0, 1);
	map_item(&mem_iortab[IOMEM8(0x1101)], io_rcnt_read8_count0odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1104)], io_rcnt_read_mode0, 1);
	map_item(&mem_iortab[IOMEM8(0x1105)], io_rcnt_read8_mode0odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1108)], io_rcnt_read_target0, 1);
	map_item(&mem_iortab[IOMEM8(0x1109)], io_rcnt_read8_target0odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1110)], psxRcntRcount1, 1);
	map_item(&mem_iortab[IOMEM8(0x1111)], io_rcnt_read8_count1odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1114)], io_rcnt_read_mode1, 1);
	map_item(&mem_iortab[IOMEM8(0x1115)], io_rcnt_read8_mode1odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1118)], io_rcnt_read_target1, 1);
	map_item(&mem_iortab[IOMEM8(0x1119)], io_rcnt_read8_target1odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1120)], psxRcntRcount2, 1);
	map_item(&mem_iortab[IOMEM8(0x1121)], io_rcnt_read8_count2odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1124)], io_rcnt_read_mode2, 1);
	map_item(&mem_iortab[IOMEM8(0x1125)], io_rcnt_read8_mode2odd, 1);
	map_item(&mem_iortab[IOMEM8(0x1128)], io_rcnt_read_target2, 1);
	map_item(&mem_iortab[IOMEM8(0x1129)], io_rcnt_read8_target2odd, 1);

	for (i = 0x1c00; i < 0x2000; i += 2) {
		map_item(&mem_iortab[IOMEM8(i)], io_spu_read8_even, 1);
		map_item(&mem_iortab[IOMEM8(i+1)], io_spu_read8_odd, 1);
		map_item(&mem_iortab[IOMEM16(i)], io_spu_read16, 1);
		map_item(&mem_iortab[IOMEM32(i)], io_spu_read32, 1);
	}

	// write(u32 data)
	map_item(&mem_iowtab[IOMEM32(0x1040)], io_write_sio16, 1);
	map_item(&mem_iowtab[IOMEM32(0x1044)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM32(0x1048)], sioWriteMode16, 1);
	map_item(&mem_iowtab[IOMEM32(0x1054)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM32(0x1070)], psxHwWriteIstat, 1);
	map_item(&mem_iowtab[IOMEM32(0x1074)], psxHwWriteImask, 1);
	map_item(&mem_iowtab[IOMEM32(0x1088)], psxHwWriteChcr0, 1);
	map_item(&mem_iowtab[IOMEM32(0x108c)], psxHwWriteChcr0, 1);
	map_item(&mem_iowtab[IOMEM32(0x1098)], psxHwWriteChcr1, 1);
	map_item(&mem_iowtab[IOMEM32(0x109c)], psxHwWriteChcr1, 1);
	map_item(&mem_iowtab[IOMEM32(0x10a8)], psxHwWriteChcr2, 1);
	map_item(&mem_iowtab[IOMEM32(0x10ac)], psxHwWriteChcr2, 1);
	map_item(&mem_iowtab[IOMEM32(0x10b8)], psxHwWriteChcr3, 1);
	map_item(&mem_iowtab[IOMEM32(0x10bc)], psxHwWriteChcr3, 1);
	map_item(&mem_iowtab[IOMEM32(0x10c8)], psxHwWriteChcr4, 1);
	map_item(&mem_iowtab[IOMEM32(0x10cc)], psxHwWriteChcr4, 1);
	#ifdef PCSX_SYSTEM12
	map_item(&mem_iowtab[IOMEM32(0x10d8)], psxHwWriteChcr5, 1);
	map_item(&mem_iowtab[IOMEM32(0x10dc)], psxHwWriteChcr5, 1);
	#endif
	map_item(&mem_iowtab[IOMEM32(0x10e8)], psxHwWriteChcr6, 1);
	map_item(&mem_iowtab[IOMEM32(0x10ec)], psxHwWriteChcr6, 1);
	map_item(&mem_iowtab[IOMEM32(0x10f0)], psxHwWriteDmaPcr32, 1);
	map_item(&mem_iowtab[IOMEM32(0x10f4)], psxHwWriteDmaIcr32, 1);
	map_item(&mem_iowtab[IOMEM32(0x1100)], io_rcnt_write_count0, 1);
	map_item(&mem_iowtab[IOMEM32(0x1104)], io_rcnt_write_mode0, 1);
	map_item(&mem_iowtab[IOMEM32(0x1108)], io_rcnt_write_target0, 1);
	map_item(&mem_iowtab[IOMEM32(0x1110)], io_rcnt_write_count1, 1);
	map_item(&mem_iowtab[IOMEM32(0x1114)], io_rcnt_write_mode1, 1);
	map_item(&mem_iowtab[IOMEM32(0x1118)], io_rcnt_write_target1, 1);
	map_item(&mem_iowtab[IOMEM32(0x1120)], io_rcnt_write_count2, 1);
	map_item(&mem_iowtab[IOMEM32(0x1124)], io_rcnt_write_mode2, 1);
	map_item(&mem_iowtab[IOMEM32(0x1128)], io_rcnt_write_target2, 1);
//	map_item(&mem_iowtab[IOMEM32(0x1810)], GPU_writeData, 1);
	map_item(&mem_iowtab[IOMEM32(0x1814)], psxHwWriteGpuSR, 1);
	map_item(&mem_iowtab[IOMEM32(0x1820)], mdecWrite0, 1);
	map_item(&mem_iowtab[IOMEM32(0x1824)], mdecWrite1, 1);

	map_item(&mem_iowtab[IOMEM16(0x1014)], io_write_force32_1014, 1);
	map_item(&mem_iowtab[IOMEM16(0x1040)], io_write_sio16, 1);
	map_item(&mem_iowtab[IOMEM16(0x1044)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM16(0x1046)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM16(0x1048)], sioWriteMode16, 1);
	map_item(&mem_iowtab[IOMEM16(0x104a)], sioWriteCtrl16, 1);
	map_item(&mem_iowtab[IOMEM16(0x1054)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM16(0x1056)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM16(0x1060)], io_write_force32_1060, 1);
	map_item(&mem_iowtab[IOMEM16(0x1070)], psxHwWriteIstat, 1);
	map_item(&mem_iowtab[IOMEM16(0x1074)], psxHwWriteImask, 1);
	map_item(&mem_iowtab[IOMEM16(0x1080)], io_write_force32_1080, 1);
	map_item(&mem_iowtab[IOMEM16(0x1088)], psxHwWriteChcr0, 1);
	map_item(&mem_iowtab[IOMEM16(0x108c)], psxHwWriteChcr0, 1);
	map_item(&mem_iowtab[IOMEM16(0x1090)], io_write_force32_1090, 1);
	map_item(&mem_iowtab[IOMEM16(0x1098)], psxHwWriteChcr1, 1);
	map_item(&mem_iowtab[IOMEM16(0x109c)], psxHwWriteChcr1, 1);
	map_item(&mem_iowtab[IOMEM16(0x10a0)], io_write_force32_10a0, 1);
	map_item(&mem_iowtab[IOMEM16(0x10a8)], psxHwWriteChcr2, 1);
	map_item(&mem_iowtab[IOMEM16(0x10ac)], psxHwWriteChcr2, 1);
	map_item(&mem_iowtab[IOMEM16(0x10b0)], io_write_force32_10b0, 1);
	map_item(&mem_iowtab[IOMEM16(0x10b8)], psxHwWriteChcr3, 1);
	map_item(&mem_iowtab[IOMEM16(0x10bc)], psxHwWriteChcr3, 1);
	map_item(&mem_iowtab[IOMEM16(0x10c0)], io_write_force32_10c0, 1);
	map_item(&mem_iowtab[IOMEM16(0x10c8)], psxHwWriteChcr4, 1);
	map_item(&mem_iowtab[IOMEM16(0x10cc)], psxHwWriteChcr4, 1);
	map_item(&mem_iowtab[IOMEM16(0x10e0)], io_write_force32_10e0, 1);
	#ifdef PCSX_SYSTEM12
	map_item(&mem_iowtab[IOMEM16(0x10d8)], psxHwWriteChcr5, 1);
	map_item(&mem_iowtab[IOMEM16(0x10dc)], psxHwWriteChcr5, 1);
	#endif
	map_item(&mem_iowtab[IOMEM16(0x10e8)], psxHwWriteChcr6, 1);
	map_item(&mem_iowtab[IOMEM16(0x10ec)], psxHwWriteChcr6, 1);
	map_item(&mem_iowtab[IOMEM16(0x10f0)], psxHwWriteDmaPcr32, 1);
	map_item(&mem_iowtab[IOMEM16(0x10f4)], psxHwWriteDmaIcr32, 1);
	map_item(&mem_iowtab[IOMEM16(0x1100)], io_rcnt_write_count0, 1);
	map_item(&mem_iowtab[IOMEM16(0x1104)], io_rcnt_write_mode0, 1);
	map_item(&mem_iowtab[IOMEM16(0x1108)], io_rcnt_write_target0, 1);
	map_item(&mem_iowtab[IOMEM16(0x1110)], io_rcnt_write_count1, 1);
	map_item(&mem_iowtab[IOMEM16(0x1114)], io_rcnt_write_mode1, 1);
	map_item(&mem_iowtab[IOMEM16(0x1118)], io_rcnt_write_target1, 1);
	map_item(&mem_iowtab[IOMEM16(0x1120)], io_rcnt_write_count2, 1);
	map_item(&mem_iowtab[IOMEM16(0x1124)], io_rcnt_write_mode2, 1);
	map_item(&mem_iowtab[IOMEM16(0x1128)], io_rcnt_write_target2, 1);

	map_item(&mem_iowtab[IOMEM8(0x1040)], sioWrite8, 1);
	map_item(&mem_iowtab[IOMEM8(0x1044)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1045)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1046)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1047)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1054)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1055)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1056)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1057)], write_mem_ro, 1);
	map_item(&mem_iowtab[IOMEM8(0x1800)], cdrWrite0, 1);
	map_item(&mem_iowtab[IOMEM8(0x1801)], cdrWrite1, 1);
	map_item(&mem_iowtab[IOMEM8(0x1802)], cdrWrite2, 1);
	map_item(&mem_iowtab[IOMEM8(0x1803)], cdrWrite3, 1);

	for (i = 0x1c00; i < 0x2000; i += 2) {
		map_item(&mem_iowtab[IOMEM8(i)], io_spu_write8_even, 1);
		map_item(&mem_iowtab[IOMEM8(i+1)], io_spu_write8_odd, 1);
		map_item(&mem_iowtab[IOMEM16(i)], io_spu_write16, 1);
		map_item(&mem_iowtab[IOMEM32(i)], io_spu_write32, 1);
	}

	// misc
	map_item(&mem_readtab[0xfffe0130u >> 12], mem_ffrtab, 1);
	map_item(&mem_writetab[0xfffe0130u >> 12], mem_ffwtab, 1);
	for (i = 0; i < 0x1000/4 + 0x1000/2 + 0x1000; i++) {
		map_item(&mem_ffrtab[i], read_biu, 1);
		map_item(&mem_ffwtab[i], write_biu, 1);
	}

	mem_rtab = mem_readtab;
	mem_wtab = mem_writetab;

#ifdef PCSX_SYSTEM12
	/* System 10 and Capcom ZN use both real PSX serial devices. Console SIO handlers
	 * and direct-register shortcuts must not bypass the board's SIO1 state. */
	if (system10_active() || systemzn_capcom_active()) {
		for (i = 0x1040; i < 0x1060; i += 4) {
			map_item(&mem_iortab[IOMEM32(i)], board_read32, 1);
			map_item(&mem_iowtab[IOMEM32(i)], board_write32, 1);
		}
		for (i = 0x1040; i < 0x1060; i += 2) {
			map_item(&mem_iortab[IOMEM16(i)], board_read16, 1);
			map_item(&mem_iowtab[IOMEM16(i)], board_write16, 1);
		}
		for (i = 0x1040; i < 0x1060; i++) {
			map_item(&mem_iortab[IOMEM8(i)], board_read8, 1);
			map_item(&mem_iowtab[IOMEM8(i)], board_write8, 1);
		}
	}
#endif

	new_dyna_pcsx_mem_load_state();
	if (new_dynarec_arcade_memory_enabled() && !new_dyna_pcsx_mem_arcade_selftest()) {
		SysPrintf("Arcade DRC memory-table selftest failed\n");
		abort();
	}
}

static int ndrc_check_direct_mapping(uintptr_t *table, u32 address, const u8 *expected)
{
	uintptr_t msb = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
	uintptr_t l1 = table[address >> 12];
	if (l1 & msb)
		return 0;
	return (const u8 *)((l1 << 1) + address) == expected;
}

static int ndrc_check_board_handler(uintptr_t *table, u32 address,
	unsigned width, uintptr_t expected)
{
	uintptr_t msb = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
	uintptr_t l1 = table[address >> 12];
	uintptr_t *l2;
	unsigned index;
	if (!(l1 & msb))
		return 0;
	l2 = (uintptr_t *)(l1 << 1);
	if (width == 1)
		index = 0x1000 / 4 + 0x1000 / 2 + (address & 0xfff);
	else if (width == 2)
		index = 0x1000 / 4 + ((address & 0xfff) / 2);
	else
		index = (address & 0xfff) / 4;
	if (!(l2[index] & msb))
		return 0;
	return (uintptr_t)(l2[index] << 1) == expected;
}

int new_dyna_pcsx_mem_arcade_selftest(void)
{
	static const u32 segment[] = { 0, 0x80000000u, 0xa0000000u };
	u32 ram_size = PSX_RAM_SIZE;
	u32 offsets[4], count = 0, i, s;
	u32 rom_offset;
	if (!mem_readtab || !mem_writetab || ram_size < 0x200000u ||
	    (ram_size & (ram_size - 1)) != 0)
		return 0;
	offsets[count++] = 0;
	if (ram_size >= 0x400000u)
		offsets[count++] = 0x200000u;
	if (ram_size >= 0x1000000u)
		offsets[count++] = 0x800000u;
	offsets[count++] = ram_size - 4;
	for (i = 0; i < count; i++) {
		u32 offset = offsets[i];
		for (s = 0; s < sizeof(segment) / sizeof(segment[0]); s++) {
			u32 address = segment[s] + offset;
			if (!ndrc_check_direct_mapping(mem_readtab, address,
					psxRegs.ptrs.psxM + offset) ||
			    !ndrc_check_direct_mapping(mem_writetab, address,
					psxRegs.ptrs.psxM + offset))
				return 0;
		}
	}
	if (ram_size < 0x800000u &&
	    !ndrc_check_direct_mapping(mem_readtab, ram_size, psxRegs.ptrs.psxM))
		return 0;
	rom_offset = PSX_ROM_SIZE - 4;
	if (!ndrc_check_direct_mapping(mem_readtab, 0x1fc00000u + rom_offset,
			psxRegs.ptrs.psxR + rom_offset) ||
	    !ndrc_check_direct_mapping(mem_readtab, 0x9fc00000u + rom_offset,
			psxRegs.ptrs.psxR + rom_offset) ||
	    !ndrc_check_direct_mapping(mem_readtab, 0xbfc00000u + rom_offset,
			psxRegs.ptrs.psxR + rom_offset))
		return 0;
	if (new_dynarec_arcade_memory_enabled()) {
		static const u32 checks[] = { 0x1f000000u, 0x9f000000u,
			0xbf000000u, 0x1fa00000u, 0x9fa00000u, 0xbfa00000u };
		for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
			if (!ndrc_check_board_handler(mem_readtab, checks[i], 4,
					(uintptr_t)board_read32) ||
			    !ndrc_check_board_handler(mem_writetab, checks[i], 4,
					(uintptr_t)board_write32) ||
			    !ndrc_check_board_handler(mem_readtab, checks[i] + 1, 1,
					(uintptr_t)board_read8) ||
			    !ndrc_check_board_handler(mem_writetab, checks[i] + 1, 1,
					(uintptr_t)board_write8))
				return 0;
	}
	return 1;
}

void new_dyna_pcsx_mem_reset(void)
{
	// plugins might change so update the pointers
	map_item(&mem_iortab[IOMEM32(0x1810)], GPU_readData, 1);
	map_item(&mem_iowtab[IOMEM32(0x1810)], GPU_writeData, 1);
}

void new_dyna_pcsx_mem_shutdown(void)
{
	psxUnmap(mem_readtab, 0x200000 * sizeof(mem_readtab[0]), MAP_TAG_LUTS);
	mem_writetab = mem_readtab = NULL;
}
