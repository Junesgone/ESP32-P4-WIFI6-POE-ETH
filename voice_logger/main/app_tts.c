#include "app_tts.h"
#include "app_audio.h"
#include "app_conversation.h"
#include "app_tts_cloud.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_tts.h"
#include "esp_tts_voice_template.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "tts";

static esp_tts_handle_t s_tts;
static unsigned s_speed = 3;
static const void *s_voice_mapped;
static void *s_voice_copy;
static tts_engine_t s_engine = TTS_ENGINE_AUTO;

/*
 * Playback runs on its own task.
 *
 * tts_say() used to write straight to I2S and block for the whole sentence,
 * which stopped the capture loop from reading the microphone for those seconds
 * - so talking over the top was impossible no matter how good the echo
 * canceller was.  Now a reply is rendered up front into PSRAM, handed to the
 * player task, and tts_say() returns immediately: the main loop keeps feeding
 * the AFE (and the voiceprint / ASR path) while xiaoxin is still talking.
 */
#define TTS_MAX_SAMPLES  (30 * AUDIO_SAMPLE_RATE)
#define TTS_TEXT_MAX     1024        /* sanitized text buffer (bytes, UTF-8) */

typedef struct {
    int16_t *pcm;
    size_t   n;
} clip_t;

static QueueHandle_t s_clip_q;
static volatile bool s_speaking;
static uint32_t s_last_clip_ms;

static void player_task(void *arg);

uint32_t tts_last_clip_ms(void)
{
    return s_last_clip_ms;
}

bool tts_speaking(void)
{
    return s_speaking;
}

const char *tts_engine_name(void)
{
    switch (s_engine) {
    case TTS_ENGINE_CLOUD: return "cloud";
    case TTS_ENGINE_LOCAL: return "local";
    default:               return "auto";
    }
}

void tts_set_engine(tts_engine_t engine)
{
    if (engine != TTS_ENGINE_AUTO && engine != TTS_ENGINE_CLOUD &&
        engine != TTS_ENGINE_LOCAL) {
        return;
    }
    s_engine = engine;
    ESP_LOGW(TAG, "engine -> %s", tts_engine_name());
}

tts_engine_t tts_get_engine(void)
{
    return s_engine;
}

unsigned tts_local_speed(void)
{
    return s_speed;
}

bool tts_uses_cloud(void)
{
    if (s_engine == TTS_ENGINE_LOCAL) {
        return false;
    }
    if (s_engine == TTS_ENGINE_CLOUD) {
        return tts_cloud_configured();
    }
    return tts_cloud_active();
}

/* --------------------------- TTS text cleanup ---------------------------- */
/* The model still slips markdown, emoji or a URL into a sentence now and then.
 * ESP-SR cannot pronounce any of it and even neural TTS reads "**" aloud as a
 * pause, so strip what would only add noise.  Chinese punctuation is kept: it
 * is what gives the synthesizer its pauses. */

static bool tts_drop_codepoint(uint32_t cp)
{
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;   /* emoji + pictographs */
    if (cp >= 0x2600 && cp <= 0x27BF)   return true;   /* ☀ ✅ ✨ ... */
    if (cp >= 0x2B00 && cp <= 0x2BFF)   return true;
    if (cp >= 0x2190 && cp <= 0x21FF)   return true;   /* arrows */
    if (cp >= 0x25A0 && cp <= 0x25FF)   return true;   /* geometric shapes */
    if (cp >= 0xFE00 && cp <= 0xFE0F)   return true;   /* variation selectors */
    if (cp == 0x200D || cp == 0x20E3)   return true;   /* ZWJ / keycap */
    if (cp == 0x00A0)                   return true;
    switch (cp) {
    case 0x201C: case 0x201D: case 0x2018: case 0x2019:   /* “ ” ‘ ’ */
    case 0x300C: case 0x300D: case 0x300E: case 0x300F:   /* 「」『』 */
    case 0x300A: case 0x300B: case 0x3010: case 0x3011:   /* 《》【】 */
    case 0x3014: case 0x3015: case 0x3016: case 0x3017:   /* 〔〕〖〗 */
    case 0xFF08: case 0xFF09: case 0xFF3B: case 0xFF3D:   /* （）［］ */
    case '[': case ']': case '(': case ')':
        return true;    /* spoken punctuation only adds odd pauses */
    default:
        break;
    }
    return false;
}

static bool tts_space_codepoint(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x3000;
}

