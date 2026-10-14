#include "../libpcsxcore/arcade/zn_board.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
	uint32_t last_read_address;
	uint32_t last_write_address;
	uint8_t external_byte;
	int mcu_selected;
	int analog_selected;
	int trackball_selected;
	int dataout[2];
	unsigned dataout_changes[2];
	int mcu_dsr, mcu_txd;
} fixture_t;

static uint8_t input_read(void *opaque, zn_board_input_t input)
{
	(void)opaque;
	return (uint8_t)(0xf0u | (unsigned)input);
}

static int external_read(void *opaque, uint32_t address, uint8_t *value)
{
	fixture_t *f = (fixture_t *)opaque;
	f->last_read_address = address;
	if (address != 0x12345678)
		return 0;
	*value = f->external_byte;
	return 1;
}

static int external_write(void *opaque, uint32_t address, uint8_t value)
{
	fixture_t *f = (fixture_t *)opaque;
	f->last_write_address = address;
	if (address != 0x12345678)
		return 0;
	f->external_byte = value;
	return 1;
}

static void mcu_select(void *opaque, int selected)
{
	((fixture_t *)opaque)->mcu_selected = selected;
}

static void analog_select(void *opaque, int selected)
{
	((fixture_t *)opaque)->analog_selected = selected;
}

static void trackball_select(void *opaque, int selected)
{
	((fixture_t *)opaque)->trackball_selected = selected;
}

static void dataout(void *opaque, unsigned chip, int state)
{
	fixture_t *f = (fixture_t *)opaque;
	assert(chip < 2);
	f->dataout[chip] = state;
	f->dataout_changes[chip]++;
}

static void mcu_dsr(void *opaque, int state)
{ ((fixture_t *)opaque)->mcu_dsr = state; }
static void mcu_txd(void *opaque, int state)
{ ((fixture_t *)opaque)->mcu_txd = state; }
static uint8_t mcu_analog(void *opaque)
{ (void)opaque; return 0x35; }

static void test_connected_mcu(void)
{
	fixture_t f = {0};
	ZNMCUConfig mcu_config = {0};
	zn_board_config_t config = {0};
	ZNMCU *mcu = NULL;
	zn_board_t board;
	unsigned i, byte = 0;
	mcu_config.opaque = &f;
	mcu_config.dsr = mcu_dsr; mcu_config.txd = mcu_txd;
	mcu_config.analog[0] = mcu_analog;
	assert(zn_mcu_create(&mcu_config, &mcu) == 0);
	config.model = ZN_BOARD_ZN1; config.mcu = mcu;
	assert(zn_board_init(&board, &config, NULL));
	/* Select MCU and analog mode through the actual security/mux register. */
	assert(zn_board_write8(&board, 0x1fa10300, 0x9c) == ZN_BOARD_HANDLED);
	assert(zn_board_advance_to_us(&board, 49) == 0 && f.mcu_dsr == 1);
	assert(zn_board_advance_to_us(&board, 50) == 0 && f.mcu_dsr == 0);
	assert(zn_board_advance_to_us(&board, 55) == 0 && f.mcu_dsr == 1);
	for (i = 0; i < 8; i++) {
		assert(zn_board_sio_clock(&board, 0) == 0);
		byte |= f.mcu_txd << i;
		assert(zn_board_sio_clock(&board, 1) == 0);
	}
	assert(byte == 0x8f); /* Eight analog bytes and pull-up DIP nibble. */
	assert(zn_board_advance_to_us(&board, 105) == 0 && f.mcu_dsr == 0);
	assert(zn_board_advance_to_us(&board, 110) == 0 && f.mcu_dsr == 1);
	byte = 0;
	for (i = 0; i < 8; i++) {
		assert(zn_board_sio_clock(&board, 0) == 0);
		byte |= f.mcu_txd << i;
		assert(zn_board_sio_clock(&board, 1) == 0);
	}
	assert(byte == 0x35);
	zn_board_reset(&board); /* Source mux reset deselects and cancels the timer. */
	assert(f.mcu_txd == 1 && f.mcu_dsr == 1);
	assert(zn_board_advance_to_us(&board, 1000) == 0 && f.mcu_dsr == 1);
	zn_board_destroy(&board);
	assert(zn_mcu_advance_to_us(mcu, 1001) == 0); /* Borrowed lifetime. */
	zn_mcu_destroy(mcu);
}

