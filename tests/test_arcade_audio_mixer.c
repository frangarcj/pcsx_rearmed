/* GPL-2.0-or-later. */
#include "../frontend/arcade_audio_mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static unsigned frames, mode;
static void output(void *opaque, const int16_t *pcm, unsigned n)
{
    (void)opaque;
    assert(n == 256);
    for (unsigned i = 0; i < n * 2; ++i) {
        if (!mode) assert(pcm[i] == 4096);
        if (mode == 1) assert(pcm[i] == 32767);
        if (mode == 2) assert(pcm[i] == 0);
    }
    frames += n;
}
static void second(ArcadeAudioMixer *mix, unsigned spu_clock, float a, float b)
{
    /* Chronological input events at exact rational source rates, including
     * the asynchronous batching of the SPU callback relative to Zoom. */
    unsigned ns[2] = {spu_clock / 768, 25000000 / 768};
    unsigned i[2] = {0,0};
    while (i[0] < ns[0] || i[1] < ns[1]) {
        unsigned lane = i[0] == ns[0] ? 1 : i[1] == ns[1] ? 0 :
            (uint64_t)i[0] * 25000000 <= (uint64_t)i[1] * spu_clock ? 0 : 1;
        arcade_mix_push(mix, lane, lane ? b : a, lane ? b : a); i[lane]++;
    }
    assert(!mix->error && frames == 44032);
    assert(mix->lane[0].count < 256 && mix->lane[1].count < 256);
}
int main(void)
{
    ArcadeAudioMixer mix;
    for (unsigned clock = 33868800; clock <= 50000000; clock += 16131200) {
        frames = mode = 0;
        arcade_mix_init(&mix,clock,output,NULL);
        second(&mix,clock,.25f,-.125f);
        assert(mix.clipped == 0);
        /* Reset discards both residual queues and interpolation history. */
        arcade_mix_reset(&mix); frames = 0; mode = 1;
        second(&mix,clock,1,1); assert(mix.clipped == frames * 2);
        arcade_mix_reset(&mix); frames = 0; mode = 2;
        second(&mix,clock,1,-1);
    }
    /* Raizing has two exact 44.1 kHz streams: SPU/768 and YMF271/384.
     * A ramp catches an accidental Zoom divisor and phase/interpolation drift. */
    frames = mode = 0;
    arcade_mix_init_rates(&mix,33868800,16934400,384,output,NULL);
    for (unsigned i = 0; i < 44100; ++i) {
        float ramp = (i % 256) / 1024.0f;
        arcade_mix_push(&mix,0,.25f + ramp,.25f + ramp);
        arcade_mix_push(&mix,1,-.125f - ramp,-.125f - ramp);
    }
    assert(frames == 44032 && mix.lane[0].inputs == mix.lane[1].inputs && !mix.error);
    arcade_mix_reset(&mix); assert(mix.lane[1].divider == 384);
    frames = mode = 0;
    arcade_mix_init_rates(&mix,33868800,16934400,192,output,NULL);
    for (unsigned i = 0; i < 44100; ++i) {
        arcade_mix_push(&mix,0,.25f,.25f);
        arcade_mix_push(&mix,1,-.125f,-.125f);
        arcade_mix_push(&mix,1,-.125f,-.125f);
    }
    assert(frames==44032 && mix.lane[1].inputs==88200 && !mix.error);
    arcade_mix_reset(&mix); assert(mix.lane[1].divider==192);
    /* A variable-rate DAC keeps one absolute epoch across clock-divider
     * changes. Include the authentic RAX 48k -> 31571 Hz startup transition. */
    frames=mode=0;
    arcade_mix_init_rates(&mix,33868800,16670000,1,output,NULL);
    unsigned spu=0,dac=1;uint64_t tick=(16670000u+47999)/48000;
    while(spu<44100 || tick<16670000u){
        if(spu<44100 && (tick>=16670000u || (uint64_t)spu*16670000u<=tick*44100u)){
            arcade_mix_push(&mix,0,.25f,.25f);spu++;
        }else{
            arcade_mix_push_timed(&mix,1,-.125f,-.125f,tick);dac++;
            if(tick<15058){tick=((uint64_t)dac*16670000u+47999)/48000;if(tick>=15058){dac=1;tick=15058+(16670000u+31570)/31571;}}
            else tick=15058+((uint64_t)dac*16670000u+31570)/31571;
        }
    }
    assert(!mix.error && frames==44032 && mix.lane[1].timed);
    arcade_mix_push_timed(&mix,1,0,0,mix.lane[1].position/44100u);assert(mix.error);
    arcade_mix_reset(&mix);arcade_mix_push_timed(&mix,1,0,0,1);
    arcade_mix_push(&mix,1,0,0);assert(mix.error);
    arcade_mix_init(&mix,33868800,output,NULL);
    arcade_mix_push(&mix,1,NAN,0); assert(mix.error);
    arcade_mix_reset(&mix);
    for (unsigned i = 0; i < 4 * ARCADE_MIX_RING; ++i) arcade_mix_push(&mix,1,0,0);
    assert(mix.error); /* producer imbalance fails instead of dropping samples */
    puts("Exact source-clock resampling, stereo sum, clipping, reset epochs and overflow/NaN checks PASS");
}
