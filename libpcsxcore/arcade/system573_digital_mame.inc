// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf,windyfairy
// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a; do not edit.
struct DigitalFpga {
	void mpeg_frame_sync(int state);
	void mas3507d_demand(int state);

	void set_crypto_key1(uint16_t v);
	void set_crypto_key2(uint16_t v);
	void set_crypto_key3(uint8_t v);

	uint32_t get_mp3_start_addr() { return mp3_start_addr; }
	void set_mp3_start_addr(uint32_t v);

	uint32_t get_mp3_end_addr() { return mp3_end_addr; }
	void set_mp3_end_addr(uint32_t v);

	uint16_t mas_i2c_r();
	void mas_i2c_w(uint16_t data);

	uint16_t get_fpga_ctrl();
	void set_fpga_ctrl(uint16_t data);

	uint16_t get_mpeg_ctrl();

	uint32_t get_counter();
	uint32_t get_counter_diff();
	uint16_t get_mp3_frame_count();

	void reset_counter();

void device_reset();
void update_stream();
void update_counter();
void update_mp3_decode_state();
uint16_t decrypt_common(uint16_t,uint16_t);
uint16_t decrypt_default(uint16_t);
uint16_t decrypt_ddrsbm(uint16_t);
DigitalMachine *clock;
DigitalMachine &machine() { return *clock; }
DigitalTimer *m_stream_timer;
unsigned *faults;
uint64_t words_decrypted=0, bytes_sent=0, stream_hash=0xcbf29ce484222325ull;
bool read_ram(uint32_t index,uint16_t &out) {
if(index>=0x1000000) { *faults|=2; return false; }
out=ram[index]; return true; }
	enum {
		PLAYBACK_STATE_DEMAND = 12,
		PLAYBACK_STATE_IDLE = 13,
		PLAYBACK_STATE_PLAYING = 14,
		PLAYBACK_STATE_ENABLED = 15,
	};

	enum {
		// Allows MP3 data to be decrypted?
		// If this is 0 then data won't be sent to the MAS3507D even if FPGA_STREAMING_ENABLE is 1.
		FPGA_MP3_ENABLE = 13,

		// Allows data to be streamed to MAS3507D.
		// This needs to be set before the register at 0x1f6400ae will return the streaming status.
		FPGA_STREAMING_ENABLE = 14,

		// Allows frame counter to be incremented based on the MPEG frame sync pin from the MAS3507D.
		// Setting this to 0 resets the frame counter register.
		FPGA_FRAME_COUNTER_ENABLE = 15,
	};

	uint16_t *ram;
	MasProxy *mas3507d;

	bool is_ddrsbm_fpga;

	uint16_t mpeg_status, fpga_status;

	uint16_t crypto_key1, crypto_key2;
	uint8_t crypto_key3;
	uint16_t crypto_key1_start, crypto_key2_start;
	uint8_t crypto_key3_start;

	uint32_t mp3_start_addr, mp3_end_addr;
	uint32_t mp3_cur_start_addr, mp3_cur_end_addr, mp3_cur_addr;
	uint16_t mp3_data;
	int mp3_remaining_bytes;

	bool is_mpeg_frame_synced;
	uint32_t mp3_frame_counter;

	DigitalTime counter_current, counter_base;
	double counter_value;
};
void DigitalFpga::device_reset()
{
	fpga_status = 0;
	mp3_start_addr = 0;
	mp3_end_addr = 0;
	mp3_cur_start_addr = 0;
	mp3_cur_end_addr = 0;
	mp3_cur_addr = 0;
	mp3_data = 0;
	mp3_remaining_bytes = 0;

	crypto_key1_start = crypto_key1 = 0;
	crypto_key2_start = crypto_key2 = 0;
	crypto_key3_start = crypto_key3 = 0;

	counter_current = counter_base = machine().time();

	mpeg_status = 0;
	mp3_frame_counter = 0;
	counter_value = 0;
	is_mpeg_frame_synced = false;

	mas3507d->reset_playback();
}

