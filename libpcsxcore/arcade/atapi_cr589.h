/* SPDX-License-Identifier: BSD-3-Clause. MAME smf. Source CR-589/ATAPI/MMC.
 * See atapi_mame_LICENSE.txt and docs/system573-mac.md. */
#ifndef PCSX_ATAPI_CR589_H
#define PCSX_ATAPI_CR589_H
#include "nscsi_cd.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AtapiCr589 AtapiCr589;
typedef struct AtapiTrack {
    uint32_t type,frames,pregap,pgdatasize,session;
} AtapiTrack;
typedef struct AtapiCr589Config {
    nscsi_cd_media_ops_t media;
    /* Types follow the pinned MAME/libchdr CD_TRACK_* enum. Raw requests
     * require actual stored bytes; callbacks must not invent ECC/subheaders. */
    int (*track_info)(void *opaque,unsigned track,AtapiTrack *out);
    int (*read_raw)(void *opaque,uint32_t lba,uint8_t out[2352]);
    void (*irq)(void *opaque,int level);
    void (*dmarq)(void *opaque,int level);
    void *line_opaque;
} AtapiCr589Config;
typedef struct AtapiCr589Status {
    uint64_t packets,sectors,diagnostics;
    uint32_t faults;
    uint8_t status,error,command,packet_command,download;
    uint8_t sense_key,sense_asc;
    unsigned byte_count,buffer_offset;
} AtapiCr589Status;
AtapiCr589 *atapi_cr589_create(const AtapiCr589Config *config);
void atapi_cr589_destroy(AtapiCr589 *drive);
/* Mutating calls and destruction are refused while a device callback/API call
 * is active; status inspection is allowed. Rejected reentry records fault 8. */
void atapi_cr589_reset(AtapiCr589 *drive);
int atapi_cr589_advance_to(AtapiCr589 *drive,uint64_t ns);
uint16_t atapi_cr589_read(AtapiCr589 *drive,int cs,unsigned reg,uint16_t mask);
void atapi_cr589_write(AtapiCr589 *drive,int cs,unsigned reg,uint16_t data,uint16_t mask);
void atapi_cr589_get_status(AtapiCr589 *drive,AtapiCr589Status *out);
/* Original source drive buffer, including writable inquiry identity. */
int atapi_cr589_nvram_export(AtapiCr589 *drive,uint8_t *out,size_t bytes);
int atapi_cr589_nvram_import(AtapiCr589 *drive,const uint8_t *image,size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