static bool tts_trim_codepoint(uint32_t cp)
{
    if (tts_space_codepoint(cp)) {
        return true;
    }
    switch (cp) {
    case 0xFF0C:    /* ， */
    case 0x3002:    /* 。 */
    case 0x3001:    /* 、 */
    case 0xFF1B:    /* ； */
    case 0xFF1A:    /* ： */
    case ',':
    case '.':
    case ';':
    case ':':
    case '!':
    case '?':
    case 0xFF01:    /* ！ */
    case 0xFF1F:    /* ？ */
    case 0x2026:    /* … */
    case '~':
    case 0xFF5E:    /* ～ */
    case 0x00B7:    /* · */
        return true;
    default:
        return false;
    }
}

static int tts_utf8_decode(const unsigned char *s, uint32_t *cp)
{
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static size_t tts_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Returns the sanitized length; out is always NUL-terminated. */
static size_t tts_sanitize(const char *in, char *out, size_t out_len)
{
    if (!out_len) {
        return 0;
    }
    out[0] = 0;
    if (!in) {
        return 0;
    }
    size_t o = 0;
    uint32_t last = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 8 < out_len) {
        /* Drop URLs wholesale - reading "https://..." is pure noise. */
        if (strncmp((const char *)p, "http://", 7) == 0 ||
            strncmp((const char *)p, "https://", 8) == 0 ||
            strncmp((const char *)p, "www.", 4) == 0) {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                p++;
            }
            continue;
        }
        uint32_t cp;
        int n = tts_utf8_decode(p, &cp);
        p += n;
        if (tts_drop_codepoint(cp)) {
            continue;
        }
        /* Markdown / code markers would be read as pauses or garbage. */
        if (cp == '*' || cp == '`' || cp == '#' || cp == '<' || cp == '>' ||
            cp == '|' || cp == '~' || cp == '^' || cp == '\\' || cp == '"') {
            continue;
        }
        if (tts_space_codepoint(cp) || cp == 0x2026) {
            cp = 0xFF0C;    /* newline or "……" -> a spoken comma pause */
        }
        if (cp == 0xFF0C || cp == ',') {
            if (last == 0xFF0C || last == ',') {
                continue;   /* collapse ,,, */
            }
            cp = 0xFF0C;
            last = cp;
        } else {
            last = cp;
        }
        if (o + 4 >= out_len) {
            break;
        }
        o += tts_utf8_encode(cp, out + o);
    }
    out[o] = 0;

    /* Trim leading/trailing punctuation and spaces. */
    while (o > 0) {
        size_t start = o - 1;
        while (start > 0 && ((unsigned char)out[start] & 0xC0) == 0x80) {
            start--;
        }
        uint32_t cp;
        tts_utf8_decode((const unsigned char *)out + start, &cp);
        if (!tts_trim_codepoint(cp)) {
            break;
        }
        o = start;
    }
    out[o] = 0;
    if (o >= out_len - 1) {
        ESP_LOGW(TAG, "tts text truncated to %u bytes", (unsigned)o);
    }
    return o;
}

/* ------------------------------ playback -------------------------------- */

/* TTS hands back mono 16-bit; the I2S bus runs stereo, so duplicate. */
static esp_err_t write_mono_as_stereo(const short *mono, int n)
{
    enum { CHUNK = 512 };
    int16_t buf[CHUNK * AUDIO_CHANNELS];
    int done = 0;
    while (done < n) {
        int k = n - done;
        if (k > CHUNK) {
            k = CHUNK;
        }
        for (int i = 0; i < k; i++) {
            buf[i * AUDIO_CHANNELS] = mono[done + i];
            buf[i * AUDIO_CHANNELS + 1] = mono[done + i];
        }
        esp_err_t err = audio_write(buf, (size_t)k * AUDIO_CHANNELS * sizeof(int16_t));
        if (err != ESP_OK) {
            return err;
        }
        done += k;
    }
    return ESP_OK;
}

