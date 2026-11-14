/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* Cloud neural TTS through the DeepSeek standalone Text-to-Speech API.
 *
 * Why this exists: the on-board ESP-SR engine is concatenative, so every reply
 * has the same stiff, roughly 2010-style cadence no matter what the text says.
 * A neural voice fixes exactly the complaint "说话有点僵硬".
 *
 * Endpoints (one voice set, two voices: "stella" female / "tide" male):
 *   - WebSocket  wss://api.deepseek.com/v1/tools/tts?voice_id=<id>&format=pcm
 *     (primary path when CONFIG_VOICE_TTS_WS).  Text frames
 *     {"event":"text","text":"..."} + {"event":"finish_text"}, binary frames
 *     back are raw s16le mono PCM at 24 kHz.
 *   - HTTP POST /v1/tools/tts  {"text","voice_id","format":"pcm"} - chunked
 *     response, same raw 24 kHz PCM; used as the fallback when the WebSocket
 *     handshake or synthesis fails before any audio, and as the only path when
 *     CONFIG_VOICE_TTS_WS is off.
 *
 * The board's ES8311 / NS4150B path runs at 16 kHz, so this file owns the
 * 24 kHz -> 16 kHz decimation (integer 2:3 linear interpolation; state carried
 * across arbitrary chunk boundaries) and trims the leading digital silence.
 * The sink always receives mono 16-bit PCM at 16 kHz.
 *
 * The local ESP-SR engine stays as the offline fallback: no Wi-Fi, no key, no
 * internet or a failed request simply falls back to it (app_tts.c).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Compiled in AND an API key is present. */
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
 * Accepts "stella" (female) or "tide" (male); anything else is ignored.
 * An empty string restores CONFIG_TTS_VOICE_ID. */
void tts_cloud_set_voice(const char *voice);

/* Synthesizes UTF-8 text into mono 16-bit PCM at 16 kHz (batch path, used when
 * CONFIG_VOICE_TTS_STREAM is off).  One POST /v1/tools/tts with format=pcm.
 * On ESP_OK the caller owns where *pcm_out points (PSRAM) and must free() it.
 * On failure nothing is allocated.  Blocks for the whole HTTPS round trip. */
esp_err_t tts_cloud_synthesize(const char *utf8_text, int16_t **pcm_out, size_t *samples_out);

/* Streaming synthesis: hands every resampled 16 kHz PCM block to sink() as
 * soon as it arrives, so playback can start while the rest is still being
 * generated.  Leading digital silence is trimmed before the first block is
 * handed over.  Blocks until the audio ends (or the sink returns an error);
 * returns ESP_OK when at least one audio sample was delivered, so partial
 * audio still counts as a spoken reply.  The sink runs in the caller's task
 * (POST fallback) or in the WebSocket client task (WS path).
 *
 * WebSocket specifics (see _api_probe/REPORT.md section 5): the server sends
 * no PCM before finish_text, the first frame arrives 0.83-1.68 s after it, the
 * bulk often arrives much faster than realtime, and the closing finish event
 * (plus an optional last partial frame) only shows up ~12 s later - so the end
 * of the audio is detected by a short idle gap, never by blocking on finish. */
typedef esp_err_t (*tts_cloud_sink_t)(const int16_t *pcm, size_t samples, void *ctx);

esp_err_t tts_cloud_stream(const char *utf8_text, tts_cloud_sink_t sink, void *ctx);
