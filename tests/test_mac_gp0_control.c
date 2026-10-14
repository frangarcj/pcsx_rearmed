/* Verify the production GP0 bridge without invoking PsyCross' retail parser. */
#define ParseTaglessPrimitive gp0_test_primitive
#include "../plugins/gpu_vita_ctr/gpulib_if.c"
#include <assert.h>
static unsigned calls, fills;
int gp0_test_primitive(u32 *words)
{
    if((*words >> 24)==0x02){
        assert(((P_TAG *)(words - P_LEN))->len==3);fills++;return 3;
    }
    /* The old bridge incorrectly forwarded cache clear here as DR_MOVE. */
    assert((*words >> 24) == 0x60);
    assert(((P_TAG *)(words - P_LEN))->len == 3);
    if(calls==0)assert(words[1]==0x00020001 && words[2]==0x00040003);
    else assert(words[1]==0x00060005 && words[2]==0x00080007);
    calls++;
    return 3;
}
int main(void)
{
    uint32_t words[]={0x600000ff,0x00020001,0x00040003,
        0x01000000,0x01abcdef,0x0000fedc,0x1f010101,
        0x6000ff00,0x00060005,0x00080007};
    uint32_t regs[8]={0};int sum=0,last=0,command=0;
    assert(!g_window && !g_glContext);
    assert(gpu_set_model(2)==0);
    s_context_ready=1;
    int consumed=renderer_do_cmd_list(words,sizeof(words)/sizeof(words[0]),regs,&sum,&last,&command);
    assert(consumed==(int)(sizeof(words)/sizeof(words[0])) && calls==2);
    calls=0;
    submit_gp0_packet(words+3,1,regs);
    assert(calls==0);
    uint32_t fill[]={0x020000ff,0x00020001,0x00040010};
    submit_gp0_packet(fill,3,regs);
    assert(fills==1);
    puts("GP0 cache clear/control words stay out of the retail DR_MOVE parser; adjacent draw packets consumed PASS");
    return 0;
}
