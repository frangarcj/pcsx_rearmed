/*
 * Sony ZN-1/ZN-2 common board I/O, based on MAME src/mame/sony/zn.cpp.
 * MAME source revision: deb2f54fe7300e7a192690318beb860159bfd52a
 *
 * Copyright smf, R. Belmont (upstream does not specify years).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "zn_board.h"

#include <string.h>

/* CAT callback routes from the fixed slot index to the host's serial input. */
static void cat0_dataout(void *opaque, int state)
{
	zn_board_t *board = (zn_board_t *)opaque;
	if (board->callbacks.cat702_dataout)
		board->callbacks.cat702_dataout(board->callbacks.opaque, 0, state);
}

static void cat1_dataout(void *opaque, int state)
{
	zn_board_t *board = (zn_board_t *)opaque;
	if (board->callbacks.cat702_dataout)
		board->callbacks.cat702_dataout(board->callbacks.opaque, 1, state);
}

int zn_board_init(zn_board_t *board, const zn_board_config_t *config,
		const zn_board_callbacks_t *callbacks)
{
	static cat702_dataout_cb const dataout_callbacks[2] = {
		cat0_dataout, cat1_dataout
	};
	unsigned i;

	if (!board || !config ||
		(config->model != ZN_BOARD_ZN1 && config->model != ZN_BOARD_ZN2) ||
		config->cat702_count > 2 ||
		(config->cat702_count && !config->cat702_keys))
		return 0;
	memset(board, 0, sizeof(*board));
	board->model = config->model;
	board->ram_bytes = config->ram_bytes ? config->ram_bytes : 0x400000;
	board->vram_bytes = config->vram_bytes ? config->vram_bytes :
		(config->model == ZN_BOARD_ZN1 ? 0x100000 : 0x200000);
	if ((board->ram_bytes != 0x400000 && board->ram_bytes != 0x800000 &&
			board->ram_bytes != 0x1000000) ||
		(board->model == ZN_BOARD_ZN1 && board->vram_bytes != 0x100000 &&
			board->vram_bytes != 0x200000) ||
		(board->model == ZN_BOARD_ZN2 && board->vram_bytes != 0x200000))
		return 0;
	board->cat702_count = config->cat702_count;
	board->mcu = config->mcu;
	if (callbacks)
		board->callbacks = *callbacks;
	board->eeprom = at28c16_create(config->eeprom,
		config->eeprom_size ? config->eeprom_size :
		(config->eeprom ? AT28C16_DATA_BYTES : 0));
	if (!board->eeprom) return 0;
	for (i = 0; i < board->cat702_count; i++) {
		const uint8_t *key = config->cat702_keys[i];
		if (!key)
			return 0;
		cat702_init(&board->cat702[i], key, 8u,
				dataout_callbacks[i], board);
	}
	zn_board_reset(board);
	return 1;
}

void zn_board_destroy(zn_board_t *board)
{
	if (!board) return;
	at28c16_destroy(board->eeprom);
	memset(board, 0, sizeof(*board));
}
int zn_board_advance_to_us(zn_board_t *board, uint64_t now_us)
{
	int result;
	if (!board) return -1;
	result = at28c16_advance_to_us(board->eeprom, now_us);
	if (!result && board->mcu) result = zn_mcu_advance_to_us(board->mcu, now_us);
	return result;
}
int zn_board_eeprom_import(zn_board_t *board, const uint8_t *image, size_t size)
{ return board ? at28c16_import_image(board->eeprom, image, size) : -1; }
int zn_board_eeprom_export(const zn_board_t *board, uint8_t *image, size_t size)
{ return board ? at28c16_get_image(board->eeprom, image, size) : -1; }

void zn_board_reset(zn_board_t *board)
{
	unsigned i;
	if (!board)
		return;
	board->coin = 0;
	board->znsecsel = 0;
	board->zn2_spu_hack = 0;
	if (board->mcu) {
		zn_mcu_analog_read(board->mcu, 0);
		zn_mcu_trackball_read(board->mcu, 0);
		zn_mcu_select(board->mcu, 1);
	}
	for (i = 0; i < board->cat702_count; i++)
		cat702_reset(&board->cat702[i]);
	if (board->callbacks.mcu_select)
		board->callbacks.mcu_select(board->callbacks.opaque, 1);
	if (board->callbacks.analog_select)
		board->callbacks.analog_select(board->callbacks.opaque, 0);
	if (board->callbacks.trackball_select)
		board->callbacks.trackball_select(board->callbacks.opaque, 0);
}

