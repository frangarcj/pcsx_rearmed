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

#include "psxcommon.h"
#include "r3000a.h"
#include "psxbios.h"

#include "cheat.h"
#include "ppf.h"

PcsxConfig Config;

/* Most PCSX timing still uses the retail PSX clock. Arcade source profiles
 * replace it during the init/reset lifecycle, not from PSXCLK hot paths. */
u32 psxCycleClockHz = 33868800u;
static u32 psx_source_input_clock_hz;
static int psx_source_timing_active;
static int psx_source_multiplier_forced;
static int psx_source_saved_multiplier;

#if defined(PCSX_SYSTEM12)
extern int systemtaito_active(void);
extern int systemzn_active(void);
#ifdef PCSX_SYSTEM_TWINKLE
extern int systemtwinkle_active(void);
#endif
#ifdef PCSX_SYSTEM573
extern int system573_active(void);
#endif
extern unsigned int systemzn_main_clock_hz(void);
extern int system10_active(void);
extern int system11_active(void);
extern int system12_active(void);
#ifdef PCSX_SYSTEM_GNET
extern int systemgnet_active(void);
extern uint32_t systemgnet_main_clock_hz(void);
#endif
#ifdef PCSX_SYSTEM_GV
extern int systemgv_active(void);
#endif
#ifdef PCSX_SYSTEM_GQ
extern int systemgq_active(void);
#endif
#endif

static u32 arcade_source_input_clock(void)
{
    u32 clock = 0;

#if defined(PCSX_ARCADE_SOURCE_TIMING) && defined(PCSX_SYSTEM12)
#ifdef PCSX_SYSTEM_GNET
    if (systemgnet_active()) clock = systemgnet_main_clock_hz();
#endif
#ifdef PCSX_SYSTEM_TWINKLE
    if (!clock && systemtwinkle_active()) clock = 67737600u;
#endif
#ifdef PCSX_SYSTEM573
    if (!clock && system573_active()) clock = 67737600u;
#endif
    if (!clock && systemtaito_active()) clock = 67737600u;
    if (!clock && systemzn_active()) clock = systemzn_main_clock_hz();
#ifdef PCSX_SYSTEM_GV
    if (!clock && systemgv_active()) clock = 67737600u;
#endif
#ifdef PCSX_SYSTEM_GQ
    if (!clock && systemgq_active()) clock = 67737600u;
#endif
    if (!clock && system11_active()) clock = 67737600u;
    if (!clock && system10_active()) clock = 101491200u;
#endif

/* Keep the existing S12 source-timing switch independently controllable.
 * It is the only source profile admitted for S12, including builds that also
 * enable generic arcade timing for the other supported families. */
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) && defined(PCSX_SYSTEM12)
    if (!clock && system12_active()) clock = 100000000u;
#endif
    return clock;
}

u32 psxArcadeSourceInputClockHz(void)
{
    return psx_source_input_clock_hz;
}

int psxArcadeSourceTimingActive(void)
{
    return psx_source_timing_active;
}

void psxTimingRefresh(void)
{
    u32 input_clock = arcade_source_input_clock();
    int source_active = input_clock >= 2 && !(input_clock & 1);

    if (psx_source_multiplier_forced && Config.cycle_multiplier != 200)
        psx_source_saved_multiplier = Config.cycle_multiplier;
    if (source_active) {
        if (!psx_source_multiplier_forced)
            psx_source_saved_multiplier = Config.cycle_multiplier;
        Config.cycle_multiplier = 200;
        psxCycleClockHz = input_clock / 2;
    } else {
        if (psx_source_multiplier_forced)
            Config.cycle_multiplier = psx_source_saved_multiplier;
        psxCycleClockHz = 33868800u;
    }
    psx_source_input_clock_hz = source_active ? input_clock : 0;
    psx_source_timing_active = source_active;
    psx_source_multiplier_forced = source_active;
}

double psxGpuSourceRefreshHz(u32 gpu_status)
{
    if (gpu_status & (1u << 20)) return 50.0;
    return gpu_status & (1u << 19) ? 59.9400523286 : 59.8260978565;
}

int EmuInit() {
	return psxInit();
}

void EmuReset() {
	FreeCheatSearchResults();
	FreeCheatSearchMem();

	psxReset();
}

void EmuShutdown() {
	ClearAllCheats();
	FreeCheatSearchResults();
	FreeCheatSearchMem();

	FreePPFCache();

	psxShutdown();
}

void EmuUpdate() {
	ApplyCheats();

	// reamed hack
	{
		extern void pl_frame_limit(void);
		pl_frame_limit();
	}
}
