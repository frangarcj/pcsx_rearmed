/***************************************************************************
 *   Copyright (C) 2010 by Blade_Arma                                      *
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
 * Internal PSX counters.
 */

#include "psxcounters.h"
#include "psxevents.h"
#include "gpu.h"
#include "arcade/psx_screen_clock.h"
#ifdef PCSX_SYSTEM12
#include "system12.h"
#include "systemzn.h"
#endif
//#include "debug.h"
#define DebugVSync()

/******************************************************************************/

enum
{
    RcSyncModeEnable  = 0x0001, // 0
    Rc01BlankPause    = 0 << 1, // 1,2
    Rc01UnblankReset  = 1 << 1, // 1,2
    Rc01UnblankReset2 = 2 << 1, // 1,2
    Rc2Stop           = 0 << 1, // 1,2
    Rc2Stop2          = 3 << 1, // 1,2
    RcCountToTarget   = 0x0008, // 3
    RcIrqOnTarget     = 0x0010, // 4
    RcIrqOnOverflow   = 0x0020, // 5
    RcIrqRegenerate   = 0x0040, // 6
    RcUnknown7        = 0x0080, // 7    ?
    Rc0PixelClock     = 0x0100, // 8    fake implementation
    Rc1HSyncClock     = 0x0100, // 8
    Rc2Unknown8       = 0x0100, // 8    ?
    Rc0Unknown9       = 0x0200, // 9    ?
    Rc1Unknown9       = 0x0200, // 9    ?
    Rc2OneEighthClock = 0x0200, // 9
    RcUnknown10       = 0x0400, // 10   ?
    RcCountEqTarget   = 0x0800, // 11
    RcOverflow        = 0x1000, // 12
    RcUnknown13       = 0x2000, // 13   ? (always zero)
    RcUnknown14       = 0x4000, // 14   ? (always zero)
    RcUnknown15       = 0x8000, // 15   ? (always zero)
};

#define CounterQuantity           ( 4 )
//static const u32 CounterQuantity  = 4;

static const u32 CountToOverflow  = 0;
static const u32 CountToTarget    = 1;

static const u32 HSyncTotal[]     = { 263, 314 };
#define VBlankStart vblankStart()

#define VERBOSE_LEVEL 0

/******************************************************************************/
#ifdef DRC_DISABLE
Rcnt rcnts[ CounterQuantity ];
#endif
unsigned int hSyncCount = 0;
unsigned int frame_counter = 0;
static u32 hsync_steps = 0;
#ifdef PCSX_SYSTEM12
static psx_screen_clock source_screen;
static uint64_t source_main_cycles, source_epoch_cycles;
static u32 source_last_cycle;
static int source_screen_updating;

static int sourceScreenActive(void)
{
    return system12_active() && psxArcadeSourceTimingActive() && source_screen.configured;
}

static psx_screen_time sourceScreenTime(void)
{
    psx_screen_time now;
    source_main_cycles += (u32)(psxRegs.cycle - source_last_cycle);
    source_last_cycle = psxRegs.cycle;
    /* System12's 50 MHz main clock has an exact 20 ns period. */
    now.seconds = source_main_cycles / PSXCLK;
    now.attoseconds = (source_main_cycles % PSXCLK) *
        (PSX_SCREEN_ATTOSECONDS_PER_SECOND / PSXCLK);
    return now;
}

static uint64_t sourceScreenPeriod(u32 status)
{
    return status & (1u << 20) ? PSX_SCREEN_PAL_PERIOD :
        status & (1u << 19) ? PSX_SCREEN_NTSC_480_PERIOD : PSX_SCREEN_NTSC_240_PERIOD;
}
#endif

/******************************************************************************/

#define FPS_FRACTIONAL_PAL  (53203425/314./3406) // ~49.75
#define FPS_FRACTIONAL_NTSC (53693175/263./3413) // ~59.81

#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
static inline double arcadeSourceFps(void)
{
    return psxGpuSourceRefreshHz(SWAP32(HW_GPU_STATUS));
}
#endif

static inline u32 hsyncTotal(void)
{
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
    if (psxArcadeSourceTimingActive()) return HSyncTotal[!!(SWAP32(HW_GPU_STATUS) & (1u << 20))];
#endif
    return HSyncTotal[Config.PsxType];
}

