/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_deepseek.h"
#include "app_conversation.h"
#include "app_identity.h"
#include "app_persona.h"
#include "app_storage.h"
#include "app_tools.h"
#include "app_tts.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

/* esp_crt_bundle.c exports the real verification callback but does not declare
 * it in esp_crt_bundle.h.  Declared here so it can be wrapped. */
int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, const int depth,
                            uint32_t *flags);

static const char *TAG = "deepseek";

/* ---------------- certificate verifier robustness -------------------------
 * The DeepSeek handshake intermittently fails while the rest of the board keeps
 * working: the chain itself is fine (root, TrustAsia intermediate and leaf all
 * arrive), the root is validated against the bundle, but mbedtls' chain builder
 * marks the *leaf* NOT_TRUSTED - i.e. its own leaf->intermediate signature
 * check returned an error that is not a "bad signature".  When that happens the
 * bundle callback is then asked about the leaf, cannot find "TrustAsia DV TLS
 * RSA CA 2025" among the roots, and the whole handshake is thrown away with
 * "No matching trusted root certificate found".
 *
 * So: if (and only if) the upper chain has already been validated, retry the
 * leaf link by hand.  A successful RSA check means the chain is sound and the
 * link is cleared; a failure is logged with its error code. */
static void ds_log_cn(const mbedtls_x509_buf *raw)
{
    static const unsigned char pat[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };  /* CN OID */
    for (size_t i = 0; i + sizeof(pat) + 2 <= raw->len; i++) {
        if (memcmp(raw->p + i, pat, sizeof(pat)) != 0) {
            continue;
        }
        const unsigned char *p = raw->p + i + sizeof(pat);
        unsigned char type = *p++;
        size_t len = *p++;
        if ((type == 0x0C || type == 0x13 || type == 0x16) && p + len <= raw->p + raw->len) {
            ESP_LOGI(TAG, "  cert CN=%.*s", (int)len, (const char *)p);
            return;
        }
    }
    ESP_LOGI(TAG, "  cert CN=<unparsed>");
}

static void ds_log_serial(const mbedtls_x509_crt *crt)
{
    char hex[2 * 16 + 1];
    int pos = 0;
    for (size_t i = 0; i < crt->serial.len && i < 16; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - (size_t)pos, "%02x", crt->serial.p[i]);
    }
    hex[pos] = 0;
    ESP_LOGI(TAG, "  cert serial=%s tbs=%u sig=%u", hex, (unsigned)crt->tbs.len,
             (unsigned)crt->MBEDTLS_PRIVATE(sig).len);
}

/* Fingerprint (SHA-256 of the DER certificate) of api.deepseek.com's leaf.
 * Today's chain is: leaf (RSA-2048) <- TrustAsia DV TLS RSA CA 2025 (RSA-4096)
 * <- DigiCert Global Root G2.  The leaf link is signed with a 4096-bit key and
 * mbedtls' chain builder intermittently drops it with PSA error -141
 * (PSA_ERROR_INSUFFICIENT_MEMORY) even though ~54 KB of internal heap is free;
 * the bundle callback is then asked about the leaf, does not have TrustAsia
 * among the roots, and the whole handshake is lost (the "No matching trusted
 * root certificate found" in the log).
 *
 * When normal verification fails, accept exactly this one known certificate by
 * fingerprint.  Everything else still goes through the bundle: the upper chain
 * (leaf -> TrustAsia -> DigiCert) has already been validated by mbedtls before
 * this callback runs for the leaf, and the EXPIRED / NOT_YET_VALID flags are
 * kept, so an expired copy of the same certificate is still rejected.  When
 * DeepSeek rotates the certificate the pin stops matching and verification
 * falls back to the normal path. */
static const unsigned char DS_LEAF_SHA256[32] = {
    0x30, 0xd8, 0x25, 0x29, 0xd1, 0x9c, 0x5d, 0xa6, 0x17, 0x5a, 0x80, 0xdb, 0x1c, 0x55, 0x22, 0xac,
    0x3b, 0xa5, 0xb4, 0x25, 0x89, 0x21, 0x24, 0x05, 0xcd, 0xc8, 0x78, 0x3a, 0x1b, 0xf0, 0x47, 0xfa,
};

static bool ds_leaf_pinned(const mbedtls_x509_crt *crt)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    unsigned char digest[32];
    if (md == NULL || crt->raw.len == 0) {
        return false;
    }
    if (mbedtls_md(md, crt->raw.p, crt->raw.len, digest) != 0) {
        return false;
    }
    return memcmp(digest, DS_LEAF_SHA256, sizeof(digest)) == 0;
}

/* The chain builder also intermittently drops the ROOT link: mbedtls reports
 * DigiCert Global Root G2 at depth 2 as NOT_TRUSTED after its own signature
 * check failed under PSA memory pressure, the bundle callback then cannot match
 * it, and the handshake dies with -0x3000.  The HTTP paths retry and usually win
 * the next handshake; the WebSocket transport gives up after one attempt, so
 * pin the exact root by subject CN + serial number. */
static bool ds_root_pinned(const mbedtls_x509_crt *crt)
{
    static const unsigned char serial[] = {
        0x03, 0x3a, 0xf1, 0xe6, 0xa7, 0x11, 0xa9, 0xa0,
        0xbb, 0x28, 0x64, 0xb1, 0x1d, 0x09, 0xfa, 0xe5,
    };
    static const char cn[] = "DigiCert Global Root G2";
    if (crt->serial.len != sizeof(serial) ||
        memcmp(crt->serial.p, serial, sizeof(serial)) != 0) {
        ESP_LOGW(TAG, "root pin: serial mismatch (len %u)", (unsigned)crt->serial.len);
        return false;
    }
    const unsigned char *p = crt->subject_raw.p;
    size_t n = crt->subject_raw.len;
    for (size_t i = 0; i + sizeof(cn) <= n; i++) {
        if (memcmp(p + i, cn, sizeof(cn) - 1) == 0) {
            return true;
        }
    }
    ESP_LOGW(TAG, "root pin: CN not found in subject_raw (%u bytes)", (unsigned)n);
    return false;
}

