/* Standalone flash command/state engines for PSX arcade boards. */
#ifndef LIBPCSXCORE_ARCADE_NOR_FLASH_H
#define LIBPCSXCORE_ARCADE_NOR_FLASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nor_flash nor_flash_t;

typedef enum nor_flash_profile {
    NOR_FLASH_INTEL_28F640J5_X16 = 0,
    NOR_FLASH_FUJITSU_MBM29F016A,
    NOR_FLASH_AMD_29F040,
    NOR_FLASH_SST_39SF040,
    NOR_FLASH_INTEL_E28F400B_X16,
    NOR_FLASH_INTEL_TE28F160_X16,
    NOR_FLASH_SHARP_LH28F400_X16
} nor_flash_profile_t;

typedef enum nor_flash_result {
    NOR_FLASH_OK = 0,
    NOR_FLASH_ERR_ARGUMENT = -1,
    NOR_FLASH_ERR_GEOMETRY = -2,
    NOR_FLASH_ERR_RANGE = -3,
    NOR_FLASH_ERR_BUSY = -4,
    NOR_FLASH_ERR_SEQUENCE = -5,
    NOR_FLASH_ERR_UNSUPPORTED = -6
} nor_flash_result_t;

/* The image is borrowed and writable; its contents are preserved on creation. */
nor_flash_t *nor_flash_create(nor_flash_profile_t profile,
                              uint8_t *image, size_t image_size);
void nor_flash_destroy(nor_flash_t *flash);

/* Addresses are byte offsets for x8 devices and 16-bit word offsets for x16. */
int nor_flash_read(nor_flash_t *flash, uint32_t address, uint16_t *value);
int nor_flash_write(nor_flash_t *flash, uint32_t address, uint16_t value);
/* Byte span changed by the most recent nor_flash_write call, if any. Erases
 * report their complete changed span; command/status writes report no span. */
int nor_flash_last_write_dirty_span(const nor_flash_t *flash,
                                    size_t *byte_offset, size_t *byte_length);
void nor_flash_advance_us(nor_flash_t *flash, uint64_t microseconds);
int nor_flash_ready(const nor_flash_t *flash);

uint8_t *nor_flash_image(nor_flash_t *flash);
size_t nor_flash_image_size(const nor_flash_t *flash);
unsigned nor_flash_bus_width(const nor_flash_t *flash);
uint8_t nor_flash_manufacturer_id(const nor_flash_t *flash);
uint16_t nor_flash_device_id(const nor_flash_t *flash);
/* Erase span in image bytes for the supplied bus address; zero if out of range. */
uint32_t nor_flash_sector_size(const nor_flash_t *flash, uint32_t address);
const char *nor_flash_profile_name(nor_flash_profile_t profile);

#ifdef __cplusplus
}
#endif

#endif
