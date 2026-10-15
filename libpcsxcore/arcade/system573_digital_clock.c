/* GPL-2.0-or-later. Clock plumbing around the source FPGA/MAS model. */
#include "system573_digital_clock.h"
#include <math.h>
#define CLOCK_UNITS 169344000u
#define OUTPUT_STEP 3840u
static int period(unsigned rate,unsigned *value){
    switch(rate){case 8000:case 11025:case 12000:case 16000:case 22050:
    case 24000:case 32000:case 44100:case 48000:*value=CLOCK_UNITS/rate;return 0;default:return -1;}
}
static int nanoseconds(uint64_t tick,uint64_t *out){
    uint64_t seconds=tick/CLOCK_UNITS;
    if(seconds>(UINT64_MAX-1000000000u)/1000000000u)return -1;
    *out=seconds*1000000000u+(tick%CLOCK_UNITS)*1000000000u/CLOCK_UNITS;return 0;
}
int system573_digital_clock_init(System573DigitalClock *c,System573Digital *chip,system573_digital_pcm_t sink,void *opaque){
    if(!c || !chip || !sink || c->initialized)return -1;
    System573DigitalStatus s;system573_digital_get_status(chip,&s);unsigned step;
    if(s.now_ns || s.faults || period(s.decoder.sample_rate,&step))return -1;
    c->chip=chip;c->sink=sink;c->opaque=opaque;c->rate=s.decoder.sample_rate;
    c->next_input=step;c->next_output=OUTPUT_STEP;c->initialized=1;return 0;
}
int system573_digital_clock_advance(System573DigitalClock *c,uint64_t cycles){
    if(!c || !c->initialized)return -1;
    if(c->busy){c->faults|=8;return -1;}
    if(c->faults)return -1;
    if(cycles>UINT64_MAX/5 || cycles*5<c->target){c->faults|=1;return -1;}
    uint64_t target=cycles*5,ns;c->busy=1;
    while(c->next_input<=target && !c->faults){
        uint64_t tick=c->next_input;float pcm[2];
        if(nanoseconds(tick,&ns) || system573_digital_advance_to(c->chip,ns) || system573_digital_render(c->chip,pcm,1)){
            c->faults|=2;break;
        }
        System573DigitalStatus s;system573_digital_get_status(c->chip,&s);unsigned step;
        if(period(s.decoder.sample_rate,&step) || !isfinite(pcm[0]) || !isfinite(pcm[1]) || tick>UINT64_MAX-step){
            c->faults|=2;break;
        }
        c->inputs++;c->nonzero+=pcm[0]!=0 || pcm[1]!=0;
        if(c->rate!=s.decoder.sample_rate){c->rate=s.decoder.sample_rate;c->rate_changes++;}
        while(c->next_output<=tick && !c->faults){
            if(c->next_output<c->previous_tick || c->next_output>UINT64_MAX-OUTPUT_STEP){c->faults|=4;break;}
            float t=(float)(c->next_output-c->previous_tick)/(float)(tick-c->previous_tick);
            float left=c->next_output==tick?pcm[0]:c->previous[0]+t*(pcm[0]-c->previous[0]);
            float right=c->next_output==tick?pcm[1]:c->previous[1]+t*(pcm[1]-c->previous[1]);
            uint64_t index=c->outputs++;c->next_output+=OUTPUT_STEP;
            if(c->sink(c->opaque,index,left,right))c->faults|=16;
        }
        c->previous_tick=tick;c->previous[0]=pcm[0];c->previous[1]=pcm[1];c->next_input=tick+step;
    }
    if(!c->faults && (nanoseconds(target,&ns) || system573_digital_advance_to(c->chip,ns)))c->faults|=2;
    if(!c->faults)c->target=target;c->busy=0;return c->faults?-1:0;
}
