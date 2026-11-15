/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_tts_cloud.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_deepseek.h"
#include "app_ds_http.h"
#include "app_wifi.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_VOICE_TTS_WS
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

static const char *TAG = "tts-cloud";

/* DeepSeek standalone TTS returns raw s16le mono PCM at 24 kHz for
 * format=pcm.  The board's ES8311/NS4150B path runs at 16 kHz mono int16, so
 * this file decimates 2:3 and feeds the caller's sink. */
#define CLOUD_RATE            24000
#define PCM_RATE              16000
#define PCM_BYTES_PER_SAMPLE  2

/* Longest audio we keep for one reply.  The API itself allows 30 minutes, the
 * persona keeps replies to a couple of sentences; this is the safety net the
 * ring buffer in app_tts.c is sized from. */
#define PCM_MAX_SAMPLES       ((size_t)CONFIG_TTS_MAX_SECONDS * PCM_RATE)
#define CLOUD_MAX_RAW_BYTES   ((size_t)CONFIG_TTS_MAX_SECONDS * CLOUD_RATE * PCM_BYTES_PER_SAMPLE)

/* Leading digital silence trimmed before the first sample is handed to the
 * sink; the neural voice sometimes starts with up to ~0.5 s of exact zeros. */
#define TRIM_AMP              300
#define TRIM_MAX_SAMPLES      PCM_RATE      /* stop trimming after 1 s */

/* The sink is fed in ~64 ms blocks so one server frame becomes few calls. */
#define SINK_STAGING_SAMPLES  1024

#define HTTP_TIMEOUT_MS       20000
#define HTTP_RX_BUFFER        2048

/* ---- WebSocket transport tuning (measured, see _api_probe/REPORT.md 5) ----
 * handshake 0.14-0.45 s; no PCM before finish_text; first frame 0.83-1.68 s
 * after finish_text; frames are 4800 B (100 ms) and often arrive much faster
 * than realtime; the closing finish event (+ optional last partial frame)
 * only arrives ~12 s later, so the end of the audio is detected by an idle
 * gap, never by blocking on the finish event. */
#define WS_RX_BUFFER          2048          /* server frame 4800 B -> split, fine */
/* TLS handshake runs in this task.  12 KB kept failing mbedtls_ssl_setup with
 * -0x008D (PSA_ERROR_INSUFFICIENT_MEMORY) on this board: the task stack plus the
 * 2x2 KB client buffers came out of the same internal heap the 16 KB TLS input
 * buffer needs.  8 KB is enough for the handshake and leaves the heap a chance. */
#define WS_TASK_STACK         8192
#define WS_CONNECT_TIMEOUT_MS 10000
#define WS_FIRST_AUDIO_MS     15000         /* no PCM at all after finish_text */
#define WS_IDLE_END_MS        2500          /* 2.5 s without a binary frame = done */
#define WS_TOTAL_MS           (CONFIG_TTS_MAX_SECONDS * 1000 + 15000)
#define WS_SEND_TIMEOUT       pdMS_TO_TICKS(3000)
#define WS_TEXT_CHUNK_MAX     320           /* bytes of UTF-8 per text event */

static bool s_user_enabled = true;
static char s_voice[16];        /* empty = use CONFIG_TTS_VOICE_ID */

/* ------------------------------- helpers -------------------------------- */

/* Dedicated TTS key first, then the shared DeepSeek key (same vendor). */
static const char *api_key(void)
{
    if (CONFIG_TTS_API_KEY[0] != '\0') {
        return CONFIG_TTS_API_KEY;
    }
    return CONFIG_DEEPSEEK_API_KEY;
}

bool tts_cloud_configured(void)
{
#if CONFIG_VOICE_TTS_CLOUD
    return api_key()[0] != '\0';
#else
    return false;
#endif
}

const char *tts_cloud_model(void)
{
    return "deepseek-tts";
}

const char *tts_cloud_voice(void)
{
    return s_voice[0] ? s_voice : CONFIG_TTS_VOICE_ID;
}

