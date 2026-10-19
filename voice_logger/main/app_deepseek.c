#include "app_deepseek.h"
#include "app_identity.h"
#include "app_persona.h"
#include "app_tools.h"
#include "app_tts.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "deepseek";

#define API_URL   "https://api.deepseek.com/chat/completions"
#define HTTP_TIMEOUT_MS 45000
#define MAX_TURNS 6          /* user+assistant pairs kept as context */
/* A web-search turn needs two model round trips (decide to search -> phrase the
 * answer), and the model sometimes also calls get_time / system_status.  Three
 * rounds ran out and produced a completely silent turn. */
#define MAX_TOOL_ROUNDS 6

typedef struct {
    char *role;
    char *content;
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
    }
    s_hist_len = 0;
    s_turns = 0;
}

static void push_msg(const char *role, const char *content)
{
    if (s_hist_len == MAX_TURNS * 2) {
        free(s_hist[0].role);
        free(s_hist[0].content);
        memmove(&s_hist[0], &s_hist[1], sizeof(msg_t) * (MAX_TURNS * 2 - 1));
        s_hist_len--;
    }
    s_hist[s_hist_len].role = strdup(role);
    s_hist[s_hist_len].content = strdup(content);
    if (s_hist[s_hist_len].role && s_hist[s_hist_len].content) {
        s_hist_len++;
    } else {
        free(s_hist[s_hist_len].role);
        free(s_hist[s_hist_len].content);
    }
}