static inline u32 vblankStart(void)
{
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
    if (psxArcadeSourceTimingActive()) {
        double blank_lines = hsyncTotal() * arcadeSourceFps() / 400.0;
        return hsyncTotal() - (u32)(blank_lines + 0.999999);
    }
#endif
    return 240;
}

static inline
u32 frameCycles(void)
{
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
    if (psxArcadeSourceTimingActive()) return (u32)(PSXCLK / arcadeSourceFps());
#endif
    int ff = Config.FractionalFramerate >= 0
        ? Config.FractionalFramerate : Config.hacks.fractional_Framerate;
    if (ff)
    {
        if (Config.PsxType)
            return (u32)(PSXCLK / FPS_FRACTIONAL_PAL);
        else
            return (u32)(PSXCLK / FPS_FRACTIONAL_NTSC);
    }
    return Config.PsxType ? (PSXCLK / 50) : (PSXCLK / 60);
}

// used to inform the frontend about the exact framerate
double psxGetFps()
{
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
    if (psxArcadeSourceTimingActive()) return arcadeSourceFps();
#endif
    int ff = Config.FractionalFramerate >= 0
        ? Config.FractionalFramerate : Config.hacks.fractional_Framerate;
    if (ff)
        return Config.PsxType ? FPS_FRACTIONAL_PAL : FPS_FRACTIONAL_NTSC;
    else
        return Config.PsxType ? 50.0 : 60.0;
}

// to inform the frontend about the exact famerate
static inline
u32 lineCycles(void)
{
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
    if (psxArcadeSourceTimingActive()) return frameCycles() / hsyncTotal();
#endif
    // should be more like above, but our timing is already poor anyway
    if (Config.PsxType)
        return PSXCLK / 50 / HSyncTotal[1];
    else
        return PSXCLK / 60 / HSyncTotal[0];
}

static inline
void setIrq( u32 irq )
{
    psxHu32ref(0x1070) |= SWAPu32(irq);
}

static
void verboseLog( u32 level, const char *str, ... )
{
#if VERBOSE_LEVEL > 0
    if( level <= VERBOSE_LEVEL )
    {
        va_list va;
        char buf[ 4096 ];

        va_start( va, str );
        vsprintf( buf, str, va );
        va_end( va );

        printf( "%s", buf );
        fflush( stdout );
    }
#endif
}

/******************************************************************************/

static inline
void _psxRcntWcount( u32 index, u32 value )
{
    value &= 0xffff;

    rcnts[index].cycleStart  = psxRegs.cycle;
    rcnts[index].cycleStart -= value * rcnts[index].rate;

    // TODO: <=.
    if( value < rcnts[index].target )
    {
        rcnts[index].cycle = rcnts[index].target * rcnts[index].rate;
        rcnts[index].counterState = CountToTarget;
    }
    else
    {
        rcnts[index].cycle = 0x10000 * rcnts[index].rate;
        rcnts[index].counterState = CountToOverflow;
    }
}

static inline
u32 _psxRcntRcount( u32 index )
{
    u32 count;

    count  = psxRegs.cycle;
    count -= rcnts[index].cycleStart;
    if (rcnts[index].rate > 1)
        count /= rcnts[index].rate;

    if( count > 0x10000 )
    {
        verboseLog( 1, "[RCNT %i] rcount > 0x10000: %x\n", index, count );
    }
    count &= 0xffff;

    return count;
}

static
void _psxRcntWmode( u32 index, u32 value )
{
    rcnts[index].mode = value;

    switch( index )
    {
        case 0:
            if( value & Rc0PixelClock )
            {
                rcnts[index].rate = 5;
            }
            else
            {
                rcnts[index].rate = 1;
            }
        break;
        case 1:
            if( value & Rc1HSyncClock )
            {
                rcnts[index].rate = lineCycles();
            }
            else
            {
                rcnts[index].rate = 1;
            }
        break;
        case 2:
            if( value & Rc2OneEighthClock )
            {
                rcnts[index].rate = 8;
            }
            else
            {
                rcnts[index].rate = 1;
            }

            // TODO: wcount must work.
            if( (value & 7) == (RcSyncModeEnable | Rc2Stop) ||
                (value & 7) == (RcSyncModeEnable | Rc2Stop2) )
            {
                rcnts[index].rate = 0xffffffff;
            }
        break;
    }
}

/******************************************************************************/

