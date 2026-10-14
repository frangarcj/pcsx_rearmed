/* SPDX-License-Identifier: BSD-3-Clause. Callback plumbing for pinned MMC. */
#ifndef PCSX_ATAPI_MAME_COMPAT_H
#define PCSX_ATAPI_MAME_COMPAT_H
#include "ata_mame/ata_mame_compat.h"
#include "atapi_cr589.h"
#include <array>
#include <string>
#include <iterator>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
template<class T> constexpr auto BIT(T value,unsigned start,unsigned count=1){return (value>>start)&((T(1)<<count)-1);}
inline uint16_t get_u16be(const uint8_t *p){return (p[0]<<8)|p[1];}
inline uint32_t get_u24be(const uint8_t *p){return (uint32_t(p[0])<<16)|(p[1]<<8)|p[2];}
inline uint32_t get_u32be(const uint8_t *p){return (uint32_t(p[0])<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
inline void put_u16be(uint8_t *p,uint16_t v){p[0]=v>>8;p[1]=v;}
inline void put_u24be(uint8_t *p,uint32_t v){p[0]=v>>16;p[1]=v>>8;p[2]=v;}
class cdrom_file {
public:
    enum {CD_TRACK_MODE1=0,CD_TRACK_MODE1_RAW,CD_TRACK_MODE2,CD_TRACK_MODE2_FORM1,
          CD_TRACK_MODE2_FORM2,CD_TRACK_MODE2_FORM_MIX,CD_TRACK_MODE2_RAW,CD_TRACK_AUDIO,CD_TRACK_RAW_DONTCARE};
    static int32_t msf_to_lba(int32_t v){return ((v>>16)&255)*75*60+((v>>8)&255)*75+(v&255);}
    struct toc {std::array<AtapiTrack,99> tracks;};
};
class cdrom_image_device {
public:
    explicit cdrom_image_device(const AtapiCr589Config &c):config(c){}
    bool exists(){return config.media.present(config.media.opaque)!=0;}
    const char *tag(){return "callback original CD";}
    bool is_dvd(){return false;}
    unsigned get_last_track(){return config.media.last_track(config.media.opaque);}
    unsigned get_last_session(){unsigned count=1;for(unsigned i=0;i<get_last_track();i++){AtapiTrack info{};if(config.track_info(config.media.opaque,i,&info))count=std::max(count,info.session+1);else faults|=1;}return count;}
    uint32_t get_track_start(unsigned t){return config.media.track_start(config.media.opaque,t);}
    unsigned get_track(uint32_t lba){return config.media.track_for_lba(config.media.opaque,lba);}
    uint8_t get_adr_control(unsigned t){return config.media.track_adr_control(config.media.opaque,t);}
    int get_track_type(unsigned t){AtapiTrack info{};if(!config.track_info(config.media.opaque,t,&info)){faults|=1;return -1;}return info.type;}
    unsigned get_track_index(uint32_t lba){return lba<get_track_start(get_track(lba))?0:1;}
    cdrom_file::toc get_toc(){cdrom_file::toc t{};for(unsigned i=0;i<get_last_track();i++)if(!config.track_info(config.media.opaque,i,&t.tracks[i]))faults|=1;return t;}
    bool read_data(uint32_t lba,uint8_t *out,unsigned type){
        int ok=type==cdrom_file::CD_TRACK_MODE1?config.media.read_mode1_sector(config.media.opaque,lba,out):config.read_raw(config.media.opaque,lba,out);
        if(ok)sectors++;else faults|=2;return ok;
    }
    AtapiCr589Config config;uint32_t faults=0;uint64_t sectors=0;
};
class cdda_device {
public:
    explicit cdda_device(const AtapiCr589Config &c):config(c){}
    void start_audio(uint32_t lba,uint32_t blocks){config.media.cdda_start(config.media.opaque,lba,blocks);}
    void pause_audio(int p){config.media.cdda_pause(config.media.opaque,p);}
    void stop_audio(){config.media.cdda_start(config.media.opaque,0,0);}
    nscsi_cd_audio_status_t status(){nscsi_cd_audio_status_t s{};config.media.cdda_status(config.media.opaque,&s);return s;}
    bool audio_active(){return status().active;}
    bool audio_paused(){return status().paused;}
    bool audio_ended(){return status().ended;}
    uint32_t get_audio_lba(){return status().lba;}
    void set_output_gain(unsigned ch,float gain){config.media.cdda_set_gain(config.media.opaque,ch,gain);}
private:AtapiCr589Config config;
};
void atapi_source_log(device_t *d,const char *format,...);
#endif
