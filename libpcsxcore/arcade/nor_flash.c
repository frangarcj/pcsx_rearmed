// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/*
 * Flash command handling derived from MAME's intelfsh.cpp / intelfsh.h.
 * Copyright (c) 1998-2024, Aaron Giles
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Standalone wrapper and virtual-time adapter: GPL-2.0-or-later.
 */
#include "nor_flash.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum flash_mode {
    MODE_ARRAY,
    MODE_INTEL_ID,
    MODE_INTEL_STATUS,
    MODE_INTEL_PROGRAM,
    MODE_INTEL_ERASE_CONFIRM,
    MODE_INTEL_SET_MASTER,
    MODE_INTEL_BUFFER_COUNT, MODE_INTEL_BUFFER_DATA,
    MODE_AMD_UNLOCK1,
    MODE_AMD_UNLOCK2,
    MODE_AMD_ID,
    MODE_AMD_ERASE1,
    MODE_AMD_ERASE2,
    MODE_AMD_ERASE3,
    MODE_AMD_PROGRAM,
    MODE_AMD_ERASE_BUSY
};

typedef struct flash_spec {
    size_t size;
    unsigned width;
    uint8_t maker;
    uint16_t device;
    uint32_t sector;
    uint64_t sector_erase_us;
    uint64_t chip_erase_us;
    int intel;
    int fujitsu_busy_reads_all;
    int intel_mame_program;
    int intel_amd_commands;
} flash_spec;

struct nor_flash {
    nor_flash_profile_t profile;
    flash_spec spec;
    uint8_t *image;
    enum flash_mode mode;
    uint8_t status;
    int master_lock;
    uint64_t now_us, busy_until_us;
    uint32_t erase_start, busy_read_start, busy_read_length;
    uint8_t toggle;
    size_t dirty_start, dirty_end;
    int dirty_valid;
    uint8_t write_buffer[32];
    uint32_t buffer_start;
    unsigned buffer_count, buffer_used;
};

static void mark_dirty(nor_flash_t *flash, size_t start, size_t length)
{
    if (!length) return;
    flash->dirty_start = start;
    flash->dirty_end = start + length;
    flash->dirty_valid = 1;
}

static int profile_spec(nor_flash_profile_t profile, flash_spec *spec)
{
    switch (profile) {
    case NOR_FLASH_INTEL_28F640J5_X16:
        spec->size = 0x800000; spec->width = 16;
        spec->maker = 0x89; spec->device = 0x15;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 0; spec->intel = 1;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 0; spec->intel_amd_commands = 0;
        return 1;
    case NOR_FLASH_FUJITSU_MBM29F016A:
        spec->size = 0x200000; spec->width = 8;
        spec->maker = 0x04; spec->device = 0xad;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 16000000; spec->intel = 0;
        spec->fujitsu_busy_reads_all = 1;
        spec->intel_mame_program = 0; spec->intel_amd_commands = 0;
        return 1;
    case NOR_FLASH_AMD_29F040:
        spec->size = 0x80000; spec->width = 8;
        spec->maker = 0x01; spec->device = 0xa4;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 16000000; spec->intel = 0;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 0; spec->intel_amd_commands = 0;
        return 1;
    case NOR_FLASH_SST_39SF040:
        spec->size = 0x80000; spec->width = 8;
        spec->maker = 0xbf; spec->device = 0xb7;
        spec->sector = 0x1000; spec->sector_erase_us = 125000;
        spec->chip_erase_us = 1000000; spec->intel = 0;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 0; spec->intel_amd_commands = 0;
        return 1;
    case NOR_FLASH_INTEL_E28F400B_X16:
        spec->size = 0x80000; spec->width = 16;
        spec->maker = 0x89; spec->device = 0x4471;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 16000000; spec->intel = 1;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 1; spec->intel_amd_commands = 1;
        return 1;
    case NOR_FLASH_INTEL_TE28F160_X16:
        spec->size = 0x200000; spec->width = 16;
        spec->maker = 0xb0; spec->device = 0x00d0;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 16000000; spec->intel = 1;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 1; spec->intel_amd_commands = 1;
        return 1;
    case NOR_FLASH_SHARP_LH28F400_X16:
        /* Pinned intelfsh16_device: 512 KiB, uniform 64 KiB erase blocks,
         * Sharp ID B0/ED and the shared source command machine. */
        spec->size = 0x80000; spec->width = 16;
        spec->maker = 0xb0; spec->device = 0x00ed;
        spec->sector = 0x10000; spec->sector_erase_us = 1000000;
        spec->chip_erase_us = 16000000; spec->intel = 1;
        spec->fujitsu_busy_reads_all = 0;
        spec->intel_mame_program = 1; spec->intel_amd_commands = 1;
        return 1;
    default:
        return 0;
    }
}

