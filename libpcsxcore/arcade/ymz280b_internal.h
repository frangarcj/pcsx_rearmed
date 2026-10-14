// SPDX-License-Identifier: BSD-3-Clause
// Copyright Aaron Giles (upstream does not specify years).
// Private MAME adaptation, commit deb2f54fe7300e7a192690318beb860159bfd52a.
#include "ymz280b.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
using u16=uint16_t; using u8=uint8_t; using s8=int8_t; using s16=int16_t; using s32=int32_t; using u32=uint32_t; using offs_t=uint32_t;
struct YMZ280B {
    YMZ280BConfig config{};
    uint64_t cycles=0,frames=0,next_sample=192,voice_ends=0,irq_edges=0;
    uint32_t diagnostics=0;
    bool busy=false,resetting=false;
    float output[2]{};
    struct Timer {
        bool pending=false;
        void adjust(int, int) { pending=true; }
    } timers[8];
    struct IrqRoute {
        YMZ280B *owner;
        bool isunset() const { return !owner->config.irq; }
        void operator()(int state) {
            owner->irq_edges++;
            if (owner->config.irq) owner->config.irq(owner->config.opaque,state,owner->cycles);
        }
    } m_irq_handler{this};
    uint8_t read_byte(uint32_t address) const { return config.rom[address & (config.rom_size-1)]; }
    void write_byte(uint32_t, uint8_t) {} // source ROM mapping ignores RAM writes
    template<typename... T> void logerror(const char *format, T...) {
        if(!resetting) diagnostics |= std::strstr(format,"DSP register") ? YMZ280B_DIAG_DSP : YMZ280B_DIAG_UNKNOWN_REGISTER;
    }
    void sound_stream_update();
    void device_reset();
    u8 read(offs_t offset);
    void write(offs_t offset,u8 data);
	/* struct describing a single playing ADPCM voice */
	struct YMZ280BVoice
	{
		u8 playing;          /* 1 if we are actively playing */
		bool ended;          /* indicate voice has ended in case samples_left is 0 */

		u8 keyon;            /* 1 if the key is on */
		u8 looping;          /* 1 if looping is enabled */
		u8 mode;             /* current playback mode */
		u16 fnum;            /* frequency */
		u8 level;            /* output level */
		u8 pan;              /* panning */

		u32 start;           /* start address, in nibbles */
		u32 stop;            /* stop address, in nibbles */
		u32 loop_start;      /* loop start address, in nibbles */
		u32 loop_end;        /* loop end address, in nibbles */
		u32 position;        /* current position, in nibbles */

		s32 signal;          /* current ADPCM signal */
		s32 step;            /* current ADPCM step */

		s32 loop_signal;     /* signal at loop start */
		s32 loop_step;       /* step at loop start */
		u32 loop_count;      /* number of loops so far */

		s32 output_left;     /* output volume (left) */
		s32 output_right;    /* output volume (right) */
		s32 output_step;     /* step value for frequency conversion */
		s32 output_pos;      /* current fractional position */
		s16 last_sample;     /* last sample output */
		s16 curr_sample;     /* current sample target */
		u8 irq_schedule;     /* 1 if the IRQ state is updated by timer */

		Timer *timer;
	};

	void update_irq_state();
	void update_step(YMZ280BVoice *voice);
	void update_volumes(YMZ280BVoice *voice);
	void update_irq_state_timer_common(int param);
	int generate_adpcm(YMZ280BVoice *voice, s16 *buffer, int samples);
	int generate_pcm8(YMZ280BVoice *voice, s16 *buffer, int samples);
	int generate_pcm16(YMZ280BVoice *voice, s16 *buffer, int samples);
	void write_to_register(int data);
	int compute_status();

	// internal state
	YMZ280BVoice m_voice[8];          /* the 8 voices */
	u8 m_current_register;            /* currently accessible register */
	u8 m_status_register;             /* current status register */
	u8 m_irq_state;                   /* current IRQ state */
	u8 m_irq_mask;                    /* current IRQ mask */
	u8 m_irq_enable;                  /* current IRQ enable */
	u8 m_keyon_enable;                /* key on enable */
	u8 m_ext_mem_enable;              /* external memory enable */
	u8 m_ext_readlatch;               /* external memory prefetched data */
	u32 m_ext_mem_address_hi;
	u32 m_ext_mem_address_mid;
	u32 m_ext_mem_address;            /* where the CPU can read the ROM */

	/* Native timestamped IRQ route replaces devcb_write_line. */
	// devcb_write_line m_irq_handler;   /* IRQ callback */

	std::unique_ptr<s16[]> m_scratch;

};
