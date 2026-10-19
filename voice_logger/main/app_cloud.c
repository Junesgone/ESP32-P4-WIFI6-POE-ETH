/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_cloud.h"
#include "app_audio.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cloud";

#define BRIDGE_BASE  CONFIG_VOICE_BRIDGE_URL
#define HTTP_TIMEOUT_MS 60000

static void wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate)
{
    uint32_t byte_rate = rate * 2;
    uint16_t block_align = 2;
    uint16_t channels = 1, bits = 16, pcm = 1;
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

static esp_err_t read_body(esp_http_client_handle_t c, char *dst, size_t dst_len)
{
    int total = esp_http_client_fetch_headers(c);
    if (total < 0) {
        return ESP_FAIL;
    }
    size_t used = 0;
    int guard = 256;
    while (guard-- > 0) {
        int space = (int)(dst_len - 1 - used);
        if (space <= 0) {
            break;
        }
        int n = esp_http_client_read(c, dst + used, space);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    dst[used] = 0;
    return used ? ESP_OK : ESP_FAIL;
}

static void json_str(const cJSON *root, const char *key, char *dst, size_t dst_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(dst, dst_len, "%s", item->valuestring);
    } else {
        dst[0] = 0;
    }
}

/* Pull the reply WAV from the bridge and push it straight into the I2S DAC. */
static esp_err_t stream_reply_audio(void)
{
    esp_http_client_config_t cfg = {
        .url = BRIDGE_BASE "/reply.wav",
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply.wav open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    int total = esp_http_client_fetch_headers(c);
    if (total <= 44) {
        ESP_LOGW(TAG, "bridge has no reply audio (%d bytes)", total);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    /* Skip the canonical 44-byte WAV header; the bridge always emits
     * 16 kHz / mono / 16-bit to match this firmware. */
    uint8_t skip[44];
    int got = 0;
    while (got < 44) {
        int n = esp_http_client_read(c, (char *)skip + got, 44 - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }

    static int16_t pcm[2048];
    int remaining = total - 44;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(pcm) ? (int)sizeof(pcm) : remaining;
        int n = esp_http_client_read(c, (char *)pcm, want);
        if (n <= 0) {
            break;
        }
        remaining -= n;
        if (audio_write(pcm, (size_t)n) != ESP_OK) {
            break;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "reply audio streamed (%d bytes)", total - 44);
    return ESP_OK;
}

esp_err_t cloud_ask(const int16_t *mono, size_t samples, uint32_t sample_rate,
                    cloud_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    size_t wav_len = 44 + data_bytes;
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = malloc(wav_len);
    }
    if (!wav) {
        return ESP_ERR_NO_MEM;
    }
    wav_header(wav, data_bytes, sample_rate);
    memcpy(wav + 44, mono, data_bytes);

    esp_http_client_config_t cfg = {
        .url = BRIDGE_BASE "/utterance",
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(wav);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    esp_http_client_set_header(c, "X-Sample-Rate", "16000");

    esp_err_t err = esp_http_client_open(c, (int)wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge unreachable at %s (%s)", BRIDGE_BASE, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        free(wav);
        return err;
    }
    int written = esp_http_client_write(c, (const char *)wav, (int)wav_len);
    free(wav);
    if (written != (int)wav_len) {
        ESP_LOGE(TAG, "short upload (%d/%u)", written, (unsigned)wav_len);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    char *body = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(8192);
    }
    if (!body) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    err = read_body(c, body, 8192);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "bridge returned HTTP %d", status);
        free(body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "bridge reply is not JSON");
        return ESP_FAIL;
    }
    if (out) {
        json_str(root, "text", out->text, sizeof(out->text));
        json_str(root, "reply", out->reply, sizeof(out->reply));
    }
    cJSON_Delete(root);

    if (stream_reply_audio() == ESP_OK && out) {
        out->played = true;
    }
    return ESP_OK;
}