const char *nor_flash_profile_name(nor_flash_profile_t profile)
{
    switch (profile) {
    case NOR_FLASH_INTEL_28F640J5_X16: return "Intel 28F640J5 x16";
    case NOR_FLASH_FUJITSU_MBM29F016A: return "Fujitsu MBM29F016A";
    case NOR_FLASH_AMD_29F040: return "AMD 29F040";
    case NOR_FLASH_SST_39SF040: return "SST 39SF040";
    case NOR_FLASH_INTEL_E28F400B_X16: return "Intel E28F400B x16";
    case NOR_FLASH_INTEL_TE28F160_X16: return "Intel TE28F160 x16";
    case NOR_FLASH_SHARP_LH28F400_X16: return "Sharp LH28F400 x16";
    default: return "unsupported NOR profile";
    }
}

nor_flash_t *nor_flash_create(nor_flash_profile_t profile,
                              uint8_t *image, size_t image_size)
{
    nor_flash_t *flash;
    flash_spec spec;
    if (!image || !profile_spec(profile, &spec) || image_size != spec.size)
        return NULL;
    flash = (nor_flash_t *)calloc(1, sizeof(*flash));
    if (!flash) return NULL;
    flash->profile = profile;
    flash->spec = spec;
    flash->image = image;
    flash->mode = MODE_ARRAY;
    flash->status = 0x80;
    return flash;
}

void nor_flash_destroy(nor_flash_t *flash)
{
    free(flash);
}

static uint32_t bus_size(const nor_flash_t *flash)
{
    return (uint32_t)(flash->spec.size / (flash->spec.width / 8));
}

static uint32_t image_offset(const nor_flash_t *flash, uint32_t address)
{
    return address * (flash->spec.width / 8);
}

static uint16_t array_read(const nor_flash_t *flash, uint32_t address)
{
    uint32_t off = image_offset(flash, address);
    if (flash->spec.width == 16)
        return (uint16_t)((uint16_t)flash->image[off] << 8 | flash->image[off + 1]);
    return flash->image[off];
}

static void array_program(nor_flash_t *flash, uint32_t address, uint16_t value)
{
    uint32_t off = image_offset(flash, address);
    if (flash->spec.width == 16) {
        if (flash->spec.intel_mame_program) {
            /* MAME intelfsh writes x16 words directly in FM_WRITEPART1. */
            flash->image[off] = (uint8_t)(value >> 8);
            flash->image[off + 1] = (uint8_t)value;
        } else {
            flash->image[off] &= (uint8_t)(value >> 8);
            flash->image[off + 1] &= (uint8_t)value;
        }
    } else if (flash->profile == NOR_FLASH_FUJITSU_MBM29F016A) {
        /* Pinned intelfsh.cpp FM_BYTEPROGRAM assigns data on this x8 part. */
        flash->image[off] = (uint8_t)value;
    } else {
        flash->image[off] &= (uint8_t)value;
    }
    mark_dirty(flash, off, flash->spec.width / 8);
}

