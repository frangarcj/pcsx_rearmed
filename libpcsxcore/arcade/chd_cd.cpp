/* MAME-derived CD CHD layout and Red Book behavior. See chd_cd_LICENSE.txt. */
#include "chd_cd.h"

#include "libchdr/chd.h"
#include "libchdr/cdrom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kFrameBytes = 2448;
constexpr uint32_t kSectorBytes = 2352;
constexpr uint32_t kMode1Bytes = 2048;
constexpr uint32_t kTrackPadding = 4;
constexpr uint32_t kAudioFramesPerSector = 588;
constexpr uint64_t kNsPerSecond = 1000000000ULL;
constexpr uint32_t kMaxTracks = 99;

struct Track {
  uint32_t type = 0;
  uint32_t data_bytes = 0;
  uint32_t pregap = 0;
  uint32_t postgap = 0;
  uint32_t frames = 0;
  uint32_t extra_frames = 0;
  uint32_t pg_data_bytes = 0;
  uint32_t logical_start = 0;
  uint32_t physical_start = 0;
  uint32_t chd_start = 0;
};

static bool fail_text(char *out, size_t cap, const char *what) {
  if (out && cap) std::snprintf(out, cap, "%s", what);
  return false;
}

static bool parse_track(chd_file *file, unsigned index, Track &track,
                        char *error, size_t error_cap) {
  std::array<char, 256> text{};
  uint32_t len = 0;
  uint32_t tag = 0;
  uint8_t flags = 0;
  chd_error err = chd_get_metadata(file, CDROM_TRACK_METADATA2_TAG, index,
                                   text.data(), text.size() - 1, &len, &tag,
                                   &flags);
  const bool modern = err != CHDERR_METADATA_NOT_FOUND;
  if (!modern)
    err = chd_get_metadata(file, CDROM_TRACK_METADATA_TAG, index,
                           text.data(), text.size() - 1, &len, &tag, &flags);
  if (err != CHDERR_NONE || len >= text.size())
    return fail_text(error, error_cap, "unsupported or malformed CHD track metadata");
  uint32_t number = 0, frames = 0, pregap = 0, postgap = 0;
  char type[16]{}, subtype[16]{}, pgtype[16]{}, pgsub[16]{};
  const char *cursor = text.data();
  auto key = [&](const char *name) {
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    const size_t n = std::strlen(name);
    if (std::strncmp(cursor, name, n)) return false;
    cursor += n;
    return true;
  };
  auto number_field = [&](const char *name, uint32_t &value) {
    if (!key(name)) return false;
    const char *begin = cursor;
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    if (cursor == begin || (*cursor && !std::isspace(static_cast<unsigned char>(*cursor))))
      return false;
    const auto result = std::from_chars(begin, cursor, value);
    return result.ec == std::errc{} && result.ptr == cursor;
  };
  auto word_field = [&](const char *name, char (&value)[16]) {
    if (!key(name)) return false;
    const char *begin = cursor;
    while (*cursor && !std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    const size_t n = size_t(cursor - begin);
    if (!n || n >= sizeof(value)) return false;
    std::memcpy(value, begin, n);
    return true;
  };
  bool valid = number_field("TRACK:", number) && word_field("TYPE:", type) &&
               word_field("SUBTYPE:", subtype) && number_field("FRAMES:", frames);
  if (modern)
    valid = valid && number_field("PREGAP:", pregap) && word_field("PGTYPE:", pgtype) &&
            word_field("PGSUB:", pgsub) && number_field("POSTGAP:", postgap);
  while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (!valid || *cursor || (modern && std::strcmp(pgsub, "NONE") && std::strcmp(pgsub, "RW") && std::strcmp(pgsub, "RW_RAW")))
    return fail_text(error, error_cap, "unsupported or malformed CHD track metadata");
  if (number != index + 1 || frames == 0 ||
      (std::strcmp(subtype, "NONE") && std::strcmp(subtype, "RW_RAW") && std::strcmp(subtype, "RW")))
    return fail_text(error, error_cap, "unsupported CHD track numbering, subtype, or geometry");
  if (!std::strcmp(type, "MODE1") || !std::strcmp(type, "MODE1/2048")) {
    track.type = CD_TRACK_MODE1;
    track.data_bytes = kMode1Bytes;
  } else if (!std::strcmp(type, "MODE1_RAW") || !std::strcmp(type, "MODE1/2352")) {
    track.type = CD_TRACK_MODE1_RAW;
    track.data_bytes = kSectorBytes;
  } else if (!std::strcmp(type, "AUDIO")) {
    track.type = CD_TRACK_AUDIO;
    track.data_bytes = kSectorBytes;
  } else if (!std::strcmp(type, "MODE2_RAW")) {
    track.type = CD_TRACK_MODE2_RAW;
    track.data_bytes = kSectorBytes;
  } else {
    return fail_text(error, error_cap, "CHD track type is not supported by this backend");
  }
  if (pgtype[0] && pgtype[0] != 'V' &&
      std::strcmp(pgtype, "MODE1") && std::strcmp(pgtype, "MODE1/2048") &&
      std::strcmp(pgtype, "AUDIO") && std::strcmp(pgtype, "MODE2_RAW"))
    return fail_text(error, error_cap, "unsupported pregap metadata encoding");
  if (pgtype[0] == 'V') {
    if (!std::strcmp(pgtype + 1, "MODE1") || !std::strcmp(pgtype + 1, "MODE1/2048"))
      track.pg_data_bytes = kMode1Bytes;
    else if (!std::strcmp(pgtype + 1, "MODE1_RAW") || !std::strcmp(pgtype + 1, "MODE1/2352") ||
             !std::strcmp(pgtype + 1, "AUDIO") || !std::strcmp(pgtype + 1, "MODE2_RAW"))
      track.pg_data_bytes = kSectorBytes;
    else
      return fail_text(error, error_cap, "unsupported stored pregap type");
  }
  if (track.pg_data_bytes && pregap > frames)
    return fail_text(error, error_cap, "stored pregap exceeds track frames");
  track.frames = uint32_t(frames);
  track.pregap = uint32_t(pregap);
  track.postgap = uint32_t(postgap);
  track.extra_frames = uint32_t((uint64_t(track.frames) + kTrackPadding - 1) /
                           kTrackPadding * kTrackPadding - track.frames);
  return true;
}

static bool has_track_metadata(chd_file *file, unsigned index) {
  std::array<char, 256> scratch{};
  uint32_t len = 0, tag = 0;
  uint8_t flags = 0;
  chd_error a = chd_get_metadata(file, CDROM_TRACK_METADATA2_TAG, index,
                                 scratch.data(), scratch.size(), &len, &tag,
                                 &flags);
  if (a == CHDERR_NONE) return true;
  chd_error b = chd_get_metadata(file, CDROM_TRACK_METADATA_TAG, index,
                                 scratch.data(), scratch.size(), &len, &tag,
                                 &flags);
  return b != CHDERR_METADATA_NOT_FOUND;
}
} // namespace

