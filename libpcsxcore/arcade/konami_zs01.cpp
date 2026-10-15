// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf
#include "konami_zs01.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>
using std::memcpy;
using std::memset;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "konami_zs01_mame.inc"
#pragma GCC diagnostic pop

struct KonamiZS01 { ZS01 device; };

extern "C" {
KonamiZS01 *konami_zs01_create(const uint8_t *image, size_t size,
                              const uint8_t *serial, size_t serial_size) {
    if (!image || size != KONAMI_ZS01_IMAGE_SIZE || !serial || serial_size != 8)
        return nullptr;
    auto chip = new (std::nothrow) KonamiZS01{};
    if (!chip)
        return nullptr;
    auto &c = chip->device;
    memcpy(c.m_response_to_reset, image, 4);
    memcpy(c.m_command_key, image + 4, 8);
    memcpy(c.m_data_key, image + 12, 8);
    memcpy(c.m_configuration_registers, image + 20, 8);
    memcpy(c.m_data, image + 28, 112);
    memcpy(c.serial, serial, 8);
    c.device_reset();
    return chip;
}
void konami_zs01_destroy(KonamiZS01 *c) { delete c; }
void konami_zs01_reset(KonamiZS01 *c) { if (c) c->device.device_reset(); }
void konami_zs01_cs(KonamiZS01 *c, int level) { if (c) c->device.write_cs(level != 0); }
void konami_zs01_rst(KonamiZS01 *c, int level) { if (c) c->device.write_rst(level != 0); }
void konami_zs01_scl(KonamiZS01 *c, int level) { if (c) c->device.write_scl(level != 0); }
void konami_zs01_sda(KonamiZS01 *c, int level) { if (c) c->device.write_sda(level != 0); }
int konami_zs01_read_sda(KonamiZS01 *c) { return c ? c->device.read_sda() : 1; }
int konami_zs01_get_image(const KonamiZS01 *chip, uint8_t *out, size_t size) {
    if (!chip || !out || size != KONAMI_ZS01_IMAGE_SIZE)
        return -1;
    const auto &c = chip->device;
    memcpy(out, c.m_response_to_reset, 4);
    memcpy(out + 4, c.m_command_key, 8);
    memcpy(out + 12, c.m_data_key, 8);
    memcpy(out + 20, c.m_configuration_registers, 8);
    memcpy(out + 28, c.m_data, 112);
    return 0;
}
}