static void finish_if_due(nor_flash_t *flash)
{
    if (flash->mode == MODE_AMD_ERASE_BUSY && flash->now_us >= flash->busy_until_us) {
        flash->mode = MODE_ARRAY;
        flash->status = 0x80;
        flash->toggle = 0;
    } else if (flash->spec.intel && flash->busy_until_us &&
               flash->now_us >= flash->busy_until_us) {
        flash->status = 0x80;
        flash->busy_until_us = 0;
    }
}

static void intel_erase_block(nor_flash_t *flash, uint32_t address)
{
    uint32_t base = image_offset(flash, address);
    uint32_t length = flash->spec.sector;
    uint64_t delay = flash->spec.sector_erase_us;
    if (flash->profile == NOR_FLASH_INTEL_E28F400B_X16) {
        base = (address & 0x3ffffu) * 2;
        if (base < 0x4000) {
            base = 0;
            length = 0x4000;
            delay = 300000;
        } else if (base < 0x8000) {
            base &= 0x6000;
            length = 0x2000;
            delay = 300000;
        } else if (base < 0x20000) {
            base = 0x8000;
            length = 0x18000;
            delay = 600000;
        } else {
            base &= 0x60000;
            length = 0x20000;
            delay = 600000;
        }
    } else {
        base &= ~(length - 1);
    }
    for (uint32_t i = 0; i < length; i++) flash->image[base + i] = 0xff;
    mark_dirty(flash, base, length);
    flash->status = 0;
    flash->mode = MODE_INTEL_STATUS;
    flash->busy_until_us = UINT64_MAX - flash->now_us < delay
        ? UINT64_MAX : flash->now_us + delay;
}

void nor_flash_advance_us(nor_flash_t *flash, uint64_t microseconds)
{
    if (!flash) return;
    flash->now_us = UINT64_MAX - flash->now_us < microseconds
        ? UINT64_MAX : flash->now_us + microseconds;
    finish_if_due(flash);
}

int nor_flash_ready(const nor_flash_t *flash)
{
    if (!flash) return 0;
    if (flash->mode == MODE_AMD_ERASE_BUSY)
        return flash->now_us >= flash->busy_until_us;
    return !flash->busy_until_us || flash->now_us >= flash->busy_until_us;
}

static uint16_t status_read(nor_flash_t *flash)
{
    flash->toggle ^= 1;
    return (uint16_t)(flash->status ^ (flash->toggle ? 0x44 : 0));
}

int nor_flash_read(nor_flash_t *flash, uint32_t address, uint16_t *value)
{
    uint16_t result;
    uint32_t id_address;
    int in_busy_range;
    if (!flash || !value) return NOR_FLASH_ERR_ARGUMENT;
    if (address >= bus_size(flash)) return NOR_FLASH_ERR_RANGE;
    finish_if_due(flash);
    if (flash->spec.intel && flash->mode != MODE_AMD_ID &&
        flash->mode != MODE_AMD_ERASE_BUSY) {
        switch (flash->mode) {
        case MODE_INTEL_ID:
            switch (address & 0xff) {
            case 0: result = flash->spec.maker; break;
            case 1: result = flash->spec.device; break;
            case 2: result = 0; break; /* block-lock config unsupported by MAME */
            case 3: result = flash->master_lock ? 1 : 0; break;
            default: result = 0; break;
            }
            break;
        case MODE_INTEL_BUFFER_COUNT: result = 0x80; break;
        case MODE_INTEL_BUFFER_DATA:
        case MODE_INTEL_STATUS: result = flash->status; break;
        case MODE_ARRAY:
        case MODE_INTEL_PROGRAM:
        case MODE_INTEL_ERASE_CONFIRM:
        case MODE_INTEL_SET_MASTER:
        default: result = array_read(flash, address); break;
        }
    } else if (flash->mode == MODE_AMD_ID) {
        id_address = address & 0xff;
        result = id_address == 0 ? flash->spec.maker :
                 id_address == 1 ? flash->spec.device :
                 0;
    } else if (flash->mode == MODE_AMD_ERASE_BUSY) {
        in_busy_range = flash->spec.fujitsu_busy_reads_all ||
            (flash->busy_read_length && address >= flash->busy_read_start &&
             address - flash->busy_read_start < flash->busy_read_length);
        result = in_busy_range ? status_read(flash) : array_read(flash, address);
    } else {
        result = array_read(flash, address);
    }
    *value = result;
    return NOR_FLASH_OK;
}