void DigitalFpga::reset_counter()
{
	is_mpeg_frame_synced = false;
	counter_current = counter_base = machine().time();
	counter_value = 0;
}

void DigitalFpga::update_counter()
{
	if (is_ddrsbm_fpga) {
		// The counter for Solo Bass Mix is used differently than other games.
		// DDR Solo Bass Mix will sync the internal playback timer to the first second of the MP3 using the MP3 frame counter.
		// After that the playback timer is incremented using the difference between the last counter value and the current counter value.
		// This counter register itself is always running even when no audio is playing.
		// TODO: What happens when mp3_counter_low_w is written to on Solo Bass Mix?
		counter_value = (machine().time() - counter_base).as_double();
	}
	else if (is_mpeg_frame_synced) {
		// Timer only seems to start when the first MPEG frame sync is encountered, so wait for that trigger
		counter_base = counter_current;
		counter_current = machine().time();
		counter_value += (counter_current - counter_base).as_double();
	}
}

uint32_t DigitalFpga::get_counter()
{
	update_counter();
	return wrapped_counter(counter_value * 44100);
}

uint32_t DigitalFpga::get_counter_diff()
{
	// Delta playback time since last counter update.
	// I couldn't find any active usages of this register but it exists in some code paths.
	// The functionality was tested using custom code running on real hardware.
	// When this is called, it will return the difference between the current counter value
	// and the last read counter value, and then reset the counter back to the previously read counter's value.
	auto prev = counter_value;
	update_counter();
	auto diff = counter_value - prev;
	counter_value = prev;
	return wrapped_counter(diff * 44100);
}

uint16_t DigitalFpga::get_mp3_frame_count()
{
	// All games can read this but only DDR Solo Bass Mix actively uses it.
	// Returns the same value as using a default read to get the frame counter from the MAS3507D over i2c.
	return mp3_frame_counter & 0xffff;
}

uint16_t DigitalFpga::mas_i2c_r()
{
	uint16_t scl = mas3507d->i2c_scl_r() << 13;
	uint16_t sda = mas3507d->i2c_sda_r() << 12;
	return scl | sda;
}

void DigitalFpga::mas_i2c_w(uint16_t data)
{
	mas3507d->i2c_scl_w(data & 0x2000);
	mas3507d->i2c_sda_w(data & 0x1000);
}

void DigitalFpga::set_crypto_key1(uint16_t v)
{
	crypto_key1_start = crypto_key1 = v;
	update_mp3_decode_state();
}
void DigitalFpga::set_crypto_key2(uint16_t v)
{
	crypto_key2_start = crypto_key2 = v;
	update_mp3_decode_state();
}
void DigitalFpga::set_crypto_key3(uint8_t v)
{
	crypto_key3_start = crypto_key3 = v;
	update_mp3_decode_state();
}

void DigitalFpga::set_mp3_start_addr(uint32_t v)
{
	mp3_start_addr = v;
	update_mp3_decode_state();
}

void DigitalFpga::set_mp3_end_addr(uint32_t v)
{
	mp3_end_addr = v;
	update_mp3_decode_state();
}

uint16_t DigitalFpga::get_mpeg_ctrl()
{
	return mpeg_status;
}

uint16_t DigitalFpga::get_fpga_ctrl()
{
	// 0x0000 Not Streaming
	// 0x1000 Streaming
	int is_streaming = BIT(fpga_status, FPGA_STREAMING_ENABLE)
		&& mp3_cur_addr >= mp3_cur_start_addr
		&& mp3_cur_addr < mp3_cur_end_addr;
	return is_streaming << 12;
}

