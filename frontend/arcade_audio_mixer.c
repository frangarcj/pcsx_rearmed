/* GPL-2.0-or-later. Linear resampling at exact oscillator/divider input rates. */
#include "arcade_audio_mixer.h"
#include <math.h>
#include <string.h>
#include <limits.h>

void arcade_mix_reset(ArcadeAudioMixer *mix)
{
    for (unsigned i = 0; i < 2; ++i) {
        unsigned clock = mix->lane[i].clock, divider = mix->lane[i].divider;
        memset(&mix->lane[i], 0, sizeof(mix->lane[i]));
        mix->lane[i].clock = clock; mix->lane[i].divider = divider;
    }
    mix->error = 0;
}

void arcade_mix_init(ArcadeAudioMixer *mix, unsigned spu_clock,
    void (*output)(void *, const int16_t *, unsigned), void *opaque)
{
    arcade_mix_init_rates(mix, spu_clock, 25000000, 768, output, opaque);
}

void arcade_mix_init_rates(ArcadeAudioMixer *mix, unsigned spu_clock,
    unsigned daughter_clock, unsigned daughter_divider,
    void (*output)(void *, const int16_t *, unsigned), void *opaque)
{
    memset(mix, 0, sizeof(*mix));
    mix->lane[0].clock = spu_clock;
    mix->lane[0].divider = 768;
    mix->lane[1].clock = daughter_clock;
    mix->lane[1].divider = daughter_divider;
    mix->gain = 100;
    mix->output = output;
    mix->opaque = opaque;
    if (!spu_clock || !daughter_clock || !daughter_divider || daughter_divider > UINT32_MAX / 44100u || !output) mix->error = 1;
}

static void deliver(ArcadeAudioMixer *mix)
{
    int16_t stereo[256 * 2];
    ArcadeMixLane *a = &mix->lane[0], *b = &mix->lane[1];
    unsigned n = a->count < b->count ? a->count : b->count;
    if (n > 256) n = 256;
    /* Batch callbacks: a Zoom sample must not become an SDL call by itself. */
    if (n < 256) return;
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            int64_t value = ((int64_t)a->ring[a->head][ch] + b->ring[b->head][ch]) * mix->gain / 100;
            if (value > INT16_MAX) { value = INT16_MAX; mix->clipped++; }
            if (value < INT16_MIN) { value = INT16_MIN; mix->clipped++; }
            stereo[i * 2 + ch] = (int16_t)value;
        }
        a->head = (a->head + 1) % ARCADE_MIX_RING;
        b->head = (b->head + 1) % ARCADE_MIX_RING;
    }
    a->count -= n; b->count -= n;
    mix->frames += n;
    mix->output(mix->opaque, stereo, n);
}

void arcade_mix_push(ArcadeAudioMixer *mix, unsigned source, float left, float right)
{
    if (source > 1 || mix->error) return;
    ArcadeMixLane *lane = &mix->lane[source];
    if(lane->timed){mix->error=1;return;}
    const uint64_t output_ticks = (uint64_t)44100 * lane->divider;
    if (!isfinite(left) || !isfinite(right) || !lane->clock ||
        lane->position > UINT64_MAX - output_ticks) { mix->error = 1; return; }
    float current[2] = {left, right};
    if (!lane->inputs) memcpy(lane->previous, current, sizeof(current));
    lane->inputs++;
    lane->nonzero += left != 0 || right != 0;
    while (lane->next_output <= lane->position) {
        if (lane->count == ARCADE_MIX_RING) { mix->error = 1; return; }
        float t = lane->position ? (float)(lane->next_output + output_ticks - lane->position) / output_ticks : 1.0f;
        unsigned tail = (lane->head + lane->count) % ARCADE_MIX_RING;
        for (unsigned ch = 0; ch < 2; ++ch) {
            float value = lane->previous[ch] + t * (current[ch] - lane->previous[ch]);
            if (value > 1) value = 1;
            if (value < -1) value = -1;
            lane->ring[tail][ch] = (int16_t)lrintf(value * 32767.0f);
        }
        lane->count++;
        lane->next_output += lane->clock;
        deliver(mix);
    }
    memcpy(lane->previous, current, sizeof(current));
    lane->position += output_ticks;
}

void arcade_mix_push_timed(ArcadeAudioMixer *mix,unsigned source,float left,float right,uint64_t tick)
{
    if(source>1 || mix->error)return;
    ArcadeMixLane *lane=&mix->lane[source];
    if(!isfinite(left) || !isfinite(right) || !lane->clock || tick>UINT64_MAX/44100u ||
       (lane->inputs && !lane->timed)){mix->error=1;return;}
    uint64_t position=tick*44100u;
    if(lane->inputs && position<=lane->position){mix->error=1;return;}
    float current[2]={left,right};
    if(!lane->inputs)memcpy(lane->previous,current,sizeof(current));
    lane->timed=1;lane->inputs++;lane->nonzero+=left!=0 || right!=0;
    while(lane->next_output<=position){
        if(lane->count==ARCADE_MIX_RING || lane->next_output>UINT64_MAX-lane->clock){mix->error=1;return;}
        float t=position>lane->position?(float)(lane->next_output-lane->position)/(position-lane->position):1;
        unsigned tail=(lane->head+lane->count)%ARCADE_MIX_RING;
        for(unsigned ch=0;ch<2;ch++){
            float v=lane->previous[ch]+t*(current[ch]-lane->previous[ch]);
            if(v>1)v=1;if(v<-1)v=-1;
            lane->ring[tail][ch]=(int16_t)lrintf(v*32767.0f);
        }
        lane->count++;lane->next_output+=lane->clock;deliver(mix);
    }
    memcpy(lane->previous,current,sizeof(current));lane->position=position;
}
