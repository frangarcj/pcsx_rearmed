#include "../libpcsxcore/arcade/system573_audio.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static int16_t output[40000][2];static unsigned used;
static void sink(void *opaque,const int16_t *pcm,unsigned frames){
    (void)opaque;assert(used+frames<=40000);memcpy(output+used,pcm,frames*4);used+=frames;
}
int main(void){
    System573Audio *a=calloc(1,sizeof(*a));assert(a);system573_audio_init(a);system573_audio_set_sink(a,sink,NULL);
    int16_t samples[1200];for(unsigned i=0;i<600;i++){samples[2*i]=20000;samples[2*i+1]=-20000;}
    assert(!system573_audio_spu(a,0,samples,600));assert(used==0); // no invented CD frames
    for(unsigned i=0;i<300;i++)assert(!system573_audio_cd(a,i,20000,-20000));
    assert(used==256 && output[0][0]==32767 && output[0][1]==-32768);
    for(unsigned i=300;i<599;i++)assert(!system573_audio_cd(a,i,0,0));
    assert(!system573_audio_flush(a) && used==599 && a->spu_next-a->next==1);
    assert(output[300][0]==20000 && output[300][1]==-20000);
    assert(!system573_audio_cd(a,599,0,0));assert(!system573_audio_flush(a));
    assert(used==600 && a->clipped==600 && a->peak==32768 && !a->faults);
    /* Late attachment: unobserved history may roll; real recent epochs align. */
    system573_audio_init(a);used=0;
    for(unsigned i=0;i<35000;i++)assert(!system573_audio_cd(a,i,123,-456));
    system573_audio_set_sink(a,sink,NULL);assert(!system573_audio_spu(a,34990,samples,10));
    assert(!system573_audio_flush(a) && used==10 && output[0][0]==20123 && output[0][1]==-20456);
    /* Missing/duplicated clock samples and overwritten needed history fail. */
    assert(system573_audio_spu(a,35001,samples,1)==-1 && a->faults==2);
    system573_audio_init(a);assert(system573_audio_cd(a,1,0,0)==-1 && a->faults==1);
    system573_audio_init(a);used=0;system573_audio_set_sink(a,sink,NULL);
    assert(!system573_audio_spu(a,0,samples,1));
    for(unsigned i=0;i<SYSTEM573_AUDIO_RING;i++)assert(!system573_audio_cd(a,i,0,0));
    assert(system573_audio_cd(a,SYSTEM573_AUDIO_RING,0,0)==-1 && a->faults==1);
    system573_audio_init(a);used=0;
    for(unsigned i=0;i<35000;i++)assert(!system573_audio_cd(a,i,0,0));
    system573_audio_set_sink(a,sink,NULL);
    assert(system573_audio_spu(a,0,samples,1)==-1 && a->faults==4);
    /* All three source clocks must arrive; keep digital float gain until the
     * final sum so early saturation does not change the actual speaker mix. */
    system573_audio_init(a);used=0;assert(!system573_audio_enable_digital(a));system573_audio_set_sink(a,sink,NULL);
    int16_t quiet[4]={1000,-1000,20000,-20000};assert(!system573_audio_spu(a,0,quiet,2));
    assert(!system573_audio_cd(a,0,1000,-1000));assert(!system573_audio_cd(a,1,20000,-20000));
    assert(!system573_audio_flush(a) && !used);
    assert(!system573_audio_digital(a,0,.5f,-.5f));assert(!system573_audio_flush(a) && used==1);
    assert(output[0][0]==18384 && output[0][1]==-18384);
    assert(!system573_audio_digital(a,1,-1.0f,1.0f));assert(!system573_audio_flush(a) && used==2);
    assert(output[1][0]==7232 && output[1][1]==-7232 && !a->clipped);
    assert(a->digital_inputs==2 && a->digital_nonzero==2 && !a->faults);
    assert(system573_audio_digital(a,2,NAN,0)==-1 && a->faults==8);
    system573_audio_init(a);used=0;assert(!system573_audio_enable_digital(a));system573_audio_set_sink(a,sink,NULL);
    assert(!system573_audio_spu(a,0,quiet,2));assert(!system573_audio_cd(a,0,0,0));assert(!system573_audio_cd(a,1,20000,-20000));
    assert(!system573_audio_digital(a,0,.5f/32768.0f,-.5f/32768.0f));
    assert(!system573_audio_digital(a,1,.75f,-.75f));assert(!system573_audio_flush(a));
    assert(output[0][0]==1000 && output[0][1]==-1000); // Fractional PCM truncates, never rounds to an extra sample unit.
    assert(output[1][0]==32767 && output[1][1]==-32768 && a->clipped==2);
    system573_audio_init(a);assert(!system573_audio_enable_digital(a));
    assert(system573_audio_digital(a,1,0,0)==-1 && a->faults==8);
    system573_audio_init(a);assert(system573_audio_digital(a,0,0,0)==-1);
    system573_audio_init(a);used=0;assert(!system573_audio_enable_digital(a));
    for(unsigned i=0;i<35000;i++){assert(!system573_audio_cd(a,i,1,2));assert(!system573_audio_digital(a,i,.25f,-.25f));}
    system573_audio_set_sink(a,sink,NULL);assert(!system573_audio_spu(a,34990,quiet,2));
    assert(!system573_audio_flush(a) && used==2 && output[0][0]==9193 && output[0][1]==-9190);
    system573_audio_init(a);used=0;assert(!system573_audio_enable_digital(a));system573_audio_set_sink(a,sink,NULL);
    assert(!system573_audio_spu(a,0,quiet,1));
    for(unsigned i=0;i<SYSTEM573_AUDIO_RING;i++)assert(!system573_audio_digital(a,i,0,0));
    assert(system573_audio_digital(a,SYSTEM573_AUDIO_RING,0,0)==-1 && a->faults==8);
    /* GV's source swaps SPU only, at 0.75; CDDA retains its channel order.
     * Fractional SPU values must survive until the final cancellation. */
    system573_audio_init(a);used=0;assert(!system573_audio_spu_route(a,.75f,1));system573_audio_set_sink(a,sink,NULL);
    int16_t gv[4]={1,-1,32767,-32768};assert(!system573_audio_spu(a,0,gv,2));
    assert(!system573_audio_cd(a,0,1,-1));assert(!system573_audio_cd(a,1,-10000,10000));
    assert(!system573_audio_flush(a) && used==2);
    assert(output[0][0]==0 && output[0][1]==0);
    assert(output[1][0]==-32768 && output[1][1]==32767 && a->clipped==2);
    assert(system573_audio_spu_route(a,1,0)==-1);
    system573_audio_init(a);assert(system573_audio_spu_route(a,NAN,0)==-1);
    assert(system573_audio_spu_route(a,.75f,2)==-1);
    /* Twinkle keeps SPU/CD/RF channels direct and RF wide until the final
     * sum. A lagging RF event must retain the actual pending SPU sample. */
    system573_audio_init(a);used=0;assert(!system573_audio_spu_route(a,.75f,0));assert(!system573_audio_enable_digital(a));system573_audio_set_sink(a,sink,NULL);
    int16_t twinkle[6]={1,-1,20000,-20000,32767,-32768};
    assert(!system573_audio_spu(a,0,twinkle,3));
    assert(!system573_audio_cd(a,0,0,0));assert(!system573_audio_cd(a,1,20000,-20000));assert(!system573_audio_cd(a,2,0,0));
    assert(!system573_audio_flush(a) && !used);
    assert(!system573_audio_digital(a,0,-.75f/32768.0f,.75f/32768.0f));
    assert(!system573_audio_digital(a,1,-40000.0f/32768.0f,40000.0f/32768.0f));assert(!system573_audio_flush(a));
    assert(used==2 && a->spu_next-a->next==1 && !a->faults && !a->clipped);
    assert(output[0][0]==0 && output[0][1]==0 && output[1][0]==-5000 && output[1][1]==5000);
    assert(!system573_audio_digital(a,2,1.0f,-1.0f));assert(!system573_audio_flush(a));
    assert(used==3 && output[2][0]==32767 && output[2][1]==-32768 && a->clipped==2);
    free(a);puts("System573/GV/Twinkle timestamped source routes, float cancellation, saturation, source deferral and history rejection PASS");return 0;
}