static void test_boardconfig_and_inputs(void)
{
	fixture_t f = {0};
	zn_board_callbacks_t cb = {0};
	zn_board_config_t cfg = {0};
	zn_board_t b;
	uint8_t v;
	cb.opaque = &f;
	cb.read_input = input_read;
	cfg.model = ZN_BOARD_ZN1;
	cfg.ram_bytes = 0x400000;
	cfg.vram_bytes = 0x100000;
	assert(zn_board_init(&b, &cfg, &cb));
	assert(zn_board_read8(&b, 0x1fa10200, &v) == ZN_BOARD_HANDLED && v == 0x61);
	assert(zn_board_read8(&b, 0x1fa00000, &v) == ZN_BOARD_HANDLED && v == 0xf0);
	assert(zn_board_read8(&b, 0x1fa00001, &v) == ZN_BOARD_HANDLED && v == 0);
	assert(zn_board_read8(&b, 0x1fa00100, &v) == ZN_BOARD_HANDLED && v == 0xf1);
	assert(zn_board_read8(&b, 0x1fa00200, &v) == ZN_BOARD_HANDLED && v == 0xf2);
	assert(zn_board_read8(&b, 0x1fa00300, &v) == ZN_BOARD_HANDLED && v == 0xf3);
	assert(zn_board_read8(&b, 0x1fa10000, &v) == ZN_BOARD_HANDLED && v == 0xf4);
	assert(zn_board_read8(&b, 0x1fa10100, &v) == ZN_BOARD_HANDLED && v == 0xf5);
	{
		uint32_t wide;
		assert(zn_board_read(&b, 0x1fa00000, 4, &wide) == ZN_BOARD_HANDLED);
		assert(wide == 0x000000f0);
	}

	cfg.ram_bytes = 0x800000;
	cfg.vram_bytes = 0x200000;
	zn_board_destroy(&b);
	assert(zn_board_init(&b, &cfg, &cb));
	assert(zn_board_read8(&b, 0x1fa10200, &v) == ZN_BOARD_HANDLED && v == 0x6a);
	{
		uint32_t wide;
		assert(zn_board_read(&b, 0x1fa10200, 4, &wide) == ZN_BOARD_HANDLED && wide == 0x6a);
	}
	cfg.ram_bytes = 0x1000000;
	zn_board_destroy(&b);
	assert(zn_board_init(&b, &cfg, &cb));
	assert(zn_board_read8(&b, 0x1fa10200, &v) == ZN_BOARD_HANDLED && v == 0x6b);
	zn_board_destroy(&b);
}