static
void psxRcntSet()
{
    s32 countToUpdate;
    u32 i;

    psxRegs.psxNextsCounter = psxRegs.cycle;
    psxRegs.psxNextCounter  = 0x7fffffff;

    for( i = 0; i < CounterQuantity; ++i )
    {
        countToUpdate = rcnts[i].cycle - (psxRegs.psxNextsCounter - rcnts[i].cycleStart);

        if( countToUpdate < 0 )
        {
            psxRegs.psxNextCounter = 0;
            break;
        }

        if( countToUpdate < (s32)psxRegs.psxNextCounter )
        {
            psxRegs.psxNextCounter = countToUpdate;
        }
    }

    set_event(PSXINT_RCNT, psxRegs.psxNextCounter);
}

/******************************************************************************/

static
void psxRcntReset( u32 index )
{
    u32 rcycles;

    rcnts[index].mode |= RcUnknown10;

    if( rcnts[index].counterState == CountToTarget )
    {
        rcycles = psxRegs.cycle - rcnts[index].cycleStart;
        if( rcnts[index].mode & RcCountToTarget )
        {
            rcycles -= rcnts[index].target * rcnts[index].rate;
            rcnts[index].cycleStart = psxRegs.cycle - rcycles;
        }
        else
        {
            rcnts[index].cycle = 0x10000 * rcnts[index].rate;
            rcnts[index].counterState = CountToOverflow;
        }

        if( rcnts[index].mode & RcIrqOnTarget )
        {
            if( (rcnts[index].mode & RcIrqRegenerate) || (!rcnts[index].irqState) )
            {
                verboseLog( 3, "[RCNT %i] irq\n", index );
                setIrq( rcnts[index].irq );
                rcnts[index].irqState = 1;
            }
        }

        rcnts[index].mode |= RcCountEqTarget;

        if( rcycles < 0x10000 * rcnts[index].rate )
            return;
    }

    if( rcnts[index].counterState == CountToOverflow )
    {
        rcycles = psxRegs.cycle - rcnts[index].cycleStart;
        rcycles -= 0x10000 * rcnts[index].rate;

        rcnts[index].cycleStart = psxRegs.cycle - rcycles;

        if( rcycles < rcnts[index].target * rcnts[index].rate )
        {
            rcnts[index].cycle = rcnts[index].target * rcnts[index].rate;
            rcnts[index].counterState = CountToTarget;
        }

        if( rcnts[index].mode & RcIrqOnOverflow )
        {
            if( (rcnts[index].mode & RcIrqRegenerate) || (!rcnts[index].irqState) )
            {
                verboseLog( 3, "[RCNT %i] irq\n", index );
                setIrq( rcnts[index].irq );
                rcnts[index].irqState = 1;
            }
        }

        rcnts[index].mode |= RcOverflow;
    }
}

static void scheduleRcntBase(void)
{
#ifdef PCSX_SYSTEM12
    if (sourceScreenActive()) {
        psx_screen_event next;
        uint64_t deadline, delay;
        psx_screen_clock_next_deadline(&source_screen, &next);
        deadline = psx_screen_time_to_cycles_ceil(next.time, PSXCLK);
        delay = deadline > source_main_cycles ? deadline - source_main_cycles : 0;
        rcnts[3].cycleStart = psxRegs.cycle;
        rcnts[3].cycle = delay > 0x7fffffffu ? 0x7fffffffu : (u32)delay;
        return;
    }
#endif
    // Schedule next call, in hsyncs
    if (hSyncCount < VBlankStart)
        hsync_steps = VBlankStart - hSyncCount;
    else
        hsync_steps = hsyncTotal() - hSyncCount;

    if (hSyncCount + hsync_steps == hsyncTotal())
    {
        rcnts[3].cycle = frameCycles();
    }
    else
    {
        // clk / 50 / 314 ~= 2157.25
        // clk / 60 / 263 ~= 2146.31
#if defined(PCSX_SYSTEM12_SOURCE_TIMING) || defined(PCSX_ARCADE_SOURCE_TIMING)
        if (psxArcadeSourceTimingActive())
            rcnts[3].cycle = frameCycles() - PSXCLK / 400;
        else
#endif
        {
            u32 mult = Config.PsxType ? 8836089 : 8791293;
            rcnts[3].cycle = hsync_steps * mult >> 12;
        }
    }
}

