// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:Olivier Galibert,windyfairy
// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a; do not edit.
class mp3_audio
{
public:
	mp3_audio(const void *base);
	~mp3_audio();


	bool decode_buffer(int &pos, int limit, short *output, int &output_samples, int &sample_rate, int &channels);

	void clear();

private:
	struct mp3dec_local_t;

	const uint8_t *base;

	std::unique_ptr<mp3dec_local_t> dec;
	bool m_found_stream;
};

#define MINIMP3_IMPLEMENTATION
#define MAX_FRAME_SYNC_MATCHES 2
#include "minimp3.h"

// To avoid modifying minimp3.h, forward declare mp3dec_local_t in mp3_audio.h and then make it an mp3dec_t using inheritance
struct mp3_audio::mp3dec_local_t : public mp3dec_t
{
};

mp3_audio::mp3_audio(const void *_base)
	: base((const uint8_t *)_base)
{
	dec = std::make_unique<mp3dec_local_t>();
	clear();
}

mp3_audio::~mp3_audio()
{
}

void mp3_audio::clear()
{
	mp3dec_init(dec.get());
	m_found_stream = false;
}

bool mp3_audio::decode_buffer(int &pos, int limit, short *output, int &output_samples, int &sample_rate, int &channels)
{
	mp3dec_frame_info_t info = {};

	if (!m_found_stream)
	{
		// Guarantee a specified number of frames are buffered before starting decoding to ensure it's not full of garbage that looks like a valid frame
		int free_format_bytes = 0;
		int frame_bytes = 0;
		int frame_offset = mp3d_find_frame(base, limit, &free_format_bytes, &frame_bytes);

		if (frame_bytes && frame_offset + frame_bytes <= limit)
		{
			int i = 0, nmatch = 0;

			for (i = frame_offset, nmatch = 0; nmatch < MAX_FRAME_SYNC_MATCHES; nmatch++)
			{
				i += hdr_frame_bytes(base + i, frame_bytes) + hdr_padding(base + i);
				if (i + HDR_SIZE > limit || !hdr_compare(base + frame_offset, base + i))
					break;
			}

			m_found_stream = nmatch >= MAX_FRAME_SYNC_MATCHES;
		}

		if (!m_found_stream)
		{
			output_samples = 0;
			sample_rate = 0;
			channels = 0;
			pos = 0;
			return false;
		}
	}

	output_samples = mp3dec_decode_frame(dec.get(), base, limit, output, &info);
	sample_rate = info.hz;
	channels = info.channels;
	pos = info.frame_bytes;

	return pos > 0 && output_samples > 0;
}

struct MasDevice {
	int i2c_scl_r();
	int i2c_sda_r();
	void i2c_scl_w(bool line);
	void i2c_sda_w(bool line);

	void sid_w(uint8_t byte);

	void reset_playback();

	void device_reset();
	void sound_stream_update(MasStream &stream);

	void i2c_nak();
	bool i2c_device_got_address(uint8_t address);
	void i2c_device_got_byte(uint8_t byte);
	void i2c_device_got_stop();

	void mem_write(int bank, uint32_t adr, uint32_t val);
	void run_program(uint32_t adr);
	void reg_write(uint32_t adr, uint32_t val);

	void fill_buffer();
	void append_buffer(MasStream &stream, int &pos, int scount);

	int gain_to_db(double val);
	float gain_to_percentage(int val);

	MasPin cb_mpeg_frame_sync;
	MasPin cb_demand;

	enum {
		CMD_DEV_WRITE = 0x3a,
		CMD_DEV_READ = 0x3b,

		CMD_DATA_WRITE = 0x68,
		CMD_DATA_READ = 0x69,
		CMD_CONTROL_WRITE = 0x6a
	};

	enum i2c_bus_state_t : uint8_t { IDLE = 0, STARTED, NAK, ACK, ACK2 };
	enum i2c_bus_address_t : uint8_t { UNKNOWN = 0, VALIDATED, WRONG };
	enum i2c_subdest_t : uint8_t { UNDEFINED = 0, CONTROL, DATA_READ, DATA_WRITE, BAD };
	enum i2c_command_t : uint8_t { CMD_BAD = 0, CMD_RUN, CMD_READ_CTRL, CMD_WRITE_REG, CMD_WRITE_MEM, CMD_READ_REG, CMD_READ_MEM };