static char *build_request(const char *user_text, const cJSON *extra, bool stream)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", CONFIG_DEEPSEEK_MODEL);
    cJSON_AddBoolToObject(root, "stream", stream ? 1 : 0);
    cJSON_AddNumberToObject(root, "max_tokens", CONFIG_DEEPSEEK_MAX_TOKENS);
    cJSON_AddNumberToObject(root, "temperature",
                            (double)CONFIG_DEEPSEEK_TEMPERATURE_X100 / 100.0);
    /* deepseek-flash thinks before answering by default.  A spoken reply wants
     * the answer, not a hidden reasoning budget, and reasoning tokens were
     * eating max_tokens (and adding ~0.5 s before the first word). */
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    cJSON_AddStringToObject(thinking, "type", "disabled");

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    /* Tell the model who it is talking to, so it can use their name.  The
     * voiceprint match was made on the board moments ago.  Static so a long
     * persona prompt (a few hundred Chinese chars) fits without eating the
     * caller's stack; calls are serialised by the cloud lock. */
    static char sysbuf[4096];
    const char *spk = identity_speaker_id();
    if (spk && strcmp(spk, IDENTITY_UNKNOWN) != 0) {
        snprintf(sysbuf, sizeof(sysbuf), "%s\n当前和你说话的人已通过声纹识别为：%s。",
                 XIAOXIN_SYSTEM_PROMPT, spk);
    } else {
        snprintf(sysbuf, sizeof(sysbuf), "%s\n当前说话人未能通过声纹识别。",
                 XIAOXIN_SYSTEM_PROMPT);
    }
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", sysbuf);
    cJSON_AddItemToArray(msgs, sys);

    for (int i = 0; i < s_hist_len; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", s_hist[i].role);
        cJSON_AddStringToObject(m, "content", s_hist[i].content);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", user_text);
    cJSON_AddItemToArray(msgs, u);

    /* Assistant tool_calls + tool results produced during this turn. */
    if (extra) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, (cJSON *)extra) {
            cJSON_AddItemToArray(msgs, cJSON_Duplicate(item, 1));
        }
    }

    cJSON *tools = tools_spec();
    if (tools) {
        cJSON_AddItemToObject(root, "tools", tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* One HTTPS round trip.  Returns the parsed JSON body, or NULL. */
static cJSON *post_chat(const char *body, size_t body_len)
{
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);

    esp_http_client_config_t cfg = {
        .url = API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    if (esp_http_client_open(c, (int)body_len) != ESP_OK) {
        ESP_LOGE(TAG, "connect failed");
        esp_http_client_cleanup(c);
        return NULL;
    }
    esp_http_client_write(c, body, (int)body_len);

    int total = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    size_t cap = (size_t)(total > 0 ? total : 4096) + 1;
    char *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(cap);
    }
    if (!resp) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
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
        return NULL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    return root;
}


/* ------------------------- streaming (SSE) support ----------------------- */
/* The reply is spoken sentence by sentence as it streams in, so the first words
 * come out of the speaker about a second after the model starts answering
 * instead of after the whole paragraph has been generated. */

typedef struct {
    bool    speak;
    char    content[3072];
    size_t  content_len;
    char    pending[600];
    size_t  pending_len;
    cJSON  *tool_calls;
    bool    spoken;
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

/* POST with stream:true, parse the SSE stream, return a synthetic
 * choices[0].message object so the caller's tool loop is unchanged. */
static cJSON *post_chat_stream(const char *body, stream_ctx_t *c)
{
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);

    esp_http_client_config_t cfg = {
        .url = API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        return NULL;
    }
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    esp_http_client_set_header(h, "Authorization", auth);

    int blen = (int)strlen(body);
    if (esp_http_client_open(h, blen) != ESP_OK) {
        ESP_LOGE(TAG, "stream connect failed");
        esp_http_client_cleanup(h);
        return NULL;
    }
    esp_http_client_write(h, body, blen);
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        char err[256] = { 0 };
        int n = esp_http_client_read(h, err, sizeof(err) - 1);
        if (n > 0) {
            err[n] = 0;
        }
        ESP_LOGE(TAG, "stream HTTP %d: %.200s", status, err);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return NULL;
    }

    static char chunk[1024];
    static char line[1024];
    size_t line_len = 0;
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
                line[line_len] = 0;
                if (strncmp(line, "data:", 5) == 0) {
                    const char *payload = line + 5;
                    while (*payload == ' ') {
                        payload++;
                    }
                    if (strncmp(payload, "[DONE]", 6) == 0) {
                        done = true;
                    } else {
                        cJSON *root = cJSON_Parse(payload);
                        if (root) {
                            const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
                            const cJSON *ch0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
                            const cJSON *delta = ch0 ? cJSON_GetObjectItemCaseSensitive(ch0, "delta") : NULL;
                            if (delta) {
                                const cJSON *ct = cJSON_GetObjectItemCaseSensitive(delta, "content");
                                if (cJSON_IsString(ct) && ct->valuestring) {
                                    stream_feed(c, ct->valuestring);
                                }
                                const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
                                if (cJSON_IsArray(tcs)) {
                                    stream_tool_delta(c, tcs);
                                }
                            }
                            cJSON_Delete(root);
                        }
                    }
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
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

    /* Messages the model must see while it works through tool calls. */
    cJSON *extra = cJSON_CreateArray();
    if (!extra) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_FAIL;
    bool spoke_any = false;
    bool transport_retried = false;
    for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {
        char *body = build_request(user_text, extra, speak);
        if (!body) {
            break;
        }
        stream_ctx_t sc = { 0 };
        sc.speak = speak;
        cJSON *root = speak ? post_chat_stream(body, &sc)
                            : post_chat(body, strlen(body));
        spoke_any = spoke_any || sc.spoken;
        free(body);
        if (!root) {
            /* One transient TLS failure used to turn a whole question into
             * "sorry, I didn't catch that".  Wake-up/other-task memory pressure
             * is transient, so give the stream one more go. */
            if (!transport_retried) {
                transport_retried = true;
                round--;
                ESP_LOGW(TAG, "transport failed, retrying once");
                vTaskDelay(pdMS_TO_TICKS(900));
                continue;
            }
            break;
        }

        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        const cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;

        if (message) {
            const cJSON *calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
            if (cJSON_IsArray(calls) && cJSON_GetArraySize(calls) > 0) {
                /* Some models attach a sentence next to the tool call.  Keep it:
                 * if the tool loop never reaches a final answer, saying that
                 * sentence beats going completely silent. */
                const cJSON *pre = cJSON_GetObjectItemCaseSensitive(message, "content");
                if (cJSON_IsString(pre) && pre->valuestring && pre->valuestring[0]) {
                    snprintf(reply_out, reply_len, "%s", pre->valuestring);
                    ret = ESP_OK;
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
            if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
                snprintf(reply_out, reply_len, "%s", content->valuestring);
                push_msg("user", user_text);
                push_msg("assistant", reply_out);
                s_turns++;
                ret = ESP_OK;
            }
        }
        cJSON_Delete(root);
        break;
    }

    cJSON_Delete(extra);

    if (ret != ESP_OK) {
        /* Hard guarantee: a failed or exhausted tool loop must never leave the
         * user in silence again ("最近有什么AI新闻" did exactly that). */
        ESP_LOGW(TAG, "no final answer after %d tool rounds", MAX_TOOL_ROUNDS);
        if (reply_out && reply_len) {
            snprintf(reply_out, reply_len, "抱歉，刚才没整理出答案，你再说一次好不好？");
            ret = ESP_OK;
        }
    }
    /* Cloud voice owes us one clip for the final answer: the streaming path
     * above deliberately stayed silent so the TTS HTTPS request does not
     * overlap the still-open DeepSeek stream (internal RAM on this board only
     * fits one mbedTLS session at a time).  Now the stream is closed. */
    if (speak && ret == ESP_OK && reply_out && reply_out[0] && tts_ready() &&
        tts_uses_cloud()) {
        if (tts_say(reply_out) == ESP_OK) {
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
