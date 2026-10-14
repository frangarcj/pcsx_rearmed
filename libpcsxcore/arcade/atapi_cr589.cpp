/* SPDX-License-Identifier: BSD-3-Clause. Source MAME CR-589/SPC/MMC command
 * engine; standalone callback adapter. See atapi_mame/PROVENANCE.txt. */
#include "atapi_cr589.h"
#include "atapi_mame/atapihle_port.h"
#include "atapi_mame/t10mmc_port.h"
#include <cstring>
#include <new>
#include <vector>
#include <cstdio>
#include <cstdarg>

class matsushita_cr589_device : public atapi_hle_device, public t10mmc {
public:
    explicit matsushita_cr589_device(const AtapiCr589Config &cfg):image(cfg),audio(cfg),config(cfg){
        m_image=&image;m_cdda=&audio;
        std::memset(buffer,0,sizeof(buffer));
        std::memcpy(buffer+0x3ab,"MATSHITACD-ROM CR-589   GS0N",28);
        device_start();device_reset();
    }
    void ExecCommand() override;
    void ReadData(uint8_t *data,int length) override;
    void WriteData(uint8_t *data,int length) override;
    void device_start() override;
    void device_reset() override {atapi_hle_device::device_reset();download=bufferOffset=0;}
    void identify_packet_device() override {}
    void perform_diagnostic() override {m_error=IDE_ERROR_DIAGNOSTIC_PASSED;}
    void process_buffer() override {if(m_packet_command_pending)packets++;atapi_hle_device::process_buffer();m_packet_command_pending=false;}
    void process_command() override {m_packet_command_pending=m_command==IDE_COMMAND_PACKET;atapi_hle_device::process_command();}
    void set_irq_out(int state) override {if(config.irq)config.irq(config.line_opaque,state);}
    void set_dmarq_out(int state) override {if(config.dmarq)config.dmarq(config.line_opaque,state);}
    void set_dasp_out(int) override {}
    void set_pdiag_out(int) override {}
    bool valid_buffer_range(uint32_t offset,uint32_t bytes){
        if(offset>sizeof(buffer) || bytes>sizeof(buffer)-offset){malformed_buffer();return false;}return true;
    }
    void malformed_buffer(){faults|=4;set_sense(SCSI_SENSE_KEY_ILLEGAL_REQUEST,SCSI_SENSE_ASC_ASCQ_ILLEGAL_FIELD_IN_CDB);m_phase=SCSI_PHASE_STATUS;m_status_code=SCSI_STATUS_CODE_CHECK_CONDITION;m_transfer_length=0;}
    void safe_mmc_read(uint8_t *data,int bytes){
        if(bytes<0 || bytes>0xf800){malformed_buffer();return;}
        // Source MMC mode/inquiry responders write complete allocation headers.
        // Keep those bytes in bounded scratch rather than the smaller ATA chunk.
        std::vector<uint8_t> scratch(65536,0);
        t10mmc::ReadData(scratch.data(),bytes);
        if(image.faults){faults|=image.faults;return;}
        std::memcpy(data,scratch.data(),bytes);
    }
    void safe_mmc_write(uint8_t *data,int bytes){
        if(bytes<0 || bytes>0xf800){malformed_buffer();return;}
        std::vector<uint8_t> scratch(65536,0);std::memcpy(scratch.data(),data,bytes);
        t10mmc::WriteData(scratch.data(),bytes);
    }
    uint16_t read(int cs,unsigned reg,uint16_t mask){return (cs?control_r(reg):command_r(reg))&mask;}
    void write(int cs,unsigned reg,uint16_t value,uint16_t mask){(void)mask;if(cs)control_w(reg,value);else command_w(reg,value);}
    void status(AtapiCr589Status *out){
        *out={packets,image.sectors,diagnostics,faults|image.faults,m_status,m_error,m_command,
              command[0],uint8_t(download),m_sense_key,m_sense_asc,
              unsigned(m_cylinder_low|m_cylinder_high<<8),unsigned(m_buffer_offset)};
    }
    int download=0,bufferOffset=0;
    uint8_t buffer[65536]{};
    cdrom_image_device image;cdda_device audio;AtapiCr589Config config;
    uint32_t faults=0;uint64_t packets=0,diagnostics=0;
    char last_log[384]{};
    bool m_packet_command_pending=false;
};
#include "atapi_mame/cr589_methods.inc"
struct AtapiCr589 : matsushita_cr589_device {using matsushita_cr589_device::matsushita_cr589_device;bool busy=false;};
struct Operation {
    AtapiCr589 *drive;bool entered=false;
    explicit Operation(AtapiCr589 *d):drive(d){if(!d)return;if(d->busy)d->faults|=8;else {d->busy=true;entered=true;}}
    ~Operation(){if(entered)drive->busy=false;}
};
void atapi_source_log(device_t *d,const char *format,...){
    auto *drive=dynamic_cast<matsushita_cr589_device *>(d);if(!drive)return;
    drive->diagnostics++;va_list args;va_start(args,format);
    std::vsnprintf(drive->last_log,sizeof(drive->last_log),format,args);va_end(args);
}
extern "C" AtapiCr589 *atapi_cr589_create(const AtapiCr589Config *c){
    if(!c || !c->track_info || !c->read_raw || !c->media.present || !c->media.sequence_counter ||
       !c->media.read_mode1_sector || !c->media.track_start || !c->media.last_track ||
       !c->media.track_for_lba || !c->media.track_adr_control || !c->media.track_is_audio ||
       !c->media.cdda_start || !c->media.cdda_pause || !c->media.cdda_status || !c->media.cdda_set_gain)return nullptr;
    unsigned tracks=c->media.last_track(c->media.opaque);
    if(tracks>99 || (c->media.present(c->media.opaque) && !tracks))return nullptr;
    try{return new AtapiCr589(*c);}catch(...){return nullptr;}
}
extern "C" void atapi_cr589_destroy(AtapiCr589 *d){if(d && d->busy){d->faults|=8;return;}delete d;}
extern "C" void atapi_cr589_reset(AtapiCr589 *d){Operation op(d);if(op.entered)d->device_reset();}
extern "C" int atapi_cr589_advance_to(AtapiCr589 *d,uint64_t ns){Operation op(d);return op.entered && d->run_until(ns)?0:-1;}
extern "C" uint16_t atapi_cr589_read(AtapiCr589 *d,int cs,unsigned reg,uint16_t mask){Operation op(d);return op.entered && (cs==0 || cs==1) && reg<8?d->read(cs,reg,mask):mask;}
extern "C" void atapi_cr589_write(AtapiCr589 *d,int cs,unsigned reg,uint16_t data,uint16_t mask){Operation op(d);if(op.entered && (cs==0 || cs==1) && reg<8)d->write(cs,reg,data,mask);}
extern "C" void atapi_cr589_get_status(AtapiCr589 *d,AtapiCr589Status *s){if(d && s)d->status(s);}
extern "C" int atapi_cr589_nvram_export(AtapiCr589 *d,uint8_t *out,size_t bytes){if(!d || !out || bytes!=65536)return -1;std::memcpy(out,d->buffer,bytes);return 0;}
extern "C" int atapi_cr589_nvram_import(AtapiCr589 *d,const uint8_t *image,size_t bytes){Operation op(d);if(!op.entered || !image || bytes!=65536)return -1;std::memcpy(d->buffer,image,bytes);return 0;}