static void test_coin_eeprom_and_mapped_unknowns(void)
{
	fixture_t f = {0};
	zn_board_callbacks_t cb = {0};
	zn_board_config_t cfg = {0};
	zn_board_t b;
	uint8_t nvram[ZN_BOARD_EEPROM_SIZE];
	uint8_t v;
	memset(nvram, 0xff, sizeof(nvram));
	cb.opaque = &f;
	cb.read_external = external_read;
	cb.write_external = external_write;
	cfg.model = ZN_BOARD_ZN2;
	cfg.vram_bytes = 0x200000;
	cfg.eeprom = nvram;
	assert(zn_board_init(&b, &cfg, &cb));
	assert(zn_board_write8(&b, 0x1fa20000, 0x33) == ZN_BOARD_HANDLED);
	assert(zn_board_read8(&b, 0x1fa20000, &v) == ZN_BOARD_HANDLED && v == 0x33);
	assert(zn_board_write(&b, 0x1fa20000, 4, 0x55) == ZN_BOARD_HANDLED);
	{
		uint32_t wide;
		assert(zn_board_read(&b, 0x1fa20000, 4, &wide) == ZN_BOARD_HANDLED);
		assert(wide == 0x55);
	}
	assert(b.coin == 0x55);
	assert(zn_board_write8(&b, 0x1faf0123, 0xa5) == ZN_BOARD_HANDLED);
	assert(nvram[0x123] == 0xff); /* Supplied defaults are copied, never mutated. */
	assert(zn_board_read8(&b, 0x1faf0123, &v) == ZN_BOARD_HANDLED && v == 0x25);
	assert(zn_board_write8(&b, 0x1faf0124, 0x33) == ZN_BOARD_HANDLED);
	zn_board_reset(&b); /* Neither a reset nor another write bypasses busy. */
	assert(zn_board_advance_to_us(&b, 199) == 0);
	assert(zn_board_read8(&b, 0x1faf0124, &v) == ZN_BOARD_HANDLED && v == 0x25);
	assert(zn_board_advance_to_us(&b, 200) == 0);
	assert(zn_board_read8(&b, 0x1faf0123, &v) == ZN_BOARD_HANDLED && v == 0xa5);
	assert(zn_board_read8(&b, 0x1faf0124, &v) == ZN_BOARD_HANDLED && v == 0xff);
	{
		uint8_t image[AT28C16_IMAGE_BYTES];
		assert(zn_board_eeprom_export(&b, image, sizeof(image)) == 0);
		assert(image[0x123] == 0xa5 && image[0x124] == 0xff);
		image[AT28C16_DATA_BYTES + 3] = 0x41;
		assert(zn_board_eeprom_import(&b, image, sizeof(image)) == 0);
		memset(image, 0, sizeof(image));
		assert(zn_board_eeprom_export(&b, image, sizeof(image)) == 0);
		assert(image[AT28C16_DATA_BYTES + 3] == 0x41);
		assert(zn_board_advance_to_us(&b, 199) == -1);
	}
	assert(zn_board_read8(&b, 0x1fb20007, &v) == ZN_BOARD_HANDLED && v == 0xff);
	assert(zn_board_read8(&b, 0x1fa30003, &v) == ZN_BOARD_HANDLED && v == 0);
	assert(zn_board_read8(&b, 0x1fa40003, &v) == ZN_BOARD_HANDLED && v == 0);
	/* Pinned default-zero nopr values are observed by the original Judge IRQ
	 * dispatcher. Verify all lanes/widths; writes must not create a latch. */
	for (unsigned region = 0; region < 3; ++region) {
		uint32_t base = region == 0 ? 0x1fa30000 : region == 1 ? 0x1fa40000 : 0x1fa51c00;
		for (unsigned width = 1; width <= 4; width *= 2) {
			uint32_t value;
			for (unsigned lane = 0; lane < 4; lane += width)
				assert(zn_board_read(&b, base + lane, width, &value) == ZN_BOARD_HANDLED && value == 0);
		}
	}
	assert(zn_board_write8(&b, 0x1fa30000, 0xff) == ZN_BOARD_HANDLED);
	assert(zn_board_read8(&b, 0x1fa30000, &v) == ZN_BOARD_HANDLED && v == 0);
	assert(zn_board_write8(&b, 0x1fa40000, 0) == ZN_BOARD_UNMAPPED);
	assert(zn_board_read8(&b, 0x1fa51c00, &v) == ZN_BOARD_HANDLED && v == 0);
	assert(zn_board_read8(&b, 0x1fa60000, &v) == ZN_BOARD_HANDLED && v == 8);
	assert(zn_board_read8(&b, 0x1fa60001, &v) == ZN_BOARD_HANDLED && v == 0);
	{
		uint32_t wide;
		assert(zn_board_read(&b, 0x1fa60000, 2, &wide) == ZN_BOARD_HANDLED && wide == 8);
		assert(zn_board_read(&b, 0x1fa60000, 2, &wide) == ZN_BOARD_HANDLED && wide == 0);
		assert(zn_board_read(&b, 0x1fa60000, 4, &wide) == ZN_BOARD_UNMAPPED);
		assert(zn_board_read(&b, 0x1fa60002, 2, &wide) == ZN_BOARD_UNMAPPED);
	}
	assert(zn_board_write8(&b, 0x12345678, 0x6c) == ZN_BOARD_HANDLED);
	assert(f.external_byte == 0x6c && f.last_write_address == 0x12345678);
	assert(zn_board_read8(&b, 0x12345678, &v) == ZN_BOARD_HANDLED && v == 0x6c);
	assert(f.last_read_address == 0x12345678);
	assert(zn_board_read8(&b, 0x1fc00000, &v) == ZN_BOARD_UNMAPPED);
	assert(zn_board_write8(&b, 0x1fb00000, 0x00) == ZN_BOARD_UNMAPPED);
	zn_board_destroy(&b);
}

