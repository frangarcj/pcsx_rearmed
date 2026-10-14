/* Original-disc integration check, separate from synthetic device fixtures. */
#include "../libpcsxcore/arcade/atapi_cr589.h"
#include "../libpcsxcore/arcade/chd_cd.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <vector>
static uint64_t pcm_frames,pcm_nonzero;static unsigned pcm_peak;
static void pcm(void *,int16_t l,int16_t r){pcm_frames++;pcm_nonzero+=l!=0 || r!=0;unsigned a=l<0?-int(l):l,b=r<0?-int(r):r;if(a>pcm_peak)pcm_peak=a;if(b>pcm_peak)pcm_peak=b;}
static int info(void *cd,unsigned t,AtapiTrack *out){chd_cd_track_info_t i{};if(!chd_cd_get_track_info(static_cast<ChdCd *>(cd),t,&i))return 0;*out={i.type,i.frames,i.pregap,i.pregap_data_bytes,0};return 1;}
static int raw(void *cd,uint32_t lba,uint8_t *out){return chd_cd_read_raw_sector(static_cast<ChdCd *>(cd),lba,out);}
int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: %s original-DDR2m.chd\n",argv[0]);return 2;}
    char error[256]{};chd_cd_config_t cc{pcm,nullptr};ChdCd *cd=chd_cd_open(argv[1],&cc,error,sizeof(error));
    if(!cd){fprintf(stderr,"%s\n",error);return 1;}
    assert(chd_cd_verify_sha1(cd,"cfe3a6f3ed62ba388b07045e29e22472d17dcfe4"));
    assert(chd_cd_track_count(cd)==73);AtapiCr589Config c{};assert(chd_cd_get_media_ops(cd,&c.media));c.track_info=info;c.read_raw=raw;
    AtapiCr589 *d=atapi_cr589_create(&c);assert(d);uint64_t now=10000000;assert(!atapi_cr589_advance_to(d,now));
    auto packet=[&](std::array<uint8_t,12> cmd){
        atapi_cr589_write(d,0,4,0,0xffff);atapi_cr589_write(d,0,5,8,0xffff);atapi_cr589_write(d,0,7,0xa0,0xffff);
        now+=1000000;assert(!atapi_cr589_advance_to(d,now));assert(atapi_cr589_read(d,1,6,0xffff)&8);
        for(unsigned i=0;i<12;i+=2)atapi_cr589_write(d,0,0,cmd[i]|cmd[i+1]<<8,0xffff);
    };
    auto input=[&](){std::vector<uint8_t> data;
        for(unsigned j=0;j<12;j++){
            if(!(atapi_cr589_read(d,1,6,0xffff)&8))return data;
            unsigned n=atapi_cr589_read(d,0,4,0xffff)|atapi_cr589_read(d,0,5,0xffff)<<8;assert(n && !(n&1));
            for(unsigned i=0;i<n;i+=2){uint16_t v=atapi_cr589_read(d,0,0,0xffff);data.push_back(v);data.push_back(v>>8);}
            now+=1000000;assert(!atapi_cr589_advance_to(d,now));
        }assert(false);return data;
    };
    std::array<uint8_t,12> cmd{};cmd[0]=0x12;cmd[4]=36;packet(cmd);auto identity=input();assert(identity.size()==36 && !memcmp(identity.data()+8,"MATSHITACD-ROM CR-589   GS0N",28));
    std::array<uint8_t,2048> direct{};assert(c.media.read_mode1_sector(c.media.opaque,16,direct.data()));
    cmd={};cmd[0]=0x28;cmd[5]=16;cmd[8]=1;packet(cmd);auto sector=input();assert(sector.size()==2048 && !memcmp(sector.data(),direct.data(),2048));
    cmd={};cmd[0]=0x43;cmd[7]=4;packet(cmd);auto toc=input();assert(toc.size()==596 && toc[2]==1 && toc[3]==73 && toc[6]==1);
    unsigned audio=1;uint32_t lba=c.media.track_start(c.media.opaque,audio);
    cmd={};cmd[0]=0x45;for(unsigned i=0;i<4;i++)cmd[2+i]=lba>>(24-8*i);cmd[8]=75;packet(cmd);
    assert(chd_cd_advance_to(cd,1000000000));
    assert(pcm_frames==44100 && pcm_nonzero && pcm_peak);
    AtapiCr589Status st{};atapi_cr589_get_status(d,&st);assert(!st.faults && st.sectors==1);
    printf("Original DDR2m CR589: header SHA matched, 73 real tracks, source identity, actual LBA16 sector/TOC and CDDA frames=%llu nonzero=%llu peak=%u; faults=0 PASS\n",(unsigned long long)pcm_frames,(unsigned long long)pcm_nonzero,pcm_peak);
    atapi_cr589_destroy(d);chd_cd_close(cd);return 0;
}