	i2c_bus_state_t i2c_bus_state;
	i2c_bus_address_t i2c_bus_address;
	i2c_subdest_t i2c_subdest;
	i2c_command_t i2c_command;

	MasStream *stream;

	std::array<uint8_t, 0xe00> mp3data;
	std::array<short, 1152*2> samples;

	bool i2c_scli, i2c_sclo, i2c_sdai, i2c_sdao;
	int i2c_bus_curbit;
	uint8_t i2c_bus_curval;
	int i2c_bytecount;
	uint32_t i2c_io_bank, i2c_io_adr, i2c_io_count, i2c_io_val;
	uint32_t i2c_sdao_data;

	uint32_t mp3data_count;
	uint32_t decoded_frame_count;
	int32_t sample_count, samples_idx;
	int32_t frame_channels;

	bool is_muted;
	float gain_ll, gain_rr;

	std::unique_ptr<mp3_audio> mp3dec;
};
void MasDevice::device_reset()
{
	i2c_scli = i2c_sdai = true;
	i2c_sclo = i2c_sdao = true;
	i2c_bus_state = IDLE;
	i2c_bus_address = UNKNOWN;
	i2c_bus_curbit = -1;
	i2c_bus_curval = 0;
	i2c_bytecount = 0;
	i2c_io_bank = 0;
	i2c_io_adr = 0;
	i2c_io_count = 0;
	i2c_io_val = 0;
	i2c_sdao_data = 0;

	is_muted = false;
	gain_ll = gain_rr = 1.0;

	frame_channels = 2;

	stream->set_sample_rate(44100);

	reset_playback();
}

void MasDevice::i2c_scl_w(bool line)
{
	if(line == i2c_scli)
		return;
	i2c_scli = line;
	// FIXME: sda output should only change state when clock is low.
	// On wrong device-id ensure sda is left high (Think it's OK but not certain)
	if(i2c_scli) {
		if(i2c_bus_state == STARTED) {
			if(i2c_sdai)
				i2c_bus_curval |= 1 << i2c_bus_curbit;

			if(i2c_subdest == DATA_READ)
				i2c_sdao = BIT(i2c_sdao_data, i2c_bus_curbit + (i2c_bytecount * 8));
			else {
				i2c_sdao_data = 0;
				i2c_sdao = false;
			}

			i2c_bus_curbit --;
			if(i2c_bus_curbit == -1) {
				if(i2c_bus_address == UNKNOWN) {
					if(i2c_device_got_address(i2c_bus_curval)) {
						i2c_bus_state = ACK;
						i2c_bus_address = VALIDATED;
						i2c_bus_curval = 0;
					} else {
						i2c_bus_state = NAK;
						i2c_bus_address = WRONG;
					}
				} else if(i2c_bus_address == VALIDATED) {
					i2c_bus_state = ACK;
					i2c_device_got_byte(i2c_bus_curval);
				}
			}
		} else if(i2c_bus_state == ACK) {
			i2c_bus_state = ACK2;
			i2c_sdao = false;
		}
	} else {
		if(i2c_bus_state == ACK2) {
			i2c_bus_state = STARTED;
			i2c_bus_curbit = 7;
			i2c_bus_curval = 0;
			i2c_sdao = false;
		}
	}
}

void MasDevice::i2c_nak()
{
	assert(i2c_bus_state == ACK);
	i2c_bus_state = NAK;
}

void MasDevice::i2c_sda_w(bool line)
{
	if(line == i2c_sdai)
		return;
	i2c_sdai = line;

	if(i2c_scli) {
		if(!i2c_sdai) {
			i2c_bus_state = STARTED;
			i2c_bus_address = UNKNOWN;
			i2c_bus_curbit = 7;
			i2c_bus_curval = 0;
		} else {
			i2c_device_got_stop();
			i2c_bus_state = IDLE;
			i2c_bus_address = UNKNOWN;
			i2c_bus_curbit = 7;
			i2c_bus_curval = 0;
		}
	}
}

int MasDevice::i2c_scl_r()
{
	return i2c_scli && i2c_sclo;
}

int MasDevice::i2c_sda_r()
{
	return i2c_sdai && i2c_sdao;
}

