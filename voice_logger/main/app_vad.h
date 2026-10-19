#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Energy based voice activity detector, one 20 ms frame at a time. */
typedef enum {
    VAD_EVENT_NONE = 0,
    VAD_EVENT_UTTERANCE_DONE,
} vad_event_t;

typedef struct {
    uint32_t sample_rate;
    int      start_rms;        /* open above this */
    int      hangover_ms;      /* trailing silence that closes an utterance */
    uint32_t min_speech_ms;    /* discard short blips */
    uint32_t max_utterance_ms; /* hard cut-off */
    /* internal state */
    bool     in_speech;
    uint32_t speech_ms;
    uint32_t silence_ms;
    int      noise_floor;     /* tracked quiet baseline (adaptive gate) */
    int      cur_threshold;   /* gate actually in use for this utterance */
} vad_t;

void vad_init(vad_t *v, uint32_t sample_rate, int start_rms,
              int hangover_ms, uint32_t min_speech_ms, uint32_t max_utterance_ms);

/* Feed one frame.  Returns an event; *out_rms receives the frame RMS. */
vad_event_t vad_feed(vad_t *v, const int16_t *samples, size_t count, int *out_rms);

bool vad_in_speech(const vad_t *v);

/* Gate currently in force (configured minimum or noise-floor derived). */
int vad_threshold(const vad_t *v);

/* RMS of a block of 16-bit samples. */
int vad_rms(const int16_t *samples, size_t count);