static int ds_verify_cb(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    uint32_t before = *flags;

    if (depth == 0 && (before & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0 &&
        ds_leaf_pinned(crt)) {
        ESP_LOGW(TAG, "chain builder dropped the leaf link; pinned DeepSeek certificate accepted");
        *flags = before & ~(uint32_t)MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        if (*flags == 0) {
            return 0;
        }
    }
    if (depth > 0 && (before & MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0 &&
        ds_root_pinned(crt)) {
        ESP_LOGW(TAG, "chain builder dropped the root link; pinned DigiCert Global Root G2 accepted");
        *flags = before & ~(uint32_t)MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        if (*flags == 0) {
            return 0;
        }
    }

    int r = esp_crt_verify_callback(buf, crt, depth, flags);
    if (r != 0) {
        ESP_LOGE(TAG, "certificate rejected at depth %d (flags 0x%x)", depth, (unsigned)before);
        ds_log_cn(&crt->subject_raw);
        ds_log_serial(crt);
    } else if (before != *flags) {
        ESP_LOGW(TAG, "certificate accepted at depth %d after clearing flags 0x%x", depth,
                 (unsigned)(before & ~*flags));
    }
    return r;
}

esp_err_t deepseek_tls_attach(void *conf)
{
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err == ESP_OK) {
        mbedtls_ssl_conf_verify((mbedtls_ssl_config *)conf, ds_verify_cb, NULL);
    }
    return err;
}

/* ======================================================================== */
/* Endpoints (menuconfig: Voice logger configuration -> DeepSeek)           */
/* ======================================================================== */
#define CHAT_URL          "https://api.deepseek.com/chat/completions"
#define HTTP_TIMEOUT_MS   45000
#define FILE_TIMEOUT_MS   135000   /* upload + parsing may take up to 120 s */
#define HTTP_RESP_MAX     (256 * 1024)
#define MAX_TURNS         6        /* user+assistant pairs kept as context */
/* A web-search turn needs two model round trips (decide to search -> phrase the
 * answer), and the model sometimes also calls get_time / system_status.  Three
 * rounds ran out and produced a completely silent turn. */
#define MAX_TOOL_ROUNDS   6
/* A content-filtered input/output still gets a spoken answer. */
#define REFUSAL_REPLY     "这个话题我就不展开了，换个别的聊聊吧。"
/* server_tool_calls are echoed back into the next request so a page read from
 * a link stays in the context.  They carry an encrypted page signature, so
 * only the most recent turn is kept and only up to this size. */
#define DS_STC_MAX        6144

/* ======================================================================== */
/* Runtime capability switches                                              */
/* ======================================================================== */
static ds_features_t s_feat;
static bool s_feat_ready;
static char s_effort[5];
static char s_title[96];
static bool s_title_pending = true;

static void feat_init(void)
{
    if (s_feat_ready) {
        return;
    }
#ifdef CONFIG_DEEPSEEK_THINKING
    s_feat.thinking = true;
#endif
#ifdef CONFIG_DEEPSEEK_WEB_SEARCH
    s_feat.web_search = true;
#endif
#ifdef CONFIG_DEEPSEEK_READ_URL
    s_feat.read_url = true;
#endif
#ifdef CONFIG_DEEPSEEK_SAFETY
    s_feat.safety = true;
#endif
#ifdef CONFIG_DEEPSEEK_TITLE
    s_feat.title = true;
#endif
    snprintf(s_effort, sizeof(s_effort), "%s", CONFIG_DEEPSEEK_REASONING_EFFORT);
    s_feat_ready = true;
}

const ds_features_t *deepseek_features(void)
{
    feat_init();
    return &s_feat;
}

void deepseek_set_thinking(bool on)   { feat_init(); s_feat.thinking = on;   ESP_LOGW(TAG, "thinking %s", on ? "on" : "off"); }
void deepseek_set_web_search(bool on){ feat_init(); s_feat.web_search = on; ESP_LOGW(TAG, "builtin web search %s", on ? "on" : "off"); }
void deepseek_set_read_url(bool on)   { feat_init(); s_feat.read_url = on;   ESP_LOGW(TAG, "builtin read url %s", on ? "on" : "off"); }
void deepseek_set_safety(bool on)     { feat_init(); s_feat.safety = on;     ESP_LOGW(TAG, "safety check %s", on ? "high" : "off"); }
void deepseek_set_title(bool on)      { feat_init(); s_feat.title = on;      ESP_LOGW(TAG, "conversation title %s", on ? "on" : "off"); }

const char *deepseek_effort(void)
{
    feat_init();
    return s_effort;
}

bool deepseek_set_effort(const char *effort)
{
    feat_init();
    if (!effort || (strcmp(effort, "low") != 0 && strcmp(effort, "high") != 0 &&
                    strcmp(effort, "max") != 0)) {
        return false;
    }
    snprintf(s_effort, sizeof(s_effort), "%s", effort);
    ESP_LOGW(TAG, "reasoning effort -> %s", s_effort);
    return true;
}

const char *deepseek_title(void)
{
    return s_title;
}

/* ======================================================================== */
/* Per-turn report                                                          */
/* ======================================================================== */
static ds_turn_info_t s_turn_info;

const ds_turn_info_t *deepseek_turn_info(void)
{
    return &s_turn_info;
}

static void turn_info_reset(void)
{
    memset(&s_turn_info, 0, sizeof(s_turn_info));
    s_turn_info.first_content_ms = -1;
}

static void turn_info_title(const char *title)
{
    if (!title || !title[0]) {
        return;
    }
    snprintf(s_turn_info.title, sizeof(s_turn_info.title), "%s", title);
    if (strcmp(s_title, title) != 0) {
        snprintf(s_title, sizeof(s_title), "%s", title);
        s_title_pending = false;
        ESP_LOGI(TAG, "conversation title: %s", s_title);
    }
}

/* ======================================================================== */
/* History (role/content + the last server_tool_calls blob)                 */
/* ======================================================================== */
typedef struct {
    char *role;
    char *content;
    char *stc;      /* owned server_tool_calls JSON, only on the newest turn */
} msg_t;

static msg_t s_hist[MAX_TURNS * 2];
static int   s_hist_len;
static int   s_turns;

bool deepseek_configured(void)
{
    return CONFIG_DEEPSEEK_API_KEY[0] != 0;
}

int deepseek_turn_count(void)
{
    return s_turns;
}

void deepseek_reset_history(void)
{
    for (int i = 0; i < s_hist_len; i++) {
        free(s_hist[i].role);
        free(s_hist[i].content);
        free(s_hist[i].stc);
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_hist_len = 0;
    s_turns = 0;
    s_title[0] = 0;
    s_title_pending = true;
}

static void hist_clear_stc(void)
{
    for (int i = 0; i < s_hist_len; i++) {
        free(s_hist[i].stc);
        s_hist[i].stc = NULL;
    }
}

/* Append one message.  stc_owned (may be NULL) is transferred to the history
 * and only ever attached to the newest assistant turn: older pages are dropped
 * so the request body (and the internal-RAM cost of re-parsing it) stays small. */
static void push_msg_full(const char *role, const char *content, char *stc_owned)
{
    if (s_hist_len == MAX_TURNS * 2) {
        free(s_hist[0].role);
        free(s_hist[0].content);
        free(s_hist[0].stc);
        memmove(&s_hist[0], &s_hist[1], sizeof(msg_t) * (MAX_TURNS * 2 - 1));
        s_hist_len--;
        s_hist[s_hist_len].stc = NULL;
    }
    if (stc_owned) {
        hist_clear_stc();
    }
    s_hist[s_hist_len].role = strdup(role);
    s_hist[s_hist_len].content = strdup(content);
    s_hist[s_hist_len].stc = stc_owned;
    if (s_hist[s_hist_len].role && s_hist[s_hist_len].content) {
        s_hist_len++;
    } else {
        free(s_hist[s_hist_len].role);
        free(s_hist[s_hist_len].content);
        free(s_hist[s_hist_len].stc);
        s_hist[s_hist_len].stc = NULL;
    }
}

static void push_msg(const char *role, const char *content)
{
    push_msg_full(role, content, NULL);
}

/* ======================================================================== */
/* User metadata marker                                                     */
/* ======================================================================== */
/* DeepSeek Chat tells the model the date, weekday, location, platform and
 * language; the API equivalent is a latest_reminder message placed directly
 * before the user message it belongs to:
 *     <date>,<weekday>,<location>,<platform>,<language>
 * Skipped before SNTP synced - a made-up date is worse than no marker (and a
 * malformed value is rejected with 400). */
static bool build_reminder(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) {
        return false;
    }
    static const char *const wd[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    int w = tm.tm_wday;
    if (w < 0 || w > 6) {
        w = 0;
    }
    const char *city = conversation_city();
    if (!city || !city[0]) {
        city = CONFIG_DEEPSEEK_USER_CITY;
    }
    snprintf(out, len, "%04d-%02d-%02d,%s,%s,%s,%s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wd[w], city,
             CONFIG_DEEPSEEK_USER_PLATFORM, CONFIG_DEEPSEEK_USER_LANGUAGE);
    return true;
}

/* ======================================================================== */
/* Request builder                                                          */
/* ======================================================================== */
typedef struct {
    const char *user_text;      /* plain text user turn (NULL when user_content) */
    const cJSON *user_content;  /* borrowed multimodal content array */
    const cJSON *extra;         /* assistant tool-call + tool messages, borrowed */
    bool stream;
    bool with_history;
    bool title_round;           /* allow title.enabled on this request */
} ds_req_opts_t;

typedef struct {
    cJSON *arr;
    int n;
} ds_tools_t;

/* Built-in server-side tools first, then the device function tools.  The
 * client-side "web_search" function is dropped while the built-in tool is on:
 * the service then runs the search itself (and with thinking enabled it is
 * deep thinking search), so two search paths would only confuse the model. */
static void add_tools(cJSON *root, bool include_device_tools)
{
    cJSON *tools = cJSON_CreateArray();
    if (!tools) {
        return;
    }
    if (s_feat.web_search) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "deepseek_web_search");
        cJSON_AddItemToArray(tools, t);
    }
    if (s_feat.read_url) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "deepseek_read_url");
        cJSON_AddItemToArray(tools, t);
    }
    if (include_device_tools) {
        cJSON *fn = tools_spec();
        if (fn) {
            const cJSON *it = NULL;
            cJSON_ArrayForEach(it, fn) {
                const cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "function");
                const cJSON *nm = f ? cJSON_GetObjectItemCaseSensitive(f, "name") : NULL;
                if (s_feat.web_search && cJSON_IsString(nm) &&
                    strcmp(nm->valuestring, "web_search") == 0) {
                    continue;
                }
                cJSON_AddItemToArray(tools, cJSON_Duplicate(it, 1));
            }
            cJSON_Delete(fn);
        }
    }
    if (cJSON_GetArraySize(tools) == 0) {
        cJSON_Delete(tools);
        return;
    }
    cJSON_AddItemToObject(root, "tools", tools);
    cJSON_AddStringToObject(root, "tool_choice", "auto");
}

static void add_thinking(cJSON *root)
{
    cJSON *th = cJSON_AddObjectToObject(root, "thinking");
    if (!th) {
        return;
    }
    if (s_feat.thinking) {
        cJSON_AddStringToObject(th, "type", "enabled");
        cJSON_AddStringToObject(root, "reasoning_effort", s_effort);
    } else {
        cJSON_AddStringToObject(th, "type", "disabled");
    }
}

static void add_safety_and_title(cJSON *root, bool title_round)
{
    if (s_feat.safety) {
        cJSON *sc = cJSON_AddObjectToObject(root, "safety_check");
        if (sc) {
            cJSON_AddStringToObject(sc, "level", "high");
        }
    }
    /* The service waits up to 5 s for the title after the answer, so it is only
     * requested until the conversation has one, and only on the first model
     * request of a turn (the guide's recommendation). */
    if (s_feat.title && s_title_pending && title_round) {
        cJSON *ti = cJSON_AddObjectToObject(root, "title");
        if (ti) {
            cJSON_AddBoolToObject(ti, "enabled", 1);
        }
    }
}

