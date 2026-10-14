/* Read-only CHD CD backend, using the repository-pinned libchdr. */
#ifndef LIBPCSXCORE_ARCADE_CHD_CD_H
#define LIBPCSXCORE_ARCADE_CHD_CD_H
#include "nscsi_cd.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct ChdCd ChdCd;
typedef void (*chd_cd_pcm_sink_t)(void *opaque, int16_t left, int16_t right);
typedef struct chd_cd_config {
  chd_cd_pcm_sink_t pcm_sink;
  void *pcm_opaque;
} chd_cd_config_t;

/* Opens a real CHD containing supported CD track metadata. Failure is closed:
 * no virtual disc, track, zero sector, or ready state is supplied. */
ChdCd *chd_cd_open(const char *path, const chd_cd_config_t *config,
                   char *error, size_t error_capacity);
void chd_cd_close(ChdCd *cd);
int chd_cd_get_media_ops(ChdCd *cd, nscsi_cd_media_ops_t *ops);
/* Real stored 2352-byte sectors for ATAPI READ CD (MODE1_RAW/MODE2_RAW/AUDIO).
 * Packed 2048-byte tracks are rejected rather than inventing raw ECC bytes. */
int chd_cd_read_raw_sector(ChdCd *cd, uint32_t lba, uint8_t out[2352]);
int chd_cd_verify_sha1(ChdCd *cd, const char *expected_hex);
/* Advance absolute nanoseconds. Time must be monotonic. CDDA emits 44.1 kHz,
 * stereo signed PCM16 samples from the actual audio sectors to the sink. */
int chd_cd_advance_to(ChdCd *cd, uint64_t now_ns);
/* Optional continuous DAC clock, configured before advancing/starting audio.
 * Index 0 is the first 44.1 kHz sample of the device epoch. Inactive, paused
 * and completed playback emits actual DAC silence; failed reads still fail.
 * The existing PCM sink continues receiving only successfully decoded audio. */
typedef void (*chd_cd_clocked_pcm_sink_t)(void *opaque,int16_t left,int16_t right,
                                         uint64_t sample_index,int playing);
int chd_cd_set_clocked_pcm_sink(ChdCd *cd,chd_cd_clocked_pcm_sink_t sink,void *opaque);
uint32_t chd_cd_last_lba(const ChdCd *cd);
uint32_t chd_cd_track_count(const ChdCd *cd);
typedef struct chd_cd_track_info {
  uint32_t type,frames,pregap,pregap_data_bytes;
} chd_cd_track_info_t;
/* Exact parsed CHD metadata, for MMC raw-type/TOC validation. */
int chd_cd_get_track_info(const ChdCd *cd,unsigned track,chd_cd_track_info_t *out);
const char *chd_cd_last_error(const ChdCd *cd);
#ifdef __cplusplus
}
#endif
#endif