void DigitalFpga::set_fpga_ctrl(uint16_t data)
{


	if (!BIT(data, FPGA_FRAME_COUNTER_ENABLE) && BIT(fpga_status, FPGA_FRAME_COUNTER_ENABLE)) {
		mp3_frame_counter = 0;
	}

	if ((BIT(data, FPGA_MP3_ENABLE) != BIT(fpga_status, FPGA_MP3_ENABLE))
		|| (BIT(data, FPGA_STREAMING_ENABLE) != BIT(fpga_status, FPGA_STREAMING_ENABLE))) {
		mas3507d->reset_playback();
	}

	fpga_status = data;
}


void DigitalFpga::update_mp3_decode_state()
{
	// HACK: The exact timing of when the internal state in the FPGA updates is still unknown
	// so update the state any time one of the core settings (decryption keys or data start/stop addr)
	// for a stream changes.
	mp3_cur_addr = mp3_start_addr;
	mp3_cur_start_addr = mp3_start_addr;
	mp3_cur_end_addr = mp3_end_addr;
	is_mpeg_frame_synced = false;
	mp3_remaining_bytes = 0;
	crypto_key1 = crypto_key1_start;
	crypto_key2 = crypto_key2_start;
	crypto_key3 = crypto_key3_start;
	mp3_frame_counter = 0;
	reset_counter();

	if (is_ddrsbm_fpga) {
		crypto_key3 = crypto_key3_start = 0;
	}

	mas3507d->reset_playback();
}

uint16_t DigitalFpga::decrypt_common(uint16_t data, uint16_t key)
{
	// In both variants of the decryption algorithm, a 16-bit key is derived
	// from the current key state and treated as a set of 8 pairs of bits. Bit 1
	// of each pair (i.e. bits 1, 3, 5, etc. of the key) controls whether the
	// respective bit pair of the data is swapped or not.
	data = bitswap<16>(
		data,
		15 - BIT(key, 15), 14 + BIT(key, 15),
		13 - BIT(key, 13), 12 + BIT(key, 13),
		11 - BIT(key, 11), 10 + BIT(key, 11),
		9 - BIT(key,  9),  8 + BIT(key,  9),
		7 - BIT(key,  7),  6 + BIT(key,  7),
		5 - BIT(key,  5),  4 + BIT(key,  5),
		3 - BIT(key,  3),  2 + BIT(key,  3),
		1 - BIT(key,  1),  0 + BIT(key,  1)
	);

	// Bit 0 of each pair (bits 0, 2, 4, etc. of the key) controls whether the
	// respective data bit is inverted after performing the swap.
	data ^= key & 0x5555;

	return data;
}

uint16_t DigitalFpga::decrypt_default(uint16_t data)
{
	const uint16_t derived_key = bitswap<16>(
		crypto_key1 ^ crypto_key2,
		15, 13, 14, 12,
		11, 10,  9,  7,
		8,  6,  5,  4,
		3,  1,  2,  0
	);

	data = decrypt_common(data, derived_key);

	data ^= bitswap<16>(
		(uint16_t)crypto_key3,
		7, 0, 6, 1,
		5, 2, 4, 3,
		3, 4, 2, 5,
		1, 6, 0, 7
	);

	if (BIT(crypto_key1, 14) ^ BIT(crypto_key1, 15))
		crypto_key2 = (crypto_key2 << 1) | (crypto_key2 >> 15);

	crypto_key1 = (crypto_key1 & 0x8000) | ((crypto_key1 << 1) & 0x7ffe) | ((crypto_key1 >> 14) & 1);
	crypto_key3++;

	return data;
}

uint16_t DigitalFpga::decrypt_ddrsbm(uint16_t data)
{
	data = decrypt_common(data, crypto_key1);
	crypto_key1 = (crypto_key1 << 1) | (crypto_key1 >> 15);
	return data;
}

