/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_asr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "app_deepseek.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "asr";

/* A real transcription takes 0.5-1.5 s on this link.  60 s meant a broken
 * connection blocked the utterance worker (and the only TLS lock) for two
 * full minutes while the capture queue overflowed and dropped sentences. */
#define ASR_TIMEOUT_MS 25000
#define BOUNDARY       "----esp32p4voiceboundary"
/* DeepSeek: at most 8 MiB and 120 s of audio per request. */
#define ASR_MAX_BYTES  (8u * 1024 * 1024)

/* Key precedence: dedicated ASR key first, then the DeepSeek key (the default
 * endpoint is DeepSeek's POST /v1/tools/asr, and the full-suite APIs share one
 * key).  A SiliconFlow / self-hosted endpoint still works by filling the ASR
 * key + URL + model in menuconfig. */
static const char *api_key(void)
{
    if (CONFIG_ASR_API_KEY[0]) {
        return CONFIG_ASR_API_KEY;
    }
    return CONFIG_DEEPSEEK_API_KEY;
}

bool asr_configured(void)
{
    return api_key()[0] != 0;
}

static void wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate)
{
    uint32_t byte_rate = rate * 2;
    uint16_t block_align = 2, channels = 1, bits = 16, pcm = 1;
    uint32_t fmt_len = 16, riff = 36 + data_bytes;
    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

/* Shared multipart body.  The DeepSeek endpoint takes no model field, so it is
 * only emitted for OpenAI-compatible /audio/transcriptions backends. */
static char *build_preamble(char *buf, size_t len)
{
    if (CONFIG_ASR_MODEL[0]) {
        snprintf(buf, len,
                 "--" BOUNDARY "\r\n"
                 "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                 "%s\r\n"
                 "--" BOUNDARY "\r\n"
                 "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                 "Content-Type: audio/wav\r\n\r\n",
                 CONFIG_ASR_MODEL);
    } else {
        snprintf(buf, len,
                 "--" BOUNDARY "\r\n"
                 "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                 "Content-Type: audio/wav\r\n\r\n");
    }
    return buf;
}

/* POSTs pre + wav bytes + tail and parses {"text": ...}. */
static esp_err_t asr_post(const char *pre, size_t pre_len, const uint8_t *wav, size_t wav_len,
                          char *out_text, size_t out_len)
{
    const char *tail = "\r\n--" BOUNDARY "--\r\n";
    int n_tail = (int)strlen(tail);
    int content_len = (int)pre_len + (int)wav_len + n_tail;

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key());
    char ctype[64];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=" BOUNDARY);

    esp_http_client_config_t cfg = {
        .url = CONFIG_ASR_BASE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ASR_TIMEOUT_MS,
        .buffer_size = 4096,
        /* Same pinned verifier as the chat path: the plain bundle
         * intermittently rejects api.deepseek.com's TrustAsia chain. */
        .crt_bundle_attach = deepseek_tls_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_header(c, "Authorization", auth);

    esp_err_t err = esp_http_client_open(c, content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    esp_http_client_write(c, pre, (int)pre_len);
    esp_http_client_write(c, (const char *)wav, (int)wav_len);
    esp_http_client_write(c, tail, n_tail);

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    size_t cap = 4096;
    char *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(cap);
    }
    if (!resp) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (used + 1 < cap) {
        int n = esp_http_client_read(c, resp + used, (int)(cap - 1 - used));
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    resp[used] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.200s", status, resp);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *dur = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    esp_err_t ret = ESP_FAIL;
    if (cJSON_IsString(txt) && txt->valuestring) {
        snprintf(out_text, out_len, "%s", txt->valuestring);
        ret = ESP_OK;
        if (cJSON_IsNumber(dur)) {
            ESP_LOGI(TAG, "transcribed %d ms: %.120s", dur->valueint, out_text);
        }
    } else {
        ESP_LOGE(TAG, "no \"text\" in ASR reply");
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t asr_transcribe(const int16_t *mono, size_t samples, uint32_t sample_rate,
                         char *out_text, size_t out_len)
{
    if (out_text && out_len) {
        out_text[0] = 0;
    }
    if (!asr_configured()) {
        ESP_LOGE(TAG, "no ASR key: set CONFIG_ASR_API_KEY or CONFIG_DEEPSEEK_API_KEY");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t pcm_bytes = (uint32_t)(samples * sizeof(int16_t));
    uint32_t wav_len = 44 + pcm_bytes;
    if (wav_len > ASR_MAX_BYTES) {
        ESP_LOGE(TAG, "clip too long: %u bytes", (unsigned)wav_len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = malloc(wav_len);
    }
    if (!wav) {
        return ESP_ERR_NO_MEM;
    }
    wav_header(wav, pcm_bytes, sample_rate);
    memcpy(wav + 44, mono, pcm_bytes);

    char pre[512];
    build_preamble(pre, sizeof(pre));
    esp_err_t err = asr_post(pre, strlen(pre), wav, wav_len, out_text, out_len);
    free(wav);
    return err;
}

/* Same transcription path, but from a WAV file already on the TF card.  The
 * bytes are uploaded as they are (DeepSeek detects the container from the
 * content and accepts 8/16/22.05/24/32/44.1/48 kHz WAV), which makes it easy to
 * exercise the standalone Speech-to-Text API from the serial console. */
esp_err_t asr_transcribe_file(const char *path, char *out_text, size_t out_len)
{
    if (out_text && out_len) {
        out_text[0] = 0;
    }
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!asr_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 44 || sz > (long)ASR_MAX_BYTES) {
        fclose(f);
        ESP_LOGE(TAG, "wav size %ld out of range", (long)sz);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)sz);
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    char pre[512];
    build_preamble(pre, sizeof(pre));
    esp_err_t err = asr_post(pre, strlen(pre), buf, rd, out_text, out_len);
    free(buf);
    return err;
}
