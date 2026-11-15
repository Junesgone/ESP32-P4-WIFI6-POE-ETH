/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* One persistent HTTPS connection to api.deepseek.com, reused by every cloud
 * call (ASR, chat, TTS, Search, Fetch, OCR...).  Why this exists:
 *
 * Every TLS handshake on this board costs ~3.5 s (RSA certificate chain
 * verification for a DNS-verified DV cert, plus the TLS 1.3 key exchange).
 * The voice turn used to open three fresh sessions - ASR, chat, TTS - so ~10 s
 * of every conversation was handshake.  The DeepSeek edge answers with
 * "Connection: keep-alive" on every endpoint, so one esp_http_client handle can
 * keep the TCP+TLS session open and serve the next request; when the server
 * does close it, ESP-TLS session tickets let the next handshake resume in a
 * fraction of the time.
 *
 * The calls are serialised by the firmware's cloud lock, so a single connection
 * is enough; the console command "ds" prints how many requests it served and
 * how many new connections were opened.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Body consumer: called with each received chunk.  Return ESP_OK to continue,
 * anything else aborts the request (connection dropped). */
typedef esp_err_t (*ds_http_sink_t)(const char *data, size_t len, void *ctx);

/* Send one request over the shared connection.
 *  - method: "GET" / "POST"
 *  - content_type / accept: optional headers (NULL = not set)
 *  - body/body_len: request body (NULL/0 for GET)
 *  - sink: optional streaming consumer; when NULL the body is not collected
 *          (use ds_http_request_collect for JSON endpoints)
 *  - status_out: HTTP status code (0 on transport failure)
 *  Returns ESP_OK when the request completed (any HTTP status), ESP_FAIL on
 *  transport errors (the connection is reset so the next call reconnects).
 */
esp_err_t ds_http_request(const char *url, const char *method,
                          const char *content_type, const char *accept,
                          const char *body, size_t body_len,
                          int timeout_ms,
                          ds_http_sink_t sink, void *sink_ctx,
                          int *status_out);

/* Same, but collects the whole response body into a PSRAM buffer.
 * *body_out is NUL-terminated and owned by the caller (free()).
 * On non-2xx the response body is still returned (it carries the API's error
 * message) and *status_out is set. */
esp_err_t ds_http_request_collect(const char *url, const char *method,
                                  const char *content_type, const char *accept,
                                  const char *body, size_t body_len,
                                  int timeout_ms,
                                  char **body_out, int *status_out);

/* Override the Bearer token used by subsequent requests (NULL or "" restores
 * CONFIG_DEEPSEEK_API_KEY).  ASR/TTS call this when they have a dedicated key
 * configured; with the default single-key setup it is never needed. */
void ds_http_set_auth(const char *token);

/* --- streaming request over the same shared connection -------------------
 * Needed for the chat SSE endpoint, which answers "Connection: close" and
 * leaves the chunked body open after [DONE]: we open the request, read until
 * the caller has seen [DONE], then close the socket while KEEPING the client
 * (and its TLS session ticket) for the next request. */
esp_err_t ds_http_stream_open(const char *url, const char *content_type,
                              const char *accept, const char *body, size_t body_len,
                              int timeout_ms, int *status_out);
/* >0 bytes read, 0 on end of stream, <0 on transport error. */
int ds_http_stream_read(char *buf, size_t len);
/* Close the current response/connection but keep the client for reuse. */
void ds_http_stream_close(void);

/* Last non-200 response body (for the log), always NUL-terminated. */
const char *ds_http_last_error(void);

/* Statistics for the console: requests served, connections opened. */
void ds_http_stats(int *requests, int *connects);

/* Timestamp (esp_timer_get_time) of the last new TLS connection, 0 when none
 * has been opened yet.  Callers compare before/after a request to tell whether
 * the connection was reused. */
int64_t ds_http_last_connect_us(void);

/* Drop the shared connection (console "dshttp reset" / after a transport
 * error).  The next request opens a fresh one. */
void ds_http_reset(void);
