#include "../libpcsxcore/arcade/system573_audio.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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
    free(a);puts("System573 timestamped gain-1 SPU/CDDA sum, stereo, saturation, silence, future deferral and clock/history rejection PASS");return 0;
}