static void deliverVblankStart(const psx_screen_time *when)
{
    HW_GPU_STATUS &= SWAP32(~PSXGPU_LCF);
    GPU_vBlank(1, 0);
#ifdef PCSX_SYSTEM12
    systemzn_vblank(1);
    if (when) system12_vblank_at(1, when);
    else system12_vblank(1);
#endif
    setIrq(0x01);
    EmuUpdate();
    if (SPU_async) SPU_async(psxRegs.cycle, 1);
}

static void deliverVblankEnd(const psx_screen_time *when)
{
    u32 status, field = 0;
    hSyncCount = 0;
    frame_counter++;
    gpuSyncPluginSR();
    status = SWAP32(HW_GPU_STATUS) | PSXGPU_FIELD;
    if ((status & PSXGPU_ILACE_BITS) == PSXGPU_ILACE_BITS) {
        field = frame_counter & 1;
        status |= field << 31;
        status ^= field << 13;
    }
    HW_GPU_STATUS = SWAP32(status);
    GPU_vBlank(0, field);
#ifdef PCSX_SYSTEM12
    systemzn_vblank(0);
    if (when) system12_vblank_at(0, when);
    else system12_vblank(0);
#endif
    if ((s32)(psxRegs.gpuIdleAfter - psxRegs.cycle) < 0)
        psxRegs.gpuIdleAfter = psxRegs.cycle - 1;
    if ((rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset) ||
        (rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset2))
        rcnts[0].cycleStart = rcnts[3].cycleStart;
    if ((rcnts[1].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset) ||
        (rcnts[1].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset2))
        rcnts[1].cycleStart = rcnts[3].cycleStart;
    else if (rcnts[1].mode & Rc1HSyncClock)
        _psxRcntWcount(1, (psxRegs.cycle - rcnts[1].cycleStart) / rcnts[1].rate);
}

#ifdef PCSX_SYSTEM12
static void deliverSourceScreenEvent(const psx_screen_event *event)
{
    rcnts[3].cycleStart = (u32)(psx_screen_time_to_cycles_ceil(event->time, PSXCLK) -
        source_epoch_cycles);
    if (event->type == PSX_SCREEN_EVENT_VBLANK_START) {
        hSyncCount = VBlankStart;
        deliverVblankStart(&event->time);
    } else deliverVblankEnd(&event->time);
}

void psxRcntSourceScreenUpdate(void)
{
    psx_screen_event event;
    psx_screen_time target;
    int delivered = 0;
    if (!sourceScreenActive() || source_screen_updating) return;
    source_screen_updating = 1;
    target = sourceScreenTime();
    while (psx_screen_clock_advance_to(&source_screen, target, &event)) {
        deliverSourceScreenEvent(&event);
        delivered = 1;
    }
    /* Some frontends rely exclusively on the core's vblank catch-up. Advance
     * RTC/EEPROM/PCM too, after every overdue edge is delivered; advancing to
     * the late MIPS endpoint between two edges would let H8 pass the next one.
     * Keep the guard set during system12_sync's screen-update reentry. */
    if (delivered) system12_sync(psxRegs.cycle);
    scheduleRcntBase();
    psxRcntSet();
    source_screen_updating = 0;
}

void psxRcntSourceScreenConfigure(u32 gpu_status)
{
    psx_screen_event event;
    if (!sourceScreenActive()) return;
    psxRcntSourceScreenUpdate();
    if (psx_screen_clock_configure(&source_screen, sourceScreenTime(),
        sourceScreenPeriod(gpu_status), &event)) {
        deliverSourceScreenEvent(&event);
        system12_sync(psxRegs.cycle);
    }
    scheduleRcntBase();
    psxRcntSet();
}

void psxRcntSourceScreenReset(int cold, u32 gpu_status)
{
    psx_screen_event event;
    if (cold) {
        psx_screen_clock_init(&source_screen);
        source_main_cycles = 0;
    }
    source_epoch_cycles = source_main_cycles;
    source_last_cycle = psxRegs.cycle;
    if (psx_screen_clock_configure(&source_screen, sourceScreenTime(),
        sourceScreenPeriod(gpu_status), &event)) deliverSourceScreenEvent(&event);
    scheduleRcntBase();
    psxRcntSet();
}

/* Absolute source time of the main CPU, on the screen timeline's epoch. */
psx_screen_time psxRcntSourceTime(void)
{
    return sourceScreenTime();
}

