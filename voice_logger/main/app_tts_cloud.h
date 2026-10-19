#pragma once
/* Cloud neural TTS through the SiliconFlow "audio/speech" endpoint.
 *
 * Why this exists: the on-board ESP-SR engine is concatenative, so every reply
 * has the same stiff, roughly 2010-style cadence no matter what the text says.
 * A neural voice fixes exactly the complaint "说话有点僵硬".  The request asks
 * for raw mono PCM at 16 kHz, which is bit-for-bit the format the ES8311 /
 * NS4150B path already consumes, so no decoder or resampler is needed.
 *
 * The local ESP-SR engine stays as the offline fallback: no Wi-Fi, no key, no
 * internet or a failed request simply falls back to it (app_tts.c).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Compiled in AND a SiliconFlow key is present. */
bool tts_cloud_configured(void);

/* Configured, not switched off at runtime, and Wi-Fi is up. */
bool tts_cloud_active(void);

/* Runtime A/B switch (console: "tts cloud" / "tts local" / "tts auto"). */
void tts_cloud_set_user_enabled(bool on);
bool tts_cloud_user_enabled(void);

/* Human-readable configuration for the console status line. */
const char *tts_cloud_model(void);
const char *tts_cloud_voice(void);

/* Runtime voice switch for auditioning without a rebuild.
 * Accepts "claire" (the model prefix is added) or a full
 * "FunAudioLLM/CosyVoice2-0.5B:claire" id. */
void tts_cloud_set_voice(const char *voice);

/* Synthesizes UTF-8 text into mono 16-bit PCM at 16 kHz.
 * On ESP_OK the caller owns where *pcm_out points (PSRAM) and must free() it.
 * On failure nothing is allocated.  Blocks for the whole HTTPS round trip. */
esp_err_t tts_cloud_synthesize(const char *utf8_text, int16_t **pcm_out, size_t *samples_out);

/* Streaming variant: POSTs with "stream": true and hands every PCM chunk to
 * sink() as soon as it arrives, so playback can start long before the whole
 * sentence has been generated.  Leading digital silence is trimmed before the
 * first chunk is handed over.  Blocks until the response ends (or the sink
 * returns an error); returns ESP_OK when at least one audio sample was
 * delivered.  The sink runs in the caller's task. */
typedef esp_err_t (*tts_cloud_sink_t)(const int16_t *pcm, size_t samples, void *ctx);

esp_err_t tts_cloud_stream(const char *utf8_text, tts_cloud_sink_t sink, void *ctx);
