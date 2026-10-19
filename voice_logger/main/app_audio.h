#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_CHANNELS      2      /* I2S runs stereo; storage keeps mono */

esp_err_t audio_init(void);
esp_err_t audio_set_volume(int volume_0_100);
int       audio_get_volume(void);
void      audio_set_mic_gain_db(int gain_db);

/* Reads interleaved stereo frames, blocking up to timeout_ms. */
esp_err_t audio_read(int16_t *dst, size_t bytes, size_t *bytes_read, uint32_t timeout_ms);
/* Writes interleaved stereo frames. */
esp_err_t audio_write(const int16_t *src, size_t bytes);

/* Playback reference for echo cancellation: audio_write() mirrors every frame
 * it plays into a small ring, and this pops exactly the samples the AEC needs,
 * zero-filling on underrun so mic and reference stay time-aligned. */
int audio_ref_pop(int16_t *out, int n);

/* Delay the playback reference by n samples (0..3200) so it lines up with what
 * the microphone actually hears.  Set by the 'calib' console command. */
void audio_ref_set_delay(int samples);
int  audio_ref_get_delay(void);

/* Discards whatever is pending in the RX DMA ring (used after a long SD write). */
void audio_drain_rx(void);

/* Average the two interleaved channels into a mono buffer. */
void audio_downmix(const int16_t *stereo, size_t stereo_samples, int16_t *mono);

/* Square wave straight to the DAC path - no SD card, no network needed. */
esp_err_t audio_write_tone(int freq_hz, int ms, int amplitude);