static void add_system_prompt(cJSON *msgs, const char *speaker)
{
    /* Static so a long persona prompt (a few hundred Chinese chars) fits
     * without eating the caller's stack; calls are serialised by the cloud
     * lock.  The speaker line is the on-board voiceprint result - someone
     * recognised by the board is named, everyone else stays anonymous. */
    static char sysbuf[4096];
    /* 联网搜索/读链接由服务端完成，模型拿到的是整页资料，很容易切成
     * markdown 列表和加粗标题 - 那些符号会被 TTS 念成噪音，长答案也会
     * 让板子说上十几秒。 所以补一句"查到也只用口语说结论"。 */
    static const char *spoken_rules =
        "\n补充：你的回答会被直接念出来。不管有没有联网查资料，都只用一到三句"
        "口语说结论：不要列条目、不要标题、不要加粗、不要编号、不要念链接。"
        "想清楚再答，但只说结论，不复述推理过程。";
    if (speaker && strcmp(speaker, IDENTITY_UNKNOWN) != 0) {
        snprintf(sysbuf, sizeof(sysbuf), "%s%s\n当前和你说话的人已通过声纹识别为：%s。",
                 XIAOXIN_SYSTEM_PROMPT, spoken_rules, speaker);
    } else {
        snprintf(sysbuf, sizeof(sysbuf), "%s%s\n当前说话人未能通过声纹识别。",
                 XIAOXIN_SYSTEM_PROMPT, spoken_rules);
    }
    cJSON *sys = cJSON_CreateObject();
    if (!sys) {
        return;
    }
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", sysbuf);
    cJSON_AddItemToArray(msgs, sys);
}

static void add_history(cJSON *msgs)
{
    for (int i = 0; i < s_hist_len; i++) {
        cJSON *m = cJSON_CreateObject();
        if (!m) {
            return;
        }
        cJSON_AddStringToObject(m, "role", s_hist[i].role);
        cJSON_AddStringToObject(m, "content", s_hist[i].content);
        if (s_hist[i].stc && s_hist[i].stc[0] && strcmp(s_hist[i].role, "assistant") == 0) {
            cJSON *stc = cJSON_Parse(s_hist[i].stc);
            if (stc) {
                cJSON_AddItemToObject(m, "server_tool_calls", stc);
            }
        }
        cJSON_AddItemToArray(msgs, m);
    }
}

static char *build_request(const ds_req_opts_t *o)
{
    feat_init();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", o->stream ? 1 : 0);
    cJSON_AddNumberToObject(root, "max_tokens", CONFIG_DEEPSEEK_MAX_TOKENS);
    cJSON_AddNumberToObject(root, "temperature",
                            (double)CONFIG_DEEPSEEK_TEMPERATURE_X100 / 100.0);
    add_thinking(root);
    add_safety_and_title(root, o->title_round);
    add_tools(root, true);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    add_system_prompt(msgs, identity_speaker_id());
    if (o->with_history) {
        add_history(msgs);
    }
    /* latest_reminder must be directly followed by the user message. */
    char reminder[192];
    if (build_reminder(reminder, sizeof(reminder))) {
        cJSON *rm = cJSON_CreateObject();
        if (rm) {
            cJSON_AddStringToObject(rm, "role", "latest_reminder");
            cJSON_AddStringToObject(rm, "content", reminder);
            cJSON_AddItemToArray(msgs, rm);
        }
    }
    cJSON *u = cJSON_CreateObject();
    if (!u) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(u, "role", "user");
    if (o->user_content) {
        cJSON_AddItemToObject(u, "content", cJSON_Duplicate((cJSON *)o->user_content, 1));
    } else {
        cJSON_AddStringToObject(u, "content", o->user_text ? o->user_text : "");
    }
    cJSON_AddItemToArray(msgs, u);

    /* Assistant tool_calls + tool results produced during this turn. */
    if (o->extra) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, (cJSON *)o->extra) {
            cJSON_AddItemToArray(msgs, cJSON_Duplicate((cJSON *)item, 1));
        }
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* ======================================================================== */
/* HTTPS helpers                                                            */
/* ======================================================================== */
static void ds_add_auth(esp_http_client_handle_t c)
{
    static char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);
    esp_http_client_set_header(c, "Authorization", auth);
}

/* One HTTPS POST with a JSON body; the whole response body is returned in
 * PSRAM (NUL-terminated).  status_out carries the HTTP status even for
 * non-200 replies, because the caller wants the error body for the log. */