bool MasDevice::i2c_device_got_address(uint8_t address)
{
	if(address == CMD_DEV_READ)
		i2c_subdest = DATA_READ;
	else
		i2c_subdest = UNDEFINED;

	return (address & 0xfe) == CMD_DEV_WRITE;
}

void MasDevice::i2c_device_got_byte(uint8_t byte)
{
	switch(i2c_subdest) {
	case UNDEFINED:
		if(byte == CMD_DATA_WRITE)
			i2c_subdest = DATA_WRITE;
		else if(byte == CMD_DATA_READ) {
			i2c_subdest = DATA_READ;

			// Default read, returns the current frame count
			i2c_sdao_data = ((decoded_frame_count >> 8) & 0xff)
							| ((decoded_frame_count & 0xff) << 8)
							| (((decoded_frame_count >> 24) & 0xff) << 16)
							| (((decoded_frame_count >> 16) & 0xff) << 24);
		}
		else if(byte == CMD_CONTROL_WRITE)
			i2c_subdest = CONTROL;
		else
			i2c_subdest = BAD;

		i2c_bytecount = 0;
		i2c_io_val = 0;

		break;

	case BAD:

		break;

	case DATA_READ:
		switch(i2c_bytecount) {
		case 0: i2c_io_val = byte; break;
		case 1: i2c_io_val |= byte << 8; break;
		case 2: i2c_nak(); return;
		}



		i2c_bytecount++;

		break;

	case DATA_WRITE:
		if(!i2c_bytecount) {
			switch(byte >> 4) {
			case 0: case 1:
				i2c_command = CMD_RUN;
				i2c_io_adr = byte << 8;
				break;
			case 3:
				i2c_command = CMD_READ_CTRL;

				break;
			case 9:
				i2c_io_adr = (byte & 15) << 4;
				i2c_command = CMD_WRITE_REG;
				break;
			case 0xa: case 0xb:
				i2c_io_bank = (byte >> 4) & 1;
				i2c_command = CMD_WRITE_MEM;
				break;
			case 0xd:
				i2c_command = CMD_READ_REG;

				break;
			case 0xe: case 0xf:
				i2c_io_bank = (byte >> 4) & 1;
				i2c_command = CMD_READ_MEM;

				break;
			default:
				i2c_command = CMD_BAD;

				break;
			}
		} else {
			switch(i2c_command) {
			default:

				break;

			case CMD_WRITE_REG:
				switch(i2c_bytecount) {
				case 1: i2c_io_adr |= byte >> 4; i2c_io_val = byte & 15; break;
				case 2: i2c_io_val |= byte << 12; break;
				case 3: i2c_io_val |= byte << 4; reg_write(i2c_io_adr, i2c_io_val); break;
				case 4: i2c_nak(); return;
				}
				break;

			case CMD_RUN:
				if(i2c_bytecount > 1) {
					i2c_nak();
					return;
				}
				i2c_io_adr |= byte;
				run_program(i2c_io_adr);
				break;

			case CMD_WRITE_MEM:
				switch(i2c_bytecount) {
				case 2: i2c_io_count = byte << 8; break;
				case 3: i2c_io_count |= byte; break;
				case 4: i2c_io_adr = byte << 8; break;
				case 5: i2c_io_adr |= byte; break;
				}
				if(i2c_bytecount >= 6) {
					uint32_t i2c_wordid = (i2c_bytecount - 6) >> 2;
					uint32_t i2c_offset = (i2c_bytecount - 6) & 3;
					if(i2c_wordid >= i2c_io_count) {
						i2c_nak();
						return;
					}
					switch(i2c_offset) {
					case 0: i2c_io_val = byte << 8; break;
					case 1: i2c_io_val |= byte; break;
					case 3: i2c_io_val |= (byte & 15) << 16; mem_write(i2c_io_bank, i2c_io_adr + i2c_wordid, i2c_io_val); break;
					}
				}
				break;
			}
		}

		i2c_bytecount++;
		break;

	case CONTROL:

		break;
	}
}

void MasDevice::i2c_device_got_stop()
{

}

int MasDevice::gain_to_db(double val) {
	return round(20 * log10((0x100000 - val) / 0x80000));
}

