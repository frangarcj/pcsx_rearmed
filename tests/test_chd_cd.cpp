#include "chd_cd.h"
#include "ncr53cf96.h"
#include "nscsi_bus.h"
#include "nscsi_cd.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Audio { std::vector<int16_t> l, r; };
static void pcm(void *p, int16_t l, int16_t r) {
  auto *a = static_cast<Audio *>(p); a->l.push_back(l); a->r.push_back(r);
}
static std::vector<uint8_t> expected_sector(unsigned lba) {
  std::vector<uint8_t> out(2048);
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = uint8_t((lba * 17 + i * 3 + 9) & 255);
  return out;
}
static void test_media_and_cdda(const char *path) {
  Audio audio;
  chd_cd_config_t cfg{pcm, &audio};
  char error[256]{};
  std::string missing = std::string(path) + ".missing";
  ChdCd *absent = chd_cd_open(missing.c_str(), nullptr, error, sizeof(error));
  assert(!absent && error[0]);
  error[0] = 0;
  ChdCd *cd = chd_cd_open(path, &cfg, error, sizeof(error));
  if (!cd) std::fprintf(stderr, "CHD open: %s\n", error);
  assert(cd && chd_cd_track_count(cd) == 3 && chd_cd_last_lba(cd) == 16);
  chd_cd_track_info_t track{};
  assert(chd_cd_get_track_info(cd,0,&track) && track.type==0 && !track.pregap);
  assert(chd_cd_get_track_info(cd,1,&track) && track.type==7 && track.pregap==2);
  assert(!chd_cd_get_track_info(cd,3,&track) && !chd_cd_get_track_info(cd,0xaa,&track));
  assert(!chd_cd_get_track_info(nullptr,0,&track) && !chd_cd_get_track_info(cd,0,nullptr));
  nscsi_cd_media_ops_t m{};
  assert(chd_cd_get_media_ops(cd, &m));
  assert(m.present(m.opaque) && m.last_track(m.opaque) == 3);
  assert(m.track_start(m.opaque, 0) == 0);
  assert(m.track_start(m.opaque, 1) == 9); // preceding postgap and stored pregap
  assert(m.track_start(m.opaque, 2) == 13);
  assert(m.track_start(m.opaque, 0xaa) == 16);
  assert(m.track_for_lba(m.opaque, 8) == 0);
  assert(m.track_for_lba(m.opaque, 9) == 1);
  assert(m.track_for_lba(m.opaque, 13) == 2);
  assert(m.track_adr_control(m.opaque, 0) == 0x14);
  assert(m.track_adr_control(m.opaque, 1) == 0x10);
  assert(!m.track_is_audio(m.opaque, 0) && m.track_is_audio(m.opaque, 1));
  std::array<uint8_t, 2048> sector{};
  assert(m.read_mode1_sector(m.opaque, 0, sector.data()));
  assert(std::vector<uint8_t>(sector.begin(), sector.end()) == expected_sector(0));
  assert(m.read_mode1_sector(m.opaque, 4, sector.data()));
  assert(std::vector<uint8_t>(sector.begin(), sector.end()) == expected_sector(4));
  assert(m.read_mode1_sector(m.opaque, 7, sector.data()));
  for (uint8_t b : sector) assert(b == 0); // source-defined track padding
  assert(!m.read_mode1_sector(m.opaque, 9, sector.data()));
  assert(m.read_mode1_sector(m.opaque, 13, sector.data()));
  for (size_t i = 0; i < sector.size(); ++i)
    assert(sector[i] == uint8_t((i * 5 + 7) & 255)); // raw MODE1 skips its 16-byte header

  m.cdda_set_gain(m.opaque, 0, 0.5f);
  m.cdda_set_gain(m.opaque, 1, 0.25f);
  m.cdda_start(m.opaque, 9, 4);
  nscsi_cd_audio_status_t status{};
  m.cdda_status(m.opaque, &status);
  assert(status.active && !status.paused && !status.ended && status.lba == 9);
  // Fractional budgets accumulate exactly to 588 samples at 44.1 kHz.
  assert(chd_cd_advance_to(cd, 6666666));
  assert(audio.l.size() == 293);
  assert(chd_cd_advance_to(cd, 13333333));
  assert(audio.l.size() == 587);
  assert(chd_cd_advance_to(cd, 13333334));
  assert(audio.l.size() == 588 && audio.l[0] == 6921 && audio.r[0] == -192);
  assert(audio.l[587] == 6921 && audio.r[587] == -192);
  m.cdda_pause(m.opaque, 1);
  assert(chd_cd_advance_to(cd, 40000000));
  assert(audio.l.size() == 588);
  m.cdda_status(m.opaque, &status);
  assert(status.active && status.paused && status.lba == 10);
  m.cdda_pause(m.opaque, 0);
  assert(chd_cd_advance_to(cd, 53333334));
  assert(audio.l.size() == 1176 && audio.l[588] == 7049 && audio.r[588] == -256);
  // Exercise split and one-shot budgets against identical actual PCM frames.
  Audio split;
  chd_cd_config_t split_cfg{pcm, &split};
  ChdCd *cd2 = chd_cd_open(path, &split_cfg, error, sizeof(error));
  assert(cd2);
  nscsi_cd_media_ops_t m2{}; assert(chd_cd_get_media_ops(cd2, &m2));
  m2.cdda_set_gain(m2.opaque, 0, 0.5f);
  m2.cdda_set_gain(m2.opaque, 1, 0.25f);
  m2.cdda_start(m2.opaque, 9, 1);
  assert(chd_cd_advance_to(cd2, 5000000));
  assert(chd_cd_advance_to(cd2, 10000000));
  assert(chd_cd_advance_to(cd2, 13333334));
  assert(split.l.size() == 588 &&
         std::equal(split.l.begin(), split.l.end(), audio.l.begin()) &&
         std::equal(split.r.begin(), split.r.end(), audio.r.begin()));
  m.cdda_status(m.opaque, &status);
  assert(status.active && !status.ended && status.lba == 11);
  assert(chd_cd_advance_to(cd, 120000000));
  m.cdda_status(m.opaque, &status);
  assert(!status.active && status.ended && status.lba == 13);
  assert(!chd_cd_advance_to(cd, 119999999));
  assert(std::strstr(chd_cd_last_error(cd), "backwards"));
  chd_cd_close(cd2); chd_cd_close(cd);
}