void tts_cloud_set_voice(const char *voice)
{
    if (!voice || !voice[0]) {
        s_voice[0] = 0;
        ESP_LOGW(TAG, "voice -> %s (default)", tts_cloud_voice());
        return;
    }
    if (strcmp(voice, "stella") != 0 && strcmp(voice, "tide") != 0) {
        ESP_LOGW(TAG, "voice '%s' not supported (stella|tide); keeping %s",
                 voice, tts_cloud_voice());
        return;
    }
    snprintf(s_voice, sizeof(s_voice), "%s", voice);
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

/* ------------------- PCM pipe: 24 kHz -> 16 kHz + trim ------------------
 *
 * The output timeline is fixed by the ratio 24000/16000 = 3/2: output sample n
 * sits at input position 1.5*n.  Everything is integer: positions are tracked
 * in half input samples (q2 = 2*position), so the output positions are
 * 0, 3, 6, 9 ... Every input sample therefore either lands exactly on an
 * output sample (even q2) or halfway between two (odd q2), and the odd case is
 * just the (prev + cur) >> 1 linear interpolation.  That is plenty for speech
 * (the alias image at 8-12 kHz is far below what the ES8311 path reproduces);
 * a polyphase FIR would cost RAM and latency for no audible gain here.
 *
 * All state (carried odd byte, previous input sample, next output position)
 * lives in this struct, so any chunking of the 24 kHz byte stream - WS frames
 * are arbitrary lengths and the final partial frame can be odd - resamples to
 * exactly the same 16 kHz samples.
 */
typedef struct {
    tts_cloud_sink_t sink;
    void *sink_ctx;

    /* byte carry between arbitrary chunks */
    uint8_t  carry;
    bool     have_carry;

    /* resampler state (q2 = 2 * position in the 24 kHz input stream) */
    int16_t  prev;
    bool     have_prev;
    uint32_t prev_q2;
    uint32_t next_out_q2;

    /* leading-silence trim (counted at the 16 kHz output rate) */
    bool     voiced;
    size_t   trimmed;

    /* staging so the sink is called with reasonably sized blocks */
    int16_t  staging[SINK_STAGING_SAMPLES];
    size_t   staging_len;
    size_t   out_samples;
    bool     full;

    /* statistics for tuning the jitter buffer */
    int64_t  t0;
    int64_t  t_connected;
    int64_t  t_finish_text;
    int64_t  t_first_audio;
    uint32_t last_data_ms;
    bool     got_data;
    uint32_t blocks;
    size_t   raw_bytes;
    uint32_t max_gap_ms;
    uint64_t gap_total_ms;
    uint32_t gaps_over_50;

    esp_err_t sink_err;
} pcm_pipe_t;

static pcm_pipe_t *s_pipe;

static pcm_pipe_t *pipe_get(void)
{
    if (!s_pipe) {
        s_pipe = heap_caps_malloc(sizeof(pcm_pipe_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pipe) {
            s_pipe = malloc(sizeof(pcm_pipe_t));
        }
        if (s_pipe) {
            ESP_LOGI(TAG, "PCM pipe ready (%u bytes)", (unsigned)sizeof(pcm_pipe_t));
        }
    }
    return s_pipe;
}

static void pipe_reset(pcm_pipe_t *p, tts_cloud_sink_t sink, void *ctx, int64_t t0)
{
    memset(p, 0, sizeof(*p));
    p->sink = sink;
    p->sink_ctx = ctx;
    p->t0 = t0;
}

static esp_err_t pipe_flush(pcm_pipe_t *p)
{
    if (p->staging_len == 0 || p->sink_err != ESP_OK) {
        p->staging_len = 0;
        return p->sink_err;
    }
    size_t n = p->staging_len;
    size_t room = PCM_MAX_SAMPLES - p->out_samples;
    if (n > room) {
        n = room;
        p->full = true;
    }
    p->staging_len = 0;
    if (n == 0) {
        return ESP_OK;
    }
    p->out_samples += n;
    esp_err_t err = p->sink(p->staging, n, p->sink_ctx);
    if (err != ESP_OK) {
        p->sink_err = err;
        ESP_LOGW(TAG, "sink rejected audio (%s) after %.2f s",
                 esp_err_to_name(err), (double)p->out_samples / PCM_RATE);
    }
    return err;
}

/* One resampled 16 kHz sample: trim the leading silence, then stage/ship it. */
static void pipe_emit(pcm_pipe_t *p, int16_t sample)
{
    if (p->full || p->sink_err != ESP_OK) {
        return;
    }
    if (!p->voiced) {
        if (sample > -TRIM_AMP && sample < TRIM_AMP && p->trimmed < TRIM_MAX_SAMPLES) {
            p->trimmed++;
            return;
        }
        p->voiced = true;
        p->t_first_audio = esp_timer_get_time();
    }
    p->staging[p->staging_len++] = sample;
    if (p->staging_len == SINK_STAGING_SAMPLES) {
        pipe_flush(p);
    }
}

/* 24 kHz input samples -> 16 kHz output samples, state carried across calls. */
static void pipe_resample(pcm_pipe_t *p, int16_t sample)
{
    if (!p->have_prev) {
        /* First input sample produces output sample 0. */
        p->have_prev = true;
        p->prev = sample;
        p->prev_q2 = 0;
        p->next_out_q2 = 3;
        pipe_emit(p, sample);
        return;
    }
    uint32_t cur_q2 = p->prev_q2 + 2;      /* position of this input sample */
    while (p->next_out_q2 <= cur_q2) {
        int16_t out;
        if (p->next_out_q2 == cur_q2) {
            out = sample;                  /* exact input sample */
        } else {
            out = (int16_t)(((int32_t)p->prev + (int32_t)sample) >> 1);  /* midpoint */
        }
        pipe_emit(p, out);
        p->next_out_q2 += 3;
    }
    p->prev = sample;
    p->prev_q2 = cur_q2;
}

/* Feeds one arbitrary PCM block: carries the odd byte, updates the stream
 * statistics (block count / max gap), then resamples into the sink. */
static void pipe_feed(pcm_pipe_t *p, const uint8_t *data, size_t len)
{
    if (!p || !data || len == 0 || p->sink_err != ESP_OK || p->full) {
        return;
    }
    {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (p->got_data) {
            uint32_t gap = (uint32_t)(now_ms - p->last_data_ms);
            p->gap_total_ms += gap;
            if (gap > p->max_gap_ms) {
                p->max_gap_ms = gap;
            }
            if (gap > 50) {
                p->gaps_over_50++;
            }
        }
        p->got_data = true;
        p->last_data_ms = now_ms;
        p->blocks++;
        p->raw_bytes += len;
    }

    if (p->have_carry) {
        int16_t sample = (int16_t)((uint16_t)p->carry | ((uint16_t)data[0] << 8));
        p->have_carry = false;
        data++;
        len--;
        pipe_resample(p, sample);
    }
    while (len >= 2) {
        int16_t sample = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
        data += 2;
        len -= 2;
        pipe_resample(p, sample);
        if (p->sink_err != ESP_OK || p->full) {
            return;
        }
    }
    if (len == 1) {
        p->carry = data[0];
        p->have_carry = true;
    }
}

static void pipe_finish(pcm_pipe_t *p)
{
    pipe_flush(p);
}

/* ---------------------- streaming POST (fallback) ----------------------- */

/* Response consumer for the shared keep-alive connection: raw 24 kHz s16le
 * PCM chunks go straight into the resampling pipe. */
static esp_err_t tts_post_sink(const char *data, size_t len, void *ctx)
{
    pcm_pipe_t *p = (pcm_pipe_t *)ctx;
    if (p && data && len) {
        pipe_feed(p, (const uint8_t *)data, len);
    }
    return ESP_OK;
}

/* POST /v1/tools/tts with {"text","voice_id","format":"pcm"}.  The response is
 * chunked raw 24 kHz s16le and ends when the server finishes the chunked body;
 * the shared connection hands each block to the sink, so playback starts while
 * the rest is still being synthesized. */
static esp_err_t http_stream(pcm_pipe_t *p, const char *utf8_text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", utf8_text);
    cJSON_AddStringToObject(root, "voice_id", tts_cloud_voice());
    cJSON_AddStringToObject(root, "format", "pcm");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    if (api_key()[0] && strcmp(api_key(), CONFIG_DEEPSEEK_API_KEY) != 0) {
        ds_http_set_auth(api_key());
    }

    int64_t connect_before = ds_http_last_connect_us();
    int status = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ds_http_request(CONFIG_DEEPSEEK_TTS_URL, "POST", "application/json",
                                    "audio/pcm", body, strlen(body), HTTP_TIMEOUT_MS,
                                    tts_post_sink, p, &status);
    free(body);
    if (ds_http_last_connect_us() != connect_before) {
        p->t_connected = ds_http_last_connect_us();
    } else {
        p->t_connected = t0;      /* connection reused: no handshake on this call */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS POST transport failed after %lld ms",
                 (long long)((esp_timer_get_time() - t0) / 1000));
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "TTS POST HTTP %d: %.160s", status, ds_http_last_error());
        return ESP_FAIL;
    }
    if (p->out_samples == 0) {
        ESP_LOGE(TAG, "TTS POST returned no PCM");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------ batch path ------------------------------ */

typedef struct {
    int16_t *out;
    size_t   cap;
    size_t   n;
} collect_ctx_t;

static esp_err_t collect_sink(const int16_t *pcm, size_t samples, void *ctx)
{
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    size_t room = c->cap - c->n;
    if (samples > room) {
        samples = room;
    }
    memcpy(c->out + c->n, pcm, samples * sizeof(int16_t));
    c->n += samples;
    return ESP_OK;
}

/* Batch synthesis (CONFIG_VOICE_TTS_STREAM=n): one POST with format=pcm, then
 * resample the whole 24 kHz reply to 16 kHz.  The caller owns the returned
 * buffer (PSRAM) and free()s it. */
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

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", utf8_text);
    cJSON_AddStringToObject(root, "voice_id", tts_cloud_voice());
    cJSON_AddStringToObject(root, "format", "pcm");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key());

    esp_http_client_config_t cfg = {
        .url = CONFIG_DEEPSEEK_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = deepseek_tls_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        ESP_LOGE(TAG, "http client init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = ESP_FAIL;
    uint8_t *raw = NULL;
    int64_t t0 = esp_timer_get_time();
    int64_t t_connect = 0;
    bool opened = false;

    if (esp_http_client_open(client, (int)strlen(body)) != ESP_OK) {
        ESP_LOGE(TAG, "POST connect failed (wifi=%s)", wifi_connected() ? "up" : "down");
        goto out;
    }
    opened = true;
    t_connect = esp_timer_get_time();
    if (esp_http_client_write(client, body, (int)strlen(body)) < 0) {
        ESP_LOGE(TAG, "POST body write failed");
        goto out;
    }
    (void)esp_http_client_fetch_headers(client);
    {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            char errbuf[256] = { 0 };
            int n = esp_http_client_read(client, errbuf, sizeof(errbuf) - 1);
            if (n > 0) {
                errbuf[n] = 0;
            }
            ESP_LOGE(TAG, "TTS POST HTTP %d: %.240s", status, errbuf);
            goto out;
        }
    }

    raw = heap_caps_malloc(CLOUD_MAX_RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        raw = malloc(CLOUD_MAX_RAW_BYTES);
    }
    if (!raw) {
        ESP_LOGE(TAG, "no memory for the %u-byte 24 kHz reply",
                 (unsigned)CLOUD_MAX_RAW_BYTES);
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    {
        size_t used = 0;
        while (used < CLOUD_MAX_RAW_BYTES) {
            int n = esp_http_client_read(client, (char *)raw + used,
                                         (int)(CLOUD_MAX_RAW_BYTES - used));
            if (n <= 0) {
                break;
            }
            used += (size_t)n;
        }
        if (used >= CLOUD_MAX_RAW_BYTES) {
            ESP_LOGW(TAG, "reply longer than %d s, truncated", CONFIG_TTS_MAX_SECONDS);
        }
        if (used < 2) {
            ESP_LOGE(TAG, "empty 24 kHz reply");
            goto out;
        }

        int16_t *out = heap_caps_malloc(PCM_MAX_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            out = malloc(PCM_MAX_SAMPLES * sizeof(int16_t));
        }
        if (!out) {
            ESP_LOGE(TAG, "no memory for the 16 kHz reply");
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        pcm_pipe_t *p = pipe_get();
        if (!p) {
            free(out);
            err = ESP_ERR_NO_MEM;
            goto out;
        }
        collect_ctx_t col = { .out = out, .cap = PCM_MAX_SAMPLES, .n = 0 };
        pipe_reset(p, collect_sink, &col, t0);
        p->t_connected = t_connect;
        for (size_t off = 0; off < used && col.n < PCM_MAX_SAMPLES; off += 1024) {
            size_t n = used - off;
            if (n > 1024) {
                n = 1024;
            }
            pipe_feed(p, raw + off, n);
        }
        pipe_finish(p);
        if (col.n == 0) {
            free(out);
            ESP_LOGE(TAG, "reply had no audible samples");
            goto out;
        }
        *pcm_out = out;
        *samples_out = col.n;
        err = ESP_OK;
        ESP_LOGI(TAG, "batch: %u chars -> %.2f s in %lld ms (connect %lld ms)",
                 (unsigned)strlen(utf8_text), (double)col.n / PCM_RATE,
                 (long long)((esp_timer_get_time() - t0) / 1000),
                 (long long)((t_connect - t0) / 1000));
    }

out:
    if (opened) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    free(raw);
    free(body);
    return err;
}

/* ------------------------- WebSocket streaming path --------------------- */
#if CONFIG_VOICE_TTS_WS

static SemaphoreHandle_t s_ws_evt;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_connected;
static volatile bool s_ws_closed;
static volatile bool s_ws_error;
static volatile bool s_ws_got_bin;
static volatile uint32_t s_ws_last_bin_ms;
static char s_ws_finish_code[24];
static char s_ws_finish_msg[128];
static bool s_ws_binary_msg;         /* current WS message carries PCM */
static char s_ws_text[512];          /* accumulated text-frame JSON */
static int  s_ws_text_len;

static void ws_signal(void)
{
    if (s_ws_evt) {
        xSemaphoreGive(s_ws_evt);
    }
}

/* Handles one TEXT frame: the service only sends {"event":"finish",...}
 * (success/non-success).  For a normal session it arrives ~12 s after
 * finish_text, long after the audio, which is why the caller never waits for
 * it to end playback. */
static void ws_handle_text_frame(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "unparsable WS text frame: %.120s", json);
        return;
    }
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "event");
    if (cJSON_IsString(ev) && strcmp(ev->valuestring, "finish") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *cstr = cJSON_IsString(code) ? code->valuestring : "?";
        const char *mstr = cJSON_IsString(msg) ? msg->valuestring : "";
        snprintf(s_ws_finish_code, sizeof(s_ws_finish_code), "%s", cstr);
        snprintf(s_ws_finish_msg, sizeof(s_ws_finish_msg), "%s", mstr);
        if (strcmp(cstr, "success") != 0) {
            ESP_LOGW(TAG, "WS finish: %s (%s)", cstr, mstr);
            if (!s_pipe || s_pipe->out_samples == 0) {
                s_ws_error = true;      /* nothing played: caller may fall back */
            }
        } else {
            ESP_LOGD(TAG, "WS finish: success");
        }
        ws_signal();
    }
    cJSON_Delete(root);
}

