/* GPL-2.0-or-later. Gain-1 speaker routes from MAME ksys573.cpp. Legacy
 * integer paths stay exact; digital floats join before final saturation.
 * Timestamped buffering is host output plumbing. */
#include "system573_audio.h"
#include <string.h>
#include <limits.h>
#include <math.h>
void system573_audio_init(System573Audio *a){memset(a,0,sizeof(*a));}
int system573_audio_enable_digital(System573Audio *a){
    if(!a || a->started || a->cd_inputs || a->spu_inputs || a->digital_inputs)return -1;
    a->digital_enabled=1;return 0;
}
void system573_audio_set_sink(System573Audio *a,system573_audio_sink_t sink,void *opaque){
    a->sink=sink;a->opaque=opaque;if(sink)a->started=0;
}
static void deliver(System573Audio *a,int force){
    if(!a->sink || !a->started || a->faults)return;
    uint64_t end=a->spu_next<a->cd_next?a->spu_next:a->cd_next;
    if(a->digital_enabled && a->digital_next<end)end=a->digital_next;
    while(end>a->next){
        uint64_t ready=end-a->next;if(ready<256 && !force)break;
        unsigned n=ready>256?256:(unsigned)ready;int16_t data[512];
        for(unsigned i=0;i<n;i++,a->next++)for(unsigned ch=0;ch<2;ch++){
            unsigned slot=a->next%SYSTEM573_AUDIO_RING;
            int value=(int)a->spu[slot][ch]+a->cd[slot][ch];
            if(a->digital_enabled){
                float sum=(float)value+a->digital[slot][ch]*32768.0f;
                if(sum>INT16_MAX){sum=INT16_MAX;a->clipped++;}
                if(sum<INT16_MIN){sum=INT16_MIN;a->clipped++;}
                value=(int)sum; /* Same truncation as source float -> PCM output. */
            }
            if(value>INT16_MAX){value=INT16_MAX;a->clipped++;}
            if(value<INT16_MIN){value=INT16_MIN;a->clipped++;}
            data[2*i+ch]=value;unsigned peak=value<0?-value:value;if(peak>a->peak)a->peak=peak;
        }
        a->frames+=n;a->sink(a->opaque,data,n);
    }
}
int system573_audio_digital(System573Audio *a,uint64_t index,float left,float right){
    if(!a || a->faults)return -1;
    if(!a->digital_enabled || !isfinite(left) || !isfinite(right) || index!=a->digital_next ||
       (a->sink && a->started && index>=a->next && index-a->next>=SYSTEM573_AUDIO_RING)){
        a->faults|=8;return -1;
    }
    unsigned slot=index%SYSTEM573_AUDIO_RING;a->digital[slot][0]=left;a->digital[slot][1]=right;
    a->digital_next++;a->digital_inputs++;a->digital_nonzero+=left!=0 || right!=0;
    deliver(a,0);return a->faults?-1:0;
}
int system573_audio_cd(System573Audio *a,uint64_t index,int16_t left,int16_t right){
    if(!a || a->faults)return -1;
    if(index!=a->cd_next || (a->sink && a->started && index>=a->next && index-a->next>=SYSTEM573_AUDIO_RING)){
        a->faults|=1;return -1;
    }
    unsigned slot=index%SYSTEM573_AUDIO_RING;a->cd[slot][0]=left;a->cd[slot][1]=right;
    a->cd_next++;a->cd_inputs++;a->cd_nonzero+=left!=0 || right!=0;deliver(a,0);return a->faults?-1:0;
}
int system573_audio_spu(System573Audio *a,uint64_t first,const int16_t *stereo,unsigned frames){
    if(!a || a->faults || (!stereo && frames))return -1;
    if(!frames || !a->sink)return 0;
    if(frames>SYSTEM573_AUDIO_RING || first>UINT64_MAX-frames){a->faults|=2;return -1;}
    if(!a->started){
        if(first<a->cd_next && a->cd_next-first>SYSTEM573_AUDIO_RING){a->faults|=4;return -1;}
        if(a->digital_enabled && first<a->digital_next && a->digital_next-first>SYSTEM573_AUDIO_RING){a->faults|=4;return -1;}
        a->next=a->spu_next=first;a->started=1;
    }
    if(first!=a->spu_next || first+frames-a->next>SYSTEM573_AUDIO_RING){a->faults|=2;return -1;}
    for(unsigned i=0;i<frames;i++){
        unsigned slot=(first+i)%SYSTEM573_AUDIO_RING;
        a->spu[slot][0]=stereo[2*i];a->spu[slot][1]=stereo[2*i+1];
        a->spu_nonzero+=stereo[2*i]!=0 || stereo[2*i+1]!=0;
    }
    a->spu_inputs+=frames;a->spu_next=first+frames;deliver(a,0);return a->faults?-1:0;
}
int system573_audio_flush(System573Audio *a){if(!a)return -1;deliver(a,1);return a->faults?-1:0;}