struct NcrWire { NscsiBus *bus{}; Ncr53cf96 *ncr{}; uint32_t wait = 0; };
static void wire_ctrl(void *p, uint32_t v, uint32_t mask) {
  auto *w = static_cast<NcrWire *>(p); nscsi_bus_drive_ctrl(w->bus, 7, v, mask);
}
static void wire_data(void *p, uint32_t v) {
  auto *w = static_cast<NcrWire *>(p); nscsi_bus_drive_data(w->bus, 7, v);
}
static void wire_wait(void *p, uint32_t v, uint32_t mask) {
  auto *w = static_cast<NcrWire *>(p); w->wait = (w->wait & ~mask) | (v & mask);
  nscsi_bus_wait_ctrl(w->bus, 7, v, mask);
}
static void bus_changed(void *p, uint32_t c, uint32_t d) {
  auto *w = static_cast<NcrWire *>(p); ncr53cf96_bus_changed(w->ncr, c, d);
}
static void no_line(void *, int) {}
static void real_ncr_to_chd_read(const char *path) {
  NscsiBus *bus = nscsi_bus_create(); assert(bus);
  NcrWire wire{}; wire.bus = bus;
  nscsi_bus_ops_t busops{&wire, bus_changed}; assert(nscsi_bus_attach(bus, 7, &busops));
  ncr53cf96_bus_ops_t nops{}; nops.opaque=&wire; nops.drive_ctrl=wire_ctrl;
  nops.drive_data=wire_data; nops.wait_ctrl=wire_wait; nops.irq=no_line; nops.drq=no_line;
  wire.ncr=ncr53cf96_create(16000000,&nops); assert(wire.ncr);
  char error[256]{}; ChdCd *chd=chd_cd_open(path,nullptr,error,sizeof(error));
  if (!chd) std::fprintf(stderr,"NCR CHD open: %s\n",error);
  assert(chd); nscsi_cd_media_ops_t media{}; assert(chd_cd_get_media_ops(chd,&media));
  nscsi_cdrom_config_t cc{}; cc.bus=bus; cc.scsi_id=4; cc.media=media;
  NscsiCdrom *cd=nscsi_cdrom_create(&cc); assert(cd);
  uint8_t cdb[10]={0x28,0,0,0,0,0,0,0,1,0};
  for (uint8_t b:cdb) ncr53cf96_write(wire.ncr,2,b);
  ncr53cf96_write(wire.ncr,8,7); ncr53cf96_write(wire.ncr,4,4);
  ncr53cf96_write(wire.ncr,3,0x41);
  for (uint64_t cycle=1; cycle<=2700; ++cycle) {
    nscsi_cdrom_advance_to(cd,(cycle*125)/2); nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr,cycle); nscsi_bus_flush(bus);
  }
  (void)ncr53cf96_read(wire.ncr,5); ncr53cf96_write(wire.ncr,11,8);
  ncr53cf96_write(wire.ncr,0,0); ncr53cf96_write(wire.ncr,1,8);
  ncr53cf96_write(wire.ncr,14,0); ncr53cf96_write(wire.ncr,3,0x90);
  std::vector<uint8_t> actual;
  for (uint64_t cycle=2701; cycle<25000 && actual.size()<2048; ++cycle) {
    nscsi_cdrom_advance_to(cd,(cycle*125)/2); nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr,cycle); nscsi_bus_flush(bus);
    ncr53cf96_status_t s{}; ncr53cf96_get_status(wire.ncr,&s);
    while (s.fifo_count && actual.size()<2048) {
      actual.push_back(ncr53cf96_dma_read(wire.ncr)); ncr53cf96_get_status(wire.ncr,&s);
    }
  }
  assert(actual==expected_sector(0));
  ncr53cf96_status_t ns{}; ncr53cf96_get_status(wire.ncr,&ns);
  (void)ncr53cf96_read(wire.ncr,5);
  ncr53cf96_write(wire.ncr,3,0x11); // CI_COMPLETE consumes STATUS + MSG IN
  assert(nscsi_bus_flush(bus));
  for (uint64_t cycle=ns.cycles+1; cycle<ns.cycles+128; ++cycle) {
    nscsi_cdrom_advance_to(cd,(cycle*125)/2); nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr,cycle); nscsi_bus_flush(bus);
  }
  ncr53cf96_get_status(wire.ncr,&ns);
  assert(ns.fifo_count == 2);
  assert(ncr53cf96_read(wire.ncr,2)==NSCSI_STATUS_GOOD);
  assert(ncr53cf96_read(wire.ncr,2)==NSCSI_MESSAGE_COMMAND_COMPLETE);
  (void)ncr53cf96_read(wire.ncr,5);
  ncr53cf96_write(wire.ncr,3,0x12); // CI_MSG_ACCEPT releases final message
  assert(nscsi_bus_flush(bus));
  for (uint64_t cycle=ns.cycles+1; cycle<ns.cycles+128; ++cycle) {
    nscsi_cdrom_advance_to(cd,(cycle*125)/2); nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr,cycle); nscsi_bus_flush(bus);
  }
  ncr53cf96_get_status(wire.ncr,&ns);
  assert(ns.interrupt_status==0x20 && !ns.faults);
  assert(nscsi_bus_ctrl(bus)==0);
  nscsi_cdrom_status_t cs{}; nscsi_cdrom_get_status(cd,&cs);
  assert(!cs.faults && !cs.target.faults);
  nscsi_cdrom_destroy(cd); chd_cd_close(chd); ncr53cf96_destroy(wire.ncr); nscsi_bus_destroy(bus);
}