static void ws_handle_data(esp_websocket_event_data_t *d)
{
    if (!d || !d->data_ptr || d->data_len <= 0) {
        return;     /* empty control frame (ping) */
    }
    uint8_t op = d->op_code & 0x0F;
    if (op == 0x2) {
        s_ws_binary_msg = true;
    } else if (op == 0x1) {
        s_ws_binary_msg = false;
    } else if (op != 0x0) {
        return;     /* ping / pong / close: the client handles them itself */
    }

    if (s_ws_binary_msg) {
        s_ws_got_bin = true;
        s_ws_last_bin_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_pipe) {
            pipe_feed(s_pipe, (const uint8_t *)d->data_ptr, (size_t)d->data_len);
        }
        ws_signal();
        return;
    }

    /* TEXT frame: may be split across several data events by the client's rx
     * buffer; payload_offset tells us where the piece starts. */
    if (d->payload_offset == 0) {
        s_ws_text_len = 0;
    }
    if (s_ws_text_len + d->data_len < (int)sizeof(s_ws_text)) {
        memcpy(s_ws_text + s_ws_text_len, d->data_ptr, (size_t)d->data_len);
        s_ws_text_len += d->data_len;
        s_ws_text[s_ws_text_len] = 0;
        if (d->payload_offset + d->data_len >= d->payload_len) {
            ws_handle_text_frame(s_ws_text);
        }
    } else {
        ESP_LOGW(TAG, "WS text frame too long (%d bytes), dropped", d->payload_len);
        s_ws_text_len = 0;
    }
}