static uint8_t boardconfig(const zn_board_t *board)
{
	uint8_t result = 0x60; /* MAME revision value 3; fixed bits 6 and 5. */
	switch (board->vram_bytes) {
	case 0x200000: result |= 0x08; break;
	default: break;
	}
	switch (board->ram_bytes) {
	case 0x400000: result |= 0x01; break;
	case 0x800000: result |= 0x02; break;
	case 0x1000000: result |= 0x03; break;
	default: break;
	}
	return result;
}

static int read_input(zn_board_t *board, zn_board_input_t input, uint8_t *value)
{
	if (!board->callbacks.read_input)
		return 0;
	*value = board->callbacks.read_input(board->callbacks.opaque, input);
	return 1;
}

zn_board_result_t zn_board_read8(zn_board_t *board, uint32_t address, uint8_t *value)
{
	if (!board || !value)
		return ZN_BOARD_UNMAPPED;
	if (address >= 0x1fa00000 && address <= 0x1fa00003)
		return address == 0x1fa00000 ?
			(read_input(board, ZN_INPUT_P1, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address >= 0x1fa00100 && address <= 0x1fa00103)
		return address == 0x1fa00100 ?
			(read_input(board, ZN_INPUT_P2, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address >= 0x1fa00200 && address <= 0x1fa00203)
		return address == 0x1fa00200 ?
			(read_input(board, ZN_INPUT_SERVICE, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address >= 0x1fa00300 && address <= 0x1fa00303)
		return address == 0x1fa00300 ?
			(read_input(board, ZN_INPUT_SYSTEM, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address >= 0x1fa10000 && address <= 0x1fa10003)
		return address == 0x1fa10000 ?
			(read_input(board, ZN_INPUT_P3, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address >= 0x1fa10100 && address <= 0x1fa10103)
		return address == 0x1fa10100 ?
			(read_input(board, ZN_INPUT_P4, value) ? ZN_BOARD_HANDLED : ZN_BOARD_UNMAPPED) :
			(*value = 0, ZN_BOARD_HANDLED);
	if (address == 0x1fa10200) {
		*value = boardconfig(board);
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa10201 && address <= 0x1fa10203) {
		*value = 0;
		return ZN_BOARD_HANDLED;
	}
	if (address == 0x1fa10300) {
		*value = board->znsecsel;
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa10301 && address <= 0x1fa10303) {
		*value = 0;
		return ZN_BOARD_HANDLED;
	}
	if (address == 0x1fa20000) {
		*value = board->coin;
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa20001 && address <= 0x1fa20003) {
		*value = 0;
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1faf0000 && address <= 0x1faf07ff) {
		*value = at28c16_read(board->eeprom, address - 0x1faf0000);
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fb20000 && address <= 0x1fb20007) {
		*value = 0xff; /* MAME unknown_r returns 0xffff on the 16-bit bus. */
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa30000 && address <= 0x1fa30003) {
		*value = 0; /* MAME noprw uses the program map default unmapped value. */
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa40000 && address <= 0x1fa40003) {
		*value = 0; /* MAME nopr uses the program map default unmapped value. */
		return ZN_BOARD_HANDLED;
	}
	if (board->model == ZN_BOARD_ZN2 && address >= 0x1fa51c00 && address <= 0x1fa51dff) {
		*value = 0; /* ZN-2 timing range is also MAME nopr, with default zero. */
		return ZN_BOARD_HANDLED;
	}
	if (board->model == ZN_BOARD_ZN2 && address == 0x1fa60000) {
		/* ZN-2's MAME read toggles bit 3 on each side-effecting read. */
		board->zn2_spu_hack ^= 8;
		*value = board->zn2_spu_hack;
		return ZN_BOARD_HANDLED;
	}
	if (board->model == ZN_BOARD_ZN2 && address == 0x1fa60001) {
		/* The byte adapter still performs one native 16-bit read side effect. */
		board->zn2_spu_hack ^= 8;
		*value = 0;
		return ZN_BOARD_HANDLED;
	}
	if (board->callbacks.read_external &&
		board->callbacks.read_external(board->callbacks.opaque, address, value))
		return ZN_BOARD_HANDLED;
	return ZN_BOARD_UNMAPPED;
}

zn_board_result_t zn_board_read(zn_board_t *board, uint32_t address,
		unsigned width, uint32_t *value)
{
	unsigned lane;
	uint32_t result = 0;
	if (!board || !value || (width != 1 && width != 2 && width != 4) ||
		(address & (width - 1)) != 0)
		return ZN_BOARD_UNMAPPED;
	if (board->model == ZN_BOARD_ZN2 && address == 0x1fa60000 && width == 2) {
		board->zn2_spu_hack ^= 8;
		*value = board->zn2_spu_hack;
		return ZN_BOARD_HANDLED;
	}
	if (board->model == ZN_BOARD_ZN2 && width == 4 &&
		address <= 0x1fa60001 && address + width > 0x1fa60000)
		return ZN_BOARD_UNMAPPED;
	for (lane = 0; lane < width; lane++) {
		uint8_t byte_value;
		if (zn_board_read8(board, address + lane, &byte_value) != ZN_BOARD_HANDLED)
			return ZN_BOARD_UNMAPPED;
		result |= (uint32_t)byte_value << (lane * 8);
	}
	*value = result;
	return ZN_BOARD_HANDLED;
}

zn_board_result_t zn_board_write8(zn_board_t *board, uint32_t address, uint8_t value)
{
	unsigned i;
	if (!board)
		return ZN_BOARD_UNMAPPED;
	if (address == 0x1fa10300) {
		board->znsecsel = value;
		if (board->mcu) {
			zn_mcu_analog_read(board->mcu, (value >> 4) & 1);
			zn_mcu_trackball_read(board->mcu, (value >> 5) & 1);
			if (zn_mcu_select(board->mcu, (value & 0x8c) != 0x8c))
				return ZN_BOARD_UNMAPPED;
		}
		for (i = 0; i < board->cat702_count; i++)
			cat702_write_select(&board->cat702[i], (value >> (2 + i)) & 1);
		if (board->callbacks.analog_select)
			board->callbacks.analog_select(board->callbacks.opaque, (value >> 4) & 1);
		if (board->callbacks.trackball_select)
			board->callbacks.trackball_select(board->callbacks.opaque, (value >> 5) & 1);
		if (board->callbacks.mcu_select)
			board->callbacks.mcu_select(board->callbacks.opaque,
				(value & 0x8c) != 0x8c);
		return ZN_BOARD_HANDLED;
	}
	if (address == 0x1fa20000) {
		board->coin = value;
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1faf0000 && address <= 0x1faf07ff) {
		if (at28c16_write(board->eeprom, address - 0x1faf0000, value) < 0)
			return ZN_BOARD_UNMAPPED;
		return ZN_BOARD_HANDLED;
	}
	if (address >= 0x1fa30000 && address <= 0x1fa30003)
		return ZN_BOARD_HANDLED; /* MAME noprw. */
	if (board->callbacks.write_external &&
		board->callbacks.write_external(board->callbacks.opaque, address, value))
		return ZN_BOARD_HANDLED;
	return ZN_BOARD_UNMAPPED;
}

zn_board_result_t zn_board_write(zn_board_t *board, uint32_t address,
		unsigned width, uint32_t value)
{
	unsigned lane;
	if (!board || (width != 1 && width != 2 && width != 4) ||
		(address & (width - 1)) != 0)
		return ZN_BOARD_UNMAPPED;
	if (width > 1 && (address == 0x1fa10300 || address == 0x1fa20000))
		return zn_board_write8(board, address, (uint8_t)value);
	if (width > 1 && address == 0x1fa10200)
		return ZN_BOARD_UNMAPPED; /* read-only board configuration */
	for (lane = 0; lane < width; lane++) {
		if (zn_board_write8(board, address + lane,
				(uint8_t)(value >> (lane * 8))) != ZN_BOARD_HANDLED)
			return ZN_BOARD_UNMAPPED;
	}
	return ZN_BOARD_HANDLED;
}

int zn_board_sio_clock(zn_board_t *board, int state)
{
	unsigned i;
	int result = 0;
	if (!board)
		return -1;
	if (board->mcu) result = zn_mcu_sck(board->mcu, state);
	for (i = 0; i < board->cat702_count; i++)
		cat702_write_clock(&board->cat702[i], state);
	return result;
}

void zn_board_sio_tx(zn_board_t *board, int state)
{
	unsigned i;
	if (!board)
		return;
	for (i = 0; i < board->cat702_count; i++)
		cat702_write_datain(&board->cat702[i], state);
}
