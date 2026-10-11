/* Experimental COH-700 board adapter. See docs/system12.md. */
#ifndef PCSX_SYSTEM12_H
#define PCSX_SYSTEM12_H
#include <stdio.h>
#include <stddef.h>
#include "psxcommon.h"
#include "arcade/psx_screen_clock.h"

typedef void (*system12_audio_cb)(void *opaque, const int16_t *stereo,
		unsigned frames);
/* Optional diagnostics hook for accesses serviced by system12_read/write only;
 * internal DMA copies are not traced. The callback runs synchronously in the
 * board bus handler, must not re-enter the core, and is cleared on unload. */
typedef void (*system12_bus_trace_cb)(void *opaque, u32 address, unsigned width,
		int is_write, u32 value, u32 pc, u32 cycle);
/* Diagnostic pin observations, without changing the emulation schedule.
 * PORTB_INPUT is the external input before H8 GPIO masking. VBLANK is emitted
 * after line delivery. Main counters are 32-bit; H8 clocks are absolute across
 * soft reset. This synchronous observer must not re-enter/reset the core. */
enum { SYSTEM12_EDGE_PORTB_INPUT = 1, SYSTEM12_EDGE_VBLANK = 2,
       SYSTEM12_EDGE_H8_STORE = 3, SYSTEM12_EDGE_H8_LOAD = 4,
       SYSTEM12_EDGE_MAIN_SHARED_LOAD = 5, SYSTEM12_EDGE_MAIN_SHARED_STORE = 6 };
enum { SYSTEM12_TRACE_SHARED_STORES = 1, SYSTEM12_TRACE_SHARED_LOADS = 2 };
typedef struct {
	unsigned kind, value;
	u32 main_cycle, synced_main_cycle;
	u64 h8_cycle, h8_budget;
	u32 address;
	unsigned width;
} system12_edge_event;
typedef void (*system12_edge_trace_cb)(void *opaque,
		const system12_edge_event *event);

int system12_load(const char *directory);
/* A board profile as board.cfg describes it (sizes in bytes). */
typedef struct {
	const char *mame_set, *machine, *state, *unsupported;
	u32 ram_size;
	size_t main_size, bank_size, sub_size, wave_size;
	int boot_hack, alt_bank, tektagt_dma;
} system12_desc;
/* Same as system12_load, from regions already in memory (for example built
 * from a MAME romset). Takes ownership of the four malloc'd buffers, which
 * must hold exactly the sizes in desc; they are freed on failure too. */
int system12_load_regions(const system12_desc *desc, u8 *main, u8 *banked,
		u8 *sub, u8 *wave);
void system12_unload(void);
int system12_active(void);
/* Savestates (misc.c): the board's state after the PSX core's, through
 * SaveFuncs. mode 1 saves, 0 loads; 0 on success. A state is for the build
 * and the set that made it. system12_freeze_size: the bytes it writes. */
int system12_freeze(void *f, int mode);
size_t system12_freeze_size(void);
#ifdef PCSX_SYSTEM12_PROFILE
void system12_profile_counters(uint64_t *cycles, uint64_t *samples);
#endif
void system12_mem_reset(void);
/* Quiescent pre-reset catch-up, before psxRegs.cycle is cleared. */
void system12_pre_reset(void);
void system12_reset(void);
void system12_sync(u32 psx_cycle);
/* MAME-ordered timeslice for the source instruction step: begin returns how
 * many 40 ns main instructions run before end catches the H8 up. */
u32 system12_slice_begin(void);
void system12_slice_end(void);
void system12_vblank(int level);
/* Source screen edge at its scheduled absolute time, even if MIPS is late. */
void system12_vblank_at(int level, const psx_screen_time *time);
int system12_set_input(const char *control, int pressed);
/* Exact 2080-byte AT28C16 data+ID image import/export; board must be active. */
int system12_eeprom_export(unsigned char *image, size_t size);
int system12_eeprom_import(const unsigned char *image, size_t size);
int system12_open_audio(const char *path);
int system12_close_audio(void);
/* Optional 88.2 kHz stereo output; the WAV capture path can run at once.
 * Called synchronously from device advancement; must not re-enter/reset core. */
void system12_set_audio_output(system12_audio_cb callback, void *opaque);
/* Optional board thread (SYSTEM12_BOARD_THREAD builds): runs the H8 and C352
 * on another core with identical results. Audio from that thread is
 * delivered by system12_audio_pump(), which the frontend calls on its own
 * thread (e.g. at the end of retro_run). Start after load; stop is implied by
 * system12_unload(). */
int system12_board_thread_start(void);
void system12_board_thread_stop(void);
int system12_board_thread_active(void);
void system12_audio_pump(void);
void system12_set_bus_trace(system12_bus_trace_cb callback, void *opaque);
void system12_set_edge_trace(system12_edge_trace_cb callback, void *opaque);
/* Opt-in shared access flags from both CPUs; disabled by default, cleared on unload. */
void system12_set_edge_trace_shared(int flags);
unsigned char system12_jamma_read(unsigned offset);
void system12_audio_test_fixture(void);
int system12_read(u32 address, unsigned width, u32 *value);
int system12_write(u32 address, unsigned width, u32 value);
int system12_selftest(void);
void system12_dma5(u32 madr, u32 bcr, u32 chcr);
void system12_report(FILE *out);
#endif