struct ReentrantAudio {
  ChdCd *cd = nullptr;
  nscsi_cd_media_ops_t media{};
  unsigned calls = 0;
};
static void reentrant_pcm(void *opaque, int16_t, int16_t) {
  auto &f = *static_cast<ReentrantAudio *>(opaque);
  if (f.calls++) return;
  nscsi_cd_audio_status_t before{}, after{};
  f.media.cdda_status(f.media.opaque, &before);
  assert(!chd_cd_advance_to(f.cd, 999999));
  chd_cd_close(f.cd); // Rejected: outer advance still owns the backend.
  f.media.cdda_start(f.media.opaque, 0, 9);
  f.media.cdda_pause(f.media.opaque, 1);
  f.media.cdda_set_gain(f.media.opaque, 0, 0);
  std::array<uint8_t, 2048> sector{}; sector.fill(0x7a);
  assert(!f.media.read_mode1_sector(f.media.opaque, 0, sector.data()));
  for (auto byte : sector) assert(byte == 0x7a);
  f.media.cdda_status(f.media.opaque, &after);
  assert(after.active == before.active && after.paused == before.paused &&
         after.lba == before.lba);
}
static void test_pcm_callback_mutations_are_rejected(const char *path) {
  ReentrantAudio f;
  chd_cd_config_t config{reentrant_pcm, &f};
  char error[256]{};
  f.cd = chd_cd_open(path, &config, error, sizeof(error)); assert(f.cd);
  assert(chd_cd_get_media_ops(f.cd, &f.media));
  f.media.cdda_start(f.media.opaque, 9, 1);
  assert(!chd_cd_advance_to(f.cd, 22676));
  assert(f.calls == 1 && std::strstr(chd_cd_last_error(f.cd), "reentrant"));
  assert(chd_cd_advance_to(f.cd, 13333334));
  assert(f.calls == 588);
  chd_cd_close(f.cd);
}