static esp_err_t ds_http_post_raw(const char *url, const char *body, size_t body_len,
                                  int timeout_ms, bool long_lived, char **resp_out,
                                  int *status_out)
{
    if (resp_out) {
        *resp_out = NULL;
    }
    if (status_out) {
        *status_out = 0;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .crt_bundle_attach = deepseek_tls_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    ds_add_auth(c);

    esp_err_t err = esp_http_client_open(c, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return err;
    }
    if (body_len) {
        esp_http_client_write(c, body, (int)body_len);
    }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    size_t cap = 8192, used = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    while (1) {
        if (used + 1 >= cap) {
            if (cap >= HTTP_RESP_MAX) {
                ESP_LOGW(TAG, "response capped at %u bytes", (unsigned)cap);
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > HTTP_RESP_MAX) {
                ncap = HTTP_RESP_MAX;
            }
            char *nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                nb = realloc(buf, ncap);
            }
            if (!nb) {
                free(buf);
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(c, buf + used, (int)(cap - 1 - used));
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    buf[used] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    (void)long_lived;
    if (status != 200) {
        ESP_LOGW(TAG, "%s -> HTTP %d: %.240s", url, status, buf);
    }
    if (resp_out) {
        *resp_out = buf;
    } else {
        free(buf);
    }
    if (status_out) {
        *status_out = status;
    }
    return ESP_OK;
}

/* One chat round trip, parsed.  Returns NULL on transport/HTTP/JSON errors. */
static cJSON *post_chat(const char *body, size_t body_len)
{
    char *resp = NULL;
    int status = 0;
    esp_err_t err = ds_http_post_raw(CHAT_URL, body, body_len, HTTP_TIMEOUT_MS, false,
                                     &resp, &status);
    if (err != ESP_OK) {
        return NULL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.220s", status, resp);
        free(resp);
        return NULL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    return root;
}

/* ======================================================================== */
/* Streaming (SSE) support                                                  */
/* ======================================================================== */
/* The reply is spoken sentence by sentence as it streams in when the on-board
 * ESP-SR voice is active; with the cloud neural voice the final answer is
 * handed to app_tts.c once the stream is closed (one TLS session at a time).
 * The stream also carries the full-suite extras: thinking deltas, server-side
 * search / read-link progress, citations, conversation title and refusals. */

typedef struct {
    bool         speak;
    char         content[3072];
    size_t       content_len;
    char         pending[600];
    size_t       pending_len;
    cJSON       *tool_calls;
    bool         spoken;
    bool         refused;       /* finish_reason content_filter / refusal marker */
    bool         http_refused;  /* 400 "Content Exists Risk" on the input */
    int          status;
    char        *stc;           /* PSRAM, server_tool_calls JSON for the history */
    ds_turn_info_t info;
    int64_t      t0;
} stream_ctx_t;

/* 。 ！ ？ ； and their ASCII counterparts end a speakable sentence. */
static bool ends_sentence(const char *s, size_t n)
{
    if (n == 0) {
        return false;
    }
    char c = s[n - 1];
    if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n') {
        return true;
    }
    if (n >= 3 && (unsigned char)s[n - 3] == 0xE3 && (unsigned char)s[n - 2] == 0x80 &&
        (unsigned char)s[n - 1] == 0x82) {          /* 。 */
        return true;
    }
    if (n >= 3 && (unsigned char)s[n - 3] == 0xEF && (unsigned char)s[n - 2] == 0xBC &&
        ((unsigned char)s[n - 1] == 0x81 || (unsigned char)s[n - 1] == 0x9F ||
         (unsigned char)s[n - 1] == 0x9B)) {        /* ！ ？ ； */
        return true;
    }
    return false;
}

static void stream_flush(stream_ctx_t *c)
{
    if (c->pending_len == 0) {
        return;
    }
    c->pending[c->pending_len] = 0;
    size_t start = 0;
    while (start < c->pending_len && (c->pending[start] == ' ' || c->pending[start] == '\n' ||
                                      c->pending[start] == '\r')) {
        start++;
    }
    if (start < c->pending_len && c->speak && tts_ready() && !tts_uses_cloud()) {
        tts_say(&c->pending[start]);
        c->spoken = true;
        ESP_LOGI(TAG, "stream -> tts: %.80s", &c->pending[start]);
    }
    c->pending_len = 0;
}

static void stream_feed(stream_ctx_t *c, const char *text)
{
    if (c->info.first_content_ms < 0) {
        c->info.first_content_ms = (esp_timer_get_time() - c->t0) / 1000;
    }
    for (const char *p = text; *p; p++) {
        if (c->content_len + 1 < sizeof(c->content)) {
            c->content[c->content_len++] = *p;
            c->content[c->content_len] = 0;
        }
        if (c->pending_len + 1 < sizeof(c->pending)) {
            c->pending[c->pending_len++] = *p;
        }
        if (ends_sentence(c->pending, c->pending_len) || c->pending_len >= 150) {
            stream_flush(c);
        }
    }
}

/* tool_calls arrive as fragments: {index, id?, function:{name?, arguments?}} and
 * the arguments are split across deltas, so they have to be concatenated. */
static void stream_tool_delta(stream_ctx_t *c, const cJSON *tcs)
{
    const cJSON *tc = NULL;
    cJSON_ArrayForEach(tc, (cJSON *)tcs) {
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
        int i = cJSON_IsNumber(idx) ? idx->valueint : 0;
        if (i < 0) {
            i = 0;
        }
        if (!c->tool_calls) {
            c->tool_calls = cJSON_CreateArray();
            if (!c->tool_calls) {
                return;
            }
        }
        while (cJSON_GetArraySize(c->tool_calls) <= i) {
            cJSON_AddItemToArray(c->tool_calls, cJSON_CreateObject());
        }
        cJSON *slot = cJSON_GetArrayItem(c->tool_calls, i);
        if (!slot) {
            continue;
        }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
        if (cJSON_IsString(id)) {
            cJSON_DeleteItemFromObjectCaseSensitive(slot, "id");
            cJSON_AddStringToObject(slot, "id", id->valuestring);
        }
        /* Every delta repeats the tool-call scaffolding; cJSON appends a
         * duplicate key instead of replacing, and the server rejects the
         * request with "duplicate field type".  Add it once. */
        if (!cJSON_GetObjectItemCaseSensitive(slot, "type")) {
            cJSON_AddStringToObject(slot, "type", "function");
        }
        cJSON *fn = cJSON_GetObjectItemCaseSensitive(slot, "function");
        if (!fn) {
            fn = cJSON_AddObjectToObject(slot, "function");
        }
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(tc, "function");
        if (!f || !fn) {
            continue;
        }
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(f, "name");
        if (cJSON_IsString(name)) {
            cJSON_DeleteItemFromObjectCaseSensitive(fn, "name");
            cJSON_AddStringToObject(fn, "name", name->valuestring);
        }
        const cJSON *arg = cJSON_GetObjectItemCaseSensitive(f, "arguments");
        if (cJSON_IsString(arg)) {
            const cJSON *prev = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
            char *joined = NULL;
            if (cJSON_IsString(prev)) {
                size_t n = strlen(prev->valuestring) + strlen(arg->valuestring) + 1;
                joined = malloc(n);
                if (joined) {
                    snprintf(joined, n, "%s%s", prev->valuestring, arg->valuestring);
                }
            } else {
                joined = strdup(arg->valuestring);
            }
            if (joined) {
                cJSON_DeleteItemFromObjectCaseSensitive(fn, "arguments");
                cJSON_AddStringToObject(fn, "arguments", joined);
                free(joined);
            }
        }
    }
}

/* One citation url, de-duplicated, newest first. */
static void cite_add(stream_ctx_t *c, const char *url)
{
    if (!url || !url[0] || c->info.n_cites >= DS_MAX_CITES) {
        return;
    }
    for (int i = 0; i < c->info.n_cites; i++) {
        if (strcmp(c->info.cites[i], url) == 0) {
            return;
        }
    }
    snprintf(c->info.cites[c->info.n_cites], DS_CITE_URL_MAX, "%s", url);
    c->info.n_cites++;
}

static void stream_note_annotations(stream_ctx_t *c, const cJSON *arr)
{
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, (cJSON *)arr) {
        const cJSON *uc = cJSON_GetObjectItemCaseSensitive(it, "url_citation");
        const cJSON *url = uc ? cJSON_GetObjectItemCaseSensitive(uc, "url") : NULL;
        if (cJSON_IsString(url)) {
            cite_add(c, url->valuestring);
        }
    }
}

/* server_tool_event is a choice-level extension next to delta.  It is the live
 * progress of the service-side search / page reading: one event per chunk,
 * either step_reasoning_done, call_started or call_completed. */
static void stream_note_server_event(stream_ctx_t *c, const cJSON *ev)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    const cJSON *step = cJSON_GetObjectItemCaseSensitive(ev, "step");
    if (cJSON_IsNumber(step) && step->valueint + 1 > c->info.steps) {
        c->info.steps = step->valueint + 1;
    }
    if (!cJSON_IsString(type) || !type->valuestring) {
        return;
    }
    if (strcmp(type->valuestring, "call_started") != 0) {
        return;
    }
    const cJSON *call = cJSON_GetObjectItemCaseSensitive(ev, "call");
    const cJSON *kind = call ? cJSON_GetObjectItemCaseSensitive(call, "kind") : NULL;
    const cJSON *action = call ? cJSON_GetObjectItemCaseSensitive(call, "action") : NULL;
    const char *k = cJSON_IsString(kind) ? kind->valuestring : "";
    char detail[160] = { 0 };
    if (action) {
        const cJSON *queries = cJSON_GetObjectItemCaseSensitive(action, "queries");
        const cJSON *q0 = cJSON_IsArray(queries) ? cJSON_GetArrayItem(queries, 0) : NULL;
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(action, "url");
        if (cJSON_IsString(q0)) {
            snprintf(detail, sizeof(detail), "%s", q0->valuestring);
        } else if (cJSON_IsString(url)) {
            snprintf(detail, sizeof(detail), "%s", url->valuestring);
        }
    }
    if (strstr(k, "search") != NULL) {
        c->info.searches++;
    } else if (strcmp(k, "read_link") == 0) {
        c->info.link_reads++;
    }
    ESP_LOGI(TAG, "service %s%s%s", k[0] ? k : "tool",
             detail[0] ? ": " : "", detail);
}

static void stream_note_stc(stream_ctx_t *c, const cJSON *arr)
{
    char *txt = cJSON_PrintUnformatted((cJSON *)arr);
    if (!txt) {
        return;
    }
    size_t n = strlen(txt);
    if (n <= DS_STC_MAX) {
        if (!c->stc) {
            c->stc = heap_caps_malloc(DS_STC_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!c->stc) {
                c->stc = malloc(DS_STC_MAX + 1);
            }
        }
        if (c->stc) {
            memcpy(c->stc, txt, n + 1);
        }
    } else {
        ESP_LOGW(TAG, "server_tool_calls too large (%u bytes), not echoed back",
                 (unsigned)n);
    }
    free(txt);
}

static void stream_parse_chunk(stream_ctx_t *c, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        return;
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *ch0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (ch0) {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(ch0, "server_tool_event");
        if (ev) {
            stream_note_server_event(c, ev);
        }
        const cJSON *fr = cJSON_GetObjectItemCaseSensitive(ch0, "finish_reason");
        if (cJSON_IsString(fr) && fr->valuestring && fr->valuestring[0]) {
            snprintf(c->info.finish, sizeof(c->info.finish), "%s", fr->valuestring);
            if (strcmp(fr->valuestring, "content_filter") == 0) {
                c->refused = true;
            }
        }
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(ch0, "delta");
        if (delta) {
            const cJSON *rc = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
            if (cJSON_IsString(rc) && rc->valuestring && rc->valuestring[0]) {
                if (c->info.reasoning_chars == 0) {
                    ESP_LOGI(TAG, "thinking ...");
                }
                c->info.reasoning_chars += (int)strlen(rc->valuestring);
            }
            const cJSON *ct = cJSON_GetObjectItemCaseSensitive(delta, "content");
            if (cJSON_IsString(ct) && ct->valuestring && ct->valuestring[0]) {
                stream_feed(c, ct->valuestring);
            }
            const cJSON *ann = cJSON_GetObjectItemCaseSensitive(delta, "annotations");
            if (cJSON_IsArray(ann)) {
                stream_note_annotations(c, ann);
            }
            const cJSON *stc = cJSON_GetObjectItemCaseSensitive(delta, "server_tool_calls");
            if (cJSON_IsArray(stc)) {
                stream_note_stc(c, stc);
            }
            const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(delta, "title");
            if (cJSON_IsString(ttl) && ttl->valuestring) {
                turn_info_title(ttl->valuestring);
            }
            const cJSON *ref = cJSON_GetObjectItemCaseSensitive(delta, "refusal");
            if (cJSON_IsString(ref) && ref->valuestring && ref->valuestring[0]) {
                c->refused = true;
                ESP_LOGW(TAG, "refusal marker: %.120s", ref->valuestring);
            }
            const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
            if (cJSON_IsArray(tcs)) {
                stream_tool_delta(c, tcs);
            }
        }
    }
    cJSON_Delete(root);
}

/* POST with stream:true, parse the SSE stream, return a synthetic
 * choices[0].message object so the caller's tool loop is unchanged. */
static cJSON *post_chat_stream(const char *body, stream_ctx_t *c)
{
    esp_http_client_config_t cfg = {
        .url = CHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = deepseek_tls_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return NULL;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    ds_add_auth(h);

    int blen = (int)strlen(body);
    if (esp_http_client_open(h, blen) != ESP_OK) {
        ESP_LOGE(TAG, "stream connect failed");
        esp_http_client_cleanup(h);
        return NULL;
    }
    esp_http_client_write(h, body, blen);
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    c->status = status;
    if (status != 200) {
        char err[320] = { 0 };
        int n = esp_http_client_read(h, err, sizeof(err) - 1);
        if (n > 0) {
            err[n] = 0;
        }
        ESP_LOGE(TAG, "stream HTTP %d: %.280s", status, err);
        if (status == 400 && strstr(err, "Content Exists Risk") != NULL) {
            c->http_refused = true;
        }
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return NULL;
    }

    static char chunk[1024];
    /* The last chunk of a web-search turn carries every citation plus the
     * complete server_tool_calls (per-step thinking, sources, encrypted page
     * signature): 10-20 KB of JSON on one "data:" line.  A 1 KB line buffer
     * silently dropped it, which is why citations / titles never showed up.
     * The buffer lives in PSRAM and is reused; calls are serialised by the
     * cloud lock. */
    static char *line;
    static size_t line_cap;
    if (!line) {
        line_cap = 32 * 1024;
        line = heap_caps_malloc(line_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!line) {
            line_cap = 4096;
            line = heap_caps_malloc(line_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!line) {
            ESP_LOGE(TAG, "no PSRAM for the SSE line buffer");
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return NULL;
        }
    }
    size_t line_len = 0;
    bool line_overflow = false;
    bool done = false;
    while (!done) {
        int n = esp_http_client_read(h, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < n && !done; i++) {
            char ch = chunk[i];
            if (ch == '\n') {
                if (line_len && line[line_len - 1] == '\r') {
                    line_len--;
                }
                if (line_overflow) {
                    ESP_LOGW(TAG, "SSE line longer than %u bytes, dropped", (unsigned)line_cap);
                }
                line[line_len] = 0;
                if (!line_overflow && strncmp(line, "data:", 5) == 0) {
                    const char *payload = line + 5;
                    while (*payload == ' ') {
                        payload++;
                    }
                    if (strncmp(payload, "[DONE]", 6) == 0) {
                        done = true;
                    } else {
                        stream_parse_chunk(c, payload);
                    }
                }
                line_len = 0;
                line_overflow = false;
            } else if (line_len + 1 < line_cap) {
                line[line_len++] = ch;
            } else {
                line_overflow = true;
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    stream_flush(c);   /* speak a trailing sentence without punctuation */

    cJSON *msg = cJSON_CreateObject();
    cJSON *root = cJSON_CreateObject();
    if (!msg || !root) {
        cJSON_Delete(msg);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(msg, "role", "assistant");
    if (c->content_len) {
        cJSON_AddStringToObject(msg, "content", c->content);
    }
    if (c->tool_calls) {
        cJSON_AddItemToObject(msg, "tool_calls", c->tool_calls);
        c->tool_calls = NULL;
    }
    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *ch = cJSON_CreateObject();
    cJSON_AddItemToObject(ch, "message", msg);
    cJSON_AddItemToArray(choices, ch);
    return root;
}

/* ======================================================================== */
/* Chat entry points                                                        */
/* ======================================================================== */
static void turn_info_cite(ds_turn_info_t *ti, const char *url)
{
    if (!url || !url[0] || ti->n_cites >= DS_MAX_CITES) {
        return;
    }
    for (int i = 0; i < ti->n_cites; i++) {
        if (strcmp(ti->cites[i], url) == 0) {
            return;
        }
    }
    snprintf(ti->cites[ti->n_cites], DS_CITE_URL_MAX, "%s", url);
    ti->n_cites++;
}

/* Non-streaming path: pull the full-suite extras out of message. */
static char *turn_info_from_message(const cJSON *message)
{
    char *stc_owned = NULL;
    if (!message) {
        return NULL;
    }
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(message, "refusal");
    if (cJSON_IsString(ref) && ref->valuestring && ref->valuestring[0]) {
        s_turn_info.refused = true;
        ESP_LOGW(TAG, "refusal marker: %.120s", ref->valuestring);
    }
    const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(message, "title");
    if (cJSON_IsString(ttl) && ttl->valuestring) {
        turn_info_title(ttl->valuestring);
    }
    const cJSON *ann = cJSON_GetObjectItemCaseSensitive(message, "annotations");
    if (cJSON_IsArray(ann)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, (cJSON *)ann) {
            const cJSON *uc = cJSON_GetObjectItemCaseSensitive(it, "url_citation");
            const cJSON *url = uc ? cJSON_GetObjectItemCaseSensitive(uc, "url") : NULL;
            if (cJSON_IsString(url)) {
                turn_info_cite(&s_turn_info, url->valuestring);
            }
        }
    }
    const cJSON *stc = cJSON_GetObjectItemCaseSensitive(message, "server_tool_calls");
    if (cJSON_IsArray(stc)) {
        const cJSON *step_it = NULL;
        cJSON_ArrayForEach(step_it, (cJSON *)stc) {
            s_turn_info.steps++;
            const cJSON *calls = cJSON_GetObjectItemCaseSensitive(step_it, "calls");
            const cJSON *call = NULL;
            cJSON_ArrayForEach(call, (cJSON *)calls) {
                const cJSON *kind = cJSON_GetObjectItemCaseSensitive(call, "kind");
                const char *k = cJSON_IsString(kind) ? kind->valuestring : "";
                if (strstr(k, "search") != NULL) {
                    s_turn_info.searches++;
                } else if (strcmp(k, "read_link") == 0) {
                    s_turn_info.link_reads++;
                }
            }
        }
        char *txt = cJSON_PrintUnformatted((cJSON *)stc);
        if (txt) {
            if (strlen(txt) <= DS_STC_MAX) {
                stc_owned = txt;
            } else {
                free(txt);
            }
        }
    }
    return stc_owned;
}

static bool speak_short(const char *text);

static esp_err_t deepseek_ask_impl(const char *user_text, char *reply_out, size_t reply_len,
                                   bool speak, bool *spoken_out)
{
    if (spoken_out) {
        *spoken_out = false;
    }
    if (!deepseek_configured()) {
        ESP_LOGE(TAG, "no API key: set CONFIG_DEEPSEEK_API_KEY (menuconfig)");
        return ESP_ERR_INVALID_STATE;
    }
    if (reply_out && reply_len) {
        reply_out[0] = 0;
    }
    feat_init();
    turn_info_reset();

    /* Messages the model must see while it works through tool calls. */
    cJSON *extra = cJSON_CreateArray();
    if (!extra) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_FAIL;
    bool spoke_any = false;
    int transport_retries = 0;
    char *reply = reply_out;

    for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {
        ds_req_opts_t o = {
            .user_text = user_text,
            .user_content = NULL,
            .extra = extra,
            .stream = speak,
            .with_history = true,
            .title_round = (round == 0),
        };
        char *body = build_request(&o);
        if (!body) {
            break;
        }
        stream_ctx_t sc = { 0 };
        sc.speak = speak;
        sc.t0 = esp_timer_get_time();
        sc.info.first_content_ms = -1;
        cJSON *root = speak ? post_chat_stream(body, &sc)
                            : post_chat(body, strlen(body));
        spoke_any = spoke_any || sc.spoken;
        free(body);
        bool timed_out_refused = sc.http_refused;
        /* The streaming path collected citations / search counts / title. */
        if (speak) {
            s_turn_info = sc.info;
        }
        if (!root) {
            if (sc.tool_calls) {
                cJSON_Delete(sc.tool_calls);
                sc.tool_calls = NULL;
            }
            free(sc.stc);
            sc.stc = NULL;
            if (timed_out_refused) {
                /* The input itself was refused by the safety check. */
                s_turn_info.refused = true;
                if (reply_out && reply_len) {
                    snprintf(reply_out, reply_len, "%s", REFUSAL_REPLY);
                    ret = ESP_OK;
                }
                break;
            }
            /* One transient TLS failure used to turn a whole question into the
             * canned apology.  The handshake really fails on internal-RAM
             * pressure while the certificate chain is being RSA-verified (the
             * "No matching trusted root certificate found" line is a symptom:
             * the leaf->intermediate link was dropped as bad), so retry with a
             * growing pause and log the heap that caused it.  A partial SSE
             * stream may already hold tool-call fragments - freed above. */
            if (transport_retries < 3) {
                transport_retries++;
                round--;
                ESP_LOGW(TAG, "transport failed, retrying (%d/3, internal free %u B, largest %u B, dma free %u B)",
                         transport_retries,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
                vTaskDelay(pdMS_TO_TICKS(transport_retries == 1 ? 600 :
                                         transport_retries == 2 ? 1500 : 3000));
                continue;
            }
            break;
        }

        /* server_tool_calls are needed in the assistant history message so a
         * page read from a link stays in the context of the next turn. */
        char *stc_owned = sc.stc;
        sc.stc = NULL;

        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
        const cJSON *finish = first ? cJSON_GetObjectItemCaseSensitive(first, "finish_reason") : NULL;

        if (!speak && message) {
            char *stc_from_msg = turn_info_from_message(message);
            if (!stc_owned) {
                stc_owned = stc_from_msg;
            } else {
                free(stc_from_msg);
            }
        }
        if (cJSON_IsString(finish) && finish->valuestring) {
            snprintf(s_turn_info.finish, sizeof(s_turn_info.finish), "%s", finish->valuestring);
            if (strcmp(finish->valuestring, "content_filter") == 0) {
                s_turn_info.refused = true;
            } else if (strcmp(finish->valuestring, "length") == 0) {
                ESP_LOGW(TAG, "answer hit max_tokens and was cut off");
            }
        }

        if (message) {
            const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
            if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
                free(stc_owned);
                stc_owned = NULL;
                /* Some models attach a sentence next to the tool call.  Keep it:
                 * if the tool loop never reaches a final answer, saying that
                 * sentence beats going completely silent. */
                const cJSON *pre = cJSON_GetObjectItemCaseSensitive(message, "content");
                if (cJSON_IsString(pre) && pre->valuestring && pre->valuestring[0]) {
                    snprintf(reply_out, reply_len, "%s", pre->valuestring);
                    ret = ESP_OK;
                    reply = reply_out;
                }
                /* Echo the assistant turn, then answer each call. */
                cJSON_AddItemToArray(extra, cJSON_Duplicate((cJSON *)message, 1));

                cJSON *call = NULL;
                cJSON_ArrayForEach(call, (cJSON *)calls) {
                    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
                    const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
                    const cJSON *argstr = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                    const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");

                    /* Must hold a full web_search result list; the device
                     * tools only ever need a sentence or two. */
                    char result[1792];
                    cJSON *args = cJSON_IsString(argstr) ? cJSON_Parse(argstr->valuestring) : NULL;
                    bool known = tools_execute(cJSON_IsString(name) ? name->valuestring : "",
                                               args, result, sizeof(result));
                    cJSON_Delete(args);
                    if (!known) {
                        snprintf(result, sizeof(result), "本设备没有这个功能。");
                    }
                    ESP_LOGI(TAG, "tool %s -> %s",
                             cJSON_IsString(name) ? name->valuestring : "?", result);

                    cJSON *tool_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_msg, "role", "tool");
                    cJSON_AddStringToObject(tool_msg, "tool_call_id",
                                            cJSON_IsString(id) ? id->valuestring : "");
                    cJSON_AddStringToObject(tool_msg, "content", result);
                    cJSON_AddItemToArray(extra, tool_msg);
                }
                cJSON_Delete(root);
                continue;   /* let the model phrase the real answer */
            }

            const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
            const bool has_text = cJSON_IsString(content) && content->valuestring &&
                                  content->valuestring[0];
            if (has_text || s_turn_info.refused) {
                if (s_turn_info.refused) {
                    snprintf(reply_out, reply_len, "%s", REFUSAL_REPLY);
                } else {
                    snprintf(reply_out, reply_len, "%s", content->valuestring);
                }
                reply = reply_out;
                push_msg("user", user_text);
                push_msg_full("assistant", reply_out, stc_owned);
                stc_owned = NULL;
                s_turns++;
                ret = ESP_OK;
            }
        }
        free(stc_owned);
        cJSON_Delete(root);
        if (ret == ESP_OK) {
            break;
        }
        break;
    }

    cJSON_Delete(extra);

    if (ret != ESP_OK) {
        /* Hard guarantee: a failed or exhausted tool loop must never leave the
         * user in silence again ("最近有什么AI新闻" did exactly that). */
        ESP_LOGW(TAG, "no final answer after %d tool rounds (internal free %u B, largest %u B)",
                 MAX_TOOL_ROUNDS,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (reply_out && reply_len) {
            snprintf(reply_out, reply_len, "抱歉，刚才没整理出答案，你再说一次好不好？");
            ret = ESP_OK;
        }
    }

    /* Report the full-suite activity of this turn (console + JSONL record). */
    ESP_LOGI(TAG, "turn: finish=%s search=%d read=%d steps=%d think=%d first=%.0fms cites=%d refused=%d",
             s_turn_info.finish[0] ? s_turn_info.finish : "?",
             s_turn_info.searches, s_turn_info.link_reads, s_turn_info.steps,
             s_turn_info.reasoning_chars, (double)s_turn_info.first_content_ms,
             s_turn_info.n_cites, s_turn_info.refused ? 1 : 0);

    /* Cloud voice owes us one clip for the final answer: the streaming path
     * above deliberately stayed silent so the TTS connection does not
     * overlap the still-open DeepSeek stream (internal RAM on this board only
     * fits one mbedTLS session at a time).  Now the stream is closed. */
    if (speak && ret == ESP_OK && reply && reply[0] && tts_ready() &&
        tts_uses_cloud()) {
        /* speak_short() keeps the reply from hogging the speaker; it only
         * returns when the audio has been produced/played (or failed). */
        if (speak_short(reply)) {
            spoke_any = true;
        } else {
            ESP_LOGW(TAG, "cloud reply TTS failed; caller will retry via local TTS");
        }
    }

    if (spoken_out) {
        *spoken_out = spoke_any;
    }
    return ret;
}

/* Fact-checking pass for the conversation supervisor: same request shape as a
 * normal turn but with TOOLS enabled, no chat history and no streaming, so the
 * model can call web_search before deciding whether to interject.
 * Runs at most 3 model round trips (search, optional clock check, verdict). */
esp_err_t deepseek_check(const char *sys_prompt, const char *user_prompt,
                         char *out, size_t out_len)
{
    if (!deepseek_configured() || !out || !out_len) {
        return ESP_ERR_INVALID_STATE;
    }
    out[0] = 0;
    cJSON *extra = cJSON_CreateArray();
    char *result = heap_caps_malloc(1792, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!extra || !result) {
        cJSON_Delete(extra);
        free(result);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_FAIL;
    for (int round = 0; round < 3; round++) {   /* tool round(s), then verdict */
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            break;
        }
        cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
        cJSON_AddBoolToObject(root, "stream", 0);
        cJSON_AddNumberToObject(root, "max_tokens", 320);
        cJSON_AddNumberToObject(root, "temperature", 0.2);
        /* With thinking on, a multi-round web_search judgement occasionally
         * came back as an empty string, which the supervisor read as
         * "do not interject".  Thinking is pure loss here. */
        cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
        cJSON_AddStringToObject(thinking, "type", "disabled");
        cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", sys_prompt);
        cJSON_AddItemToArray(msgs, sys);
        cJSON *u = cJSON_CreateObject();
        cJSON_AddStringToObject(u, "role", "user");
        cJSON_AddStringToObject(u, "content", user_prompt);
        cJSON_AddItemToArray(msgs, u);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, extra) {
            cJSON_AddItemToArray(msgs, cJSON_Duplicate(item, 1));
        }
        cJSON *tools = tools_spec_supervisor();
        if (tools) {
            cJSON_AddItemToObject(root, "tools", tools);
            cJSON_AddStringToObject(root, "tool_choice", "auto");
        }
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!body) {
            break;
        }
        cJSON *resp = post_chat(body, strlen(body));
        free(body);
        if (!resp) {
            break;
        }
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
        const cJSON *ch0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        const cJSON *message = ch0 ? cJSON_GetObjectItemCaseSensitive(ch0, "message") : NULL;
        bool done = false;
        if (message) {
            const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
            if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
                cJSON_AddItemToArray(extra, cJSON_Duplicate((cJSON *)message, 1));
                cJSON *call = NULL;
                cJSON_ArrayForEach(call, (cJSON *)calls) {
                    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
                    const cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
                    const cJSON *argstr = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                    const cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
                    cJSON *args = cJSON_IsString(argstr) ? cJSON_Parse(argstr->valuestring) : NULL;
                    /* The transcript is untrusted room audio, so a prompt-injected
                     * model must not be able to reach reboot()/set_volume() just
                     * because it named them.  The supervisor only reads facts. */
                    const char *tool_name = cJSON_IsString(name) ? name->valuestring : "";
                    bool allowed = (strcmp(tool_name, "web_search") == 0 ||
                                    strcmp(tool_name, "get_time") == 0);
                    if (!allowed) {
                        snprintf(result, 1792, "本设备没有这个功能。");
                    } else if (!tools_execute(tool_name, args, result, 1792)) {
                        snprintf(result, 1792, "本设备没有这个功能。");
                    }
                    cJSON_Delete(args);
                    ESP_LOGI(TAG, "check tool %s -> %.120s",
                             cJSON_IsString(name) ? name->valuestring : "?", result);
                    cJSON *tool_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_msg, "role", "tool");
                    cJSON_AddStringToObject(tool_msg, "tool_call_id",
                                            cJSON_IsString(id) ? id->valuestring : "");
                    cJSON_AddStringToObject(tool_msg, "content", result);
                    cJSON_AddItemToArray(extra, tool_msg);
                }
            } else {
                const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
                if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
                    snprintf(out, out_len, "%s", content->valuestring);
                    ret = ESP_OK;
                    done = true;
                }
            }
        }
        cJSON_Delete(resp);
        if (done) {
            break;
        }
    }
    cJSON_Delete(extra);
    free(result);
    return ret;
}

/* One-shot completion used by the conversation supervisor: no history, no
 * tools, low temperature, short answer.  Kept out of the chat history on
 * purpose - judging whether to interject must not pollute the dialogue. */
esp_err_t deepseek_judge(const char *system_prompt, const char *user_prompt,
                         char *out, size_t out_len)
{
    if (!deepseek_configured() || !out || !out_len) {
        return ESP_ERR_INVALID_STATE;
    }
    out[0] = 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", 0);
    cJSON_AddNumberToObject(root, "max_tokens", 160);
    cJSON_AddNumberToObject(root, "temperature", 0.2);
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", system_prompt);
    cJSON_AddItemToArray(msgs, sys);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", user_prompt);
    cJSON_AddItemToArray(msgs, u);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *resp = post_chat(body, strlen(body));
    free(body);
    if (!resp) {
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_FAIL;
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    const cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring) {
        snprintf(out, out_len, "%s", content->valuestring);
        ret = ESP_OK;
    }
    cJSON_Delete(resp);
    return ret;
}

/* Spoken answers are capped at the first two sentences / ~110 UTF-8 bytes.
 *
 * A deep-thinking search turn returns 300-400 characters (formatted like the
 * chat App).  Reading all of it kept the speaker busy for 20-30 s, during which
 * the microphone is gated and new utterances pile up in the 6-slot capture
 * queue - talking to the board then felt like it had stopped listening.  The
 * full text is still stored in reply_out (console + JSONL); only the spoken
 * part is shortened. */
static bool speak_short(const char *text)
{
    if (!text || !text[0] || !tts_ready()) {
        return false;
    }
    char buf[160];
    size_t n = strlen(text);
    if (n > sizeof(buf) - 1) {
        n = sizeof(buf) - 1;
        /* Cut at a sentence end if there is one in the first two sentences. */
        int sentences = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)text[i];
            bool end = (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n');
            if (!end && i + 2 < n && (unsigned char)text[i] == 0xE3 &&
                (unsigned char)text[i + 1] == 0x80 && (unsigned char)text[i + 2] == 0x82) {
                end = true;
                i += 2;
            }
            if (!end && i + 2 < n && (unsigned char)text[i] == 0xEF &&
                (unsigned char)text[i + 1] == 0xBC &&
                ((unsigned char)text[i + 2] == 0x81 || (unsigned char)text[i + 2] == 0x9F ||
                 (unsigned char)text[i + 2] == 0x9B)) {
                end = true;
                i += 2;
            }
            if (end && ++sentences >= 2) {
                n = i + 1;
                break;
            }
        }
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(buf, text, n);
    buf[n] = 0;
    if (n < strlen(text)) {
        ESP_LOGI(TAG, "spoken reply shortened to %u/%u bytes", (unsigned)n,
                 (unsigned)strlen(text));
    }
    return tts_say(buf) == ESP_OK;
}

/* Text-only path: used by the serial console, which must not talk out loud. */
esp_err_t deepseek_ask(const char *user_text, char *reply_out, size_t reply_len)
{
    return deepseek_ask_impl(user_text, reply_out, reply_len, false, NULL);
}

/* Voice path: streams and speaks sentence by sentence. */
esp_err_t deepseek_ask_stream(const char *user_text, char *reply_out, size_t reply_len,
                              bool *spoken_out)
{
    return deepseek_ask_impl(user_text, reply_out, reply_len, true, spoken_out);
}

/* ======================================================================== */
/* Standalone full-suite APIs                                               */
/* ======================================================================== */
#define DS_BOUNDARY "----esp32p4dsboundary"

/* Copy at most max_bytes of src, never splitting a UTF-8 sequence.
 *
 * "%.*s" counts BYTES, so a byte limit slices Chinese characters in half.  The
 * result is invalid UTF-8, and DeepSeek rejects the entire follow-up request
 * with:
 *     HTTP 400 messages[3].content: invalid unicode code point
 * which is why "最近有什么AI新闻" used to produce no answer even though the
 * search itself had succeeded. */
static void utf8_copy_limited(char *dst, size_t dst_size, const char *src, size_t max_bytes)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n > max_bytes) {
        n = max_bytes;
    }
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
        n--;
    }
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *ds_ctype_for(const char *path)
{
    static const struct {
        const char *ext;
        const char *ctype;
    } map[] = {
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".webp", "image/webp" },
        { ".gif",  "image/gif" },
        { ".bmp",  "image/bmp" },
        { ".tif",  "image/tiff" },
        { ".tiff", "image/tiff" },
        { ".pdf",  "application/pdf" },
        { ".doc",  "application/msword" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".ppt",  "application/vnd.ms-powerpoint" },
        { ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        { ".txt",  "text/plain" },
        { ".md",   "text/markdown" },
        { ".csv",  "text/csv" },
        { ".json", "application/json" },
        { ".xls",  "application/vnd.ms-excel" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".epub", "application/epub+zip" },
    };
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(dot, map[i].ext) == 0) {
            return map[i].ctype;
        }
    }
    return "application/octet-stream";
}