static int amd_unlock1_address(const nor_flash_t *flash, uint32_t address)
{
    return (address & 0xfff) == 0x555 || (address & 0xfff) == 0xaaa ||
           (flash->profile == NOR_FLASH_SST_39SF040 &&
            (address & 0x7fff) == 0x5555);
}

static int amd_unlock2_address(uint32_t address)
{
    return (address & 0xffff) == 0x2aa || (address & 0xffff) == 0x2aaa ||
           (address & 0xfff) == 0x555;
}

static int amd_command_address(const nor_flash_t *flash, uint32_t address)
{
    return (address & 0xffff) == 0x555 || (address & 0xffff) == 0x5555 ||
           (address & 0xfff) == 0xaaa ||
           (flash->profile == NOR_FLASH_SST_39SF040 &&
            (address & 0x7fff) == 0x5555);
}

static int is_unlock_reset(uint16_t value)
{
    return (value & 0xff) == 0xf0 || (value & 0xff) == 0xff;
}

static int intel_write(nor_flash_t *flash, uint32_t address, uint16_t value)
{
    uint8_t command = (uint8_t)value;
    if (flash->busy_until_us && flash->now_us < flash->busy_until_us &&
        command != 0xf0 && command != 0xff && command != 0x70)
        return NOR_FLASH_ERR_BUSY;
    switch (flash->mode) {
    case MODE_INTEL_BUFFER_COUNT:
        flash->buffer_count = (value < 15 ? value : 15) + 1;
        flash->buffer_used = 0; flash->status = 0x80;
        flash->mode = MODE_INTEL_BUFFER_DATA;
        return NOR_FLASH_OK;
    case MODE_INTEL_BUFFER_DATA:
        if (flash->buffer_used < flash->buffer_count) {
            if (!flash->buffer_used) flash->buffer_start = address;
            if (address >= flash->buffer_start + flash->buffer_count ||
                flash->buffer_start + flash->buffer_count > bus_size(flash)) {
                flash->status = 0x30; flash->mode = MODE_INTEL_STATUS;
                return NOR_FLASH_OK; /* device sequence error, not a host bus fault */
            }
            flash->write_buffer[flash->buffer_used * 2] = value >> 8;
            flash->write_buffer[flash->buffer_used * 2 + 1] = value;
            flash->buffer_used++;
        } else {
            if (command == 0xd0) {
                uint32_t start = image_offset(flash, flash->buffer_start);
                /* MAME FM_WRITEBUFFER2 commits its big-endian byte buffer. */
                memcpy(flash->image + start, flash->write_buffer, flash->buffer_count * 2);
                mark_dirty(flash, start, flash->buffer_count * 2);
                flash->status = 0x80;
            } else flash->status = 0x30;
            flash->mode = MODE_INTEL_STATUS;
        }
        return NOR_FLASH_OK;
    case MODE_INTEL_PROGRAM:
        array_program(flash, address, value);
        flash->status = 0x80;
        flash->mode = MODE_INTEL_STATUS;
        return NOR_FLASH_OK;
    case MODE_INTEL_ERASE_CONFIRM:
        if (command != 0xd0) {
            flash->mode = MODE_INTEL_STATUS;
            return NOR_FLASH_ERR_SEQUENCE;
        }
        intel_erase_block(flash, address);
        return NOR_FLASH_OK;
    case MODE_INTEL_SET_MASTER:
        if (command == 0xf1) flash->master_lock = 1;
        else if (command == 0xd0) flash->master_lock = 0;
        else { flash->mode = MODE_INTEL_STATUS; return NOR_FLASH_ERR_SEQUENCE; }
        flash->mode = MODE_INTEL_STATUS;
        return NOR_FLASH_OK;
    default:
        break;
    }
    if (is_unlock_reset(command)) {
        flash->mode = MODE_ARRAY;
        return NOR_FLASH_OK;
    }
    switch (command) {
    case 0x90: flash->mode = MODE_INTEL_ID; return NOR_FLASH_OK;
    case 0x40:
    case 0x10: flash->mode = MODE_INTEL_PROGRAM; return NOR_FLASH_OK;
    case 0x50: flash->status = 0x80; flash->mode = MODE_INTEL_STATUS; return NOR_FLASH_OK;
    case 0x70: flash->mode = MODE_INTEL_STATUS; return NOR_FLASH_OK;
    case 0x20: flash->mode = MODE_INTEL_ERASE_CONFIRM; return NOR_FLASH_OK;
    case 0x60: flash->mode = MODE_INTEL_SET_MASTER; return NOR_FLASH_OK;
    case 0xe8:
        if (flash->profile != NOR_FLASH_INTEL_28F640J5_X16) return NOR_FLASH_ERR_UNSUPPORTED;
        flash->mode = MODE_INTEL_BUFFER_COUNT; return NOR_FLASH_OK;
    default: return NOR_FLASH_ERR_UNSUPPORTED;
    }
}