static void test_mux_and_cat702_wires(void)
{
	fixture_t f = {0};
	zn_board_callbacks_t cb = {0};
	zn_board_config_t cfg = {0};
	zn_board_t b;
	const uint8_t keys[2][8] = {
		{0,1,2,3,4,5,6,7}, {8,9,10,11,12,13,14,15}
	};
	uint8_t v;
	cb.opaque = &f;
	cb.mcu_select = mcu_select;
	cb.analog_select = analog_select;
	cb.trackball_select = trackball_select;
	cb.cat702_dataout = dataout;
	cfg.model = ZN_BOARD_ZN1;
	cfg.cat702_count = 2;
	cfg.cat702_keys = keys;
	assert(zn_board_init(&b, &cfg, &cb));
	assert(f.dataout[0] == 1 && f.dataout[1] == 1);
	assert(zn_board_write8(&b, 0x1fa10300, 0x38) == ZN_BOARD_HANDLED);
	assert(f.mcu_selected == 1 && f.analog_selected == 1 && f.trackball_selected == 1);
	assert(zn_board_read8(&b, 0x1fa10300, &v) == ZN_BOARD_HANDLED && v == 0x38);
	assert(zn_board_write8(&b, 0x1fa10300, 0x08) == ZN_BOARD_HANDLED);
	assert(zn_board_write(&b, 0x1fa10300, 4, 0x38) == ZN_BOARD_HANDLED);
	assert(f.mcu_selected == 1 && f.analog_selected == 1 && f.trackball_selected == 1);
	zn_board_sio_tx(&b, 0);
	zn_board_sio_clock(&b, 0);
	assert(f.dataout_changes[0] > 1);
	assert(f.dataout[0] == 1); /* First falling edge transforms the initial 0xfc state. */
	assert(f.dataout[1] == 1); /* chip 1 remained deselected */
	zn_board_reset(&b);
	assert(zn_board_read8(&b, 0x1fa10300, &v) == ZN_BOARD_HANDLED && v == 0);
	zn_board_destroy(&b);
}

static void test_validation_and_zn1_zn2_split(void)
{
	zn_board_config_t cfg = {0};
	zn_board_t b;
	uint8_t v;
	cfg.model = ZN_BOARD_ZN1;
	assert(zn_board_init(&b, &cfg, NULL));
	assert(zn_board_read8(&b, 0x1fa60000, &v) == ZN_BOARD_UNMAPPED);
	zn_board_destroy(&b);
	cfg.model = ZN_BOARD_ZN2;
	cfg.cat702_count = 3;
	assert(!zn_board_init(&b, &cfg, NULL));
	cfg.cat702_count = 0;
	assert(zn_board_init(&b, &cfg, NULL));
	assert(zn_board_read8(&b, 0x1fa60000, &v) == ZN_BOARD_HANDLED && v == 8);
	zn_board_destroy(&b);
	cfg.model = ZN_BOARD_ZN1;
	cfg.vram_bytes = 0x400000;
	assert(!zn_board_init(&b, &cfg, NULL));
	cfg.vram_bytes = 0x100000;
	cfg.ram_bytes = 0x200000;
	assert(!zn_board_init(&b, &cfg, NULL));
	cfg.ram_bytes = 0;
	cfg.cat702_count = 1;
	assert(!zn_board_init(&b, &cfg, NULL)); /* populated CAT702 needs explicit key bytes */
}

int main(void)
{
	test_boardconfig_and_inputs();
	test_coin_eeprom_and_mapped_unknowns();
	test_mux_and_cat702_wires();
	test_validation_and_zn1_zn2_split();
	test_connected_mcu();
	puts("ZN board tests passed");
	return 0;
}
