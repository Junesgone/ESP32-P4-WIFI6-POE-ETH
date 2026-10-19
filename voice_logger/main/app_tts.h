#pragma once
/* Text-to-speech for 小新.
 *
 * Two engines behind one interface:
 *   - ESP-SR Chinese TTS (on the board, always available offline, sounds
 *     concatenative/robotic - the fallback)
 *   - SiliconFlow CosyVoice2 neural TTS (natural voice, needs Wi-Fi, see
 *     app_tts_cloud.h)
 *
 * Both produce mono 16 kHz 16-bit PCM, which is exactly the format the
 * ES8311 / NS4150B path already runs at.  Playback happens on its own task, so
 * tts_say() returns as soon as the clip is queued and the capture loop keeps
 * running (that is what makes talking over the top possible).
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TTS_ENGINE_AUTO = 0,    /* cloud when reachable, ESP-SR otherwise (default) */
    TTS_ENGINE_CLOUD,       /* prefer cloud, still fall back if it fails */
    TTS_ENGINE_LOCAL,       /* ESP-SR only - offline mode / A/B listening test */
} tts_engine_t;

esp_err_t tts_init(void);
bool tts_ready(void);

/* True when the current reply should be spoken after the LLM stream instead of
 * sentence-by-sentence: an HTTPS round trip per sentence cannot overlap the
 * streaming DeepSeek connection on this board (internal RAM), so cloud replies
 * are synthesized as one clip once the stream has closed. */
bool tts_uses_cloud(void);

/* Queues UTF-8 text to be spoken.  Blocks only while the audio is produced
 * (cloud round trip or local render), NOT while it plays. */
esp_err_t tts_say(const char *utf8_text);

/* True while audio is actually coming out of the speaker. */
bool tts_speaking(void);

/* Drops anything queued (a new reply supersedes the old one). */
void tts_stop(void);

/* Duration of the clip tts_say() last queued, in ms (0 if nothing queued). */
uint32_t tts_last_clip_ms(void);

/* ESP-SR local voice speed, 0 (slowest) .. 5 (fastest). */
esp_err_t tts_set_speed(unsigned speed);
unsigned tts_local_speed(void);

/* Runtime engine selection (console: "tts auto|cloud|local"). */
void tts_set_engine(tts_engine_t engine);
tts_engine_t tts_get_engine(void);
const char *tts_engine_name(void);