static void amd_begin_erase(nor_flash_t *flash, uint32_t address, int chip)
{
    uint32_t start, length;
    uint64_t delay;
    if (chip) {
        start = 0; length = (uint32_t)flash->spec.size;
        delay = flash->spec.chip_erase_us;
    } else {
        start = image_offset(flash, address) & ~(flash->spec.sector - 1);
        length = flash->spec.sector;
        delay = flash->spec.sector_erase_us;
    }
    for (uint32_t i = 0; i < length; i++) flash->image[start + i] = 0xff;
    mark_dirty(flash, start, length);
    if (!chip) flash->busy_read_start = start / (flash->spec.width / 8);
    /* MAME leaves the last polled sector unchanged on chip erase. */
    /* intelfsh::read_full uses a fixed 64K address-unit polling window, also
       for x16 parts, even though the physical erase span is 64 KiB in bytes. */
    flash->busy_read_length = (chip || flash->spec.intel_amd_commands)
        ? 0x10000 : length / (flash->spec.width / 8);
    flash->status = 0x08;
    flash->toggle = 0;
    flash->mode = MODE_AMD_ERASE_BUSY;
    flash->busy_until_us = UINT64_MAX - flash->now_us < delay
        ? UINT64_MAX : flash->now_us + delay;
}

static int amd_write(nor_flash_t *flash, uint32_t address, uint16_t value)
{
    uint8_t command = (uint8_t)value;
    switch (flash->mode) {
    case MODE_AMD_ERASE_BUSY:
        return NOR_FLASH_ERR_BUSY;
    case MODE_AMD_PROGRAM:
        array_program(flash, address, value);
        flash->mode = MODE_ARRAY;
        return NOR_FLASH_OK;
    case MODE_AMD_UNLOCK1:
        if (amd_unlock2_address(address) && command == 0x55) {
            flash->mode = MODE_AMD_UNLOCK2;
            return NOR_FLASH_OK;
        }
        flash->mode = MODE_ARRAY;
        return NOR_FLASH_ERR_SEQUENCE;
    case MODE_AMD_UNLOCK2:
        if (!amd_command_address(flash, address)) {
            flash->mode = MODE_ARRAY;
            return NOR_FLASH_ERR_SEQUENCE;
        }
        if (command == 0x90) flash->mode = MODE_AMD_ID;
        else if (command == 0x80) flash->mode = MODE_AMD_ERASE1;
        else if (command == 0xa0) flash->mode = MODE_AMD_PROGRAM;
        else { flash->mode = MODE_ARRAY; return NOR_FLASH_ERR_UNSUPPORTED; }
        return NOR_FLASH_OK;
    case MODE_AMD_ERASE1:
        if (amd_unlock1_address(flash, address) && command == 0xaa) {
            flash->mode = MODE_AMD_ERASE2; return NOR_FLASH_OK;
        }
        flash->mode = MODE_ARRAY; return NOR_FLASH_ERR_SEQUENCE;
    case MODE_AMD_ERASE2:
        if (amd_unlock2_address(address) && command == 0x55) {
            flash->mode = MODE_AMD_ERASE3; return NOR_FLASH_OK;
        }
        flash->mode = MODE_ARRAY; return NOR_FLASH_ERR_SEQUENCE;
    case MODE_AMD_ERASE3:
        if ((command == 0x10 && amd_command_address(flash, address))) {
            amd_begin_erase(flash, address, 1); return NOR_FLASH_OK;
        }
        if (command == 0x30) {
            amd_begin_erase(flash, address, 0); return NOR_FLASH_OK;
        }
        flash->mode = MODE_ARRAY; return NOR_FLASH_ERR_UNSUPPORTED;
    case MODE_AMD_ID:
    case MODE_ARRAY:
    default:
        break;
    }
    if (is_unlock_reset(command)) {
        flash->mode = MODE_ARRAY;
        return NOR_FLASH_OK;
    }
    if (amd_unlock1_address(flash, address) && command == 0xaa) {
        flash->mode = MODE_AMD_UNLOCK1;
        return NOR_FLASH_OK;
    }
    return NOR_FLASH_ERR_UNSUPPORTED;
}

