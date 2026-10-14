// SPDX-License-Identifier: BSD-3-Clause
// Copyright holders: Olivier Galibert, R. Belmont, hap.
// Private standalone adaptation of MAME ymf271.h, commit deb2f54fe7300e7a192690318beb860159bfd52a.
#include "ymf271.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <vector>
using u8 = uint8_t;
using offs_t = uint32_t;
struct YMF271 {
    YMF271Config config{};
    uint64_t cycles=0, frames=0, next_sample=384, timer_a_events=0, timer_b_events=0;
    uint32_t diagnostics=0, faults=0;
    bool busy=false;
    struct Timer {
        YMF271 *owner;
        uint64_t deadline=UINT64_MAX;
        void adjust(uint64_t clocks, int) { deadline=clocks==UINT64_MAX || clocks>UINT64_MAX-owner->cycles ? UINT64_MAX : owner->cycles+clocks; }
        void reset() { deadline=UINT64_MAX; }
    } timer_a{this}, timer_b{this};
    Timer *m_timA=&timer_a, *m_timB=&timer_b;
    std::vector<int32_t> m_mix_buffer;
    float output[4]{};
    uint32_t clock() const { return config.clock_hz; }
    uint64_t clocks_to_attotime(uint64_t clocks) const { return clocks; }
    uint8_t read_byte(uint32_t address) const { return config.rom[address & (config.rom_size-1)]; }
    void write_byte(uint32_t, uint8_t) { /* source ROM writes are unconnected */ }
    void m_irq_handler(int state) { if(config.irq) config.irq(config.opaque,state,cycles); }
    template<typename... T> void logerror(const char *, T...) { diagnostics |= YMF271_DIAG_INVALID_REGISTER; }
    void popmessage(const char *message) { diagnostics |= std::strstr(message,"PFM") ? YMF271_DIAG_PFM : YMF271_DIAG_ALTLOOP; }
    void sound_stream_update();
    void device_reset();
    void timer_a_expired();
    void timer_b_expired();
    u8 read(offs_t offset);
    void write(offs_t offset, u8 data);
	struct YMF271Slot
	{
		uint8_t ext_en;
		uint8_t ext_out;
		uint8_t lfoFreq;
		uint8_t lfowave;
		uint8_t pms, ams;
		uint8_t detune;
		uint8_t multiple;
		uint8_t tl;
		uint8_t keyscale;
		uint8_t ar;
		uint8_t decay1rate, decay2rate;
		uint8_t decay1lvl;
		uint8_t relrate;
		uint8_t block;
		uint8_t fns_hi;
		uint32_t fns;
		uint8_t feedback;
		uint8_t waveform;
		uint8_t accon;
		uint8_t algorithm;
		uint8_t ch0_level, ch1_level, ch2_level, ch3_level;

		uint32_t startaddr;
		uint32_t loopaddr;
		uint32_t endaddr;
		uint8_t altloop;
		uint8_t fs;
		uint8_t srcnote, srcb;

		uint32_t step;
		uint64_t stepptr;

		uint8_t active;
		uint8_t bits;

		// envelope generator
		int32_t volume;
		int32_t env_state;
		int32_t env_attack_step;      // volume increase step in attack state
		int32_t env_decay1_step;
		int32_t env_decay2_step;
		int32_t env_release_step;

		int64_t feedback_modulation0;
		int64_t feedback_modulation1;

		int lfo_phase, lfo_step;
		int lfo_amplitude;
		double lfo_phasemod;
	};

	struct YMF271Group
	{
		uint8_t sync, pfm;
	};

	void init_tables();
	void calculate_clock_correction();
	void calculate_step(YMF271Slot *slot);
	void update_envelope(YMF271Slot *slot);
	void init_envelope(YMF271Slot *slot);
	void init_lfo(YMF271Slot *slot);
	void update_lfo(YMF271Slot *slot);
	int64_t calculate_slot_volume(YMF271Slot *slot);
	void update_pcm(int slotnum, int32_t *mixp, int length);
	int64_t calculate_op(int slotnum, int64_t inp);
	void set_feedback(int slotnum, int64_t inp);
	void write_register(int slotnum, int reg, uint8_t data);
	void ymf271_write_fm(int bank, uint8_t address, uint8_t data);
	void ymf271_write_pcm(uint8_t address, uint8_t data);
	void ymf271_write_timer(uint8_t address, uint8_t data);

	inline int get_keyscaled_rate(int rate, int keycode, int keyscale);
	inline int get_internal_keycode(int block, int fns);
	inline int get_external_keycode(int block, int fns);
	inline bool check_envelope_end(YMF271Slot *slot);
	inline void calculate_status_end(int slotnum, bool state);

	// lookup tables
	std::unique_ptr<int16_t[]> m_lut_waves[8];
	std::unique_ptr<double[]> m_lut_plfo[4][8];
	std::unique_ptr<int[]> m_lut_alfo[4];
	double m_lut_ar[64];
	double m_lut_dc[64];
	double m_lut_lfo[256];
	int m_lut_attenuation[16];
	int m_lut_total_level[128];
	int m_lut_env_volume[256];

	// internal state
	YMF271Slot m_slots[48];
	YMF271Group m_groups[12];

	uint8_t m_regs_main[0x10];

	uint32_t m_timerA = 0;
	uint32_t m_timerB = 0;
	uint8_t m_irqstate = 0;
	uint8_t m_status = 0;
	uint16_t m_end_status = 0;
	uint8_t m_enable = 0;

	uint32_t m_ext_address = 0;
	uint8_t m_ext_rw = 0;
	uint8_t m_ext_readlatch = 0;

	uint32_t m_master_clock;

};
