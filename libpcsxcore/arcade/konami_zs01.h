/* SPDX-License-Identifier: BSD-3-Clause
 * Standalone MAME-derived Konami ZS01 PIC protocol (copyright smf).
 */
#ifndef PCSX_KONAMI_ZS01_H
#define PCSX_KONAMI_ZS01_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KonamiZS01 KonamiZS01;
enum { KONAMI_ZS01_IMAGE_SIZE = 140 };
/* Both original images are mandatory. ZS01: reset response[4], command key[8],
 * data key[8], configuration[8], data[112]. Serial uses DS2401 region order.
 * Copies mutable EEPROM and immutable serial; never repairs their contents. */
KonamiZS01 *konami_zs01_create(const uint8_t *image, size_t size,
                              const uint8_t *serial, size_t serial_size);
void konami_zs01_destroy(KonamiZS01 *chip);
/* Clears transient pin/command state, retaining EEPROM and serial bytes. */
void konami_zs01_reset(KonamiZS01 *chip);
void konami_zs01_cs(KonamiZS01 *chip, int level);
void konami_zs01_rst(KonamiZS01 *chip, int level);
void konami_zs01_scl(KonamiZS01 *chip, int level);
void konami_zs01_sda(KonamiZS01 *chip, int level);
/* Device output, not wired-AND host SDA. Deselected returns high. */
int konami_zs01_read_sda(KonamiZS01 *chip);
int konami_zs01_get_image(const KonamiZS01 *chip, uint8_t *image, size_t size);
#ifdef __cplusplus
}
#endif
#endif