static void ws_event_cb(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_connected = true;
        ws_signal();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_closed = true;
        ws_signal();
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_ws_error = true;
        if (d) {
            /* The formatted error message is carried in data_ptr; the
             * error_handle fields say which layer failed. */
            ESP_LOGW(TAG, "WS error: %s (type=%d tls=%s hs=%d errno=%d)",
                     (d->data_ptr && d->data_len > 0) ? (const char *)d->data_ptr : "?",
                     d->error_handle.error_type,
                     esp_err_to_name(d->error_handle.esp_tls_last_esp_err),
                     d->error_handle.esp_ws_handshake_status_code,
                     d->error_handle.esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "WS error (no detail)");
        }
        ws_signal();
        break;
    case WEBSOCKET_EVENT_DATA:
        ws_handle_data(d);
        break;
    default:
        break;
    }
}

/* Waits for a flag signalled by the event handler; returns false on timeout or
 * when the connection died before the flag was set. */
static bool ws_wait_flag(volatile bool *flag, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!*flag) {
        if (s_ws_error || s_ws_closed) {
            return *flag;
        }
        int64_t remain_ms = (deadline - esp_timer_get_time()) / 1000;
        if (remain_ms <= 0) {
            return false;
        }
        xSemaphoreTake(s_ws_evt, pdMS_TO_TICKS(remain_ms));
    }
    return true;
}

