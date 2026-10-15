/* Original-media sound-board probe, not a native PSX/gameplay harness. */
#include "twinkle_board.h"
#include "chd_hd.h"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

struct Media {
    arcade_chd_hd_t *disk=nullptr;
    uint64_t reads=0,writes=0,failures=0;
    static int read(void *opaque,uint32_t lba,void *out){
        auto &m=*static_cast<Media *>(opaque);m.reads++;
        int ok=arcade_chd_hd_read(m.disk,lba,static_cast<uint8_t *>(out),512)==0;
        m.failures+=!ok;return ok;
    }
    static int write(void *opaque,uint32_t,const void *){
        static_cast<Media *>(opaque)->writes++;return 0; // Explicit read-only original media.
    }
    ~Media(){arcade_chd_hd_close(disk);}
};

int main(int argc,char **argv)
{
    if(argc!=3){std::fprintf(stderr,"usage: %s AUDITED-SOUNDCPU.BIN ORIGINAL-985HDDA01.CHD\n",argv[0]);return 2;}
    std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),{});
    uint8_t sha[20];char hex[41];
    CC_SHA1(rom.data(),CC_LONG(rom.size()),sha);
    for(unsigned i=0;i<20;i++)std::snprintf(hex+i*2,3,"%02x",sha[i]);
    if(rom.size()!=TwinkleBoard::ROM_BYTES || std::strcmp(hex,"23e0017c501465f50c5ccc0a2ecce9c18f5f712e")){
        std::fprintf(stderr,"Original source-word-swapped 863a05.2x size/SHA1 mismatch\n");return 1;
    }
    Media media;char error[256];media.disk=arcade_chd_hd_open(argv[2],nullptr,error,sizeof(error));
    if(!media.disk){std::fprintf(stderr,"Original HDD open failed: %s\n",error);return 1;}
    arcade_chd_hd_info_t info{};arcade_chd_hd_get_info(media.disk,&info);
    for(unsigned i=0;i<20;i++)std::snprintf(hex+i*2,3,"%02x",info.sha1[i]);
    if(std::strcmp(hex,"a5b9ec9a5afa38f36af529d3aea5355ea7d022ca") || info.sector_bytes!=512 || info.cylinders>65535 || info.heads>255 || info.sectors_per_track>255){
        std::fprintf(stderr,"Original IIDX2 HDD identity/geometry mismatch\n");return 1;
    }
    ata_storage_config_t hdd{};hdd.sector_count=info.sector_count;hdd.cylinders=info.cylinders;
    hdd.heads=info.heads;hdd.sectors_per_track=info.sectors_per_track;
    hdd.media_opaque=&media;hdd.read_sector=Media::read;hdd.write_sector=Media::write;
    std::array<uint8_t,512> identify{};size_t bytes=0;
    auto metadata=arcade_chd_hd_read_metadata(media.disk,ARCADE_CHD_HD_METADATA_ATA_IDENTIFY,identify.data(),identify.size(),&bytes);
    if(metadata==ARCADE_CHD_HD_METADATA_OK && bytes==512){hdd.identify_data=identify.data();hdd.identify_size=512;}
    else if(metadata!=ARCADE_CHD_HD_METADATA_MISSING){std::fprintf(stderr,"Invalid original ATA IDENTIFY metadata\n");return 1;}
    std::vector<uint16_t> wave(TwinkleBoard::WAVE_RAM_BYTES/2);
    TwinkleBoard::Config config{};config.program_rom=rom.data();config.program_rom_size=rom.size();
    config.wave_ram=wave.data();config.wave_ram_bytes=wave.size()*2;config.hdd=&hdd;
    TwinkleBoard board(config);
    int64_t result=0;
    while(board.valid() && board.cpu_cycles()<160000000){
        result=board.run(16000);if(result<=0)break;
    }
    uint8_t mailbox[16]={};for(unsigned i=0;i<16;i++)board.main_dpram_read(i,&mailbox[i]);
    std::printf("Twinkle ORIGINAL SOUND-BOARD ONLY: cycles=%llu PC=%06x SR=%04x ATA_status=%02x HDD_reads=%llu failed_reads=%llu rejected_writes=%llu DMA_words=%llu LED=%02x valid=%d error=%d callback_fault=%d result=%lld mailbox=",
        (unsigned long long)board.cpu_cycles(),board.cpu_pc(),board.cpu_sr(),board.ata_status(),
        (unsigned long long)media.reads,(unsigned long long)media.failures,(unsigned long long)media.writes,
        (unsigned long long)board.ata_dma_words(),board.led_outputs(),board.valid(),board.error(),board.callback_fault(),(long long)result);
    for(uint8_t b:mailbox)std::printf("%02x",b);std::puts("; no main-CPU commands injected; NOT GAMEPLAY");
    std::printf("MC68000 bus_error=%d last_bus_error_address=%06x\n",board.cpu_bus_error(),board.last_bus_error_address());
    return !board.valid() || board.callback_fault() || board.cpu_bus_error() || result<0 || media.failures;
}