struct ChdCd {
  chd_file *file = nullptr;
  uint32_t hunk_bytes = 0;
  uint32_t unit_bytes = 0;
  uint64_t logical_bytes = 0;
  std::vector<uint8_t> hunk;
  uint32_t cached_hunk = UINT32_MAX;
  std::vector<Track> tracks;
  uint32_t leadout = 0;
  uint64_t sequence = 1;
  uint64_t last_ns = 0;
  uint64_t sample_remainder = 0;
  chd_cd_pcm_sink_t pcm_sink = nullptr;
  void *pcm_opaque = nullptr;
  nscsi_cd_audio_status_t audio{0, 0, 0, 0};
  uint32_t audio_end = 0;
  std::array<float, 2> gain{1.0f, 1.0f};
  std::array<uint8_t, kSectorBytes> audio_sector{};
  uint32_t audio_sector_offset = kAudioFramesPerSector;
  std::array<char, 256> error{};
  bool in_callback = false;
  ~ChdCd() { if (file) chd_close(file); }
};

namespace {
static void set_error(ChdCd *cd, const char *message) {
  if (cd) std::snprintf(cd->error.data(), cd->error.size(), "%s", message);
}

static bool reject_callback_mutation(ChdCd *cd) {
  if (!cd || !cd->in_callback) return false;
  set_error(cd, "reentrant CHD CD mutation from PCM callback");
  return true;
}

static bool read_bytes(ChdCd *cd, uint64_t offset, void *dest, size_t bytes) {
  if (!cd || offset > cd->logical_bytes || bytes > cd->logical_bytes - offset) {
    set_error(cd, "CHD frame read is outside media bounds");
    return false;
  }
  uint8_t *out = static_cast<uint8_t *>(dest);
  while (bytes) {
    uint32_t hunk_index = uint32_t(offset / cd->hunk_bytes);
    uint32_t within = uint32_t(offset % cd->hunk_bytes);
    if (hunk_index != cd->cached_hunk) {
      cd->cached_hunk = UINT32_MAX; // A failed decoder may overwrite cached bytes.
      chd_error err = chd_read(cd->file, hunk_index, cd->hunk.data());
      if (err != CHDERR_NONE) {
        set_error(cd, chd_error_string(err));
        return false;
      }
      cd->cached_hunk = hunk_index;
    }
    size_t chunk = std::min<size_t>(bytes, cd->hunk_bytes - within);
    std::memcpy(out, cd->hunk.data() + within, chunk);
    out += chunk;
    offset += chunk;
    bytes -= chunk;
  }
  return true;
}

static int track_index_for_lba(const ChdCd *cd, uint32_t lba) {
  for (size_t i = 0; i < cd->tracks.size(); ++i) {
    const uint32_t next_start = i + 1 < cd->tracks.size()
                                    ? cd->tracks[i + 1].logical_start
                                    : cd->leadout;
    if (lba < next_start)
      return int(i);
  }
  return -1;
}

static bool logical_frame(ChdCd *cd, uint32_t lba, uint32_t &track_index,
                          uint64_t &chd_frame, bool &generated_pregap) {
  for (size_t i = 0; i < cd->tracks.size(); ++i) {
    const Track &t = cd->tracks[i];
    const uint32_t next_start = (i + 1 < cd->tracks.size())
                                    ? cd->tracks[i + 1].logical_start
                                    : cd->leadout;
    if (lba < next_start) {
      track_index = uint32_t(i);
      generated_pregap = t.pg_data_bytes == 0 && lba < t.logical_start;
      if (generated_pregap) {
        chd_frame = 0;
        return true;
      }
      if (lba < t.logical_start) {
        if (!t.pg_data_bytes || t.pregap > t.logical_start ||
            lba < t.logical_start - t.pregap) {
          set_error(cd, "LBA falls outside stored pregap data");
          return false;
        }
        chd_frame = uint64_t(t.chd_start) +
                    (lba - (t.logical_start - t.pregap));
        return true;
      }
      uint64_t physical = uint64_t(t.physical_start) + (lba - t.logical_start);
      chd_frame = t.chd_start + (physical - t.physical_start) +
                  (t.pg_data_bytes ? t.pregap : 0);
      return true;
    }
  }
  set_error(cd, "LBA is outside the CHD logical TOC");
  return false;
}

static int op_present(void *opaque) { return opaque != nullptr; }
static uint64_t op_sequence(void *opaque) { return static_cast<ChdCd *>(opaque)->sequence; }
static uint32_t op_track_start(void *opaque, unsigned track) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (track == 0xaa) return cd->leadout;
  return track < cd->tracks.size() ? cd->tracks[track].logical_start : cd->leadout;
}
static unsigned op_last_track(void *opaque) {
  return unsigned(static_cast<ChdCd *>(opaque)->tracks.size());
}
static unsigned op_track_for_lba(void *opaque, uint32_t lba) {
  int i = track_index_for_lba(static_cast<ChdCd *>(opaque), lba);
  return i < 0 ? 0 : unsigned(i);
}
static uint8_t op_adr_control(void *opaque, unsigned track) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (track == 0xaa) track = unsigned(cd->tracks.size() - 1);
  if (track >= cd->tracks.size()) return 0x14;
  return cd->tracks[track].type == CD_TRACK_AUDIO ? 0x10 : 0x14;
}
static int op_is_audio(void *opaque, unsigned i) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  return i < cd->tracks.size() && cd->tracks[i].type == CD_TRACK_AUDIO;
}
static int op_read_mode1(void *opaque, uint32_t lba, uint8_t out[kMode1Bytes]) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (reject_callback_mutation(cd) || !out) return 0;
  cd->error[0] = 0;
  uint32_t ti = 0;
  uint64_t frame = 0;
  bool pregap = false;
  if (!logical_frame(cd, lba, ti, frame, pregap)) return 0;
  if (cd->tracks[ti].type == CD_TRACK_AUDIO) {
    set_error(cd, "MODE1 sector request targets an audio track");
    return 0;
  }
  if (pregap && cd->tracks[ti].pg_data_bytes == 0) {
    std::memset(out, 0, kMode1Bytes);
    return 1;
  }
  uint8_t staged[kMode1Bytes];
  uint32_t offset = cd->tracks[ti].type == CD_TRACK_MODE1_RAW ? 16 : 0;
  if (cd->tracks[ti].type == CD_TRACK_MODE2_RAW) {
    uint8_t submode;
    if (!read_bytes(cd, frame * cd->unit_bytes + 18, &submode, 1) || (submode & 0x20)) {
      set_error(cd, "2048-byte request targets a MODE2 Form 2 sector"); return 0;
    }
    offset = 24;
  }
  uint64_t base = frame * cd->unit_bytes + offset;
  if (!read_bytes(cd, base, staged, kMode1Bytes)) return 0;
  std::memcpy(out, staged, kMode1Bytes);
  return 1;
}
static void op_audio_start(void *opaque, uint32_t lba, uint32_t blocks) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (reject_callback_mutation(cd)) return;
  if (blocks > UINT32_MAX - lba) {
    set_error(cd, "CDDA range overflows LBA space");
    cd->audio = {0, 0, 0, lba};
    return;
  }
  cd->audio = {1, 0, 0, lba};
  cd->audio_end = lba + blocks;
  cd->audio_sector_offset = kAudioFramesPerSector;
  cd->sample_remainder = 0;
}
static void op_audio_pause(void *opaque, int pause) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (!reject_callback_mutation(cd)) cd->audio.paused = !!pause;
}
static void op_audio_status(void *opaque, nscsi_cd_audio_status_t *out) {
  *out = static_cast<ChdCd *>(opaque)->audio;
}
static void op_audio_gain(void *opaque, unsigned channel, float gain) {
  ChdCd *cd = static_cast<ChdCd *>(opaque);
  if (reject_callback_mutation(cd)) return;
  if (channel < cd->gain.size() && std::isfinite(gain)) cd->gain[channel] = gain;
}
static int16_t scaled_sample(int16_t sample, float gain) {
  float value = float(sample) * gain;
  return int16_t(std::max(-32768.0f, std::min(32767.0f, std::round(value))));
}
static int16_t signed_be16(const uint8_t *p) {
  int32_t value = (int32_t(p[0]) << 8) | p[1];
  if (value & 0x8000) value -= 0x10000;
  return int16_t(value);
}
static bool load_audio_sector(ChdCd *cd) {
  uint32_t ti = 0;
  uint64_t frame = 0;
  bool pregap = false;
  if (cd->audio.lba >= cd->audio_end) {
    cd->audio.active = 0;
    cd->audio.ended = 1;
    return false;
  }
  if (!logical_frame(cd, cd->audio.lba, ti, frame, pregap) ||
      cd->tracks[ti].type != CD_TRACK_AUDIO) {
    set_error(cd, "CDDA range addresses unsupported or non-audio media");
    cd->audio.active = 0;
    return false;
  }
  if (pregap) {
    cd->audio_sector.fill(0); // Generated pregap is source-defined silence.
    cd->audio_sector_offset = 0;
    return true;
  }
  if (!read_bytes(cd, frame * cd->unit_bytes, cd->audio_sector.data(), kSectorBytes)) {
    cd->audio.active = 0;
    return false;
  }
  cd->audio_sector_offset = 0;
  return true;
}
} // namespace