/* Length of the next text chunk, never splitting a UTF-8 code point and
 * preferring to cut after sentence punctuation once the chunk is useful. */
static size_t ws_text_chunk_len(const char *s, size_t remain)
{
    size_t n = 0;
    size_t sentence = 0;
    while (n < remain && n < WS_TEXT_CHUNK_MAX) {
        unsigned char c = (unsigned char)s[n];
        size_t adv = 1;
        if (c >= 0xF0) {
            adv = 4;
        } else if (c >= 0xE0) {
            adv = 3;
        } else if (c >= 0xC0) {
            adv = 2;
        }
        if (n + adv > remain) {
            break;      /* never ship half a code point */
        }
        n += adv;
        char last = s[n - 1];
        if (last == '.' || last == '!' || last == '?' || last == ';' ||
            last == '\n' || last == '\r') {
            sentence = n;
        } else if (adv == 3 && n >= 3) {
            const unsigned char *q = (const unsigned char *)s + n - 3;
            if ((q[0] == 0xE3 && q[1] == 0x80 && q[2] == 0x82) ||    /* 。 */
                (q[0] == 0xEF && q[1] == 0xBC &&
                 (q[2] == 0x81 || q[2] == 0x9F || q[2] == 0x9B ||
                  q[2] == 0x8C || q[2] == 0x8E || q[2] == 0x9A))) {  /* ！？；，、 */
                sentence = n;
            }
        }
    }
    if (n < remain && sentence >= 64) {
        return sentence;    /* end the chunk on a natural pause */
    }
    return n;
}

