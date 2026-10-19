#include "app_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_audio.h"
#include "app_ble.h"
#include "app_reminder.h"
#include "app_speaker_id.h"
#include "app_storage.h"
#include "app_tts.h"
#include "app_wifi.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "tools";

/* Reboot is deferred so the spoken reply is not cut off mid-sentence. */
static int64_t s_reboot_at_us;

int64_t tools_pending_reboot_us(void)
{
    return s_reboot_at_us;
}

static cJSON *add_tool(cJSON *arr, const char *name, const char *desc, const char *params_json)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", name);
    cJSON_AddStringToObject(fn, "description", desc);
    cJSON *params = cJSON_Parse(params_json);
    if (params) {
        cJSON_AddItemToObject(fn, "parameters", params);
    }
    cJSON_AddItemToArray(arr, tool);
    return tool;
}

#define P_OBJ(props, req) "{\"type\":\"object\",\"properties\":{" props "},\"required\":[" req "]}"
#define P_NONE            "{\"type\":\"object\",\"properties\":{}}"

/* DeepSeek web search is a separate REST API, not a server-side tool that the
 * chat model can invoke by itself:
 *     POST https://api.deepseek.com/v1/tools/search
 *     {"query": "...", "count": n, "need_content": false}
 *  -> {"results":[{title,url,snippet,summary,date,authority,...}]}
 *
 * So 联网搜索 is implemented like every other capability here: the model asks
 * for it through function calling, the board performs the HTTP request and
 * feeds the ranked results back as a tool message.  Without this the model has
 * no way to reach the internet at all - /chat/completions has no built-in
 * search.
 */
#define DEEPSEEK_SEARCH_URL "https://api.deepseek.com/v1/tools/search"
#define SEARCH_TIMEOUT_MS   15000
#define SEARCH_MAX_RESULTS  5

/* One HTTPS POST with a raw JSON body; returns the parsed response or NULL. */
static cJSON *http_post_json(const char *url, const char *body)
{
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_DEEPSEEK_API_KEY);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SEARCH_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", auth);

    int blen = (int)strlen(body);
    if (esp_http_client_open(c, blen) != ESP_OK) {
        ESP_LOGE(TAG, "search connect failed");
        esp_http_client_cleanup(c);
        return NULL;
    }
    esp_http_client_write(c, body, blen);

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
        ESP_LOGE(TAG, "search HTTP %d: %.160s", status, resp);
        free(resp);
        return NULL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    return root;
}

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
    /* Back off while the cut lands on a continuation byte (10xxxxxx). */
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
        n--;
    }
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Turn the ranked JSON into compact text the model can use in a spoken answer:
 * title + url + a short excerpt, at most SEARCH_MAX_RESULTS entries. */
static esp_err_t deepseek_web_search(const char *query, char *out, size_t outlen)
{
    if (!CONFIG_DEEPSEEK_API_KEY[0]) {
        snprintf(out, outlen, "没有配置 DeepSeek API key，无法联网搜索。");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "query", query);
    cJSON_AddNumberToObject(req, "count", SEARCH_MAX_RESULTS);
    cJSON_AddBoolToObject(req, "need_content", false);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        snprintf(out, outlen, "搜索请求构造失败。");
        return ESP_FAIL;
    }

    cJSON *root = NULL;
    for (int attempt = 0; attempt < 3 && !root; attempt++) {
        if (attempt) {
            /* A TLS handshake needs AES DMA descriptors out of internal RAM.
             * Within one turn the chat request has just released its own, and
             * the pool is small, so the first search attempt can legitimately
             * fail with "esp-aes: Failed to allocate memory for len descriptor".
             * Give the descriptors a moment and try again. */
            ESP_LOGW(TAG, "web_search attempt %d failed, retrying", attempt);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        root = http_post_json(DEEPSEEK_SEARCH_URL, body);
    }
    free(body);
    if (!root) {
        snprintf(out, outlen, "联网搜索请求失败（网络或接口错误）。");
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
        if (shown >= SEARCH_MAX_RESULTS || off >= (int)outlen - 220) {
            break;
        }
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
        const cJSON *summary = cJSON_GetObjectItemCaseSensitive(item, "summary");
        const cJSON *snippet = cJSON_GetObjectItemCaseSensitive(item, "snippet");
        const char *blurb = cJSON_IsString(summary) ? summary->valuestring
                          : (cJSON_IsString(snippet) ? snippet->valuestring : "");
        shown++;
        char blurb_buf[200];
        char title_buf[160];
        utf8_copy_limited(blurb_buf, sizeof(blurb_buf), blurb, 160);
        utf8_copy_limited(title_buf, sizeof(title_buf),
                          cJSON_IsString(title) ? title->valuestring : "", 120);
        off += snprintf(out + off, outlen - (size_t)off, "%d. %s\n   %s\n   %s\n",
                        shown, title_buf[0] ? title_buf : "(无标题)",
                        cJSON_IsString(url) ? url->valuestring : "",
                        blurb_buf);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "web_search %s -> %d results", query, shown);
    return ESP_OK;
}

