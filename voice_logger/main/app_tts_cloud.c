/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_tts_cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_wifi.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tts-cloud";

#define TTS_SAMPLE_RATE      16000
#define TTS_BYTES_PER_SAMPLE 2
#define TTS_MAX_BYTES        (CONFIG_TTS_MAX_SECONDS * TTS_SAMPLE_RATE * TTS_BYTES_PER_SAMPLE)
#define TTS_HTTP_TIMEOUT_MS  20000
#define TTS_STREAM_RAW       (4096 + 1)   /* one odd byte carried between reads */
#define TTS_STREAM_PCM       2048

static bool s_user_enabled = true;
static char s_voice[112];       /* empty = use CONFIG_TTS_VOICE */

/* Key precedence: dedicated TTS key first, then the ASR key (same vendor). */
static const char *api_key(void)
{
    if (CONFIG_TTS_API_KEY[0]) {
        return CONFIG_TTS_API_KEY;
    }
    return CONFIG_ASR_API_KEY;
}

bool tts_cloud_configured(void)
{
#if CONFIG_VOICE_TTS_CLOUD
    return api_key()[0] != 0;
#else
    return false;
#endif
}

const char *tts_cloud_model(void)
{
    return CONFIG_TTS_MODEL;
}

const char *tts_cloud_voice(void)
{
    return s_voice[0] ? s_voice : CONFIG_TTS_VOICE;
}

void tts_cloud_set_voice(const char *voice)
{
    if (!voice || !voice[0]) {
        s_voice[0] = 0;
        return;
    }
    if (strchr(voice, ':')) {
        snprintf(s_voice, sizeof(s_voice), "%s", voice);
    } else {
        snprintf(s_voice, sizeof(s_voice), "%s:%s", CONFIG_TTS_MODEL, voice);
    }
    ESP_LOGW(TAG, "voice -> %s", s_voice);
}

void tts_cloud_set_user_enabled(bool on)
{
    s_user_enabled = on;
    ESP_LOGW(TAG, "cloud tts %s", on ? "enabled" : "disabled by user");
}

bool tts_cloud_user_enabled(void)
{
    return s_user_enabled;
}

bool tts_cloud_active(void)
{
    return tts_cloud_configured() && s_user_enabled && wifi_connected();
}

/* A WAV header sometimes appears even though raw PCM was requested; strip it
 * instead of feeding 44 bytes of "RIFF...." to the DAC. */
static void strip_wav_header(uint8_t **buf, size_t *used)
{
    if (*used < 44 || memcmp(*buf, "RIFF", 4) != 0 || memcmp(*buf + 8, "WAVE", 4) != 0) {
        return;
    }
    for (size_t i = 12; i + 8 <= *used; ) {
        uint32_t size = (uint32_t)(*buf)[i + 4] | ((uint32_t)(*buf)[i + 5] << 8) |
                        ((uint32_t)(*buf)[i + 6] << 16) | ((uint32_t)(*buf)[i + 7] << 24);
        if (memcmp(*buf + i, "data", 4) == 0) {
            size_t data_off = i + 8;
            size_t data_len = size;
            if (data_off + data_len > *used) {
                data_len = *used - data_off;
            }
            memmove(*buf, *buf + data_off, data_len);
            *used = data_len;
            ESP_LOGW(TAG, "response was a WAV file; stripped %u-byte header",
                     (unsigned)data_off);
            return;
        }
        if (size > *used || i + 8 + size > *used) {
            break;
        }
        i += 8 + size + (size & 1);
    }
}