extern "C" ChdCd *chd_cd_open(const char *path, const chd_cd_config_t *config,
                                char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  if (!path || !*path) { fail_text(error, error_capacity, "CHD path is empty"); return nullptr; }
  try {
  std::unique_ptr<ChdCd> cd(new ChdCd);
  chd_error err = chd_open(path, CHD_OPEN_READ, nullptr, &cd->file);
  if (err != CHDERR_NONE) { fail_text(error, error_capacity, chd_error_string(err)); return nullptr; }
  const chd_header *header = chd_get_header(cd->file);
  if (!header || header->unitbytes != kFrameBytes || !header->hunkbytes ||
      header->logicalbytes % kFrameBytes) {
    fail_text(error, error_capacity, "CHD is not a frame-aligned 2448-byte CD image");
    return nullptr;
  }
  if (header->hunkbytes > (64U << 20) || !header->logicalbytes ||
      header->logicalbytes / header->hunkbytes +
        !!(header->logicalbytes % header->hunkbytes) != header->totalhunks) {
    fail_text(error, error_capacity, "unsupported CHD hunk geometry"); return nullptr;
  }
  cd->hunk_bytes = header->hunkbytes;
  cd->unit_bytes = header->unitbytes;
  cd->logical_bytes = header->logicalbytes;
  cd->hunk.resize(cd->hunk_bytes);
  for (unsigned i = 0; i < kMaxTracks; ++i) {
    if (!has_track_metadata(cd->file, i)) break;
    Track t;
    if (!parse_track(cd->file, i, t, error, error_capacity)) {
      return nullptr;
    }
    cd->tracks.push_back(t);
  }
  if (cd->tracks.empty()) {
    fail_text(error, error_capacity, "CHD has no supported CHTR/CHT2 track metadata");
    return nullptr;
  }
  uint64_t physical = 0, chd_frame = 0, logical = 0;
  for (Track &t : cd->tracks) {
    uint64_t prior_logical = logical;
    t.physical_start = uint32_t(physical);
    t.chd_start = uint32_t(chd_frame);
    t.logical_start = uint32_t(prior_logical + (t.pg_data_bytes ? t.pregap : 0) +
                               (t.pg_data_bytes ? 0 : t.pregap));
    logical = prior_logical + (t.pg_data_bytes ? 0 : t.pregap) +
              t.frames + t.postgap;
    physical += t.frames;
    chd_frame += t.frames + t.extra_frames;
    if (logical > UINT32_MAX || physical > UINT32_MAX || chd_frame > UINT32_MAX) {
      fail_text(error, error_capacity, "CHD TOC exceeds supported LBA range"); return nullptr;
    }
  }
  if (has_track_metadata(cd->file, kMaxTracks)) {
    fail_text(error, error_capacity, "CHD has too many tracks"); return nullptr;
  }
  cd->leadout = uint32_t(logical);
  if (chd_frame * kFrameBytes != cd->logical_bytes) {
    fail_text(error, error_capacity, "CHD track lengths and 4-frame padding do not match image geometry");
    return nullptr;
  }
  if (config) { cd->pcm_sink = config->pcm_sink; cd->pcm_opaque = config->pcm_opaque; }
  return cd.release();
  } catch (...) {
    fail_text(error, error_capacity, "cannot allocate CHD CD state"); return nullptr;
  }
}