bool tools_web_search(const char *query, char *out, size_t outlen)
{
    return deepseek_web_search(query, out, outlen) == ESP_OK;
}

cJSON *tools_spec(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    add_tool(arr, "set_volume",
             "设置喇叭音量。用户说声音太大/太小、调大/调小时调用。",
             P_OBJ("\"level\":{\"type\":\"integer\",\"description\":\"音量 0-100\"}", "\"level\""));
    add_tool(arr, "get_volume", "读取当前喇叭音量。", P_NONE);
    add_tool(arr, "set_tts_speed",
             "设置小新说话的快慢。",
             P_OBJ("\"speed\":{\"type\":\"integer\",\"description\":\"语速 0 最慢 到 5 最快\"}", "\"speed\""));

    add_tool(arr, "web_search",
             "联网搜索最新的网页信息。凡涉及新闻、实时数据、天气、价格、赛事、"
             "最近发生的事，或你不知道但网上查得到答案的问题，都必须调用它，"
             "不要凭记忆编造时效性内容。",
             P_OBJ("\"query\":{\"type\":\"string\",\"description\":\"搜索关键词，支持 site: lang: after: 等操作符\"}",
                   "\"query\""));

    add_tool(arr, "set_reminder",
             "给用户设置一个定时提醒，到点会主动开口说话。用户说\"十分钟后提醒我喝水\""
             "\"半小时后叫我起床\"这类话时必须调用它，不要只是口头答应。",
             P_OBJ("\"minutes\":{\"type\":\"integer\",\"description\":\"多少分钟后提醒；不到一分钟就用 seconds\"},"
                   "\"seconds\":{\"type\":\"integer\",\"description\":\"多少秒后提醒\"},"
                   "\"text\":{\"type\":\"string\",\"description\":\"到点要说的话，例如 喝水\"}",
                   "\"text\""));
    add_tool(arr, "list_reminders", "列出还没触发的提醒。", P_NONE);
    add_tool(arr, "cancel_reminders", "取消所有待触发的提醒。", P_NONE);

    add_tool(arr, "get_time", "读取当前日期和时间。", P_NONE);
    add_tool(arr, "system_status",
             "读取设备运行状态：开机时长、剩余内存、TF 卡是否正常、WiFi 是否连接。", P_NONE);

    add_tool(arr, "wifi_status", "读取当前连接的 WiFi 名称、IP 地址和信号强度。", P_NONE);
    add_tool(arr, "wifi_scan", "扫描附近有哪些 WiFi。用户问\"有哪些网络\"\"能连什么\"时调用。", P_NONE);
    add_tool(arr, "wifi_connect",
             "连接到一个指定的 WiFi。必须先知道名称；如果需要密码而用户没给，先向用户要密码。",
             P_OBJ("\"ssid\":{\"type\":\"string\",\"description\":\"WiFi 名称\"},\"password\":{\"type\":\"string\",\"description\":\"WiFi 密码\"}", "\"ssid\""));

    add_tool(arr, "list_people", "列出已经登记过声纹的家庭成员。", P_NONE);
    add_tool(arr, "forget_voice",
             "删除某个人的声纹记录。",
             P_OBJ("\"name\":{\"type\":\"string\",\"description\":\"要删除的人名\"}", "\"name\""));

    add_tool(arr, "recent_conversations",
             "查询最近说过的话和当时的回答。用户问\"我刚才说了什么\"\"帮我回忆一下\"时调用。",
             P_OBJ("\"count\":{\"type\":\"integer\",\"description\":\"要看几条，默认 3\"}", ""));

#if CONFIG_VOICE_BLE
    add_tool(arr, "ble_scan",
             "扫描附近的蓝牙设备。用户问\"附近有哪些蓝牙\"\"帮我找蓝牙设备\"时调用。",
             P_NONE);
    add_tool(arr, "ble_connect",
             "连接一个蓝牙设备。地址要用 ble_scan 返回的那种格式。",
             P_OBJ("\"address\":{\"type\":\"string\",\"description\":\"设备地址，形如 AA:BB:CC:DD:EE:FF\"}",
                   "\"address\""));
    add_tool(arr, "ble_disconnect", "断开当前蓝牙连接。", P_NONE);
    add_tool(arr, "ble_status", "查看蓝牙是否就绪、当前是否连着设备。", P_NONE);
#endif

    add_tool(arr, "reboot", "重启设备。只有在用户明确要求重启时才调用。", P_NONE);
    return arr;
}