/* Shared request body for the batch and streaming calls. */
static char *build_body(const char *utf8_text, bool stream)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_TTS_MODEL);
    cJSON_AddStringToObject(root, "input", utf8_text);
    cJSON_AddStringToObject(root, "voice", tts_cloud_voice());
    cJSON_AddStringToObject(root, "response_format", "pcm");
    cJSON_AddNumberToObject(root, "sample_rate", TTS_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "speed", (double)CONFIG_TTS_SPEED_X100 / 100.0);
    cJSON_AddNumberToObject(root, "gain", (double)CONFIG_TTS_GAIN_DB);
    if (stream) {
        cJSON_AddBoolToObject(root, "stream", 1);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

esp_err_t tts_cloud_synthesize(const char *utf8_text, int16_t **pcm_out, size_t *samples_out)
{
    if (pcm_out) {
        *pcm_out = NULL;
    }
    if (samples_out) {
        *samples_out = 0;
    }
    if (!utf8_text || !utf8_text[0] || !pcm_out || !samples_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tts_cloud_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *body = build_body(utf8_text, false);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key());

    esp_http_client_config_t cfg = {
        .url = CONFIG_TTS_BASE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        ESP_LOGE(TAG, "http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = ESP_FAIL;
    bool opened = false;
    int64_t t0 = esp_timer_get_time();
    int body_len = (int)strlen(body);
    if (esp_http_client_open(c, body_len) != ESP_OK) {
        ESP_LOGE(TAG, "connect failed (wifi=%s)", wifi_connected() ? "up" : "down");
        goto out;
    }
    opened = true;
    esp_http_client_write(c, body, body_len);

    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        char errbuf[320] = { 0 };
        int n = esp_http_client_read(c, errbuf, sizeof(errbuf) - 1);
        if (n > 0) {
            errbuf[n] = 0;
        }
        ESP_LOGE(TAG, "HTTP %d: %.260s", status, errbuf);
        goto out;
    }

    size_t cap = (total > 0) ? ((size_t)total + 8)
                             : ((size_t)65536);
    if (cap > TTS_MAX_BYTES) {
        cap = TTS_MAX_BYTES;
    }
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        ESP_LOGE(TAG, "no memory for %u-byte tts reply", (unsigned)cap);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    size_t used = 0;
    bool overflow = false;
    while (1) {
        if (used >= cap) {
            if (cap >= TTS_MAX_BYTES) {
                overflow = true;
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > TTS_MAX_BYTES) {
                ncap = TTS_MAX_BYTES;
            }
            uint8_t *nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                nb = realloc(buf, ncap);
            }
            if (!nb) {
                ESP_LOGW(TAG, "realloc failed at %u bytes", (unsigned)used);
                overflow = true;
                break;
            }
            buf = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(c, (char *)buf + used, (int)(cap - used));
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    if (overflow) {
        ESP_LOGW(TAG, "tts reply longer than %d s, truncated", CONFIG_TTS_MAX_SECONDS);
    }
    strip_wav_header(&buf, &used);
    if (used < 2) {
        ESP_LOGE(TAG, "empty audio reply (%u bytes)", (unsigned)used);
        free(buf);
        err = ESP_FAIL;
        goto out;
    }
    used &= ~(size_t)1;     /* keep whole int16 samples */

    /* The neural voice occasionally starts with up to ~0.6 s of digital
     * silence.  Trim it so 小新 starts answering sooner; the waveform (and so
     * the echo-cancellation reference) is untouched after the trim. */
    {
        size_t samples = used / 2;
        int16_t *pcm = (int16_t *)buf;
        size_t lead = 0;
        while (lead < samples && lead < TTS_SAMPLE_RATE &&
               pcm[lead] > -300 && pcm[lead] < 300) {
            lead++;
        }
        if (lead > 0 && lead < samples) {
            memmove(buf, buf + lead * 2, (samples - lead) * 2);
            used = (samples - lead) * 2;
            ESP_LOGI(TAG, "trimmed %u ms of leading silence",
                     (unsigned)(lead * 1000 / TTS_SAMPLE_RATE));
        }
    }

    *pcm_out = (int16_t *)buf;
    *samples_out = used / 2;
    err = ESP_OK;
    ESP_LOGI(TAG, "%u chars -> %u bytes (%.1f s) in %lld ms",
             (unsigned)strlen(utf8_text), (unsigned)used,
             (double)(used / 2) / TTS_SAMPLE_RATE,
             (long long)((esp_timer_get_time() - t0) / 1000));

out:
    free(body);
    if (opened) {
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return err;
}

/* Streaming variant.  Same request, plus "stream": true; SiliconFlow then
 * returns chunked raw PCM, which esp_http_client de-chunks on the fly, so the
 * sink can start playing while the rest is still being generated. */
esp_err_t tts_cloud_stream(const char *utf8_text, tts_cloud_sink_t sink, void *ctx)
{
    if (!utf8_text || !utf8_text[0] || !sink) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tts_cloud_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    char *body = build_body(utf8_text, true);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key());
    esp_http_client_config_t cfg = {
        .url = CONFIG_TTS_BASE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = ESP_FAIL;
    bool opened = false;
    uint8_t *raw = NULL;
    int16_t *pcm = NULL;
    int64_t t0 = esp_timer_get_time();
    int body_len = (int)strlen(body);
    if (esp_http_client_open(c, body_len) != ESP_OK) {
        ESP_LOGE(TAG, "stream connect failed (wifi=%s)", wifi_connected() ? "up" : "down");
        goto out;
    }
    opened = true;
    esp_http_client_write(c, body, body_len);

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        char errbuf[320] = { 0 };
        int n = esp_http_client_read(c, errbuf, sizeof(errbuf) - 1);
        if (n > 0) {
            errbuf[n] = 0;
        }
        ESP_LOGE(TAG, "stream HTTP %d: %.260s", status, errbuf);
        goto out;
    }

    /* Heap, not stack: tts_say can run on the 4 KB reminder task or the 16 KB
     * utterance worker, and 8 KB of local buffers next to a TLS handshake is
     * not affordable there.  PSRAM is plentiful. */
    raw = heap_caps_malloc(TTS_STREAM_RAW, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcm = heap_caps_malloc(TTS_STREAM_PCM * sizeof(int16_t),
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || !pcm) {
        ESP_LOGE(TAG, "no memory for stream buffers");
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    size_t raw_len = 0;
    size_t sent_samples = 0;
    size_t lead_skipped = 0;
    bool voiced = false;
    bool first_logged = false;
    const size_t max_samples = (size_t)CONFIG_TTS_MAX_SECONDS * TTS_SAMPLE_RATE;

    while (sent_samples < max_samples) {
        int n = esp_http_client_read(c, (char *)raw + raw_len,
                                     (int)(TTS_STREAM_RAW - 1 - raw_len));
        if (n <= 0) {
            break;
        }
        raw_len += (size_t)n;
        size_t npcm = raw_len / 2;
        if (npcm) {
            for (size_t i = 0; i < npcm; i++) {
                pcm[i] = (int16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
            }
            size_t start = 0;
            if (!voiced) {
                while (start < npcm && lead_skipped < TTS_SAMPLE_RATE &&
                       pcm[start] > -300 && pcm[start] < 300) {
                    start++;
                    lead_skipped++;
                }
                if (start < npcm || lead_skipped >= TTS_SAMPLE_RATE) {
                    voiced = true;      /* first real sample (or 1 s cap) */
                }
            }
            if (voiced) {
                size_t cnt = npcm - start;
                if (cnt) {
                    if (!first_logged) {
                        first_logged = true;
                        ESP_LOGI(TAG, "first audio after %lld ms (trimmed %u ms of silence)",
                                 (long long)((esp_timer_get_time() - t0) / 1000),
                                 (unsigned)(lead_skipped * 1000 / TTS_SAMPLE_RATE));
                    }
                    if (sent_samples + cnt > max_samples) {
                        cnt = max_samples - sent_samples;
                    }
                    esp_err_t e = sink(pcm + start, cnt, ctx);
                    if (e != ESP_OK) {
                        err = e;
                        goto out;
                    }
                    sent_samples += cnt;
                }
            }
            /* A chunk can end on an odd byte boundary: carry it over. */
            raw[0] = raw[npcm * 2];
            raw_len %= 2;
        }
    }

    if (sent_samples > 0) {
        err = ESP_OK;
        ESP_LOGI(TAG, "stream: %u chars -> %.1f s in %lld ms",
                 (unsigned)strlen(utf8_text), (double)sent_samples / TTS_SAMPLE_RATE,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGE(TAG, "stream produced no audio");
    }

out:
    free(raw);
    free(pcm);
    free(body);
    if (opened) {
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return err;
}

