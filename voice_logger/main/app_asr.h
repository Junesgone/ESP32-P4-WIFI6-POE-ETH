/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* Chinese speech-to-text for the voice logger.
 *
 * The P4 cannot host open-vocabulary ASR (ESP-SR only does wake word + command
 * words, ESP-DL has no speech model, and a Whisper/Paraformer-class model is
 * 20-200x too large for 32 MB of PSRAM).  So this module posts the captured WAV
 * to a cloud endpoint and gets text back - no extra device on the LAN, the
 * on-board ESP32-C6 Wi-Fi is enough.
 *
 * Default backend: SiliconFlow  POST /v1/audio/transcriptions
 *   multipart: model=FunAudioLLM/SenseVoiceSmall + file=<wav>
 *   response:  {"text": "..."}
 *
 * Backends are swappable through menuconfig (ASR_BASE_URL / ASR_MODEL), so a
 * self-hosted FunASR/Paraformer service can be dropped in later by pointing the
 * URL at the LAN box instead.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Sends mono 16-bit PCM and stores the transcription.
 * out_text is always NUL-terminated, empty on failure. */
esp_err_t asr_transcribe(const int16_t *mono, size_t samples, uint32_t sample_rate,
                         char *out_text, size_t out_len);

bool asr_configured(void);
