/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_ds_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_deepseek.h"      /* deepseek_tls_attach(): pinned cert verifier */
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ds-http";

#define DS_HOST            "api.deepseek.com"
#define REQ_BUFFER_RX      2048
#define REQ_BUFFER_TX      2048
#define COLLECT_MAX        (512 * 1024)   /* an OCR/search page can be large */
#define ERR_MAX            512

static esp_http_client_handle_t s_client;
static ds_http_sink_t s_sink;
static void          *s_sink_ctx;
static bool           s_collecting;
static char          *s_collect;
static size_t         s_collect_len;
static size_t         s_collect_cap;
static char           s_err[ERR_MAX];
static int            s_status;
static int            s_requests;
static int            s_connects;
static int64_t        s_last_connect_us;

static char auth_header[192];
static char auth_override[192];

static esp_err_t collect_append(const char *data, size_t len)
{
    if (s_collect_len + len + 1 > s_collect_cap) {
        size_t ncap = s_collect_cap ? s_collect_cap * 2 : 8192;
        while (ncap < s_collect_len + len + 1) {
            ncap *= 2;
        }
        if (ncap > COLLECT_MAX) {
            ncap = COLLECT_MAX;
        }
        if (s_collect_len + len + 1 > ncap) {
            ESP_LOGW(TAG, "response truncated at %u bytes", (unsigned)s_collect_len);
            len = ncap - s_collect_len - 1;
        }
        char *nb = heap_caps_realloc(s_collect, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            nb = realloc(s_collect, ncap);
        }
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        s_collect = nb;
        s_collect_cap = ncap;
    }
    if (len) {
        memcpy(s_collect + s_collect_len, data, len);
        s_collect_len += len;
    }
    s_collect[s_collect_len] = 0;
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        s_connects++;
        s_last_connect_us = esp_timer_get_time();
        ESP_LOGI(TAG, "new TLS connection (%d so far, %d requests)", s_connects, s_requests);
        break;
    case HTTP_EVENT_ON_STATUS_CODE:
        if (evt->data) {
            s_status = *(int *)evt->data;
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (!evt->data || evt->data_len <= 0) {
            break;
        }
        if (s_status != 200) {
            size_t room = sizeof(s_err) - 1 - (size_t)(s_err[0] ? strlen(s_err) : 0);
            /* s_err is a plain append buffer; keep it simple */
            size_t used = strlen(s_err);
            size_t n = (size_t)evt->data_len;
            if (n > room) {
                n = room;
            }
            if (n) {
                memcpy(s_err + used, evt->data, n);
                s_err[used + n] = 0;
            }
            break;
        }
        if (s_sink) {
            return s_sink((const char *)evt->data, (size_t)evt->data_len, s_sink_ctx);
        }
        if (s_collecting) {
            return collect_append((const char *)evt->data, (size_t)evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t client_ensure(void)
{
    if (s_client) {
        return ESP_OK;
    }
    esp_http_client_config_t cfg = {
        .url = "https://" DS_HOST "/",
        .timeout_ms = 45000,
        .buffer_size = REQ_BUFFER_RX,
        .buffer_size_tx = REQ_BUFFER_TX,
        .crt_bundle_attach = deepseek_tls_attach,
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        .save_client_session = true,
#endif
        .event_handler = http_event,
        .disable_auto_redirect = true,
    };
    s_client = esp_http_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "http client init failed");
        return ESP_ERR_NO_MEM;
    }
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);
    return ESP_OK;
}

void ds_http_set_auth(const char *token)
{
    if (token && token[0]) {
        snprintf(auth_override, sizeof(auth_override), "Bearer %s", token);
    } else {
        auth_override[0] = 0;
    }
}

void ds_http_reset(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    if (s_collect) {
        free(s_collect);
        s_collect = NULL;
    }
    s_collect_len = 0;
    s_collect_cap = 0;
    ESP_LOGW(TAG, "connection dropped");
}

const char *ds_http_last_error(void)
{
    return s_err;
}

void ds_http_stats(int *requests, int *connects)
{
    if (requests) {
        *requests = s_requests;
    }
    if (connects) {
        *connects = s_connects;
    }
}

int64_t ds_http_last_connect_us(void)
{
    return s_last_connect_us;
}

static esp_err_t request_common(const char *url, const char *method,
                                const char *content_type, const char *accept,
                                const char *body, size_t body_len,
                                int timeout_ms,
                                ds_http_sink_t sink, void *sink_ctx,
                                bool collect, int *status_out)
{
    if (status_out) {
        *status_out = 0;
    }
    s_sink = sink;
    s_sink_ctx = sink_ctx;
    s_collecting = collect;
    s_collect_len = 0;
    s_err[0] = 0;
    s_status = 0;
    if (s_collect && s_collect) {
        s_collect[0] = 0;
    }

    if (client_ensure() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_url(s_client, url);
    if (err != ESP_OK) {
        /* A host change closes the connection; re-seed the client. */
        ds_http_reset();
        if (client_ensure() != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
        err = esp_http_client_set_url(s_client, url);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_url failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    bool is_post = (method && strcmp(method, "POST") == 0);
    esp_http_client_set_method(s_client, is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_timeout_ms(s_client, timeout_ms);
    esp_http_client_set_header(s_client, "Authorization",
                               auth_override[0] ? auth_override : auth_header);
    if (content_type) {
        esp_http_client_set_header(s_client, "Content-Type", content_type);
    }
    if (accept) {
        esp_http_client_set_header(s_client, "Accept", accept);
    }
    if (is_post) {
        esp_http_client_set_post_field(s_client, body, (int)body_len);
    } else {
        esp_http_client_set_post_field(s_client, NULL, 0);
    }

    s_requests++;
    err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    if (status_out) {
        *status_out = status;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request %s failed: %s", url, esp_err_to_name(err));
        ds_http_reset();
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d from %s: %.200s", status, url, s_err);
    }
    return ESP_OK;
}

esp_err_t ds_http_request(const char *url, const char *method,
                          const char *content_type, const char *accept,
                          const char *body, size_t body_len,
                          int timeout_ms,
                          ds_http_sink_t sink, void *sink_ctx,
                          int *status_out)
{
    return request_common(url, method, content_type, accept, body, body_len,
                          timeout_ms, sink, sink_ctx, false, status_out);
}

/* ---------------------------- streaming request ------------------------- */

esp_err_t ds_http_stream_open(const char *url, const char *content_type,
                              const char *accept, const char *body, size_t body_len,
                              int timeout_ms, int *status_out)
{
    if (status_out) {
        *status_out = 0;
    }
    if (client_ensure() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_url(s_client, url);
    if (err != ESP_OK) {
        ds_http_reset();
        if (client_ensure() != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
        err = esp_http_client_set_url(s_client, url);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_timeout_ms(s_client, timeout_ms);
    esp_http_client_set_header(s_client, "Authorization",
                               auth_override[0] ? auth_override : auth_header);
    if (content_type) {
        esp_http_client_set_header(s_client, "Content-Type", content_type);
    }
    if (accept) {
        esp_http_client_set_header(s_client, "Accept", accept);
    }
    s_requests++;
    err = esp_http_client_open(s_client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream open %s failed: %s", url, esp_err_to_name(err));
        ds_http_reset();
        return ESP_FAIL;
    }
    if (body_len) {
        int off = 0;
        while (off < (int)body_len) {
            int w = esp_http_client_write(s_client, body + off, (int)body_len - off);
            if (w <= 0) {
                ESP_LOGW(TAG, "stream body write failed");
                ds_http_reset();
                return ESP_FAIL;
            }
            off += w;
        }
    }
    esp_http_client_fetch_headers(s_client);
    int st = esp_http_client_get_status_code(s_client);
    if (status_out) {
        *status_out = st;
    }
    if (st != 200) {
        /* keep the error body readable through ds_http_stream_read */
        ESP_LOGW(TAG, "stream HTTP %d from %s", st, url);
    }
    return ESP_OK;
}

int ds_http_stream_read(char *buf, size_t len)
{
    if (!s_client) {
        return 0;
    }
    return esp_http_client_read(s_client, buf, (int)len);
}

void ds_http_stream_close(void)
{
    if (s_client) {
        esp_http_client_close(s_client);   /* socket closed, client + ticket kept */
    }
}

esp_err_t ds_http_request_collect(const char *url, const char *method,
                                  const char *content_type, const char *accept,
                                  const char *body, size_t body_len,
                                  int timeout_ms,
                                  char **body_out, int *status_out)
{
    if (body_out) {
        *body_out = NULL;
    }
    esp_err_t err = request_common(url, method, content_type, accept, body, body_len,
                                   timeout_ms, NULL, NULL, true, status_out);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_collect) {
        s_collect_cap = 1024;
        s_collect = heap_caps_malloc(s_collect_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_collect) {
            s_collect = malloc(s_collect_cap);
        }
        if (!s_collect) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_collect[s_collect_len] = 0;
    if (body_out) {
        *body_out = s_collect;
        s_collect = NULL;       /* ownership passes to the caller */
        s_collect_len = 0;
        s_collect_cap = 0;
    }
    return ESP_OK;
}