bool deepseek_file_supported(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    return strcmp(ds_ctype_for(path), "application/octet-stream") != 0;
}

static bool path_is_image(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp",
                                        ".tif", ".tiff" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (strcasecmp(dot, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Read the rest of the response body into PSRAM (growing, capped). */
static esp_err_t ds_read_all(esp_http_client_handle_t c, char **out)
{
    size_t cap = 8192, used = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    while (1) {
        if (used + 1 >= cap) {
            if (cap >= HTTP_RESP_MAX) {
                ESP_LOGW(TAG, "response capped at %u bytes", (unsigned)cap);
                break;
            }
            size_t ncap = cap * 2;
            if (ncap > HTTP_RESP_MAX) {
                ncap = HTTP_RESP_MAX;
            }
            char *nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) {
                nb = realloc(buf, ncap);
            }
            if (!nb) {
                free(buf);
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
            cap = ncap;
        }
        int n = esp_http_client_read(c, buf + used, (int)(cap - 1 - used));
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
    }
    buf[used] = 0;
    *out = buf;
    return ESP_OK;
}

/* multipart/form-data POST that streams the file from the TF card.  "purpose"
 * is optional; when present it is sent as the first field, which the Files API
 * requires ("purpose must be sent before file"). */
static esp_err_t ds_http_post_file(const char *url, int timeout_ms, const char *purpose,
                                   const char *path, char **resp_out, int *status_out)
{
    if (resp_out) {
        *resp_out = NULL;
    }
    if (status_out) {
        *status_out = 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long fsz = ftell(f);
    if (fsz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char pre[1024];
    int n = 0;
    if (purpose && purpose[0]) {
        n += snprintf(pre + n, sizeof(pre) - (size_t)n,
                      "--%s\r\nContent-Disposition: form-data; name=\"purpose\"\r\n\r\n"
                      "%s\r\n", DS_BOUNDARY, purpose);
    }
    n += snprintf(pre + n, sizeof(pre) - (size_t)n,
                  "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
                  "filename=\"%s\"\r\nContent-Type: %s\r\n\r\n",
                  DS_BOUNDARY, base, ds_ctype_for(path));
    char tail[80];
    int tn = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", DS_BOUNDARY);
    if (n <= 0 || n >= (int)sizeof(pre) || tn <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t content_len = (size_t)n + (size_t)fsz + (size_t)tn;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .crt_bundle_attach = deepseek_tls_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", DS_BOUNDARY);
    esp_http_client_set_header(c, "Content-Type", ctype);
    ds_add_auth(c);

    esp_err_t err = esp_http_client_open(c, (int)content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(c);
        fclose(f);
        return err;
    }
    esp_http_client_write(c, pre, n);
    uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(8192);
    }
    if (!buf) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t left = (size_t)fsz;
    while (left > 0) {
        size_t want = left > 8192 ? 8192 : left;
        size_t rd = fread(buf, 1, want, f);
        if (rd == 0) {
            break;
        }
        size_t off = 0;
        while (off < rd) {
            int w = esp_http_client_write(c, (const char *)buf + off, (int)(rd - off));
            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
        if (off < rd) {
            break;
        }
        left -= rd;
    }
    free(buf);
    fclose(f);
    esp_http_client_write(c, tail, tn);

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    char *body = NULL;
    ds_read_all(c, &body);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (status != 200) {
        ESP_LOGW(TAG, "%s -> HTTP %d: %.240s", url, status, body ? body : "");
    }
    if (resp_out) {
        *resp_out = body;
    } else {
        free(body);
    }
    if (status_out) {
        *status_out = status;
    }
    return ESP_OK;
}

esp_err_t deepseek_search(const char *query, int count, bool need_content,
                          char *out, size_t outlen)
{
    if (!out || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = 0;
    if (!query || !query[0]) {
        snprintf(out, outlen, "要搜索什么内容？");
        return ESP_ERR_INVALID_ARG;
    }
    if (!deepseek_configured()) {
        snprintf(out, outlen, "没有配置 DeepSeek API key，无法联网搜索。");
        return ESP_ERR_INVALID_STATE;
    }
    if (count < 1) {
        count = 10;
    }
    if (count > 50) {
        count = 50;
    }
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "query", query);
    cJSON_AddNumberToObject(req, "count", count);
    cJSON_AddBoolToObject(req, "need_content", need_content);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char *resp = NULL;
    int status = 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            /* A TLS handshake needs AES DMA descriptors out of internal RAM.
             * Within one turn the chat request has just released its own, and
             * the pool is small, so the first attempt can legitimately fail
             * with "esp-aes: Failed to allocate memory for len descriptor". */
            ESP_LOGW(TAG, "search attempt %d failed, retrying", attempt);
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
        err = ds_http_post_raw(CONFIG_DEEPSEEK_SEARCH_URL, body, strlen(body), 15000,
                               false, &resp, &status);
        if (err == ESP_OK && status == 200 && resp) {
            break;
        }
        free(resp);
        resp = NULL;
    }
    free(body);
    if (err != ESP_OK || status != 200 || !resp) {
        snprintf(out, outlen, "联网搜索请求失败（网络或接口错误）。");
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        snprintf(out, outlen, "联网搜索返回的数据无法解析。");
        return ESP_FAIL;
    }
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    int n = cJSON_IsArray(results) ? cJSON_GetArraySize(results) : 0;
    if (n <= 0) {
        snprintf(out, outlen, "联网搜索没有找到 \"%.60s\" 的相关结果。", query);
        cJSON_Delete(root);
        return ESP_OK;
    }
    int off = snprintf(out, outlen, "联网搜索 \"%.60s\" 的结果：\n", query);
    int shown = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, results) {
        if (shown >= count || off >= (int)outlen - 220) {
            break;
        }
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
        const cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
        const cJSON *snippet = cJSON_GetObjectItemCaseSensitive(item, "snippet");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        const char *blurb = cJSON_IsString(summary) ? summary->valuestring
                          : (cJSON_IsString(snippet) ? snippet->valuestring
                          : (cJSON_IsString(content) ? content->valuestring : ""));
        char blurb_buf[200];
        char title_buf[160];
        utf8_copy_limited(blurb_buf, sizeof(blurb_buf), blurb, 160);
        utf8_copy_limited(title_buf, sizeof(title_buf),
                          cJSON_IsString(title) ? title->valuestring : "", 120);
        shown++;
        off += snprintf(out + off, outlen - (size_t)off, "%d. %s\n   %s\n   %s%s%s\n",
                        shown, title_buf[0] ? title_buf : "(无标题)",
                        cJSON_IsString(url) ? url->valuestring : "", blurb_buf,
                        cJSON_IsString(date) && date->valuestring[0] ? "  " : "",
                        cJSON_IsString(date) && date->valuestring[0] ? date->valuestring : "");
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "search %s -> %d results", query, shown);
    return ESP_OK;
}

esp_err_t deepseek_fetch(const char *url, char *out, size_t outlen)
{
    if (!url || !url[0] || !out || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = 0;
    if (!deepseek_configured()) {
        snprintf(out, outlen, "没有配置 DeepSeek API key，无法读取网页。");
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "url", url);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    char *resp = NULL;
    int status = 0;
    esp_err_t err = ds_http_post_raw(CONFIG_DEEPSEEK_FETCH_URL, body, strlen(body), 30000,
                                     false, &resp, &status);
    free(body);
    if (err != ESP_OK || status != 200 || !resp) {
        snprintf(out, outlen, "读取网页失败（HTTP %d）。", status);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    char title_buf[120];
    utf8_copy_limited(title_buf, sizeof(title_buf),
                      cJSON_IsString(title) ? title->valuestring : "", 100);
    int off = snprintf(out, outlen, "网页标题：%s\n", title_buf[0] ? title_buf : "(无标题)");
    if (off < (int)outlen - 2) {
        utf8_copy_limited(out + off, outlen - (size_t)off,
                          cJSON_IsString(content) ? content->valuestring : "",
                          outlen - (size_t)off - 1);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "fetch %s -> %u chars", url, (unsigned)strlen(out));
    return ESP_OK;
}

esp_err_t deepseek_ocr(const char *path, bool rich_layout, char *out, size_t outlen)
{
    if (!path || !path[0] || !out || !outlen) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = 0;
    if (!deepseek_configured()) {
        snprintf(out, outlen, "没有配置 DeepSeek API key，无法做 OCR。");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "ocr %s (%s)", path, rich_layout ? "rich_layout" : "text");
    char *resp = NULL;
    int status = 0;
    esp_err_t err = ds_http_post_file(CONFIG_DEEPSEEK_OCR_URL, FILE_TIMEOUT_MS,
                                      rich_layout ? "rich_layout_recognition"
                                                  : "text_recognition",
                                      path, &resp, &status);
    if (err != ESP_OK || status != 200 || !resp) {
        snprintf(out, outlen, "OCR 失败（HTTP %d）。", status);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        snprintf(out, outlen, "OCR 返回的数据无法解析。");
        return ESP_FAIL;
    }
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    int n = cJSON_IsArray(pages) ? cJSON_GetArraySize(pages) : 0;
    const cJSON *pc = cJSON_GetObjectItemCaseSensitive(root, "page_count");
    int total = cJSON_IsNumber(pc) ? pc->valueint : n;
    int off = 0;
    for (int i = 0; i < n && off < (int)outlen - 8; i++) {
        const cJSON *page = cJSON_GetArrayItem(pages, i);
        const cJSON *text = page ? cJSON_GetObjectItemCaseSensitive(page, "text") : NULL;
        if (!cJSON_IsString(text) || !text->valuestring[0]) {
            continue;
        }
        off += snprintf(out + off, outlen - (size_t)off, "--- 第 %d 页 ---\n", i + 1);
        if (off < (int)outlen - 2) {
            utf8_copy_limited(out + off, outlen - (size_t)off, text->valuestring,
                              outlen - (size_t)off - 1);
            off = (int)strlen(out);
        }
    }
    cJSON_Delete(root);
    if (out[0] == 0) {
        snprintf(out, outlen, "OCR 没有识别出文字（共 %d 页）。", total);
    }
    ESP_LOGI(TAG, "ocr done: %d pages, %u chars", total, (unsigned)strlen(out));
    return ESP_OK;
}

esp_err_t deepseek_upload(const char *path, const char *purpose,
                          char *file_id, size_t file_id_len)
{
    if (!path || !path[0] || !file_id || file_id_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    file_id[0] = 0;
    if (!deepseek_configured()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!purpose || !purpose[0]) {
        purpose = "user_data";
    }
    ESP_LOGI(TAG, "upload %s purpose=%s", path, purpose);
    char *resp = NULL;
    int status = 0;
    esp_err_t err = ds_http_post_file(CONFIG_DEEPSEEK_FILES_URL, FILE_TIMEOUT_MS, purpose,
                                      path, &resp, &status);
    if (err != ESP_OK || status != 200 || !resp) {
        ESP_LOGE(TAG, "upload failed (HTTP %d)", status);
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    esp_err_t ret = ESP_FAIL;
    if (cJSON_IsString(id) && id->valuestring) {
        snprintf(file_id, file_id_len, "%s", id->valuestring);
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(root, "bytes");
        ESP_LOGI(TAG, "uploaded %s -> %s (%d bytes)", path, file_id,
                 cJSON_IsNumber(bytes) ? bytes->valueint : -1);
        ret = ESP_OK;
    }
    cJSON_Delete(root);
    return ret;
}

/* Full 文件输入 path: the service parses the file during the upload, then the
 * chat request references the file_id in a "file" content part - exactly the
 * two steps the DeepSeek App uses. */
esp_err_t deepseek_ask_file(const char *path, const char *question,
                            char *reply, size_t reply_len)
{
    if (!path || !path[0] || !reply || reply_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    reply[0] = 0;
    if (!deepseek_file_supported(path)) {
        snprintf(reply, reply_len, "这种文件我还不认识，试试 PDF、Word、PPT、图片或文本文件。");
        return ESP_ERR_INVALID_ARG;
    }
    if (!deepseek_configured()) {
        snprintf(reply, reply_len, "没有配置 DeepSeek API key。");
        return ESP_ERR_INVALID_STATE;
    }
    feat_init();
    turn_info_reset();

    /* Images are stored as plain user data; documents are parsed, and
     * rich-layout parsing also keeps charts / figures (the App behaviour). */
    const char *purpose = path_is_image(path) ? "user_data" : "rich_layout_recognition";
    char file_id[96];
    if (deepseek_upload(path, purpose, file_id, sizeof(file_id)) != ESP_OK) {
        snprintf(reply, reply_len, "文件上传失败，可能文件太大或格式不支持。");
        return ESP_FAIL;
    }

    cJSON *content = cJSON_CreateArray();
    if (!content) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *fpart = cJSON_CreateObject();
    cJSON_AddStringToObject(fpart, "type", "file");
    cJSON_AddStringToObject(fpart, "file_id", file_id);
    cJSON_AddItemToArray(content, fpart);
    cJSON *tpart = cJSON_CreateObject();
    cJSON_AddStringToObject(tpart, "type", "text");
    cJSON_AddStringToObject(tpart, "text",
                            (question && question[0]) ? question
                                                      : "这个文件讲了什么？请用中文简要说明。");
    cJSON_AddItemToArray(content, tpart);

    ds_req_opts_t o = {
        .user_text = NULL,
        .user_content = content,
        .extra = NULL,
        .stream = false,
        .with_history = false,
        .title_round = false,
    };
    char *body = build_request(&o);
    cJSON_Delete(content);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *root = post_chat(body, strlen(body));
    free(body);
    if (!root) {
        snprintf(reply, reply_len, "读取这个文件时请求失败了。");
        return ESP_FAIL;
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    const cJSON *content_out = message ? cJSON_GetObjectItemCaseSensitive(message, "content")
                                       : NULL;
    char *stc = turn_info_from_message(message);
    free(stc);
    esp_err_t ret = ESP_FAIL;
    if (s_turn_info.refused) {
        snprintf(reply, reply_len, "%s", REFUSAL_REPLY);
        ret = ESP_OK;
    } else if (cJSON_IsString(content_out) && content_out->valuestring &&
               content_out->valuestring[0]) {
        snprintf(reply, reply_len, "%s", content_out->valuestring);
        ret = ESP_OK;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "ask_file %s -> %.80s", path, reply);
    return ret;
}
