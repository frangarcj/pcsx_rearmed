/* GPL-2.0-or-later. Native adapter, MAME deb2f54fe7300e7a192690318beb860159bfd52a.
 * Original firmware executes on the PSX; no game program substitutions. */
#include "system573.h"
#include "system12.h"
#include "system11.h"
#include "system10.h"
#include "systemzn.h"
#include "systemtaito.h"
#ifdef PCSX_SYSTEM_GV
#include "systemgv.h"
#endif
#ifdef PCSX_SYSTEM_GQ
#include "systemgq.h"
#endif
#ifdef PCSX_SYSTEM_GNET
#include "systemgnet.h"
#endif
#include "arcade/system573_flash.h"
#include "arcade/psx_sio.h"
#include "arcade/system573_audio.h"
#ifdef HAVE_CHD
#include "arcade/atapi_cr589.h"
#include "arcade/chd_cd.h"
#endif
#include "gpu_vram.h"
#include "psxmem.h"
#include "psxhw.h"
#include "r3000a.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
static struct {
    System573Flash bus;
    uint8_t *media[11],*fetch,*cassette_image;
#ifdef HAVE_CHD
    ChdCd *cd;AtapiCr589 *drive;
    uint64_t dma_due,dma_words,dma_transfers;uint32_t dma_base,dma_size;
    uint64_t cdda_frames,cdda_nonzero;unsigned cdda_peak;
#endif
    int cd_profile;
    System573Audio audio;
    psx_sio_t *sio[2];
    u32 old_ram,old_rom,last_cycle;
    uint64_t cycles,us,fraction,reads,writes;
    int active,memory_initialized,fault,nvram_loaded,synthetic;
} board;
static const struct {const char *file;size_t bytes;uint32_t crc;} media[]={
#include "arcade/system573_media.inc"
};
static int config(const char *dir){
    static const char *lines[]={"format=pcsx-aow-system573-flash-v1","mame_set=hyperbbc","mame_parent=sys573","bios=std",
        "cpu=CXD8530CQ","cpu_clock_hz=67737600","gpu=CXD8561Q","gpu_clock_hz=53693175",
        "ram_size=4194304","vram_size=2097152","flash_chips=8","flash_chip_size=2097152",
        "flash_bus_width=16","cassette=Y","cdrom_present=false","board_execution_verified=false"};
    char path[4096],line[512];unsigned seen[2]={0,0};int valid[2]={1,1};
    static const char *cd_lines[]={"format=pcsx-aow-system573-cd-v1","mame_set=ddr2m","cassette=X","cdrom_present=true"};
    if(snprintf(path,sizeof(path),"%s/board.ini",dir)>=(int)sizeof(path))return -1;
    FILE *f=fopen(path,"r");if(!f)return -1;
    while(fgets(line,sizeof(line),f)){
        line[strcspn(line,"\r\n")]=0;if(!*line || *line=='#')continue;
        for(unsigned profile=0;profile<2;profile++)if(valid[profile]){
            unsigned i;for(i=0;i<16;i++){
                const char *expected=lines[i];
                if(profile){if(i==0)expected=cd_lines[0];if(i==1)expected=cd_lines[1];if(i==13)expected=cd_lines[2];if(i==14)expected=cd_lines[3];}
                if(!strcmp(line,expected))break;
            }
            if(i==16 || (seen[profile]&(1u<<i)))valid[profile]=0;else seen[profile]|=1u<<i;
        }
    }
    int bad=ferror(f);fclose(f);if(bad)return -1;
    for(unsigned profile=0;profile<2;profile++)if(valid[profile] && seen[profile]==0xffff)return profile;return -1;
}
static void sio_irq(void *opaque,int level){
    unsigned port=(unsigned)(uintptr_t)opaque;
    if(level && psxRegs.ptrs.psxH)psxHu32ref(0x1070)|=SWAPu32(1u<<(7+port));
}
static void map_flash(void){
    if(!board.active || !psxRegs.ptrs.memRLUT)return;
    system573_flash_fetch_view(&board.bus,board.fetch);
    const uint32_t alias[]={0x1f000000,0x9f000000,0xbf000000};
    for(unsigned k=0;k<3;k++)for(unsigned off=0;off<0x400000;off+=(1u<<PSXM_SHIFT)){
        u32 a=alias[k]+off;psxRegs.ptrs.memRLUT[a>>PSXM_SHIFT]=(uintptr_t)(board.fetch+off)-a;
        psxRegs.ptrs.memWLUT[a>>PSXM_SHIFT]=INVALID_PTR_VAL;
    }
    if(psxCpu && psxCpu->Clear)for(unsigned k=0;k<3;k++)psxCpu->Clear(alias[k],0x400000/4);
}
static int original_file(const char *dir,const char *file,size_t bytes,uint32_t crc,int synthetic,uint8_t **out){
    char path[4096];if(snprintf(path,sizeof(path),"%s/%s",dir,file)>=(int)sizeof(path))return -1;
    FILE *f=fopen(path,"rb");if(!f)return -1;
    *out=malloc(bytes);if(!*out){fclose(f);return -1;}
    int invalid=fread(*out,1,bytes,f)!=bytes || fgetc(f)!=EOF || ferror(f);if(fclose(f))invalid=1;
    if(invalid || (!synthetic && crc32(0,*out,bytes)!=crc)){fprintf(stderr,"System573: original %s size/CRC mismatch\n",file);return -1;}return 0;
}
#ifdef HAVE_CHD
static int disc_info(void *cd,unsigned track,AtapiTrack *out){
    chd_cd_track_info_t t;if(!chd_cd_get_track_info(cd,track,&t))return 0;
    *out=(AtapiTrack){t.type,t.frames,t.pregap,t.pregap_data_bytes,0};return 1;
}
static int disc_raw(void *cd,uint32_t lba,uint8_t *out){return chd_cd_read_raw_sector(cd,lba,out);}
static void disc_pcm(void *opaque,int16_t left,int16_t right){
    (void)opaque;board.cdda_frames++;board.cdda_nonzero+=left!=0 || right!=0;
    unsigned l=left<0?-left:left,r=right<0?-right:right;
    if(l>board.cdda_peak)board.cdda_peak=l;if(r>board.cdda_peak)board.cdda_peak=r;
}
static void disc_clocked_pcm(void *opaque,int16_t left,int16_t right,uint64_t index,int playing){
    (void)opaque;(void)playing;if(system573_audio_cd(&board.audio,index,left,right))board.fault=1;
}
static void ata_irq(void *opaque,int level){
    (void)opaque;if(level && psxRegs.ptrs.psxH)psxHu32ref(0x1070)|=SWAPu32(1u<<10);
}
static int ata_bus(u32 a,unsigned w,u32 *v,int write){
    if(!board.drive)return 0;
    if(a>=0x1f560000 && a<0x1f560002){if(write && !(*v&1))atapi_cr589_reset(board.drive);if(!write)*v=0;return 1;}
    int cs;if(a>=0x1f480000 && a<0x1f480010)cs=0;
    else if(a>=0x1f4c0000 && a<0x1f4c0010)cs=1;else return 0;
    uint32_t result=0;
    for(unsigned off=0;off<w;off+=2){
        unsigned shift=(a&1)*8;uint16_t mask=w==1?255u<<shift:65535;
        if(write)atapi_cr589_write(board.drive,cs,((a+off)>>1)&7,(*v>>(off*8))<<shift,mask);
        else result|=(uint32_t)(atapi_cr589_read(board.drive,cs,((a+off)>>1)&7,mask)>>shift)<<(off*8);
    }
    if(!write)*v=result;return 1;
}
#endif
static int load_board(const char *dir,int synthetic){
    if(board.active || !dir || system12_active() || system11_active() || system10_active() || systemzn_active() || systemtaito_active())return -1;
#ifdef PCSX_SYSTEM_GV
    if(systemgv_active())return -1;
#endif
#ifdef PCSX_SYSTEM_GQ
    if(systemgq_active())return -1;
#endif
#ifdef PCSX_SYSTEM_GNET
    if(systemgnet_active())return -1;
#endif
    int profile=config(dir);if(profile<0 || (profile && synthetic)){fprintf(stderr,"System573: invalid or unsupported original profile\n");return -1;}
    board.cd_profile=profile;
    system573_audio_init(&board.audio);
#ifndef HAVE_CHD
    if(profile){fprintf(stderr,"System573: original DDR disc requires HAVE_CHD\n");return -1;}
#endif
    if(!profile){
        for(unsigned i=0;i<11;i++)if(original_file(dir,media[i].file,media[i].bytes,media[i].crc,synthetic,&board.media[i]))goto bad;
    }else {
        if(original_file(dir,"main.bin",0x80000,0x11812ef8,0,&board.media[0]) ||
           original_file(dir,"h8_response.bin",64,0x131e0359,0,&board.media[1]) ||
           original_file(dir,"cassette.bin",548,0x5d906be5,0,&board.cassette_image))goto bad;
        /* No flash or RTC ROM region is declared for DDR. Preserve original
         * source device defaults; installation must run on the guest CPU. */
        for(unsigned i=2;i<11;i++){size_t size=i==10?8192:0x200000;board.media[i]=malloc(size);if(!board.media[i])goto bad;memset(board.media[i],0xff,size);}
#ifdef HAVE_CHD
        char path[4096],error[256];if(snprintf(path,sizeof(path),"%s/895jaa02.chd",dir)>=(int)sizeof(path))goto bad;
        chd_cd_config_t cc={disc_pcm,NULL};board.cd=chd_cd_open(path,&cc,error,sizeof(error));
        if(!board.cd){fprintf(stderr,"System573 CD: %s\n",error);goto bad;}
        if(!chd_cd_set_clocked_pcm_sink(board.cd,disc_clocked_pcm,NULL))goto bad;
        if(!chd_cd_verify_sha1(board.cd,"cfe3a6f3ed62ba388b07045e29e22472d17dcfe4"))goto bad;
        AtapiCr589Config ac={0};if(!chd_cd_get_media_ops(board.cd,&ac.media))goto bad;
        ac.track_info=disc_info;ac.read_raw=disc_raw;ac.irq=ata_irq;
        board.drive=atapi_cr589_create(&ac);if(!board.drive)goto bad;
#endif
    }
    uint8_t *chips[8];for(unsigned i=0;i<8;i++)chips[i]=board.media[2+i];
    /* Exact source Y factory default applies only to Hyperbbc (no region). */
    uint8_t factory[132]={0x19,0,0xaa,0x55};
    time_t now=time(NULL);struct tm local;if(!localtime_r(&now,&local))goto bad;
    if(system573_flash_init_cassette(&board.bus,chips,board.media[10],board.media[1],profile?board.cassette_image:factory,profile?KONAMI_X76F041:KONAMI_X76F100,&local))goto bad;
    psx_sio_callbacks_t cb={0};cb.irq=sio_irq;
    for(unsigned p=0;p<2;p++){cb.opaque=(void *)(uintptr_t)p;if(psx_sio_create(p?PSX_SIO1:PSX_SIO0,&cb,&board.sio[p]))goto bad;}
    psx_sio_write_dsr(board.sio[1],0); /* Source cassette Y DSR. */
    board.fetch=malloc(0x400000);if(!board.fetch || gpu_set_model(2) || gpu_set_vram_height(1024))goto bad;
    board.old_ram=psxRamSize;board.old_rom=psxRomSize;psxRamSize=0x400000;psxRomSize=0x80000;
    Config.HLE=FALSE;board.active=1;board.synthetic=synthetic;
    if(profile)fprintf(stderr,"System573 ddr2m: original BIOS/H8 response/cassette CRCs matched; original CD SHA matched; cassette/CD retain source BAD_DUMP status; flash/RTC source defaults\n");
    else fprintf(stderr,"System573 hyperbbc: %s; cassette Y source factory default (no EEPROM dump), no CD-ROM\n",synthetic?"generated MIPS fixture, not original ROM evidence":"eleven source ROM CRCs matched; H8 response dump");
    return 0;
bad:system573_unload();return -1;
}
int system573_load(const char *dir){return load_board(dir,0);}
int system573_test_load(const char *dir){return load_board(dir,1);}
int system573_active(void){return board.active;}
int system573_has_cd(void){return board.active && board.cd_profile;}
void system573_set_audio_output(void (*sink)(void *,const int16_t *,unsigned),void *opaque){
    system573_audio_set_sink(&board.audio,sink,opaque);
}
int system573_audio_spu_output(const int16_t *stereo,unsigned frames,u32 end_cycle){
    if(!system573_has_cd() || !board.audio.sink || !frames)return 0;
    if(system573_sync(psxRegs.cycle))return -1;
    int64_t end=(int64_t)board.cycles+(int32_t)(end_cycle-board.last_cycle);
    if(end<0 || (uint64_t)end<(uint64_t)frames*768){board.fault=1;return -1;}
    uint64_t first=((uint64_t)end-(uint64_t)frames*768)/768;
    if(board.audio.started && first!=board.audio.spu_next)
        fprintf(stderr,"System573 SPU clock discontinuity: expected=%"PRIu64" first=%"PRIu64" frames=%u end_cycle=%u cpu_cycle=%u\n",board.audio.spu_next,first,frames,end_cycle,psxRegs.cycle);
    if(system573_audio_spu(&board.audio,first,stereo,frames)){board.fault=1;return -1;}return 0;
}
int system573_audio_flush_output(void){
    if(!system573_has_cd())return 0;
    if(system573_sync(psxRegs.cycle) || system573_audio_flush(&board.audio)){board.fault=1;return -1;}return 0;
}
void system573_unload(void){
    system573_flash_destroy(&board.bus);
#ifdef HAVE_CHD
    atapi_cr589_destroy(board.drive);chd_cd_close(board.cd);
#endif
    for(unsigned i=0;i<11;i++)free(board.media[i]);free(board.fetch);free(board.cassette_image);
    for(unsigned p=0;p<2;p++)psx_sio_destroy(board.sio[p]);
    if(board.active){psxRamSize=board.old_ram;psxRomSize=board.old_rom;}memset(&board,0,sizeof(board));
}
void system573_mem_reset(void){
    if(!board.active || !psxRegs.ptrs.psxM)return;
    memcpy(psxRegs.ptrs.psxR,board.media[0],0x80000);
    if(!board.memory_initialized)memset(psxRegs.ptrs.psxM,0,0x400000);
    board.memory_initialized=1;memset(psxRegs.ptrs.psxP,0xff,0x10000);map_flash();
}
void system573_reset(void){
    if(!board.active)return;board.last_cycle=psxRegs.cycle;system573_flash_reset(&board.bus);map_flash();
    /* Source SIO has no device_reset override; preserve its timer/pin state. */
    psx_sio_write_dsr(board.sio[1],0);
#ifdef HAVE_CHD
    if(board.drive)atapi_cr589_reset(board.drive);board.dma_due=0;
#endif
}
int system573_sync(u32 cycle){
    if(!board.active)return 0;
    uint32_t delta=cycle-board.last_cycle;board.last_cycle=cycle;board.cycles+=delta;
    uint64_t numerator=(uint64_t)delta*1000000+board.fraction;uint64_t elapsed=numerator/PSXCLK;board.fraction=numerator%PSXCLK;board.us+=elapsed;
    system573_flash_advance_us(&board.bus,elapsed);
    for(unsigned p=0;p<2;p++)if(psx_sio_advance_to_cycle(board.sio[p],board.cycles))board.fault=1;
#ifdef HAVE_CHD
    if(board.drive){
        uint64_t ns=board.us*1000+board.fraction*1000/PSXCLK;
        if(atapi_cr589_advance_to(board.drive,ns) || !chd_cd_advance_to(board.cd,ns))board.fault=1;
        if(board.dma_due && board.cycles>=board.dma_due){
            board.dma_due=0;
            if(!psxRegs.ptrs.psxM || board.dma_base>0x400000 || board.dma_size>(0x400000-board.dma_base)/4)board.fault=1;
            else for(unsigned i=0;i<board.dma_size;i++){
                uint32_t value=atapi_cr589_read(board.drive,0,0,65535);
                value|=(uint32_t)atapi_cr589_read(board.drive,0,0,65535)<<16;
                memcpy(psxRegs.ptrs.psxM+board.dma_base,&value,4);board.dma_base+=4;board.dma_words++;
            }
            /* Preserve the source retransfer when DRQ remains (konami80s hack). */
            if(atapi_cr589_read(board.drive,1,6,65535)&8)board.dma_due=board.cycles+60000ull*(board.dma_size/64);
        }
        AtapiCr589Status st;atapi_cr589_get_status(board.drive,&st);if(st.faults)board.fault=1;
    }
#endif
    return board.fault || board.bus.faults?-1:0;
}
static int sio_bus(u32 a,unsigned w,u32 *v,int write){
    if(a<0x1f801040 || a>=0x1f801060)return 0;
    unsigned p=(a-0x1f801040)>>4,shift=(a&3)*8;uint32_t mask=w==4?UINT32_MAX:(1u<<(w*8))-1,raw=0;
    int r=write?psx_sio_write(board.sio[p],(a>>2)&3,*v<<shift,mask<<shift):psx_sio_read(board.sio[p],(a>>2)&3,mask<<shift,&raw);
    if(r)board.fault=1;if(!write)*v=(raw>>shift)&mask;return 1;
}
int system573_read(u32 a,unsigned w,u32 *v){
    if(!board.active || !v)return 0;a&=0x1fffffff;
    if((a<0x1f000000 || a>=0x1f700000) && (a<0x1f801040 || a>=0x1f801060))return 0;
    if((w!=1 && w!=2 && w!=4) || (a&(w-1))){board.fault=1;*v=UINT32_MAX;return 1;}
    system573_sync(psxRegs.cycle);int r=sio_bus(a,w,v,0);
#ifdef HAVE_CHD
    if(!r)r=ata_bus(a,w,v,0);
#endif
    if(!r)r=system573_flash_read(&board.bus,a,w,v);
    if(r<0){board.fault=1;*v=UINT32_MAX;}if(r)board.reads++;return r!=0;
}
int system573_write(u32 a,unsigned w,u32 v){
    if(!board.active)return 0;a&=0x1fffffff;
    if((a<0x1f000000 || a>=0x1f700000) && (a<0x1f801040 || a>=0x1f801060))return 0;
    if((w!=1 && w!=2 && w!=4) || (a&(w-1))){board.fault=1;return 1;}
    system573_sync(psxRegs.cycle);unsigned old_bank=board.bus.control&63;int r=sio_bus(a,w,&v,1);
#ifdef HAVE_CHD
    if(!r)r=ata_bus(a,w,&v,1);
#endif
    if(!r)r=system573_flash_write(&board.bus,a,w,v);
    if(r<0)board.fault=1;
    if(r && a>=0x1f500000 && a<0x1f500002 && old_bank!=(board.bus.control&63))map_flash();
    if(r && a>=0x1f000000 && a<0x1f400000){
        unsigned bank=board.bus.control&63;
        if(bank<4)for(unsigned lane=0;lane<2;lane++){
            size_t start=board.bus.dirty_start[lane],length=board.bus.dirty_length[lane];
            nor_flash_t *f=board.bus.flash[bank*2+lane];
            if(length){
                const uint8_t *data=nor_flash_image(f);for(size_t i=start;i<start+length;i++)board.fetch[2*i+lane]=data[i];
                if(psxCpu && psxCpu->Clear){const u32 alias[]={0x1f000000,0x9f000000,0xbf000000};
                    size_t offset=(2*start+lane)&~3u;
                    size_t end=(2*(start+length-1)+lane+4)&~3u,words=(end-offset)/4;
                    for(unsigned k=0;k<3;k++)psxCpu->Clear(alias[k]+offset,words);
                }
            }
        }
    }
    if(r)board.writes++;return r!=0;
}
int system573_set_input(const char *name,int pressed){
    if(!board.active || !name || (pressed!=0 && pressed!=1))return -1;
    unsigned port=0,mask=0;
    if(!strcmp(name,"coin1")){port=1;mask=0x01000000;}
    else if(!strcmp(name,"coin2")){port=1;mask=0x02000000;}
    else if(!strcmp(name,"service1")){port=1;mask=0x10000000;}
    else if(!strcmp(name,"test")){port=3;mask=0x400;}
    else if(board.cd_profile && !strcmp(name,"boot_cd")){if(pressed)board.bus.input[1]|=8;else board.bus.input[1]&=~8u;return 0;}
    else if(name[0]=='p' && name[1]>='1' && name[1]<='3'){
        unsigned p=name[1]-'1';port=2;
        if(board.cd_profile && p==2)return -1;
        static const unsigned buttons[3][4]={{0x1000,0x2000,0x4000,0x8000},{0x10,0x20,0x40,0x80},{0x100,0x800,0x200,0x400}};
        if(!strcmp(name+2,"start"))mask=buttons[p][3];
        else if(!strcmp(name+2,"button1"))mask=buttons[p][0];
        else if(!strcmp(name+2,"button2"))mask=buttons[p][1];
        else if(!strcmp(name+2,"button3"))mask=buttons[p][2];
        if(board.cd_profile){
            unsigned direction=0;
            if(!strcmp(name+2,"left"))direction=1;else if(!strcmp(name+2,"right"))direction=2;
            else if(!strcmp(name+2,"up"))direction=4;else if(!strcmp(name+2,"down"))direction=8;
            if(direction){mask=direction<<(p?0:8);if(pressed)board.bus.stage_input&=~mask;else board.bus.stage_input|=mask;return 0;}
            if(!strcmp(name+2,"button4")){port=3;mask=0x100u<<(p?16:0);}
            else if(!strcmp(name+2,"button5")){port=3;mask=0x200u<<(p?16:0);}
            else if(!strcmp(name+2,"button6")){port=3;mask=0x800u<<(p?16:0);}
        }
    }
    if(!mask)return -1;if(pressed)board.bus.input[port]&=~mask;else board.bus.input[port]|=mask;return 0;
}
size_t system573_nvram_size(void){return board.cd_profile?32u+0x1000000u+8192u+548u+65536u:SYSTEM573_NVRAM_SIZE;}
static const char *nv_header(void){return board.cd_profile?"S573NV2\0ddr2m\0\0\0":"S573NV1\0hyperbbc";}
int system573_nvram_import(const unsigned char *image,size_t size){
    if(!board.active || !image || size!=system573_nvram_size() ||
       memcmp(image,nv_header(),16))return -1;
    uint32_t stored=0,length=0;
    for(unsigned i=0;i<4;i++){length|=(uint32_t)image[16+i]<<(8*i);stored|=(uint32_t)image[20+i]<<(8*i);}
    for(unsigned i=24;i<32;i++)if(image[i])return -1;
    if(length!=size || stored!=crc32(0,image+32,size-32))return -1;
    const uint8_t *payload=image+32;
    KonamiSecureFlash *next=konami_secure_flash_create(board.cd_profile?KONAMI_X76F041:KONAMI_X76F100,payload+0x1002000,board.cd_profile?548:132);if(!next)return -1;
#ifdef HAVE_CHD
    if(board.cd_profile && atapi_cr589_nvram_import(board.drive,payload+0x1002000+548,65536)){konami_secure_flash_destroy(next);return -1;}
#endif
    for(unsigned i=0;i<8;i++)memcpy(board.media[i+2],payload+i*0x200000,0x200000);
    m48t58_nvram_import(&board.bus.rtc,payload+0x1000000);
    konami_secure_flash_destroy(board.bus.cassette);board.bus.cassette=next;
    board.nvram_loaded=1;map_flash();return 0;
}
int system573_nvram_export(unsigned char *image,size_t size){
    if(!board.active || !image || size!=system573_nvram_size())return -1;
    memset(image,0,32);memcpy(image,nv_header(),16);
    uint8_t *payload=image+32;
    for(unsigned i=0;i<8;i++)memcpy(payload+i*0x200000,board.media[i+2],0x200000);
    memcpy(payload+0x1000000,board.bus.rtc.ram,8192);
    if(konami_secure_flash_get_image(board.bus.cassette,payload+0x1002000,board.cd_profile?548:132))return -1;
#ifdef HAVE_CHD
    if(board.cd_profile && atapi_cr589_nvram_export(board.drive,payload+0x1002000+548,65536))return -1;
#endif
    uint32_t crc=crc32(0,payload,size-32);
    for(unsigned i=0;i<4;i++){image[16+i]=size>>(8*i);image[20+i]=crc>>(8*i);}return 0;
}
int system573_selftest(void){
    if(!board.active)return -1;u32 v;
    if(board.cd_profile){
        if(system573_set_input("p1left",1) || !system573_read(0x1f400008,4,&v) || v!=0xfeff)return -1;
        system573_set_input("p1left",0);if(system573_set_input("p3start",1)!=-1)return -1;
        fprintf(stderr,"System573 DDR native input selftest PASS\n");return 0;
    }
    if(system573_set_input("p3button2",1) || !system573_read(0x1f400008,4,&v) || v!=0xf7ff)return -1;
    system573_set_input("p3button2",0);
    if(system573_set_input("p1up",1)!=-1 || system573_set_input("p1button4",1)!=-1)return -1;
    fprintf(stderr,"System573 native selftest PASS\n");return 0;
}
int system573_dma5(u32 madr,u32 bcr,u32 chcr){
    if(!board.active)return -1;
    if(system573_sync(psxRegs.cycle))return -1;
#ifdef HAVE_CHD
    if(board.drive){
        uint64_t blocks=bcr>>16;if(!blocks)blocks=65536;
        uint64_t words=(chcr&0x200)?(bcr&65535)*blocks:bcr;
        /* MAME's board write handler is the PSX RAM-bound transfer. The
         * opposite board read handler is empty. DMA completion itself is
         * immediate, while the board timer owns actual payload delivery. */
        if(chcr==0x01000201 || chcr==0x11050100 || chcr==0x11150100){
            if(words>INT32_MAX || (madr&3))return -1;
            board.dma_base=madr&0x3fffff;board.dma_size=words;board.dma_transfers++;
            /* MAME PSX CPU cycles use input/4; this core uses input/2. */
            board.dma_due=board.cycles+60000ull*(words/512);if(!board.dma_due)board.dma_due=1;
        }
    }
#else
    (void)madr;(void)bcr;(void)chcr;
#endif
    return 0;
}
void system573_report(FILE *out){
    if(!board.active || !out)return;
    fprintf(out,"System573: profile=%s RAM=4194304 VRAM=2097152 bank=%u control=%04x security=%04x faults=%u media=%s H8_response=%s H8_firmware=NO_DUMP cassette_seed=%s CDROM=%s NVRAM=%s\n",board.cd_profile?"ddr2m":"hyperbbc",board.bus.control&63,board.bus.control,board.bus.security,board.fault|board.bus.faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_CRC_VERIFIED",board.synthetic?"GENERATED":"SOURCE_DUMP",board.cd_profile?"SOURCE_BAD_DUMP":"SOURCE_FACTORY_DEFAULT",board.cd_profile?"SOURCE_BAD_DUMP":"ABSENT",board.nvram_loaded?"RESTORED":"SOURCE_SEED");
#ifdef HAVE_CHD
    if(board.drive){AtapiCr589Status st;atapi_cr589_get_status(board.drive,&st);
        fprintf(out,"System573 CD: packets=%"PRIu64" sectors=%"PRIu64" status=%02x command=%02x faults=%u DMA_transfers=%"PRIu64" DMA_words=%"PRIu64" stage_edges=%"PRIu64"\n",st.packets,st.sectors,st.status,st.packet_command,st.faults,board.dma_transfers,board.dma_words,board.bus.stage_edges);
        fprintf(out,"System573 CDDA: decoded_frames=%"PRIu64" nonzero_frames=%"PRIu64" peak=%u\n",board.cdda_frames,board.cdda_nonzero,board.cdda_peak);
        fprintf(out,"System573 speaker: rate=44100 SPU_gain=1 CDDA_gain=1 frames=%"PRIu64" SPU_inputs=%"PRIu64" SPU_nonzero=%"PRIu64" CDDA_clock=%"PRIu64" CDDA_nonzero=%"PRIu64" clipped=%"PRIu64" peak=%u pending_SPU=%"PRIu64" faults=%u\n",board.audio.frames,board.audio.spu_inputs,board.audio.spu_nonzero,board.audio.cd_inputs,board.audio.cd_nonzero,board.audio.clipped,board.audio.peak,board.audio.started?board.audio.spu_next-board.audio.next:0,board.audio.faults);
    }
#endif
    fprintf(out,"System573 devices: flash_reads=%"PRIu64" flash_writes=%"PRIu64" H8_edges=%"PRIu64" rtc_reads=%"PRIu64" rtc_writes=%"PRIu64" rtc_ticks=%"PRIu64" JVS_empty_packets=%"PRIu64" empty_ATA_reads=%"PRIu64" MMIO=%"PRIu64"/%"PRIu64"\n",board.bus.flash_reads,board.bus.flash_writes,board.bus.h8_edges,board.bus.rtc_reads,board.bus.rtc_writes,board.bus.rtc.ticks,board.bus.jvs_packets,board.bus.empty_ata_reads,board.reads,board.writes);
}