float MasDevice::gain_to_percentage(int val)
{
	if(val == 0)
		return 0; // Special case for muting it seems

	double db = gain_to_db(val);

	return powf(10.0, (db + 6) / 20.0);
}

void MasDevice::mem_write(int bank, uint32_t adr, uint32_t val)
{
	switch(adr | (bank ? 0x10000 : 0)) {
	case 0x0032d:  break;
	case 0x0032e:  break;
	case 0x0032f:  break;
	case 0x107f8:
		gain_ll = gain_to_percentage(val);

		break;
	case 0x107f9:

		break;
	case 0x107fa:

		break;
	case 0x107fb:
		gain_rr = gain_to_percentage(val);

		break;
	default:  break;
	}
}

void MasDevice::reg_write(uint32_t adr, uint32_t val)
{
	switch(adr) {
	case 0x8e:  break;
	case 0xaa:

		is_muted = val == 1;
		break;
	case 0xe6:  break;
	case 0xe7:  break;
	case 0x6b:  break;
	case 0x6f:  break;
	default:  break;
	}
}

void MasDevice::run_program(uint32_t adr)
{
	switch(adr) {
	case 0xfcb:  break;
	default:  break;
	}
}

void MasDevice::sid_w(uint8_t byte)
{
	if (mp3data_count >= mp3data.size()) {
		std::copy(mp3data.begin() + 1, mp3data.end(), mp3data.begin());
		mp3data_count--;
	}

	mp3data[mp3data_count++] = byte;

	cb_demand(mp3data_count < mp3data.size());
}

void MasDevice::fill_buffer()
{
	cb_mpeg_frame_sync(0);

	int pos = 0, frame_sample_rate = 0;
	bool decoded_frame = mp3dec->decode_buffer(pos, mp3data_count, &samples[0], sample_count, frame_sample_rate, frame_channels);
	samples_idx = 0;

	if (!decoded_frame || sample_count == 0) {
		// Frame decode failed
		if (mp3data_count >= mp3data.size()) {
			std::copy(mp3data.begin() + 1, mp3data.end(), mp3data.begin());
			mp3data_count--;
		}

		cb_demand(1); // always request more data when nothing could be decoded to force potentially stale data out of the buffer
		return;
	}

	std::copy(mp3data.begin() + pos, mp3data.end(), mp3data.begin());
	mp3data_count -= pos;

	stream->set_sample_rate(frame_sample_rate);

	decoded_frame_count++;
	cb_mpeg_frame_sync(1);

	cb_demand(mp3data_count < mp3data.size());
}

void MasDevice::append_buffer(MasStream &stream, int &pos, int scount)
{
	const int bytes_per_sample = std::min(frame_channels, 2); // More than 2 channels is unsupported here
	const int s1 = std::min(scount - pos, sample_count);
	const MasStream::sample_t sample_scale = 1.0 / 32768.0;
	const MasStream::sample_t mute_scale = is_muted ? 0.0 : 1.0;

	for(int i = 0; i < s1; i++) {
		const MasStream::sample_t lsamp_mixed = MasStream::sample_t(samples[samples_idx * bytes_per_sample]) * sample_scale * mute_scale * gain_ll;
		const MasStream::sample_t rsamp_mixed = MasStream::sample_t(samples[samples_idx * bytes_per_sample + (bytes_per_sample >> 1)]) * sample_scale * mute_scale * gain_rr;

		stream.put(0, pos, lsamp_mixed);
		stream.put(1, pos, rsamp_mixed);

		samples_idx++;
		pos++;

		if(samples_idx >= sample_count) {
			sample_count = 0;
			return;
		}
	}
}

void MasDevice::reset_playback()
{
	std::fill(mp3data.begin(), mp3data.end(), 0);
	std::fill(samples.begin(), samples.end(), 0);

	mp3dec->clear();
	mp3data_count = 0;
	sample_count = 0;
	decoded_frame_count = 0;
	samples_idx = 0;
}

void MasDevice::sound_stream_update(MasStream &stream)
{
	int csamples = stream.samples();
	int pos = 0;

	while(pos < csamples) {
		if(sample_count == 0)
			fill_buffer();

		if(sample_count <= 0)
			return;

		append_buffer(stream, pos, csamples);
	}
}