static void test_unsupported_track_metadata_is_rejected(const char *path) {
  char error[256]{};
  ChdCd *cd=chd_cd_open(path,nullptr,error,sizeof(error));
  assert(!cd && std::strstr(error,"track type"));
}
static void test_invalid_track_geometry_is_rejected(const char *path) {
  char error[256]{};
  ChdCd *cd=chd_cd_open(path,nullptr,error,sizeof(error));
  assert(!cd && std::strstr(error,"geometry"));
}

static void test_mode2_raw(const char *path) {
  char error[256]{};
  ChdCd *cd = chd_cd_open(path, nullptr, error, sizeof(error));
  assert(cd && chd_cd_last_lba(cd) == 2);
  nscsi_cd_media_ops_t media{}; assert(chd_cd_get_media_ops(cd, &media));
  std::array<uint8_t, 2352> raw{};
  std::array<uint8_t, 2048> data{};
  assert(chd_cd_read_raw_sector(cd, 0, raw.data()) && raw[15] == 2 && raw[18] == 8);
  assert(media.read_mode1_sector(media.opaque, 0, data.data()));
  for (unsigned i = 0; i < data.size(); i++) assert(data[i] == uint8_t(i * 7));
  assert(chd_cd_read_raw_sector(cd, 1, raw.data()) && raw[18] == 0x20);
  assert(!media.read_mode1_sector(media.opaque, 1, data.data()));
  assert(!chd_cd_read_raw_sector(cd, 2, raw.data()));
  assert(!chd_cd_verify_sha1(cd, "1111111111111111111111111111111111111111"));
  chd_cd_close(cd);
}
void test_cdxa_mode2(const char *path);
int main(int argc, char **argv) {
  assert(argc == 6);
  test_media_and_cdda(argv[1]);
  real_ncr_to_chd_read(argv[1]);
  test_pcm_callback_mutations_are_rejected(argv[1]);
  test_unsupported_track_metadata_is_rejected(argv[2]);
  test_invalid_track_geometry_is_rejected(argv[3]);
  char error[256]{};
  assert(!chd_cd_open(argv[4], nullptr, error, sizeof(error)));
  assert(std::strstr(error, "malformed"));
  test_mode2_raw(argv[5]);
  test_cdxa_mode2(argv[5]);
  std::puts("CHD CD backend fixture: PASS");
}
