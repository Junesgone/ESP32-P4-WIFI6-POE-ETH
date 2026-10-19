#pragma once
/* Echo cancellation, ported from the idea xiaozhi-esp32 uses.
 *
 * Without it the board has to "close its ears" for two seconds every time it
 * speaks (see the suppression window in main.c): it cannot hear you while it is
 * talking, and the tail of its own voice still leaks in as a fresh utterance.
 *
 * ESP-SR's AFE does the real work.  It needs two input channels - the
 * microphone AND a copy of whatever we are currently playing - and returns the
 * microphone signal with our own voice subtracted.  app_audio.c tees every
 * playback frame into a ring buffer, and the capture loop feeds mic+reference
 * pairs here.  AEC_MODE_FD_* is the full-duplex mode, so you can talk over it.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t aec_init(void);
bool aec_ready(void);

/* Samples per feed frame, per channel (both channels are the same length). */
int aec_feed_chunksize(void);

/* Feed one interleaved [mic, ref] frame of aec_feed_chunksize() samples each.
 * Returns the echo-cancelled mono frame, or NULL when nothing is available yet.
 * The buffer stays valid until the next call. */
const int16_t *app_aec_process(const int16_t *interleaved, int *out_samples);
