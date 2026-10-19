/* Native CHD hard-disk backing, SPDX-License-Identifier: GPL-2.0-or-later.
 * Uses libchdr's pinned read-only decoder and a separate sector overlay.
 * Original host adaptation; no MAME hard-disk algorithms are copied here. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "chd_hd.h"
#include <libchdr/chd.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

struct arcade_chd_hd {
  chd_file *image = nullptr;
  arcade_chd_hd_info_t info{};
  std::vector<uint8_t> hunk, sector;
  uint32_t cached_hunk = UINT32_MAX;
  std::map<uint32_t, std::vector<uint8_t>> overlay;
  std::string overlay_path;
  std::array<char, 256> error{};
  bool dirty = false;
  dev_t source_dev = 0;
  ino_t source_ino = 0;
  ~arcade_chd_hd() { if (image) chd_close(image); }
};

namespace {
constexpr size_t HEADER_BYTES = 80;
constexpr uint8_t MAGIC[8] = {'P','C','S','X','H','D','O','1'};
// Bounded host cache, not a limit on the base image's size.
constexpr uint64_t MAX_OVERLAY_BYTES = 256ull * 1024 * 1024;
void put32(uint8_t *p, uint32_t n) {
  p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24;
}
uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t crc(const uint8_t *p, size_t bytes) {
  return uint32_t(crc32(0, p, static_cast<uInt>(bytes)));
}
std::array<uint8_t, HEADER_BYTES> header(const arcade_chd_hd &d, uint32_t count) {
  std::array<uint8_t, HEADER_BYTES> out{};
  std::copy(std::begin(MAGIC), std::end(MAGIC), out.begin());
  put32(out.data() + 8, d.info.cylinders);
  put32(out.data() + 12, d.info.heads);
  put32(out.data() + 16, d.info.sectors_per_track);
  put32(out.data() + 20, d.info.sector_bytes);
  put32(out.data() + 24, d.info.sector_count);
  put32(out.data() + 28, count);
  std::copy(std::begin(d.info.sha1), std::end(d.info.sha1), out.begin() + 32);
  put32(out.data() + 76, crc(out.data(), 76));
  return out;
}
void fail(arcade_chd_hd &d, const char *error) {
  std::snprintf(d.error.data(), d.error.size(), "%s", error);
}
// Avoid scanf's undefined overflow for hostile decimal metadata.
uint32_t number(const char *&p, const char *prefix) {
  size_t n = std::strlen(prefix);
  if (std::strncmp(p, prefix, n)) throw std::runtime_error("invalid GDDD geometry");
  p += n;
  uint64_t result = 0;
  if (*p < '0' || *p > '9') throw std::runtime_error("invalid GDDD geometry");
  while (*p >= '0' && *p <= '9') {
    result = result * 10 + (*p++ - '0');
    if (result > UINT32_MAX) throw std::runtime_error("GDDD geometry overflow");
  }
  if (!result) throw std::runtime_error("zero GDDD geometry");
  return static_cast<uint32_t>(result);
}
void geometry(arcade_chd_hd &d) {
  const chd_header *h = chd_get_header(d.image);
  if (!h || h->version < 3 || h->version > 5 || !h->logicalbytes || !h->totalhunks ||
      !h->hunkbytes || h->hunkbytes > 64u * 1024 * 1024)
    throw std::runtime_error("unsupported CHD header");
  char meta[256]{};
  uint32_t bytes = 0;
  chd_error e = chd_get_metadata(d.image, HARD_DISK_METADATA_TAG, 0, meta, sizeof(meta), &bytes, nullptr, nullptr);
  if (e != CHDERR_NONE || !bytes || bytes > sizeof(meta) || meta[bytes - 1])
    throw std::runtime_error("missing or invalid GDDD geometry");
  const char *p = meta;
  auto &i = d.info;
  i.cylinders = number(p, "CYLS:"); i.heads = number(p, ",HEADS:");
  i.sectors_per_track = number(p, ",SECS:"); i.sector_bytes = number(p, ",BPS:");
  if (*p || size_t(p - meta) + 1 != bytes || i.sector_bytes > 65536)
    throw std::runtime_error("invalid GDDD geometry suffix/sector size");
  uint64_t sectors = uint64_t(i.cylinders) * i.heads;
  if (sectors > UINT32_MAX || sectors * i.sectors_per_track > UINT32_MAX)
    throw std::runtime_error("disk exceeds 32-bit SCSI sector range");
  sectors *= i.sectors_per_track;
  if (sectors * i.sector_bytes != h->logicalbytes || h->unitbytes != i.sector_bytes ||
      (h->logicalbytes - 1) / h->hunkbytes + 1 != h->totalhunks)
    throw std::runtime_error("CHD geometry/header mismatch");
  e = chd_get_metadata(d.image, HARD_DISK_METADATA_TAG, 1, nullptr, 0, &bytes, nullptr, nullptr);
  if (e != CHDERR_METADATA_NOT_FOUND) throw std::runtime_error("duplicate or unreadable GDDD geometry");
  i.sector_count = static_cast<uint32_t>(sectors); i.hunk_bytes = h->hunkbytes;
  std::copy(std::begin(h->sha1), std::end(h->sha1), std::begin(i.sha1));
  e = chd_get_metadata(d.image, HARD_DISK_IDENT_METADATA_TAG, 0, i.inquiry, sizeof(i.inquiry), &bytes, nullptr, nullptr);
  if (e == CHDERR_NONE) i.inquiry_bytes = std::min<size_t>(bytes, sizeof(i.inquiry));
  else if (e != CHDERR_METADATA_NOT_FOUND) throw std::runtime_error("unreadable IDNT inquiry metadata");
  d.hunk.resize(h->hunkbytes); d.sector.resize(i.sector_bytes);
}
void load_overlay(arcade_chd_hd &d) {
  if (d.overlay_path.empty()) return;
  std::unique_ptr<FILE, int(*)(FILE*)> f(std::fopen(d.overlay_path.c_str(), "rb"), std::fclose);
  if (!f) {
    if (errno == ENOENT) return;
    throw std::runtime_error("cannot open disk overlay");
  }
  std::array<uint8_t, HEADER_BYTES> hdr{};
  if (std::fread(hdr.data(), 1, hdr.size(), f.get()) != hdr.size())
    throw std::runtime_error("truncated disk overlay header");
  uint32_t count = get32(hdr.data() + 28);
  if (count > d.info.sector_count || uint64_t(count) * d.info.sector_bytes > MAX_OVERLAY_BYTES ||
      hdr != header(d, count)) throw std::runtime_error("invalid or wrong-image disk overlay");
  uint32_t last = 0;
  for (uint32_t n = 0; n < count; ++n) {
    std::array<uint8_t, 8> rec{};
    std::vector<uint8_t> data(d.info.sector_bytes);
    if (std::fread(rec.data(), 1, rec.size(), f.get()) != rec.size() ||
        std::fread(data.data(), 1, data.size(), f.get()) != data.size())
      throw std::runtime_error("truncated disk overlay sector");
    uint32_t lba = get32(rec.data());
    // CRC covers both LBA and bytes, so corrupted addressing cannot silently
    // redirect a valid sector. Canonical strict ordering also rejects duplicates.
    uint32_t sum = uint32_t(crc32(crc(rec.data(), 4), data.data(), static_cast<uInt>(data.size())));
    if (lba >= d.info.sector_count || (n && lba <= last) || get32(rec.data() + 4) != sum)
      throw std::runtime_error("invalid disk overlay sector/CRC");
    d.overlay.emplace(lba, std::move(data)); last = lba;
  }
  if (std::fgetc(f.get()) != EOF || std::ferror(f.get()))
    throw std::runtime_error("trailing data or IO error in disk overlay");
}
} // namespace

extern "C" arcade_chd_hd_t *arcade_chd_hd_open(const char *path, const char *overlay,
                                               char *error, size_t error_bytes) {
  if (error && error_bytes) *error = 0;
  try {
    if (!path || !*path || (overlay && !*overlay)) throw std::runtime_error("invalid CHD/overlay path");
    auto d = std::make_unique<arcade_chd_hd>();
    chd_error e = chd_open(path, CHD_OPEN_READ, nullptr, &d->image);
    if (e != CHDERR_NONE) throw std::runtime_error(chd_error_string(e));
    struct stat source{};
    if (stat(path, &source)) throw std::runtime_error("cannot identify source CHD");
    d->source_dev = source.st_dev; d->source_ino = source.st_ino;
    if (overlay) d->overlay_path = overlay;
    geometry(*d);
    if (overlay && std::all_of(std::begin(d->info.sha1), std::end(d->info.sha1),
                              [](uint8_t n) { return n == 0; }))
      throw std::runtime_error("writable overlay requires a CHD SHA1 identity");
    load_overlay(*d);
    return d.release();
  } catch (const std::exception &e) {
    if (error && error_bytes) std::snprintf(error, error_bytes, "%s", e.what());
    return nullptr;
  }
}
extern "C" void arcade_chd_hd_close(arcade_chd_hd_t *d) { delete d; }
extern "C" void arcade_chd_hd_get_info(const arcade_chd_hd_t *d, arcade_chd_hd_info_t *info) {
  if (info) *info = d ? d->info : arcade_chd_hd_info_t{};
}
extern "C" arcade_chd_hd_metadata_status_t arcade_chd_hd_read_metadata(
    const arcade_chd_hd_t *d, arcade_chd_hd_metadata_kind_t kind,
    uint8_t *output, size_t capacity, size_t *metadata_bytes) {
  if (!d || !metadata_bytes || (!output && capacity) ||
      kind < ARCADE_CHD_HD_METADATA_GNET_CIS ||
      kind > ARCADE_CHD_HD_METADATA_ATA_IDENTIFY)
    return ARCADE_CHD_HD_METADATA_ARGUMENT;

  uint32_t tag = 0;
  switch (kind) {
    case ARCADE_CHD_HD_METADATA_GNET_CIS: tag = CHD_MAKE_TAG('C','I','S',' '); break;
    case ARCADE_CHD_HD_METADATA_GNET_KEY: tag = CHD_MAKE_TAG('K','E','Y',' '); break;
    case ARCADE_CHD_HD_METADATA_ATA_IDENTIFY: tag = CHD_MAKE_TAG('I','D','N','T'); break;
    default: return ARCADE_CHD_HD_METADATA_ARGUMENT;
  }

  uint32_t required = 0;
  chd_error error = chd_get_metadata(d->image, tag, 0, nullptr, 0,
                                     &required, nullptr, nullptr);
  if (error == CHDERR_METADATA_NOT_FOUND)
    return ARCADE_CHD_HD_METADATA_MISSING;
  if (error != CHDERR_NONE)
    return ARCADE_CHD_HD_METADATA_ERROR;
  if (capacity < required) {
    *metadata_bytes = required;
    return ARCADE_CHD_HD_METADATA_BUFFER_TOO_SMALL;
  }

  try {
    std::vector<uint8_t> data(required);
    uint32_t actual = 0;
    error = chd_get_metadata(d->image, tag, 0,
                             required ? data.data() : nullptr, required,
                             &actual, nullptr, nullptr);
    if (error == CHDERR_METADATA_NOT_FOUND)
      return ARCADE_CHD_HD_METADATA_MISSING;
    if (error != CHDERR_NONE || actual != required)
      return ARCADE_CHD_HD_METADATA_ERROR;
    if (required) std::memcpy(output, data.data(), required);
    *metadata_bytes = required;
    return ARCADE_CHD_HD_METADATA_OK;
  } catch (...) {
    return ARCADE_CHD_HD_METADATA_ERROR;
  }
}
extern "C" int arcade_chd_hd_read(void *opaque, uint32_t lba, uint8_t *data, size_t bytes) {
  auto *d = static_cast<arcade_chd_hd *>(opaque);
  if (!d) return -1;
  if (!data || bytes != d->info.sector_bytes || lba >= d->info.sector_count) {
    fail(*d, "invalid sector read"); return -1;
  }
  auto it = d->overlay.find(lba);
  if (it != d->overlay.end()) { std::memcpy(data, it->second.data(), bytes); return 0; }
  uint64_t offset = uint64_t(lba) * bytes;
  size_t done = 0;
  while (done < bytes) {
    uint32_t hunk = static_cast<uint32_t>(offset / d->info.hunk_bytes);
    size_t position = size_t(offset % d->info.hunk_bytes);
    if (d->cached_hunk != hunk) {
      // Failed decompression must invalidate the old cache before reuse.
      d->cached_hunk = UINT32_MAX;
      chd_error e = chd_read(d->image, hunk, d->hunk.data());
      if (e != CHDERR_NONE) { fail(*d, chd_error_string(e)); return -1; }
      d->cached_hunk = hunk;
    }
    size_t n = std::min(bytes - done, d->hunk.size() - position);
    std::memcpy(d->sector.data() + done, d->hunk.data() + position, n);
    done += n; offset += n;
  }
  std::memcpy(data, d->sector.data(), bytes); return 0;
}
extern "C" int arcade_chd_hd_write(void *opaque, uint32_t lba, const uint8_t *data, size_t bytes) {
  auto *d = static_cast<arcade_chd_hd *>(opaque);
  if (!d) return -1;
  if (d->overlay_path.empty() || !data || bytes != d->info.sector_bytes || lba >= d->info.sector_count) {
    fail(*d, "read-only disk or invalid sector write"); return -1;
  }
  try {
    auto it = d->overlay.find(lba);
    if (it == d->overlay.end() && uint64_t(d->overlay.size() + 1) * bytes > MAX_OVERLAY_BYTES) {
      fail(*d, "disk overlay cache limit exceeded"); return -1;
    }
    std::vector<uint8_t> copy(data, data + bytes);
    if (it == d->overlay.end()) d->overlay.emplace(lba, std::move(copy));
    else it->second.swap(copy);
    d->dirty = true; return 0;
  } catch (const std::exception &e) { fail(*d, e.what()); return -1; }
}
extern "C" int arcade_chd_hd_flush(arcade_chd_hd_t *d) {
  if (!d) return -1;
  if (!d->dirty || d->overlay_path.empty()) return 0;
  // Check same-file identity, including a hard link/symlink to the base CHD.
  // The atomic rename would replace its pathname, so such a destination is
  // always rejected even though the decoder opened the base read-only.
  struct stat destination{};
  if (!stat(d->overlay_path.c_str(), &destination) && destination.st_dev == d->source_dev &&
      destination.st_ino == d->source_ino) {
    fail(*d, "overlay destination is the source CHD"); return -1;
  }
  std::string temporary;
  std::vector<char> name;
  try {
    temporary = d->overlay_path + ".tmp.XXXXXX";
    name.assign(temporary.begin(), temporary.end()); name.push_back(0);
  }
  catch (const std::exception &e) { fail(*d, e.what()); return -1; }
  int fd = mkstemp(name.data());
  if (fd < 0) { fail(*d, "cannot create disk overlay temporary"); return -1; }
  FILE *f = fdopen(fd, "wb");
  if (!f) { close(fd); unlink(name.data()); fail(*d, "cannot open disk overlay temporary"); return -1; }
  auto hdr = header(*d, static_cast<uint32_t>(d->overlay.size()));
  bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
  for (const auto &item : d->overlay) {
    if (!ok) break;
    std::array<uint8_t, 8> rec{}; put32(rec.data(), item.first);
    put32(rec.data() + 4, uint32_t(crc32(crc(rec.data(), 4), item.second.data(), static_cast<uInt>(item.second.size()))));
    ok = std::fwrite(rec.data(), 1, rec.size(), f) == rec.size() &&
         std::fwrite(item.second.data(), 1, item.second.size(), f) == item.second.size();
  }
  if (ok) ok = !std::fflush(f) && !fsync(fd);
  if (std::fclose(f)) ok = false;
  if (ok) ok = !rename(name.data(), d->overlay_path.c_str());
  if (!ok) { unlink(name.data()); fail(*d, "cannot commit disk overlay"); return -1; }
  d->dirty = false; return 0;
}
extern "C" uint64_t arcade_chd_hd_modified_sectors(const arcade_chd_hd_t *d) {
  return d ? d->overlay.size() : 0;
}
extern "C" const char *arcade_chd_hd_last_error(const arcade_chd_hd_t *d) {
  return d ? d->error.data() : "no disk";
}