void DigitalFpga::update_stream()
{
	if (!BIT(mpeg_status, PLAYBACK_STATE_DEMAND)) {
		return;
	}

	if (!BIT(fpga_status, FPGA_MP3_ENABLE)
		|| !BIT(fpga_status, FPGA_STREAMING_ENABLE)
		|| mp3_cur_addr < mp3_cur_start_addr
		|| mp3_cur_addr >= mp3_cur_end_addr) {
		return;
	}

	if (mp3_remaining_bytes <= 0) {
		uint16_t src; if (!read_ram(mp3_cur_addr >> 1,src)) return;
		words_decrypted++;
		mp3_data = is_ddrsbm_fpga ? decrypt_ddrsbm(src) : decrypt_default(src);
		mp3_data = ((mp3_data >> 8) & 0xff) | ((mp3_data & 0xff) << 8);
		mp3_cur_addr += 2;
		mp3_remaining_bytes = 2;
	}

	mas3507d->sid_w(mp3_data & 0xff);
	bytes_sent++;
	stream_hash = (stream_hash ^ (mp3_data & 0xff)) * 0x100000001b3ull;
	mp3_data >>= 8;
	mp3_remaining_bytes--;
}

void DigitalFpga::mpeg_frame_sync(int state)
{
	if (state) {
		mpeg_status &= ~(1 << PLAYBACK_STATE_IDLE);
		mpeg_status |= 1 << PLAYBACK_STATE_PLAYING;

		if (!is_mpeg_frame_synced) {
			reset_counter();
			is_mpeg_frame_synced = true;
		}

		if (BIT(fpga_status, FPGA_FRAME_COUNTER_ENABLE)) {
			mp3_frame_counter++;
		}
	}
	else {
		mpeg_status &= ~(1 << PLAYBACK_STATE_PLAYING);
		mpeg_status |= 1 << PLAYBACK_STATE_IDLE;
	}
}

void DigitalFpga::mas3507d_demand(int state)
{
	if (state && !BIT(mpeg_status, PLAYBACK_STATE_DEMAND)) {
		mpeg_status |= 1 << PLAYBACK_STATE_DEMAND;
	}
	else if (!state && BIT(mpeg_status, PLAYBACK_STATE_DEMAND)) {
		mpeg_status &= ~(1 << PLAYBACK_STATE_DEMAND);
		m_stream_timer->adjust(DigitalTime::never);
	}

	if (state && BIT(mpeg_status, PLAYBACK_STATE_DEMAND)) {
		m_stream_timer->adjust(DigitalTime::zero);
	}
}

struct DigitalDio {
	uint16_t a00_r();
	uint16_t a02_r();
	uint16_t a04_r();
	uint16_t a06_r();
	uint16_t a0a_r();
	void a10_w(uint16_t data);
	uint16_t a80_r();

	uint16_t mpeg_start_adr_high_r();
	void mpeg_start_adr_high_w(uint16_t data);
	uint16_t mpeg_start_adr_low_r();
	void mpeg_start_adr_low_w(uint16_t data);
	uint16_t mpeg_end_adr_high_r();
	void mpeg_end_adr_high_w(uint16_t data);
	uint16_t mpeg_end_adr_low_r();
	void mpeg_end_adr_low_w(uint16_t data);
	uint16_t mpeg_frame_counter_r();
	void mpeg_key_1_w(uint16_t data);
	uint16_t mpeg_ctrl_r();
	uint16_t mas_i2c_r();
	void mas_i2c_w(uint16_t data);
	uint16_t fpga_ctrl_r();
	void fpga_ctrl_w(uint16_t data);
	void ram_write_adr_high_w(uint16_t data);
	void ram_write_adr_low_w(uint16_t data);
	uint16_t ram_r();
	void ram_w(uint16_t data);
	void ram_read_adr_high_w(uint16_t data);
	void ram_read_adr_low_w(uint16_t data);
	uint16_t mp3_counter_high_r();
	uint16_t mp3_counter_low_r();
	void mp3_counter_low_w(uint16_t data);
	uint16_t mp3_counter_diff_r();
	void output_0_w(uint16_t data);
	void output_1_w(uint16_t data);
	void output_7_w(uint16_t data);
	void output_3_w(uint16_t data);
	void mpeg_key_2_w(uint16_t data);
	void mpeg_key_3_w(uint16_t data);
	uint16_t digital_id_r();
	void digital_id_w(uint16_t data);
	uint16_t fpga_status_r();
	void fpga_firmware_w(uint16_t data);
	void output_4_w(uint16_t data);
	void output_2_w(uint16_t data);
	void output_5_w(uint16_t data);
	uint16_t network_r();
	void network_w(uint16_t data);
	uint16_t network_output_buf_size_r();
	uint16_t network_input_buf_size_r();
	void network_id_w(uint16_t data);

void device_reset();
uint16_t *ram;
DigitalFpga *k573fpga;
DigitalSerial *digital_id;
DigitalOutput output_cb;
	uint32_t ram_adr, ram_read_adr;
	uint8_t output_data[8];