static int ws_send_json_text(const char *chunk, size_t len)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    char text[WS_TEXT_CHUNK_MAX + 4];
    if (len >= sizeof(text)) {
        len = sizeof(text) - 1;
    }
    memcpy(text, chunk, len);
    text[len] = 0;
    cJSON_AddStringToObject(root, "event", "text");
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return -1;
    }
    int len_json = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_ws, json, len_json, WS_SEND_TIMEOUT);
    cJSON_free(json);
    if (sent != len_json) {
        ESP_LOGW(TAG, "WS text send failed (%d/%d)", sent, len_json);
        return -1;
    }
    return 0;
}

static int ws_send_plain(const char *json)
{
    int len = (int)strlen(json);
    int sent = esp_websocket_client_send_text(s_ws, json, len, WS_SEND_TIMEOUT);
    if (sent != len) {
        ESP_LOGW(TAG, "WS send failed (%d/%d)", sent, len);
        return -1;
    }
    return 0;
}

/* A definitive input/content error will fail the same way on the POST path, so
 * only retry there when the failure was transport/protocol/service related. */
static bool ws_may_fallback(void)
{
    if (s_ws_finish_code[0] == 0) {
        return true;
    }
    return strcmp(s_ws_finish_code, "invalid_input") != 0 &&
           strcmp(s_ws_finish_code, "content_filter") != 0 &&
           strcmp(s_ws_finish_code, "no_content") != 0;
}

