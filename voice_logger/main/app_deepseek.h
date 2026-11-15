/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* DeepSeek full-suite client for 小新.
 *
 * Two layers live here.
 *
 * 1) Chat Completions with the same built-in capabilities the DeepSeek App
 *    uses.  The service does the work; the board only declares the parameters:
 *      - thinking {"type":"enabled"} + reasoning_effort  -> 深度思考
 *      - tool deepseek_web_search  -> 联网搜索, and with thinking enabled it
 *        becomes 深度思考搜索 (search / open pages / fact-check in reasoning)
 *      - tool deepseek_read_url    -> 阅读链接 (URLs in the user message)
 *      - safety_check {"level":"high"} -> 输入输出风控, refusal markers
 *      - title {"enabled":true}    -> short conversation title
 *      - latest_reminder message   -> 用户元数据标记 (date/weekday/city/App/中文)
 *    The device function tools (set_volume, reminders, ...) stay on top of
 *    that and are executed locally from tool_calls.
 *
 * 2) The standalone tool APIs, each usable on its own:
 *      POST /v1/tools/search  联网搜索
 *      POST /v1/tools/fetch   抓取网页
 *      POST /v1/tools/ocr     PDF/Office/图片 OCR
 *      POST /files            Files API 上传（文件输入 + 图片输入）
 *    (ASR and TTS are their own modules: app_asr.c / app_tts_cloud.c.)
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* ------------------------------------------------------------------ */
/* Runtime capability switches.  Initial values come from menuconfig;  */
/* the serial console flips them without a rebuild.                    */
/* ------------------------------------------------------------------ */
typedef struct {
    bool thinking;      /* 深度思考 */
    bool web_search;    /* deepseek_web_search (with thinking: 深度思考搜索) */
    bool read_url;      /* deepseek_read_url */
    bool safety;        /* safety_check high */
    bool title;         /* conversation title */
} ds_features_t;

const ds_features_t *deepseek_features(void);
void deepseek_set_thinking(bool on);
void deepseek_set_web_search(bool on);
void deepseek_set_read_url(bool on);
void deepseek_set_safety(bool on);
void deepseek_set_title(bool on);

/* low / high / max.  Returns false for an unknown name. */
const char *deepseek_effort(void);
bool deepseek_set_effort(const char *effort);

/* Title of the current conversation ("" until the service has returned one). */
const char *deepseek_title(void);

/* ------------------------------------------------------------------ */
/* What the service did during the last streamed turn.  Printed by the  */
/* console and written to the TF-card JSONL record.                    */
/* ------------------------------------------------------------------ */
#define DS_MAX_CITES    3
#define DS_CITE_URL_MAX 200

typedef struct {
    char    title[96];                    /* conversation title */
    char    finish[24];                   /* finish_reason of the answer */
    char    cites[DS_MAX_CITES][DS_CITE_URL_MAX];  /* citation urls */
    int     n_cites;
    int     searches;                     /* search steps the service ran */
    int     link_reads;                   /* links the service read */
    int     steps;                        /* reasoning/tool steps */
    int     reasoning_chars;              /* thinking text seen (chars) */
    bool    refused;                      /* content-filter refusal */
    int64_t first_content_ms;             /* request -> first answer token */
} ds_turn_info_t;

const ds_turn_info_t *deepseek_turn_info(void);

/* ------------------------------------------------------------------ */
/* Chat (unchanged public API)                                         */
/* ------------------------------------------------------------------ */
esp_err_t deepseek_ask(const char *user_text, char *reply_out, size_t reply_len);
esp_err_t deepseek_ask_stream(const char *user_text, char *reply_out, size_t reply_len,
                              bool *spoken_out);
/* Select whose conversation memory this turn belongs to (voiceprint label:
 * an enrolled name, 人N, or "unknown").  A speaker change clears the in-RAM
 * context and preloads that person's remembered turns from the TF card, so a
 * recognised person is not a stranger on every boot. */
void deepseek_set_speaker(const char *speaker);
const char *deepseek_speaker(void);

esp_err_t deepseek_check(const char *sys_prompt, const char *user_prompt,
                         char *out, size_t out_len);
esp_err_t deepseek_judge(const char *system_prompt, const char *user_prompt,
                         char *out, size_t out_len);
void deepseek_reset_history(void);
int deepseek_turn_count(void);
bool deepseek_configured(void);

/* ------------------------------------------------------------------ */
/* TLS verifier for every DeepSeek endpoint                            */
/* ------------------------------------------------------------------ */
/* api.deepseek.com's chain (leaf <- TrustAsia DV TLS RSA CA 2025 <-
 * DigiCert Global Root G2) intermittently fails mbedtls' chain builder on this
 * board, which then reports "No matching trusted root certificate found".  This
 * wrapper accepts exactly the known leaf fingerprint when the upper chain has
 * already been validated, and everything else still goes through the bundle.
 * Every HTTPS/WSS client that talks to DeepSeek must pass this as
 * crt_bundle_attach - ASR and TTS used the plain bundle and failed handshakes
 * with -0x3000 on the same certificate. */
esp_err_t deepseek_tls_attach(void *conf);

/* ------------------------------------------------------------------ */
/* Standalone full-suite APIs                                          */
/* ------------------------------------------------------------------ */

/* POST /v1/tools/search.  Formats the ranked results as text a model or a
 * human can read.  need_content also asks for the page body. */
esp_err_t deepseek_search(const char *query, int count, bool need_content,
                          char *out, size_t outlen);

/* POST /v1/tools/fetch.  Title + Markdown body of one URL. */
esp_err_t deepseek_fetch(const char *url, char *out, size_t outlen);

/* POST /v1/tools/ocr.  Page-by-page text of a PDF/Word/PowerPoint/image on
 * the TF card.  rich_layout keeps the figures (costs input tokens). */
esp_err_t deepseek_ocr(const char *path, bool rich_layout, char *out, size_t outlen);

/* POST /files.  Uploads one file and stores its file_id.  purpose is
 * "user_data" (plain text / image / spreadsheet / ebook) or
 * "text_recognition" / "rich_layout_recognition" (PDF, Word, PowerPoint). */
esp_err_t deepseek_upload(const char *path, const char *purpose,
                          char *file_id, size_t file_id_len);

/* Full 文件输入 path: upload (parsed) then ask a question about it in one
 * chat request, the way "upload a file" works in the App.  No history. */
esp_err_t deepseek_ask_file(const char *path, const char *question,
                            char *reply, size_t reply_len);

/* True for file extensions the full-suite file input understands. */
bool deepseek_file_supported(const char *path);
