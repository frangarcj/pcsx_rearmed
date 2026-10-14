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
    uint8_t *media[11],*fetch;
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
    char path[4096],line[512];unsigned seen=0;
    if(snprintf(path,sizeof(path),"%s/board.ini",dir)>=(int)sizeof(path))return -1;
    FILE *f=fopen(path,"r");if(!f)return -1;
    while(fgets(line,sizeof(line),f)){
        line[strcspn(line,"\r\n")]=0;if(!*line || *line=='#')continue;
        unsigned i;for(i=0;i<16;i++)if(!strcmp(line,lines[i]))break;
        if(i==16 || (seen&(1u<<i))){fclose(f);return -1;}seen|=1u<<i;
    }
    int bad=ferror(f) || seen!=0xffff;fclose(f);return bad?-1:0;
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
    if(config(dir)){fprintf(stderr,"System573: invalid or unsupported original profile\n");return -1;}
    for(unsigned i=0;i<11;i++){
        char path[4096];if(snprintf(path,sizeof(path),"%s/%s",dir,media[i].file)>=(int)sizeof(path))goto bad;
        FILE *f=fopen(path,"rb");if(!f)goto bad;
        board.media[i]=malloc(media[i].bytes);if(!board.media[i]){fclose(f);goto bad;}
        int invalid=fread(board.media[i],1,media[i].bytes,f)!=media[i].bytes || fgetc(f)!=EOF || ferror(f);
        if(fclose(f))invalid=1;
        if(invalid || (!synthetic && crc32(0,board.media[i],media[i].bytes)!=media[i].crc)){fprintf(stderr,"System573: original %s size/CRC mismatch\n",media[i].file);goto bad;}
    }
    uint8_t *chips[8];for(unsigned i=0;i<8;i++)chips[i]=board.media[2+i];
    /* Exact source nvram_default for a declared cassette Y with no ROM region.
     * This is not a dumped/authenticated game cassette and is never emitted as one. */
    uint8_t factory[132]={0x19,0,0xaa,0x55};
    time_t now=time(NULL);struct tm local;if(!localtime_r(&now,&local))goto bad;
    if(system573_flash_init(&board.bus,chips,board.media[10],board.media[1],factory,&local))goto bad;
    psx_sio_callbacks_t cb={0};cb.irq=sio_irq;
    for(unsigned p=0;p<2;p++){cb.opaque=(void *)(uintptr_t)p;if(psx_sio_create(p?PSX_SIO1:PSX_SIO0,&cb,&board.sio[p]))goto bad;}
    psx_sio_write_dsr(board.sio[1],0); /* Source cassette Y DSR. */
    board.fetch=malloc(0x400000);if(!board.fetch || gpu_set_model(2) || gpu_set_vram_height(1024))goto bad;
    board.old_ram=psxRamSize;board.old_rom=psxRomSize;psxRamSize=0x400000;psxRomSize=0x80000;
    Config.HLE=FALSE;board.active=1;board.synthetic=synthetic;
    fprintf(stderr,"System573 hyperbbc: %s; cassette Y source factory default (no EEPROM dump), no CD-ROM\n",synthetic?"generated MIPS fixture, not original ROM evidence":"eleven source ROM CRCs matched; H8 response dump");
    return 0;