	void output(int offset, uint16_t data);

	bool is_ddrsbm_fpga;
	uint16_t crypto_key1;
	uint32_t fpga_counter;

	uint16_t network_id;
};
void DigitalDio::device_reset()
{
	ram_adr = 0;
	ram_read_adr = 0;
	crypto_key1 = 0;
	fpga_counter = 0;

	network_id = 0;

	std::fill(std::begin(output_data), std::end(output_data), 0);
}

uint16_t DigitalDio::a00_r()
{

	return 0x0000;
}

uint16_t DigitalDio::a02_r()
{

	return 0x0001;
}

uint16_t DigitalDio::a04_r()
{

	return 0x0000;
}

uint16_t DigitalDio::a06_r()
{

	return 0x0000;
}

uint16_t DigitalDio::a0a_r()
{

	return 0x0000;
}

void DigitalDio::a10_w(uint16_t data)
{

}

uint16_t DigitalDio::a80_r()
{

	return 0x1234;
}

uint16_t DigitalDio::mpeg_start_adr_high_r()
{
	return k573fpga->get_mp3_start_addr() >> 16;
}

void DigitalDio::mpeg_start_adr_high_w(uint16_t data)
{

	k573fpga->set_mp3_start_addr((k573fpga->get_mp3_start_addr() & 0x0000ffff) | (data << 16)); // high
}

uint16_t DigitalDio::mpeg_start_adr_low_r()
{
	return k573fpga->get_mp3_start_addr() & 0xffff;
}

void DigitalDio::mpeg_start_adr_low_w(uint16_t data)
{

	k573fpga->set_mp3_start_addr((k573fpga->get_mp3_start_addr() & 0xffff0000) | data); // low
}

uint16_t DigitalDio::mpeg_end_adr_high_r()
{
	return k573fpga->get_mp3_end_addr() >> 16;
}

void DigitalDio::mpeg_end_adr_high_w(uint16_t data)
{

	k573fpga->set_mp3_end_addr((k573fpga->get_mp3_end_addr() & 0x0000ffff) | (data << 16)); // high
}

uint16_t DigitalDio::mpeg_end_adr_low_r()
{
	return k573fpga->get_mp3_end_addr() & 0xffff;
}

void DigitalDio::mpeg_end_adr_low_w(uint16_t data)
{

	k573fpga->set_mp3_end_addr((k573fpga->get_mp3_end_addr() & 0xffff0000) | data); // low
}

uint16_t DigitalDio::mpeg_frame_counter_r()
{
	return k573fpga->get_mp3_frame_count();
}

void DigitalDio::mpeg_key_1_w(uint16_t data)
{

	crypto_key1 = data;
	k573fpga->set_crypto_key1(data);
}

uint16_t DigitalDio::mas_i2c_r()
{
	return k573fpga->mas_i2c_r();
}

void DigitalDio::mas_i2c_w(uint16_t data)
{
	k573fpga->mas_i2c_w(data);
}

uint16_t DigitalDio::mpeg_ctrl_r()
{
	return k573fpga->get_mpeg_ctrl();
}

uint16_t DigitalDio::fpga_ctrl_r()
{
	return k573fpga->get_fpga_ctrl();
}