static esp_err_t ws_stream(pcm_pipe_t *p, const char *utf8_text)
{
    if (!s_ws_evt) {
        s_ws_evt = xSemaphoreCreateBinary();
        if (!s_ws_evt) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_ws_connected = false;
    s_ws_closed = false;
    s_ws_error = false;
    s_ws_got_bin = false;
    s_ws_last_bin_ms = 0;
    s_ws_finish_code[0] = 0;
    s_ws_finish_msg[0] = 0;
    s_ws_binary_msg = false;
    s_ws_text_len = 0;
    while (xSemaphoreTake(s_ws_evt, 0) == pdTRUE) {
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%cvoice_id=%s&format=pcm",
             CONFIG_DEEPSEEK_TTS_WS_URL,
             strchr(CONFIG_DEEPSEEK_TTS_WS_URL, '?') ? '&' : '?',
             tts_cloud_voice());
    char auth[192];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", api_key());

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .headers = auth,
        .disable_auto_reconnect = true,
        /* 10 s made every failed WebSocket attempt cost ~11 s before the POST
         * fallback could start; the TCP+TLS+upgrade either completes in ~0.5 s
         * on this link or it is not going to. */
        .network_timeout_ms = 4000,
        .buffer_size = WS_RX_BUFFER,
        .task_stack = WS_TASK_STACK,
        .crt_bundle_attach = deepseek_tls_attach,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 60,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "websocket client init failed");
        return ESP_FAIL;
    }
    esp_err_t err = esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                                  ws_event_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket event registration failed: %s", esp_err_to_name(err));
        goto out;
    }
    err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start failed: %s", esp_err_to_name(err));
        goto out;
    }
    if (!ws_wait_flag(&s_ws_connected, WS_CONNECT_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "websocket connect failed (%s)",
                 s_ws_error ? "tls/handshake error" : "timeout");
        err = ESP_FAIL;
        goto out;
    }
    p->t_connected = esp_timer_get_time();
    ESP_LOGD(TAG, "WS connected in %lld ms",
             (long long)((p->t_connected - p->t0) / 1000));

    /* Send the whole text, split on UTF-8 boundaries, then finish_text.  The
     * service does not emit PCM before finish_text (7 sessions, probe 5.1). */
    {
        size_t text_len = strlen(utf8_text);
        size_t off = 0;
        while (off < text_len) {
            size_t n = ws_text_chunk_len(utf8_text + off, text_len - off);
            if (n == 0 || ws_send_json_text(utf8_text + off, n) != 0) {
                s_ws_error = true;
                break;
            }
            off += n;
        }
        if (s_ws_error || s_ws_closed) {
            err = ESP_FAIL;
            goto out;
        }
        if (ws_send_plain("{\"event\":\"finish_text\"}") != 0) {
            s_ws_error = true;
            err = ESP_FAIL;
            goto out;
        }
        p->t_finish_text = esp_timer_get_time();
    }

    /* Receive PCM.  The session ends when no binary frame arrived for
     * WS_IDLE_END_MS; the finish text event (and a possible last partial frame)
     * only shows up ~12 s later, so it must not gate playback. */
    {
        int64_t first_audio_deadline = esp_timer_get_time() +
                                       (int64_t)WS_FIRST_AUDIO_MS * 1000;
        int64_t total_deadline = p->t0 + (int64_t)WS_TOTAL_MS * 1000;
        while (1) {
            if (p->sink_err != ESP_OK || p->full) {
                break;
            }
            if (s_ws_error || s_ws_closed) {
                break;
            }
            {
                uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                if (s_ws_got_bin &&
                    (uint32_t)(now_ms - s_ws_last_bin_ms) >= WS_IDLE_END_MS) {
                    break;
                }
            }
            if (!s_ws_got_bin && esp_timer_get_time() >= first_audio_deadline) {
                ESP_LOGW(TAG, "WS TTS: no PCM %d ms after finish_text", WS_FIRST_AUDIO_MS);
                break;
            }
            if (esp_timer_get_time() >= total_deadline) {
                ESP_LOGW(TAG, "WS TTS: session exceeded %d ms", (int)WS_TOTAL_MS);
                break;
            }
            xSemaphoreTake(s_ws_evt, pdMS_TO_TICKS(50));
        }
    }

    if (p->out_samples == 0) {
        if (s_ws_finish_code[0] && strcmp(s_ws_finish_code, "success") != 0) {
            ESP_LOGE(TAG, "WS TTS failed: %s (%s)", s_ws_finish_code, s_ws_finish_msg);
        } else if (s_ws_error) {
            ESP_LOGE(TAG, "WS TTS failed before any audio");
        } else {
            ESP_LOGW(TAG, "WS TTS delivered no audio");
        }
        err = ESP_FAIL;
    } else {
        err = ESP_OK;
    }