static void player_task(void *arg)
{
    (void)arg;
    clip_t clip;
    while (1) {
        if (xQueueReceive(s_clip_q, &clip, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_speaking = true;
        if (write_mono_as_stereo(clip.pcm, (int)clip.n) != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed mid-utterance");
        }
        s_speaking = false;
        free(clip.pcm);
    }
}

/* Hand a finished PCM buffer to the player.  Anything still queued is dropped:
 * a newer reply supersedes the old one. */
static esp_err_t queue_clip(int16_t *pcm, size_t samples)
{
    if (!s_clip_q) {
        free(pcm);
        return ESP_ERR_INVALID_STATE;
    }
    tts_stop();
    clip_t clip = { .pcm = pcm, .n = samples };
    if (xQueueSend(s_clip_q, &clip, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(pcm);
        ESP_LOGW(TAG, "player queue full; dropping reply");
        return ESP_FAIL;
    }
    s_last_clip_ms = (uint32_t)(samples * 1000 / AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

/* --------------------------- engine: ESP-SR ----------------------------- */

static esp_err_t render_local(const char *text, int16_t **pcm_out, size_t *samples_out)
{
    if (!s_tts) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_tts_parse_chinese(s_tts, text)) {
        ESP_LOGW(TAG, "local parse failed for: %.60s", text);
        return ESP_FAIL;
    }
    int16_t *buf = heap_caps_malloc(TTS_MAX_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(TTS_MAX_SAMPLES * sizeof(int16_t));
    }
    if (!buf) {
        ESP_LOGE(TAG, "no memory to render the reply");
        esp_tts_stream_reset(s_tts);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    int len = 0;
    int guard = 16384;
    do {
        short *pcm = esp_tts_stream_play(s_tts, &len, s_speed);
        if (len > 0 && pcm) {
            size_t room = TTS_MAX_SAMPLES - total;
            size_t take = (size_t)len < room ? (size_t)len : room;
            memcpy(buf + total, pcm, take * sizeof(int16_t));
            total += take;
            if (total >= TTS_MAX_SAMPLES) {
                ESP_LOGW(TAG, "reply truncated at %d s", TTS_MAX_SAMPLES / AUDIO_SAMPLE_RATE);
                break;
            }
        }
    } while (len > 0 && guard-- > 0);
    esp_tts_stream_reset(s_tts);

    if (total == 0) {
        free(buf);
        return ESP_FAIL;
    }
    *pcm_out = buf;
    *samples_out = total;
    return ESP_OK;
}

/* ------------------------------- public --------------------------------- */

esp_err_t tts_init(void)
{
    /* Official flow (esp-skainet examples/chinese_tts): the voice set is a .dat
     * blob living in its own flash partition; hand it to the generic template.
     *
     * The esp_tts_voice_xiaole / _xiaoxin structs in libvoice_set_xiaole.a are
     * NOT usable this way - esp_tts_voice_set_init() stores the pointer you pass
     * as the voice data, so passing NULL wipes the audio pointer they carry and
     * the first synthesis dies with a load access fault. */
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                          ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (!part) {
        ESP_LOGE(TAG, "voice_data partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "voice_data partition: %u bytes @ 0x%08x",
             (unsigned)part->size, (unsigned)part->address);

    /* Preferred: map the partition read-only in place (no 4 MB copy). */
    esp_partition_mmap_handle_t mmap;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                       &s_voice_mapped, &mmap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "voice data mmap'd at %p", s_voice_mapped);
    } else {
        ESP_LOGW(TAG, "mmap unavailable (%s), copying voice set into PSRAM",
                 esp_err_to_name(err));
        s_voice_copy = heap_caps_malloc(part->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_voice_copy) {
            s_voice_copy = malloc(part->size);
        }
        if (!s_voice_copy) {
            ESP_LOGE(TAG, "no memory for the voice set (%u bytes)", (unsigned)part->size);
            return ESP_ERR_NO_MEM;
        }
        err = esp_partition_read(part, 0, s_voice_copy, part->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "partition read failed: %s", esp_err_to_name(err));
            return err;
        }
        s_voice_mapped = s_voice_copy;
        ESP_LOGI(TAG, "voice data copied to %p", s_voice_mapped);
    }

    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_template,
                                                    (int16_t *)s_voice_mapped);
    if (!voice) {
        ESP_LOGE(TAG, "voice set init failed - was the voice data flashed into voice_data?");
        return ESP_FAIL;
    }
    s_tts = esp_tts_create(voice);
    if (!s_tts) {
        ESP_LOGE(TAG, "esp_tts_create failed");
        return ESP_FAIL;
    }

    s_clip_q = xQueueCreate(4, sizeof(clip_t));
    if (!s_clip_q) {
        ESP_LOGE(TAG, "cannot create the playback queue");
        return ESP_ERR_NO_MEM;
    }
    /* Playback on its own task, pinned to core 1, so the capture loop never
     * blocks on I2S while xiaoxin is talking. */
    xTaskCreatePinnedToCore(player_task, "tts_player", 4096, NULL, 5, NULL, 1);

    if (tts_cloud_configured()) {
#if CONFIG_VOICE_TTS_STREAM
        ESP_LOGI(TAG, "ESP-SR Chinese TTS ready (%d Hz mono, speed %u) + cloud voice %s (streaming)",
                 AUDIO_SAMPLE_RATE, s_speed, tts_cloud_voice());
#else
        ESP_LOGI(TAG, "ESP-SR Chinese TTS ready (%d Hz mono, speed %u) + cloud voice %s (batch)",
                 AUDIO_SAMPLE_RATE, s_speed, tts_cloud_voice());
#endif
    } else {
        ESP_LOGI(TAG, "ESP-SR Chinese TTS ready (%d Hz mono, speed %u); cloud TTS not configured",
                 AUDIO_SAMPLE_RATE, s_speed);
    }
    return ESP_OK;
}

bool tts_ready(void)
{
    return s_tts != NULL;
}

esp_err_t tts_set_speed(unsigned speed)
{
    if (speed > 5) {
        return ESP_ERR_INVALID_ARG;
    }
    s_speed = speed;
    return ESP_OK;
}

void tts_stop(void)
{
    if (!s_clip_q) {
        return;
    }
    clip_t drop;
    while (xQueueReceive(s_clip_q, &drop, 0) == pdTRUE) {
        free(drop.pcm);
    }
}

/* Streaming cloud playback: each PCM chunk is written to I2S as it arrives,
 * so the first word comes out while the rest of the sentence is still being
 * generated.  s_speaking is raised on the first chunk so the microphone gate
 * and the AEC suppression behave exactly as for a queued clip. */
#if CONFIG_VOICE_TTS_STREAM
typedef struct {
    size_t samples;
    bool   started;
} cloud_play_ctx_t;

static esp_err_t cloud_play_sink(const int16_t *pcm, size_t samples, void *ctx)
{
    cloud_play_ctx_t *c = (cloud_play_ctx_t *)ctx;
    if (!pcm || samples == 0) {
        return ESP_OK;
    }
    if (!c->started) {
        c->started = true;
        s_speaking = true;
    }
    c->samples += samples;
    return write_mono_as_stereo(pcm, (int)samples);
}
#endif /* CONFIG_VOICE_TTS_STREAM */

/* Renders the sentence into one PSRAM buffer and hands it to the player.
 * Returns as soon as it is queued, not when it has finished playing. */
esp_err_t tts_say(const char *utf8_text)
{
    if (!utf8_text || !utf8_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_clip_q) {
        return ESP_ERR_INVALID_STATE;
    }

    char clean[TTS_TEXT_MAX];
    if (tts_sanitize(utf8_text, clean, sizeof(clean)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tts_uses_cloud()) {
        /* One TLS session at a time on this board.  Replies call tts_say from
         * inside the already-locked cloud turn; reminders / interjections /
         * the console call it from their own task, so they take the lock here. */
        bool took_lock = !conversation_cloud_lock_held_by_current();
        if (took_lock) {
            conversation_lock_cloud();
        }
#if CONFIG_VOICE_TTS_STREAM
        cloud_play_ctx_t ctx = { 0 };
        esp_err_t cloud_err = tts_cloud_stream(clean, cloud_play_sink, &ctx);
        s_speaking = false;
        if (took_lock) {
            conversation_unlock_cloud();
        }
        if (ctx.samples > 0) {
            s_last_clip_ms = (uint32_t)(ctx.samples * 1000 / AUDIO_SAMPLE_RATE);
            if (cloud_err != ESP_OK) {
                ESP_LOGW(TAG, "cloud stream ended early after %.1f s",
                         (double)ctx.samples / AUDIO_SAMPLE_RATE);
            }
            ESP_LOGI(TAG, "streamed voice: %u chars -> %.1f s (%s)",
                     (unsigned)strlen(clean), (double)ctx.samples / AUDIO_SAMPLE_RATE,
                     tts_cloud_voice());
            return ESP_OK;      /* partial audio is still a spoken reply */
        }
        ESP_LOGW(TAG, "cloud stream produced no audio - falling back to ESP-SR");
#else
        int16_t *pcm = NULL;
        size_t samples = 0;
        esp_err_t cloud_err = tts_cloud_synthesize(clean, &pcm, &samples);
        if (took_lock) {
            conversation_unlock_cloud();
        }
        if (cloud_err == ESP_OK && pcm && samples) {
            ESP_LOGI(TAG, "cloud voice: %u chars -> %.1f s (%s)",
                     (unsigned)strlen(clean), (double)samples / AUDIO_SAMPLE_RATE,
                     tts_cloud_voice());
            return queue_clip(pcm, samples);
        }
        ESP_LOGW(TAG, "cloud TTS failed - falling back to ESP-SR");
#endif
    }

    int16_t *pcm = NULL;
    size_t samples = 0;
    esp_err_t err = render_local(clean, &pcm, &samples);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "local voice: %u chars -> %u samples (%.1f s, speed %u)",
             (unsigned)strlen(clean), (unsigned)samples,
             (double)samples / AUDIO_SAMPLE_RATE, s_speed);
    return queue_clip(pcm, samples);
}