void DigitalDio::fpga_ctrl_w(uint16_t data)
{
	k573fpga->set_fpga_ctrl(data);
}

void DigitalDio::ram_write_adr_high_w(uint16_t data)
{
	// read and write address are shared
	ram_adr = ((ram_adr & 0x0000ffff) | (data << 16)) & 0x1ffffff;
}

void DigitalDio::ram_write_adr_low_w(uint16_t data)
{
	// read and write address are shared
	ram_adr = ((ram_adr & 0xffff0000) | data) & 0x1ffffff;
}

uint16_t DigitalDio::ram_r()
{
	uint16_t res = ram[(ram_read_adr & 0x1ffffff) >> 1];
	ram_read_adr += 2;
	return res;
}

void DigitalDio::ram_w(uint16_t data)
{
	ram[(ram_adr & 0x1ffffff) >> 1] = data;
	ram_adr += 2;
}

void DigitalDio::ram_read_adr_high_w(uint16_t data)
{
	// read and write address are shared
	ram_read_adr = ((ram_read_adr & 0x0000ffff) | (data << 16)) & 0x1ffffff;
}

void DigitalDio::ram_read_adr_low_w(uint16_t data)
{
	// read and write address are shared
	ram_read_adr = ((ram_read_adr & 0xffff0000) | data) & 0x1ffffff;
}

uint16_t DigitalDio::mp3_counter_high_r()
{
	return (fpga_counter & 0xffff0000) >> 16;
}

uint16_t DigitalDio::mp3_counter_low_r()
{
	fpga_counter = k573fpga->get_counter();
	return fpga_counter & 0x0000ffff;
}

void DigitalDio::mp3_counter_low_w(uint16_t data)
{

	k573fpga->reset_counter();
}

uint16_t DigitalDio::mp3_counter_diff_r()
{
	return k573fpga->get_counter_diff() & 0x0000ffff;
}

void DigitalDio::output_1_w(uint16_t data)
{
	output(1, data);
}

void DigitalDio::output_0_w(uint16_t data)
{
	output(0, data);
}

void DigitalDio::output_3_w(uint16_t data)
{
	output(3, data);
}

void DigitalDio::output_7_w(uint16_t data)
{
	output(7, data);
}

void DigitalDio::mpeg_key_2_w(uint16_t data)
{

	k573fpga->set_crypto_key2(data);
}

void DigitalDio::mpeg_key_3_w(uint16_t data)
{

	k573fpga->set_crypto_key3(data);
}

uint16_t DigitalDio::digital_id_r()
{
	return digital_id->read() << 12;
}

void DigitalDio::digital_id_w(uint16_t data)
{
	digital_id->write( !( ( data >> 12 ) & 1 ) );
}

uint16_t DigitalDio::fpga_status_r()
{


	// fpga/digital board status checks
	// wants & c000 = 8000 (just after program upload?)
	// write 0000 to +f4.w
	// write 8000 to +f6.w

	/* fails if !8000 */
	/* fails if  4000 */
	/* fails if !2000 */
	/* fails if !1000 */
	return 0x8000 | 0x2000 | 0x1000;
}

void DigitalDio::fpga_firmware_w(uint16_t data)
{
	// Firmware bits in bit 15, always the same firmware
}

void DigitalDio::output_4_w(uint16_t data)
{
	output(4, data);
}

void DigitalDio::output_5_w(uint16_t data)
{
	output(5, data);
}

void DigitalDio::output_2_w(uint16_t data)
{
	output(2, data);
}

void DigitalDio::output(int offset, uint16_t data)
{
	data = (data >> 12) & 0x0f;
	static const int shift[] = { 0, 2, 3, 1 };
	for(int i = 0; i < 4; i++) {
		int oldbit = (output_data[offset] >> shift[i]) & 1;
		int newbit = (data                >> shift[i]) & 1;
		if(oldbit != newbit)
			output_cb(4*offset + i, newbit, 0xff);
	}
	output_data[offset] = data;
}