/* Fact-checking only: the supervisor listens to other people's conversation and
 * may only read (search) and check the clock - never act on the device. */
cJSON *tools_spec_supervisor(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    add_tool(arr, "web_search",
             "联网搜索最新的网页信息。听到可查证的说法（天气、价格、日期、新闻、赛事、数字）时，"
             "先用它核实，不要凭记忆判断。",
             P_OBJ("\"query\":{\"type\":\"string\",\"description\":\"搜索关键词，支持 site: lang: after: 等操作符\"}",
                   "\"query\""));
    add_tool(arr, "get_time", "读取当前日期和时间。", P_NONE);
    return arr;
}

static int arg_int(const cJSON *args, const char *key, int dflt)
{
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    if (cJSON_IsNumber(v)) return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return atoi(v->valuestring);
    return dflt;
}

static const char *arg_str(const cJSON *args, const char *key)
{
    const cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

bool tools_execute(const char *name, const cJSON *args, char *result, size_t result_len)
{
    if (!name || !result || !result_len) return false;
    result[0] = 0;

    if (strcmp(name, "set_volume") == 0) {
        int level = arg_int(args, "level", 70);
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        if (audio_set_volume(level) == ESP_OK) {
            snprintf(result, result_len, "音量已实际设置为 %d（0-100）。", level);
            ESP_LOGI(TAG, "set_volume -> %d", level);
        } else {
            snprintf(result, result_len, "设置音量失败。");
        }
        return true;
    }

    if (strcmp(name, "get_volume") == 0) {
        snprintf(result, result_len, "当前音量是 %d（0-100）。", audio_get_volume());
        return true;
    }

    if (strcmp(name, "set_tts_speed") == 0) {
        int sp = arg_int(args, "speed", 3);
        if (sp < 0) sp = 0;
        if (sp > 5) sp = 5;
        tts_set_speed((unsigned)sp);
        snprintf(result, result_len, "语速已设为 %d（0 最慢，5 最快）。", sp);
        return true;
    }

    if (strcmp(name, "web_search") == 0) {
        const cJSON *q = cJSON_GetObjectItem(args, "query");
        if (!cJSON_IsString(q) || !q->valuestring[0]) {
            snprintf(result, result_len, "要搜索什么内容？");
            return true;
        }
        deepseek_web_search(q->valuestring, result, result_len);
        return true;
    }
    if (strcmp(name, "set_reminder") == 0) {
        const char *txt = arg_str(args, "text");
        if (!txt || !txt[0]) {
            snprintf(result, result_len, "要提醒你什么内容？请说清楚。");
            return true;
        }
        int minutes = arg_int(args, "minutes", 0);
        int seconds = arg_int(args, "seconds", 0);
        int total = minutes * 60 + seconds;
        if (total <= 0) {
            total = 60;
        }
        int id = reminder_add((uint32_t)total, txt);
        if (id > 0) {
            snprintf(result, result_len, "好的，%d 秒后我会提醒你：%s。", total, txt);
        } else {
            snprintf(result, result_len, "提醒表满了，先取消一些旧的吧。");
        }
        return true;
    }
    if (strcmp(name, "list_reminders") == 0) {
        reminder_list(result, result_len);
        return true;
    }
    if (strcmp(name, "cancel_reminders") == 0) {
        int n = reminder_cancel_all();
        snprintf(result, result_len, n ? "已取消 %d 个提醒。" : "现在没有待触发的提醒。", n);
        return true;
    }
    if (strcmp(name, "get_time") == 0) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year + 1900 < 2020) {
            snprintf(result, result_len, "还没对上网络时间，只知道开机 %lld 秒。",
                     (long long)(esp_timer_get_time() / 1000000));
        } else {
            char buf[80];
            strftime(buf, sizeof(buf), "%Y年%m月%d日 %H:%M:%S", &tm);
            snprintf(result, result_len, "现在是 %s。", buf);
        }
        return true;
    }

    if (strcmp(name, "system_status") == 0) {
        snprintf(result, result_len,
                 "开机 %.0f 分钟；内部内存剩 %u KB，PSRAM 剩 %u KB；TF 卡%s；WiFi%s。",
                 esp_timer_get_time() / 60000000.0,
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                 storage_ready() ? "正常" : "未挂载",
                 wifi_connected() ? "已连接" : "未连接");
        return true;
    }

    if (strcmp(name, "wifi_status") == 0) {
        char ssid[40], ip[24];
        int rssi = 0;
        bool up = wifi_link_info(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
        if (up) {
            snprintf(result, result_len, "已连接 %s，IP %s，信号 %d dBm。", ssid, ip, rssi);
        } else {
            snprintf(result, result_len, "当前没有连接 WiFi。");
        }
        return true;
    }

    if (strcmp(name, "wifi_scan") == 0) {
        static char ssids[12][33];
        static int8_t rssi[12];
        int n = wifi_scan(ssids, rssi, 12);
        if (n <= 0) {
            snprintf(result, result_len, "没扫描到网络（或扫描失败）。");
        } else {
            int off = snprintf(result, result_len, "附近有 %d 个网络：", n);
            for (int i = 0; i < n && off < (int)result_len - 20; i++) {
                off += snprintf(result + off, result_len - off, "%s%s(%d)",
                                i ? "、" : "", ssids[i][0] ? ssids[i] : "(隐藏)", rssi[i]);
            }
        }
        return true;
    }

    if (strcmp(name, "wifi_connect") == 0) {
        const char *ssid = arg_str(args, "ssid");
        const char *pass = arg_str(args, "password");
        if (!ssid || !ssid[0]) {
            snprintf(result, result_len, "没听清要连哪个 WiFi，请再说一次名称。");
            return true;
        }
        if (wifi_join(ssid, pass ? pass : "", 20000) == ESP_OK) {
            char ip[24];
            wifi_link_info(NULL, 0, ip, sizeof(ip), NULL);
            snprintf(result, result_len, "已经连上 %s，IP 是 %s。", ssid, ip);
        } else {
            snprintf(result, result_len, "连接 %s 失败，可能是密码不对或者信号太弱。", ssid);
        }
        return true;
    }

    if (strcmp(name, "list_people") == 0) {
        char names[SPEAKER_MAX][SPEAKER_NAME_LEN];
        int n = speaker_id_list(names, SPEAKER_MAX);
        if (n == 0) {
            snprintf(result, result_len, "还没有登记任何人的声纹。");
        } else {
            int off = snprintf(result, result_len, "已登记 %d 人：", n);
            for (int i = 0; i < n && off < (int)result_len - 2; i++) {
                off += snprintf(result + off, result_len - off, "%s%s", i ? "、" : "", names[i]);
            }
        }
        return true;
    }

    if (strcmp(name, "forget_voice") == 0) {
        const char *nm = arg_str(args, "name");
        if (!nm || !nm[0]) {
            snprintf(result, result_len, "要删除谁的声纹？");
        } else if (speaker_id_forget(nm) == ESP_OK) {
            snprintf(result, result_len, "已经删掉 %s 的声纹了。", nm);
        } else {
            snprintf(result, result_len, "没找到叫 %s 的声纹记录。", nm);
        }
        return true;
    }

    if (strcmp(name, "recent_conversations") == 0) {
        int cnt = arg_int(args, "count", 3);
        if (cnt < 1) cnt = 1;
        if (cnt > 5) cnt = 5;
        static char buf[900];
        if (storage_recent_text(buf, sizeof(buf), cnt) == ESP_OK) {
            snprintf(result, result_len, "最近 %d 条记录：\n%s", cnt, buf);
        } else {
            snprintf(result, result_len, "TF 卡没准备好，查不到记录。");
        }
        return true;
    }

#if CONFIG_VOICE_BLE
    if (strcmp(name, "ble_scan") == 0) {
        if (!ble_ready()) {
            snprintf(result, result_len, "蓝牙还没就绪。");
            return true;
        }
        static char names[10][32];
        static uint8_t addrs[10][6];
        static int8_t rssi[10];
        int n = ble_scan(names, addrs, rssi, 10, 6000);
        if (n <= 0) {
            snprintf(result, result_len, "附近没扫到蓝牙设备。");
        } else {
            int off = snprintf(result, result_len, "附近有 %d 个蓝牙设备：", n);
            for (int i = 0; i < n && off < (int)result_len - 40; i++) {
                off += snprintf(result + off, result_len - off,
                                "%s%s(%02X:%02X:%02X:%02X:%02X:%02X, %d dBm)",
                                i ? "；" : "", names[i][0] ? names[i] : "未命名",
                                addrs[i][0], addrs[i][1], addrs[i][2],
                                addrs[i][3], addrs[i][4], addrs[i][5], rssi[i]);
            }
        }
        return true;
    }

    if (strcmp(name, "ble_connect") == 0) {
        const char *a = arg_str(args, "address");
        if (!a || !a[0]) {
            snprintf(result, result_len, "要连哪个设备？请给我地址。");
            return true;
        }
        unsigned v[6];
        if (sscanf(a, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            snprintf(result, result_len, "地址格式不对，应该是 AA:BB:CC:DD:EE:FF。");
            return true;
        }
        uint8_t addr[6];
        for (int i = 0; i < 6; i++) {
            addr[i] = (uint8_t)v[i];
        }
        if (ble_connect(addr) == ESP_OK) {
            snprintf(result, result_len, "已经连上这个蓝牙设备了。");
        } else {
            snprintf(result, result_len, "连接失败，可能设备不在范围内或已经关机。");
        }
        return true;
    }

    if (strcmp(name, "ble_disconnect") == 0) {
        if (ble_disconnect() == ESP_OK) {
            snprintf(result, result_len, "已经断开蓝牙连接。");
        } else {
            snprintf(result, result_len, "当前没有蓝牙连接。");
        }
        return true;
    }

    if (strcmp(name, "ble_status") == 0) {
        snprintf(result, result_len, "蓝牙%s，%s。",
                 ble_ready() ? "已就绪" : "未就绪",
                 ble_connected() ? "当前连着设备" : "当前没有连接");
        return true;
    }

#endif /* CONFIG_VOICE_BLE */

    if (strcmp(name, "reboot") == 0) {
        /* Deferred: the spoken answer must finish before the chip restarts. */
        s_reboot_at_us = esp_timer_get_time() + 8000000;
        snprintf(result, result_len, "好的，8 秒后重启。");
        ESP_LOGW(TAG, "reboot scheduled");
        return true;
    }

    ESP_LOGW(TAG, "model asked for unknown tool '%s'", name);
    return false;
}