extern "C" void chd_cd_close(ChdCd *cd) {
  if (!cd) return;
  if (reject_callback_mutation(cd)) return;
  delete cd;
}
extern "C" int chd_cd_get_media_ops(ChdCd *cd, nscsi_cd_media_ops_t *ops) {
  if (!cd || !ops) return 0;
  *ops = {cd, op_present, op_sequence, op_read_mode1, op_track_start,
          op_last_track, op_track_for_lba, op_adr_control, op_is_audio,
          op_audio_start, op_audio_pause, op_audio_status, op_audio_gain};
  return 1;
}
extern "C" int chd_cd_read_raw_sector(ChdCd *cd, uint32_t lba, uint8_t out[2352]) {
  if (!cd || !out || reject_callback_mutation(cd)) return 0;
  uint32_t track = 0;
  uint64_t frame = 0;
  bool pregap = false;
  if (!logical_frame(cd, lba, track, frame, pregap)) return 0;
  if (cd->tracks[track].data_bytes != kSectorBytes || (pregap && !cd->tracks[track].pg_data_bytes)) {
    set_error(cd, "raw sector request requires stored 2352-byte sector data"); return 0;
  }
  return read_bytes(cd, frame * cd->unit_bytes, out, kSectorBytes) ? 1 : 0;
}
extern "C" int chd_cd_verify_sha1(ChdCd *cd, const char *hex) {
  if (!cd || !hex || std::strlen(hex) != 40) return 0;
  const chd_header *header = chd_get_header(cd->file);
  char actual[41];
  for (unsigned i = 0; i < 20; i++) std::snprintf(actual + i * 2, 3, "%02x", header->sha1[i]);
  return std::strcmp(actual, hex) == 0;
}
extern "C" int chd_cd_advance_to(ChdCd *cd, uint64_t now_ns) {
  if (reject_callback_mutation(cd)) return 0;
  if (!cd || now_ns < cd->last_ns) { set_error(cd, "CHD CD clock cannot move backwards"); return 0; }
  cd->error[0] = 0;
  uint64_t delta = now_ns - cd->last_ns;
  cd->last_ns = now_ns;
  if (!cd->audio.active || cd->audio.paused) return 1;
  __uint128_t total = __uint128_t(delta) * 44100 + cd->sample_remainder;
  uint64_t samples = uint64_t(total / kNsPerSecond);
  cd->sample_remainder = uint64_t(total % kNsPerSecond);
  while (samples-- && cd->audio.active && !cd->audio.paused) {
    if (cd->audio_sector_offset == kAudioFramesPerSector && !load_audio_sector(cd)) break;
    const uint8_t *sample = cd->audio_sector.data() + cd->audio_sector_offset * 4;
    int16_t left = signed_be16(sample);
    int16_t right = signed_be16(sample + 2);
    if (cd->pcm_sink) {
      cd->in_callback = true;
      cd->pcm_sink(cd->pcm_opaque, scaled_sample(left, cd->gain[0]),
                   scaled_sample(right, cd->gain[1]));
      cd->in_callback = false;
    }
    ++cd->audio_sector_offset;
    if (cd->audio_sector_offset == kAudioFramesPerSector) {
      ++cd->audio.lba;
      if (cd->audio.lba >= cd->audio_end) { cd->audio.active = 0; cd->audio.ended = 1; }
    }
  }
  return !cd->error[0] ? 1 : 0;
}
extern "C" uint32_t chd_cd_last_lba(const ChdCd *cd) { return cd ? cd->leadout : 0; }
extern "C" uint32_t chd_cd_track_count(const ChdCd *cd) { return cd ? uint32_t(cd->tracks.size()) : 0; }
extern "C" const char *chd_cd_last_error(const ChdCd *cd) { return cd ? cd->error.data() : "null CHD CD"; }
