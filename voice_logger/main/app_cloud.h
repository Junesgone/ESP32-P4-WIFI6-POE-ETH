#pragma once
/* ---------------------------------------------------------------------------
 * Local-first voice pipeline.
 *
 *   P4: mic -> VAD -> WAV  --POST-->  bridge on the LAN
 *   bridge: faster-whisper (ASR, local) -> DeepSeek API (text LLM, cloud)
 *           -> local TTS (pyttsx3 / edge-tts)
 *   P4: <--GET-- reply WAV -> speaker
 *
 * Only the *text* ever leaves the LAN; audio stays at home.  DeepSeek itself is
 * a text-only Chat API and offers neither ASR nor TTS, which is exactly why the
 * two ends are split like this.
 * ------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char text[768];    /* what the bridge heard */
    char reply[3072];  /* what DeepSeek answered */
    bool played;       /* reply audio was streamed to the speaker */
} cloud_result_t;

/* Sends one utterance (mono, 16-bit) to the bridge and plays the reply.
 * Never blocks forever: the bridge URL is in menuconfig. */
esp_err_t cloud_ask(const int16_t *mono, size_t samples, uint32_t sample_rate,
                    cloud_result_t *out);