/* Next armed screen timer, a MAME global scheduling boundary. */
int psxRcntSourceScreenNext(psx_screen_time *when)
{
    psx_screen_event next;
    if (!sourceScreenActive() || !psx_screen_clock_next_deadline(&source_screen, &next))
        return 0;
    *when = next.time;
    return 1;
}

void psxRcntSourceScreenUnload(void)
{
    psx_screen_clock_init(&source_screen);
    source_main_cycles = source_epoch_cycles = 0;
    source_last_cycle = 0;
    source_screen_updating = 0;
}
#endif

void psxRcntUpdate()
{
    u32 cycle, cycles_passed;

    cycle = psxRegs.cycle;

    // rcnt 0.
    cycles_passed = cycle - rcnts[0].cycleStart;
    while( cycles_passed >= rcnts[0].cycle )
    {
        if (((rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset) ||
             (rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset2))
            && cycles_passed > lineCycles())
        {
            u32 q = cycles_passed / (lineCycles() + 1u);
            rcnts[0].cycleStart += q * lineCycles();
            break;
        }
        else
            psxRcntReset( 0 );

        cycles_passed = cycle - rcnts[0].cycleStart;
    }

    // rcnt 1.
    while( cycle - rcnts[1].cycleStart >= rcnts[1].cycle )
    {
        psxRcntReset( 1 );
    }

    // rcnt 2.
    while( cycle - rcnts[2].cycleStart >= rcnts[2].cycle )
    {
        psxRcntReset( 2 );
    }

    // rcnt base.
#ifdef PCSX_SYSTEM12
    if (sourceScreenActive()) psxRcntSourceScreenUpdate();
    else
#endif
    if (cycle - rcnts[3].cycleStart >= rcnts[3].cycle)
    {
        hSyncCount += hsync_steps;
        if (hSyncCount == VBlankStart) deliverVblankStart(NULL);
        if (hSyncCount >= hsyncTotal()) {
            rcnts[3].cycleStart += frameCycles();
            deliverVblankEnd(NULL);
        }
        scheduleRcntBase();
    }

    psxRcntSet();

#if 0 //ndef NDEBUG
    DebugVSync();
#endif
}

/******************************************************************************/

void psxRcntWcount( u32 index, u32 value )
{
    verboseLog( 2, "[RCNT %i] wcount: %x\n", index, value );

    _psxRcntWcount( index, value );
    psxRcntSet();
}

void psxRcntWmode( u32 index, u32 value )
{
    verboseLog( 1, "[RCNT %i] wmode: %x\n", index, value );

    _psxRcntWmode( index, value );
    _psxRcntWcount( index, 0 );

    rcnts[index].irqState = 0;
    psxRcntSet();
}

void psxRcntWtarget( u32 index, u32 value )
{
    verboseLog( 1, "[RCNT %i] wtarget: %x\n", index, value );

    rcnts[index].target = value;

    _psxRcntWcount( index, _psxRcntRcount( index ) );
    psxRcntSet();
}

/******************************************************************************/

u32 psxRcntRcount0()
{
    u32 index = 0;
    u32 count;

    if ((rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset) ||
        (rcnts[0].mode & 7) == (RcSyncModeEnable | Rc01UnblankReset2))
    {
        count = psxRegs.cycle - rcnts[index].cycleStart;
        //count = ((16u * count) % (16u * PSXCLK / 60 / 263)) / 16u;
        count = count % lineCycles();
        rcnts[index].cycleStart = psxRegs.cycle - count;
    }
    else
        count = _psxRcntRcount( index );

    verboseLog( 2, "[RCNT 0] rcount: %04x m: %04x\n", count, rcnts[index].mode);

    return count;
}

u32 psxRcntRcount1()
{
    u32 index = 1;
    u32 count;

    count = _psxRcntRcount( index );

    verboseLog( 2, "[RCNT 1] rcount: %04x m: %04x\n", count, rcnts[index].mode);

    return count;
}

u32 psxRcntRcount2()
{
    u32 index = 2;
    u32 count;

    count = _psxRcntRcount( index );

    verboseLog( 2, "[RCNT 2] rcount: %04x m: %04x\n", count, rcnts[index].mode);

    return count;
}

u32 psxRcntRmode( u32 index )
{
    u16 mode;

    mode = rcnts[index].mode;
    rcnts[index].mode &= 0xe7ff;

    verboseLog( 2, "[RCNT %i] rmode: %x\n", index, mode );

    return mode;
}