uint16_t DigitalDio::network_r()
{
	// Return a byte from the input buffer
	return 0;
}

void DigitalDio::network_w(uint16_t data)
{
	// Write a byte to the output buffer
}

uint16_t DigitalDio::network_output_buf_size_r()
{
	// Number of bytes in the output buffer waiting to be sent
	return 0;
}

uint16_t DigitalDio::network_input_buf_size_r()
{
	// Number of bytes in the input buffer waiting to be read
	return 0;
}

void DigitalDio::network_id_w(uint16_t data)
{
	// The network ID configured in the operator menu
	network_id = data;
}
static bool digital_read_reg(DigitalDio &d,unsigned offset,uint16_t &data) {
switch(offset) {
case 0x00: data=d.a00_r();return true;
case 0x02: data=d.a02_r();return true;
case 0x04: data=d.a04_r();return true;
case 0x06: data=d.a06_r();return true;
case 0x0a: data=d.a0a_r();return true;
case 0x80: data=d.a80_r();return true;
case 0xa0: data=d.mpeg_start_adr_high_r();return true;
case 0xa2: data=d.mpeg_start_adr_low_r();return true;
case 0xa4: data=d.mpeg_end_adr_high_r();return true;
case 0xa6: data=d.mpeg_end_adr_low_r();return true;
case 0xa8: data=d.mpeg_frame_counter_r();return true;
case 0xaa: data=d.mpeg_ctrl_r();return true;
case 0xac: data=d.mas_i2c_r();return true;
case 0xae: data=d.fpga_ctrl_r();return true;
case 0xb4: data=d.ram_r();return true;
case 0xc0: data=d.network_r();return true;
case 0xc2: data=d.network_output_buf_size_r();return true;
case 0xc4: data=d.network_input_buf_size_r();return true;
case 0xca: data=d.mp3_counter_high_r();return true;
case 0xcc: data=d.mp3_counter_low_r();return true;
case 0xce: data=d.mp3_counter_diff_r();return true;
case 0xee: data=d.digital_id_r();return true;
case 0xf6: data=d.fpga_status_r();return true;
default:return false;
}
}
static bool digital_write_reg(DigitalDio &d,unsigned offset,uint16_t &data) {
switch(offset) {
case 0x10: d.a10_w(data);return true;
case 0x90: d.network_id_w(data);return true;
case 0xa0: d.mpeg_start_adr_high_w(data);return true;
case 0xa2: d.mpeg_start_adr_low_w(data);return true;
case 0xa4: d.mpeg_end_adr_high_w(data);return true;
case 0xa6: d.mpeg_end_adr_low_w(data);return true;
case 0xa8: d.mpeg_key_1_w(data);return true;
case 0xac: d.mas_i2c_w(data);return true;
case 0xae: d.fpga_ctrl_w(data);return true;
case 0xb0: d.ram_write_adr_high_w(data);return true;
case 0xb2: d.ram_write_adr_low_w(data);return true;
case 0xb4: d.ram_w(data);return true;
case 0xb6: d.ram_read_adr_high_w(data);return true;
case 0xb8: d.ram_read_adr_low_w(data);return true;
case 0xc0: d.network_w(data);return true;
case 0xcc: d.mp3_counter_low_w(data);return true;
case 0xe0: d.output_1_w(data);return true;
case 0xe2: d.output_0_w(data);return true;
case 0xe4: d.output_3_w(data);return true;
case 0xe6: d.output_7_w(data);return true;
case 0xea: d.mpeg_key_2_w(data);return true;
case 0xec: d.mpeg_key_3_w(data);return true;
case 0xee: d.digital_id_w(data);return true;
case 0xf8: d.fpga_firmware_w(data);return true;
case 0xfa: d.output_4_w(data);return true;
case 0xfc: d.output_5_w(data);return true;
case 0xfe: d.output_2_w(data);return true;
default:return false;
}
}
