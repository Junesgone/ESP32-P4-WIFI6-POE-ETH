#include "app_vad.h"
#include <math.h>

int vad_rms(const int16_t *samples, size_t count)
{
    if (!count) {
        return 0;
    }
    double acc = 0.0;
    for (size_t i = 0; i < count; i++) {
        double s = samples[i];
        acc += s * s;
    }
    return (int)sqrt(acc / (double)count);
}

void vad_init(vad_t *v, uint32_t sample_rate, int start_rms,
              int hangover_ms, uint32_t min_speech_ms, uint32_t max_utterance_ms)
{
    *v = (vad_t){
        .sample_rate = sample_rate,
        .start_rms = start_rms,
        .hangover_ms = hangover_ms,
        .min_speech_ms = min_speech_ms,
        .max_utterance_ms = max_utterance_ms,
        .noise_floor = 0,
        .cur_threshold = start_rms,
    };
}

bool vad_in_speech(const vad_t *v)
{
    return v->in_speech;
}

int vad_threshold(const vad_t *v)
{
    return v->cur_threshold;
}

/* Adaptive gate.
 *
 * A fixed 800 threshold was tuned for a quiet room (noise floor 20-130), but
 * this board also sees periods with rms 600-950 (fans, traffic, a TV in the
 * next room).  Then the gate opens on noise, ASR gets a clip with no speech and
 * returns an empty transcript, and the turn dies silently.
 *
 * Track the quiet baseline instead:
 *   - drop instantly when a quieter frame arrives (proof the room can be quiet),
 *   - creep up slowly (divided by 128) so a loud passage cannot deafen us.
 * The gate is 3x that baseline, never below the configured start_rms and never
 * more than 4x it (so a genuinely loud room still lets speech through). */
static int adaptive_threshold(vad_t *v, int rms)
{
    if (v->noise_floor <= 0 || rms < v->noise_floor) {
        v->noise_floor = rms;
    } else {
        v->noise_floor += (rms - v->noise_floor) / 128;
    }
    int th = v->noise_floor * 3;
    if (th < v->start_rms) {
        th = v->start_rms;
    }
    if (th > v->start_rms * 4) {
        th = v->start_rms * 4;
    }
    return th;
}

vad_event_t vad_feed(vad_t *v, const int16_t *samples, size_t count, int *out_rms)
{
    int rms = vad_rms(samples, count);
    uint32_t frame_ms = (uint32_t)((count * 1000ULL) / v->sample_rate);
    if (frame_ms == 0) {
        frame_ms = 20;
    }
    if (out_rms) {
        *out_rms = rms;
    }

    if (!v->in_speech) {
        v->cur_threshold = adaptive_threshold(v, rms);
        if (rms >= v->cur_threshold) {
            v->in_speech = true;
            v->speech_ms = frame_ms;
            v->silence_ms = 0;
        }
        return VAD_EVENT_NONE;
    }

    if (rms >= v->cur_threshold) {
        v->speech_ms += frame_ms;
        v->silence_ms = 0;
    } else {
        v->silence_ms += frame_ms;
    }

    bool too_long = v->speech_ms >= v->max_utterance_ms;
    bool trailing = v->silence_ms >= (uint32_t)v->hangover_ms;

    if (too_long || trailing) {
        bool keep = (v->speech_ms >= v->min_speech_ms);
        v->in_speech = false;
        v->speech_ms = 0;
        v->silence_ms = 0;
        return keep ? VAD_EVENT_UTTERANCE_DONE : VAD_EVENT_NONE;
    }
    return VAD_EVENT_NONE;
}