bad:system573_unload();return -1;
}
int system573_load(const char *dir){return load_board(dir,0);}
int system573_test_load(const char *dir){return load_board(dir,1);}
int system573_active(void){return board.active;}
void system573_unload(void){
    system573_flash_destroy(&board.bus);
    for(unsigned i=0;i<11;i++)free(board.media[i]);free(board.fetch);
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
}
int system573_sync(u32 cycle){
    if(!board.active)return 0;
    uint32_t delta=cycle-board.last_cycle;board.last_cycle=cycle;board.cycles+=delta;
    uint64_t numerator=(uint64_t)delta*1000000+board.fraction;uint64_t elapsed=numerator/PSXCLK;board.fraction=numerator%PSXCLK;board.us+=elapsed;
    system573_flash_advance_us(&board.bus,elapsed);
    for(unsigned p=0;p<2;p++)if(psx_sio_advance_to_cycle(board.sio[p],board.cycles))board.fault=1;
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
    if(!r)r=system573_flash_read(&board.bus,a,w,v);
    if(r<0){board.fault=1;*v=UINT32_MAX;}if(r)board.reads++;return r!=0;
}
int system573_write(u32 a,unsigned w,u32 v){
    if(!board.active)return 0;a&=0x1fffffff;
    if((a<0x1f000000 || a>=0x1f700000) && (a<0x1f801040 || a>=0x1f801060))return 0;
    if((w!=1 && w!=2 && w!=4) || (a&(w-1))){board.fault=1;return 1;}
    system573_sync(psxRegs.cycle);int r=sio_bus(a,w,&v,1);
    if(!r)r=system573_flash_write(&board.bus,a,w,v);
    if(r<0)board.fault=1;
    if(r && a>=0x1f500000 && a<0x1f500002)map_flash();
    if(r && a>=0x1f000000 && a<0x1f400000){
        size_t start,length;unsigned bank=board.bus.control&63;
        if(bank<4 && (nor_flash_last_write_dirty_span(board.bus.flash[bank*2],&start,&length)>0 ||
                      nor_flash_last_write_dirty_span(board.bus.flash[bank*2+1],&start,&length)>0))map_flash();
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
    else if(name[0]=='p' && name[1]>='1' && name[1]<='3'){
        unsigned p=name[1]-'1';port=2;
        static const unsigned buttons[3][4]={{0x1000,0x2000,0x4000,0x8000},{0x10,0x20,0x40,0x80},{0x100,0x800,0x200,0x400}};
        if(!strcmp(name+2,"start"))mask=buttons[p][3];
        else if(!strcmp(name+2,"button1"))mask=buttons[p][0];
        else if(!strcmp(name+2,"button2"))mask=buttons[p][1];
        else if(!strcmp(name+2,"button3"))mask=buttons[p][2];
    }
    if(!mask)return -1;if(pressed)board.bus.input[port]&=~mask;else board.bus.input[port]|=mask;return 0;
}
int system573_nvram_import(const unsigned char *image,size_t size){
    if(!board.active || !image || size!=SYSTEM573_NVRAM_SIZE ||
       memcmp(image,"S573NV1\0hyperbbc",16))return -1;
    uint32_t stored=0,length=0;
    for(unsigned i=0;i<4;i++){length|=(uint32_t)image[16+i]<<(8*i);stored|=(uint32_t)image[20+i]<<(8*i);}
    for(unsigned i=24;i<32;i++)if(image[i])return -1;
    if(length!=size || stored!=crc32(0,image+32,size-32))return -1;
    const uint8_t *payload=image+32;
    KonamiSecureFlash *next=konami_secure_flash_create(KONAMI_X76F100,payload+0x1002000,132);if(!next)return -1;
    for(unsigned i=0;i<8;i++)memcpy(board.media[i+2],payload+i*0x200000,0x200000);
    m48t58_nvram_import(&board.bus.rtc,payload+0x1000000);
    konami_secure_flash_destroy(board.bus.cassette);board.bus.cassette=next;
    board.nvram_loaded=1;map_flash();return 0;
}
int system573_nvram_export(unsigned char *image,size_t size){
    if(!board.active || !image || size!=SYSTEM573_NVRAM_SIZE)return -1;
    memset(image,0,32);memcpy(image,"S573NV1\0hyperbbc",16);
    uint8_t *payload=image+32;
    for(unsigned i=0;i<8;i++)memcpy(payload+i*0x200000,board.media[i+2],0x200000);
    memcpy(payload+0x1000000,board.bus.rtc.ram,8192);
    if(konami_secure_flash_get_image(board.bus.cassette,payload+0x1002000,132))return -1;
    uint32_t crc=crc32(0,payload,size-32);
    for(unsigned i=0;i<4;i++){image[16+i]=size>>(8*i);image[20+i]=crc>>(8*i);}return 0;
}
int system573_selftest(void){
    if(!board.active)return -1;u32 v;
    if(system573_set_input("p3button2",1) || !system573_read(0x1f400008,4,&v) || v!=0xf7ff)return -1;
    system573_set_input("p3button2",0);
    if(system573_set_input("p1up",1)!=-1 || system573_set_input("p1button4",1)!=-1)return -1;
    fprintf(stderr,"System573 native selftest PASS\n");return 0;
}
void system573_report(FILE *out){
    if(!board.active || !out)return;
    fprintf(out,"System573: profile=hyperbbc RAM=4194304 VRAM=2097152 bank=%u control=%04x security=%04x faults=%u media=%s H8_response=%s H8_firmware=NO_DUMP cassette_seed=SOURCE_FACTORY_DEFAULT CDROM=ABSENT NVRAM=%s\n",board.bus.control&63,board.bus.control,board.bus.security,board.fault|board.bus.faults,board.synthetic?"SYNTHETIC_FIXTURE":"SOURCE_CRC_VERIFIED",board.synthetic?"GENERATED":"SOURCE_DUMP",board.nvram_loaded?"RESTORED":"SOURCE_SEED");
    fprintf(out,"System573 devices: flash_reads=%"PRIu64" flash_writes=%"PRIu64" H8_edges=%"PRIu64" rtc_reads=%"PRIu64" rtc_writes=%"PRIu64" rtc_ticks=%"PRIu64" JVS_empty_packets=%"PRIu64" empty_ATA_reads=%"PRIu64" MMIO=%"PRIu64"/%"PRIu64"\n",board.bus.flash_reads,board.bus.flash_writes,board.bus.h8_edges,board.bus.rtc_reads,board.bus.rtc_writes,board.bus.rtc.ticks,board.bus.jvs_packets,board.bus.empty_ata_reads,board.reads,board.writes);
}