int nor_flash_write(nor_flash_t *flash, uint32_t address, uint16_t value)
{
    if (!flash) return NOR_FLASH_ERR_ARGUMENT;
    flash->dirty_valid = 0;
    if (address >= bus_size(flash)) return NOR_FLASH_ERR_RANGE;
    finish_if_due(flash);
    if (!flash->spec.intel) return amd_write(flash, address, value);
    if (flash->spec.intel_amd_commands &&
        ((flash->mode >= MODE_AMD_UNLOCK1 && flash->mode <= MODE_AMD_ERASE_BUSY) ||
         ((flash->mode == MODE_ARRAY || flash->mode == MODE_INTEL_STATUS ||
           flash->mode == MODE_INTEL_ID) && amd_unlock1_address(flash, address) &&
          (value & 0xff) == 0xaa)))
        return amd_write(flash, address, value);
    return intel_write(flash, address, value);
}

int nor_flash_last_write_dirty_span(const nor_flash_t *flash,
                                    size_t *byte_offset, size_t *byte_length)
{
    if (!flash || !byte_offset || !byte_length) return 0;
    if (!flash->dirty_valid) return 0;
    *byte_offset = flash->dirty_start;
    *byte_length = flash->dirty_end - flash->dirty_start;
    return 1;
}

uint8_t *nor_flash_image(nor_flash_t *flash)
{
    return flash ? flash->image : NULL;
}

size_t nor_flash_image_size(const nor_flash_t *flash)
{
    return flash ? flash->spec.size : 0;
}

unsigned nor_flash_bus_width(const nor_flash_t *flash)
{
    return flash ? flash->spec.width : 0;
}

uint8_t nor_flash_manufacturer_id(const nor_flash_t *flash)
{
    return flash ? flash->spec.maker : 0;
}

uint16_t nor_flash_device_id(const nor_flash_t *flash)
{
    return flash ? flash->spec.device : 0;
}

uint32_t nor_flash_sector_size(const nor_flash_t *flash, uint32_t address)
{
    if (!flash || address >= bus_size(flash)) return 0;
    if (flash->profile == NOR_FLASH_INTEL_E28F400B_X16) {
        uint32_t byte_address = (address & 0x3ffffu) * 2;
        if (byte_address < 0x4000) return 0x4000;
        if (byte_address < 0x8000) return 0x2000;
        if (byte_address < 0x20000) return 0x18000;
        return 0x20000;
    }
    return flash->spec.sector;
}