out:
    if (s_ws) {
        /* Do not block on a clean close handshake: the service is still busy
         * with its late finish bookkeeping.  A short close + stop releases the
         * single TLS session this board can afford. */
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(300));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    return err;
}
#endif /* CONFIG_VOICE_TTS_WS */

/* --------------------------- stream statistics -------------------------- */

static void log_stream_stats(const pcm_pipe_t *p, const char *path,
                             unsigned chars, int64_t t0)
{
    int64_t now = esp_timer_get_time();
    long long total_ms = (now - t0) / 1000;
    long long connect_ms = p->t_connected ? (p->t_connected - t0) / 1000 : -1;
    long long first_ms = p->t_first_audio ? (p->t_first_audio - t0) / 1000 : -1;
    long long first_after_text = (p->t_first_audio && p->t_finish_text)
                                     ? (p->t_first_audio - p->t_finish_text) / 1000
                                     : -1;
    ESP_LOGI(TAG, "stream(%s): %u chars -> %.2f s in %lld ms (connect %lld ms, "
                  "first audio %lld ms, +%lld ms after finish_text)",
             path, chars, (double)p->out_samples / PCM_RATE, total_ms, connect_ms,
             first_ms, first_after_text);
    ESP_LOGI(TAG, "stream net: %u blocks, %u raw bytes, gap total %llu ms, "
                  "max gap %u ms, >50ms %u, silence trimmed %u ms",
             (unsigned)p->blocks, (unsigned)p->raw_bytes,
             (unsigned long long)p->gap_total_ms, (unsigned)p->max_gap_ms,
             (unsigned)p->gaps_over_50, (unsigned)(p->trimmed * 1000 / PCM_RATE));
}

/* ------------------------------- public --------------------------------- */

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

    pcm_pipe_t *p = pipe_get();
    if (!p) {
        ESP_LOGE(TAG, "no memory for the PCM pipe");
        return ESP_ERR_NO_MEM;
    }
    int64_t t0 = esp_timer_get_time();
    pipe_reset(p, sink, ctx, t0);

    const char *path;
    esp_err_t err;
#if CONFIG_VOICE_TTS_WS
    /* The WebSocket transport on this board still fails its TLS handshake /
     * upgrade read intermittently (esp_transport_read -135 / certificate chain
     * under PSA memory pressure).  Waiting for it on every sentence costs the
     * user several seconds, so after two consecutive failures the POST stream
     * is used directly for five minutes and the WS path is probed again later. */
    static int s_ws_fail_streak;
    static int64_t s_ws_skip_until_us;
    if (esp_timer_get_time() < s_ws_skip_until_us) {
        path = "http";
        err = http_stream(p, utf8_text);
    } else {
        path = "ws";
        err = ws_stream(p, utf8_text);
        if (p->out_samples > 0) {
            s_ws_fail_streak = 0;
        } else if (p->sink_err == ESP_OK && ws_may_fallback()) {
            s_ws_fail_streak++;
            if (s_ws_fail_streak >= 2) {
                s_ws_skip_until_us = esp_timer_get_time() + 300LL * 1000000;
                ESP_LOGW(TAG, "WS TTS failed twice in a row; using the POST stream for 5 min");
            }
            ESP_LOGW(TAG, "WS TTS failed (%s) after %lld ms - retrying with the POST stream",
                     esp_err_to_name(err), (long long)((esp_timer_get_time() - t0) / 1000));
            pipe_reset(p, sink, ctx, t0);   /* one TLS session at a time: WS is closed now */
            path = "http-fallback";
            err = http_stream(p, utf8_text);
        }
    }
#else
    path = "http";
    err = http_stream(p, utf8_text);
#endif

    if (p->out_samples > 0) {
        if (p->sink_err != ESP_OK) {
            ESP_LOGW(TAG, "cloud stream stopped by playback after %.2f s",
                     (double)p->out_samples / PCM_RATE);
        }
        log_stream_stats(p, path, (unsigned)strlen(utf8_text), t0);
        return ESP_OK;      /* partial audio is still a spoken reply */
    }
    if (p->sink_err != ESP_OK) {
        return p->sink_err;
    }
    ESP_LOGE(TAG, "cloud TTS produced no audio (%s, %s)", path, esp_err_to_name(err));
    return ESP_FAIL;
}