u32 psxRcntRtarget( u32 index )
{
    verboseLog( 2, "[RCNT %i] rtarget: %x\n", index, rcnts[index].target );

    return rcnts[index].target;
}

/******************************************************************************/

void psxRcntInit()
{
    s32 i;

    // rcnt 0.
    rcnts[0].rate   = 1;
    rcnts[0].irq    = 0x10;

    // rcnt 1.
    rcnts[1].rate   = 1;
    rcnts[1].irq    = 0x20;

    // rcnt 2.
    rcnts[2].rate   = 1;
    rcnts[2].irq    = 0x40;

    // rcnt base.
    rcnts[3].rate   = 1;

    for( i = 0; i < CounterQuantity; ++i )
    {
        _psxRcntWcount( i, 0 );
    }

    hSyncCount = 0;
    hsync_steps = 1;

    scheduleRcntBase();
    psxRcntSet();
}

/******************************************************************************/

s32 psxRcntFreeze( void *f, s32 Mode )
{
    u32 spuSyncCount = 0;
    u32 count;
    s32 i;

    gzfreeze( &rcnts, sizeof(Rcnt) * CounterQuantity );
    gzfreeze( &hSyncCount, sizeof(hSyncCount) );
    gzfreeze( &spuSyncCount, sizeof(spuSyncCount) );
    gzfreeze( &psxRegs.psxNextCounter, sizeof(psxRegs.psxNextCounter) );
    gzfreeze( &psxRegs.psxNextsCounter, sizeof(psxRegs.psxNextsCounter) );

    if (Mode == 0)
    {
        rcnts[3].rate = 1;
        for( i = 0; i < CounterQuantity - 1; ++i )
        {
            _psxRcntWmode( i, rcnts[i].mode );
            count = (psxRegs.cycle - rcnts[i].cycleStart) / rcnts[i].rate;
            if (count > 0x1000)
                _psxRcntWcount( i, count & 0xffff );
        }
        scheduleRcntBase();
        psxRcntSet();
    }

    return 0;
}

#ifdef PCSX_SYSTEM12
s32 psxRcntArcadeFreeze( void *f, s32 Mode )
{
    /* psxRcntFreeze's load rewrites each count from the elapsed cycles and
     * drops the part of a tick: the exact counters again. */
    gzfreeze( &rcnts, sizeof(Rcnt) * CounterQuantity );
    gzfreeze( &psxRegs.psxNextCounter, sizeof(psxRegs.psxNextCounter) );
    gzfreeze( &psxRegs.psxNextsCounter, sizeof(psxRegs.psxNextsCounter) );
    /* The interpreter's cycle remainder and delayed load, which the console
     * state leaves out: without them timing drifts after a load. */
    gzfreeze( &psxRegs.subCycle, sizeof(psxRegs.subCycle) );
    gzfreeze( &psxRegs.subCycleStep, sizeof(psxRegs.subCycleStep) );
    gzfreeze( &psxRegs.dloadSel, sizeof(psxRegs.dloadSel) );
    gzfreeze( psxRegs.dloadReg, sizeof(psxRegs.dloadReg) );
    gzfreeze( psxRegs.dloadVal, sizeof(psxRegs.dloadVal) );
    /* The pending events as saved: loading the console state reschedules
     * the counters before this clock is back, from the wrong screen time. */
    gzfreeze( &psxRegs.interrupt, sizeof(psxRegs.interrupt) );
    gzfreeze( psxRegs.intCycle, sizeof(psxRegs.intCycle) );
    gzfreeze( psxRegs.event_cycles, sizeof(psxRegs.event_cycles) );
    gzfreeze( &psxRegs.next_interupt, sizeof(psxRegs.next_interupt) );
    gzfreeze( &source_screen, sizeof(source_screen) );
    gzfreeze( &source_main_cycles, sizeof(source_main_cycles) );
    gzfreeze( &source_epoch_cycles, sizeof(source_epoch_cycles) );
    gzfreeze( &source_last_cycle, sizeof(source_last_cycle) );
    gzfreeze( &hsync_steps, sizeof(hsync_steps) );

    if (Mode == 0)
        source_screen_updating = 0;
    return 0;
}
#endif

/******************************************************************************/
// vim:ts=4:shiftwidth=4:expandtab
