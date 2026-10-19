#include "app_conversation.h"

#include <stdio.h>
#include <string.h>

#include "app_deepseek.h"
#include "app_tts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "conv";

#define CONV_LINES      24
#define CONV_TEXT_LEN   160
#define CONV_SPEAKER_LEN 16
/* Evaluate as soon as a sentence lands, after a short settle window so a
 * multi-sentence turn is judged once instead of once per fragment. */
#define CONV_SETTLE_MS      1200
#define CONV_IDLE_WAIT_MS   30000
#define CONV_COOLDOWN_US    ((int64_t)12 * 1000 * 1000)
/* After a reply the supervisor waits this long before it may speak again, so
 * it never steps on the tail of the answer the user just heard. */
#define CONV_POST_TURN_GAP_MS  2500
/* How long a reply stays recent enough to be shown to the judge (prevents the
 * supervisor from repeating a correction the reply already made). */
#define CONV_LAST_REPLY_US     ((int64_t)60 * 1000 * 1000)

typedef struct {
    char speaker[CONV_SPEAKER_LEN];
    char text[CONV_TEXT_LEN];
} conv_line_t;

static conv_line_t s_lines[CONV_LINES];
static int s_head;
static int s_count;
static volatile int s_new;                  /* utterances since last evaluation */
static volatile int64_t s_last_interject_us;
static volatile int s_mode = CONV_MODE_HYBRID;

/* Reply-turn bookkeeping, written by the utterance worker on main.c and read
 * by the supervisor task.  Plain volatile ints are enough here: the worst
 * failure from a stale read is one extra evaluation, never a lock problem. */
static volatile int s_turn_active;             /* reply turn in progress */
static volatile bool s_claim_pending;          /* heard something checkable */
static volatile int64_t s_turn_end_us;         /* end of the last reply turn */
static volatile int64_t s_last_reply_us;       /* when the board last spoke */
static volatile int64_t s_judge_retry_after_us; /* backoff after a failed judge */
static char s_last_reply[1024];                /* what it said (for the judge) */

/* One TLS handshake at a time.
 *
 * The board has very few DMA-capable internal buffers and each mbedTLS
 * handshake takes several AES DMA descriptors.  With the utterance worker
 * doing ASR/LLM while the supervisor does its fact-check search, the second
 * handshake dies with
 *     esp-aes: Failed to allocate memory for start alignment buffer
 * so every cloud call is serialised through this mutex. */
static SemaphoreHandle_t s_cloud_lock;
static SemaphoreHandle_t s_wake;   /* "a sentence arrived" notification */

void conversation_lock_cloud(void)
{
    if (s_cloud_lock) {
        xSemaphoreTake(s_cloud_lock, portMAX_DELAY);
    }
}

void conversation_unlock_cloud(void)
{
    if (s_cloud_lock) {
        xSemaphoreGive(s_cloud_lock);
    }
}

/* True when the calling task already owns the cloud lock (a non-recursive
 * mutex must not be taken twice - that would deadlock the reply path). */
bool conversation_cloud_lock_held_by_current(void)
{
    if (!s_cloud_lock) {
        return false;
    }
    return xSemaphoreGetMutexHolder(s_cloud_lock) == xTaskGetCurrentTaskHandle();
}
static volatile bool s_supervisor_on = true;

static const char *SYSTEM_PROMPT =
    "你是小新，在客厅里听着大家聊天。你不闲聊、不评价，只在听到明显不对或重要遗漏时，"
    "像朋友那样顺口提醒一句。\n"
    "1. 从对话里找出可查证的说法：天气、温度、价格、汇率、日期、赛事、新闻、数字。\n"
    "2. 出现这类说法，先调用 web_search 核实，不要凭记忆判断。\n"
    "3. 核实之后，只输出下面两种结果之一，不要多写一个字：\n"
    "   - 说法属实，或只是没实质内容的闲聊：不插话\n"
    "   - 说法有误、过时，或存在重要遗漏（要下雨却没带伞、价格变了）："
    "插话：<不超过25个汉字，像随口说的一句口语>\n"
    "4. 不要为了纠正而纠正；查不到或不确定，就输出：不插话\n"
    "5. 插话的内容必须紧跟在“插话：”后面，不用“其实、纠正一下、提醒您”开头，"
    "不用书面词，不加引号。\n"
    "6. 如果下面“小新刚才已经这样回答过”的内容已经说明了这个错误或提醒，"
    "直接输出：不插话，不要重复。\n"
    "示例（只学格式和口吻）：\n"
    "不插话\n"
    "插话：外面下着雨呢，带把伞吧。\n"
    "插话：那家肠粉店涨到十二块了。";

static char s_city[32] = "中山";

const char *conversation_city(void)
{
    return s_city;
}

void conversation_set_city(const char *city)
{
    if (!city || !city[0]) {
        return;
    }
    snprintf(s_city, sizeof(s_city), "%s", city);
    nvs_handle_t h;
    if (nvs_open("conv", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "city", s_city);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "city -> %s (saved)", s_city);
}

static void conversation_load_city(void)
{
    nvs_handle_t h;
    if (nvs_open("conv", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_city);
        if (nvs_get_str(h, "city", s_city, &len) != ESP_OK) {
            snprintf(s_city, sizeof(s_city), "%s", "中山");
        }
        nvs_close(h);
    }
}

const char *conversation_mode_name(int mode)
{
    if (mode == CONV_MODE_MONITOR) {
        return "monitor";
    }
    if (mode == CONV_MODE_ASSISTANT) {
        return "assistant";
    }
    return "hybrid";
}

int conversation_mode(void)
{
    return s_mode;
}

void conversation_set_mode(int mode)
{
    if (mode < CONV_MODE_ASSISTANT || mode > CONV_MODE_HYBRID) {
        return;
    }
    s_mode = mode;
    ESP_LOGW(TAG, "mode -> %s", conversation_mode_name(mode));
}

bool conversation_should_reply(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    if (s_mode == CONV_MODE_MONITOR) {
        return false;
    }
    if (s_mode == CONV_MODE_ASSISTANT) {
        return true;
    }
    /* HYBRID: answer only when actually addressed. */
    if (strstr(text, "小新") != NULL) {
        return true;
    }
    size_t n = strlen(text);
    if (n && (text[n - 1] == 0x9F || strstr(text, "？") || strstr(text, "?"))) {
        return true;
    }
    if (strstr(text, "吗") || strstr(text, "呢") || strstr(text, "帮我") ||
        strstr(text, "提醒") || strstr(text, "查一下") || strstr(text, "告诉我")) {
        return true;
    }
    return false;
}

/* Cheap pre-filter: only ask the model about utterances that actually contain
 * something checkable (weather, money, dates, numbers, ...).  In ASSISTANT mode
 * the board answers every utterance, and a judge round trip after every single
 * "这个玉米" would double cloud cost and delay the next reply.  This keeps the
 * proactive interjections for the sentences the supervisor was built for. */
static bool text_has_claim(const char *t)
{
    static const char *kw[] = {
        "天气", "温度", "气温", "下雨", "下雪", "晴天", "阴天", "刮风",
        "价格", "多少钱", "打折", "优惠", "涨价", "降价", "贵", "便宜",
        "汇率", "美元", "人民币", "欧元", "日元",
        "新闻", "比赛", "赛事", "比分", "冠军",
        "日期", "几号", "星期", "礼拜", "几点", "点钟",
        "明天", "今天", "昨天", "后天", "下周", "周末",
    };
    if (!t || !t[0]) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strstr(t, kw[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* ------------------- reply-turn bookkeeping (main.c) -------------------- */

void conversation_note_turn_start(void)
{
    s_turn_active++;
}

void conversation_note_turn_end(void)
{
    if (s_turn_active > 0) {
        s_turn_active--;
    }
    s_turn_end_us = esp_timer_get_time();
    if (s_wake) {
        /* Wake the supervisor: it may judge now that the reply has rung out. */
        xSemaphoreGive(s_wake);
    }
}

void conversation_note_reply(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    snprintf(s_last_reply, sizeof(s_last_reply), "%s", text);
    s_last_reply_us = esp_timer_get_time();
}

bool conversation_supervisor_enabled(void)
{
    return s_supervisor_on;
}

void conversation_add(const char *speaker, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    snprintf(s_lines[s_head].speaker, CONV_SPEAKER_LEN, "%s",
             (speaker && speaker[0]) ? speaker : "unknown");
    snprintf(s_lines[s_head].text, CONV_TEXT_LEN, "%s", text);
    s_head = (s_head + 1) % CONV_LINES;
    if (s_count < CONV_LINES) {
        s_count++;
    }
    s_new++;
    if (text_has_claim(text)) {
        s_claim_pending = true;     /* worth a supervised fact check */
    }
    if (s_wake) {
        xSemaphoreGive(s_wake);     /* wake the supervisor immediately */
    }
    ESP_LOGI(TAG, "[%s] %s", s_lines[(s_head + CONV_LINES - 1) % CONV_LINES].speaker, text);
}

void conversation_snapshot(char *out, size_t out_len)
{
    if (!out || !out_len) {
        return;
    }
    out[0] = 0;
    int off = 0;
    int start = (s_head + CONV_LINES - s_count) % CONV_LINES;
    for (int i = 0; i < s_count && off < (int)out_len - 200; i++) {
        int idx = (start + i) % CONV_LINES;
        off += snprintf(out + off, out_len - (size_t)off, "[%s] %s\n",
                        s_lines[idx].speaker, s_lines[idx].text);
    }
}

/* Returns 1 = interject (say filled), 0 = ran/skipped without interjecting,
 * -1 = could not run (worth one retry later).  The caller holds the TLS lock;
 * no retry/sleep happens in here. */
static int conversation_eval_locked(bool force, char *say, size_t say_len)
{
    if (say && say_len) {
        say[0] = 0;
    }
    if (!deepseek_configured()) {
        return 0;       /* no key: nothing to judge, do not retry forever */
    }
    if (s_count == 0) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    if (!force && (now - s_last_interject_us) < CONV_COOLDOWN_US) {
        return 0;
    }
    char *snap = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) {
        snap = malloc(4096);
    }
    if (!snap) {
        return -1;
    }
    conversation_snapshot(snap, 4096);

    char *reply = heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        reply = malloc(512);
    }
    if (!reply) {
        free(snap);
        return -1;
    }
    /* Give the model the local city so "今天天气不错" can be checked against
     * the real forecast, and let it call web_search to verify. */
    char *user_prompt = heap_caps_malloc(5120, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!user_prompt) {
        user_prompt = malloc(5120);
    }
    if (!user_prompt) {
        free(snap);
        free(reply);
        return -1;
    }
    int64_t reply_age = now - s_last_reply_us;
    if (s_last_reply_us && reply_age >= 0 && reply_age < CONV_LAST_REPLY_US && s_last_reply[0]) {
        snprintf(user_prompt, 5120,
                 "当前城市：%s\n小新刚才已经这样回答过：%.400s\n最近对话记录：\n%.4096s",
                 s_city, s_last_reply, snap);
    } else {
        snprintf(user_prompt, 5120, "当前城市：%s\n最近对话记录：\n%.4096s", s_city, snap);
    }
    esp_err_t err = deepseek_check(SYSTEM_PROMPT, user_prompt, reply, 512);
    /* No retry/sleep in here: the caller holds the only cloud/TLS lock, and a
     * sleeping retry would block the user's next ASR turn for 1.2 s.  A failed
     * judgement is retried later by the supervisor task, outside the lock. */
    free(user_prompt);
    free(snap);
    bool interject = false;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "judge: %s", reply);
        const char *mark = strstr(reply, "插话：");
        if (!mark) {
            mark = strstr(reply, "插话:");
        }
        /* "不插话" also contains "插话"; never read it as an interjection. */
        bool negated = (mark && mark >= reply + 3 && memcmp(mark - 3, "不", 3) == 0);
        if (mark && !negated) {
            /* Skip "插话" and then the colon.  The colon is either ASCII ':'
             * or the full-width EF BC 9A (：), so scan for it instead of
             * assuming a byte length. */
            const unsigned char *p = (const unsigned char *)mark;
            while (*p && !(*p == ':' || (p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x9A))) {
                p++;
            }
            if (*p == ':') {
                p++;
            } else if (*p) {
                p += 3;
            }
            while (*p == ' ' || *p == '"') {
                p++;
            }
            if (*p && say && say_len) {
                snprintf(say, say_len, "%s", (const char *)p);
                interject = true;
            }
        } else if (!mark) {
            /* Some replies drop the colon ("插话 外面下雨了").  Accept the word
             * on its own too, as long as it is not the "不插话" verdict. */
            const char *word = strstr(reply, "插话");
            bool word_negated = (word && word >= reply + 3 && memcmp(word - 3, "不", 3) == 0);
            if (word && !word_negated) {
                const unsigned char *p = (const unsigned char *)(word + strlen("插话"));
                while (*p == ' ' || *p == ':' || *p == '"') {
                    p++;
                }
                if (*p && say && say_len) {
                    snprintf(say, say_len, "%s", (const char *)p);
                    interject = true;
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "judge request failed: %s", esp_err_to_name(err));
    }
    free(reply);
    if (err != ESP_OK) {
        return -1;
    }
    /* Only clear the backlog when the model actually ran; a failed judge is
     * retried on the next wake-up instead of being forgotten. */
    s_new = 0;
    s_claim_pending = false;
    if (interject) {
        s_last_interject_us = esp_timer_get_time();
        return 1;
    }
    return 0;
}

int conversation_eval_once(bool force, char *say, size_t say_len)
{
    if (!s_cloud_lock) {
        return -1;
    }
    conversation_lock_cloud();
    /* A reply turn may have started while we waited for the lock; judging now
     * would use a stale transcript and delay the answer in flight. */
    int r;
    if (!force && (s_turn_active > 0 || tts_speaking())) {
        r = 0;
    } else {
        r = conversation_eval_locked(force, say, say_len);
    }
    conversation_unlock_cloud();
    return r;
}

/* Wait (bounded) for the reply pipeline to go quiet.  The old code skipped and
 * then waited for the 30 s idle timeout, which made interjections show up half a
 * minute late in the local-TTS fallback path. */
static bool wait_pipeline_idle(int max_ms)
{
    for (int waited = 0; waited < max_ms; waited += 200) {
        if (s_turn_active <= 0 && !tts_speaking()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return s_turn_active <= 0 && !tts_speaking();
}

static void conversation_task(void *arg)
{
    (void)arg;
    while (1) {
        /* Sleep until a sentence lands, a reply turn ends, or 30 s pass with a
         * claim still unjudged (a failed judge is retried on the timeout). */
        if (xSemaphoreTake(s_wake, pdMS_TO_TICKS(CONV_IDLE_WAIT_MS)) != pdTRUE) {
            /* timeout: fall through and re-check a pending claim */
        }
        /* Let the rest of the turn land so we judge the whole sentence. */
        vTaskDelay(pdMS_TO_TICKS(CONV_SETTLE_MS));
        xSemaphoreTake(s_wake, 0);      /* collapse extra notifications */
        if (!s_supervisor_on || !s_claim_pending || s_new <= 0) {
            continue;
        }
        if (esp_timer_get_time() < s_judge_retry_after_us) {
            continue;       /* judge failed recently; do not hammer the TLS pool */
        }
        /* Never take the only cloud/TLS lock while a reply turn is being
         * produced or played: the answer in flight already owns it. */
        if (!wait_pipeline_idle(20000)) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (s_turn_end_us && (now - s_turn_end_us) < (int64_t)CONV_POST_TURN_GAP_MS * 1000) {
            int64_t wait_ms = CONV_POST_TURN_GAP_MS - (now - s_turn_end_us) / 1000;
            if (wait_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms + 50));
            }
            if (!wait_pipeline_idle(3000)) {
                continue;
            }
        }
        char say[256];
        int r = conversation_eval_once(false, say, sizeof(say));
        if (r < 0) {
            /* Failed judge (TLS/heap): wait outside the lock, then retry once.
             * The pending claim survives, so nothing is silently forgotten. */
            ESP_LOGW(TAG, "judge failed; retrying once");
            vTaskDelay(pdMS_TO_TICKS(1200));
            if (!wait_pipeline_idle(3000)) {
                continue;
            }
            r = conversation_eval_once(false, say, sizeof(say));
        }
        if (r < 0) {
            /* Still failing (network down / TLS pool empty): back off instead
             * of retrying the whole judgement on every utterance. */
            s_judge_retry_after_us = esp_timer_get_time() + (int64_t)60 * 1000 * 1000;
        }
        if (r > 0) {
            /* The judge takes seconds; a new reply turn may have started since.
             * Never talk over it - and never let tts_stop() cut its audio. */
            if (!s_supervisor_on || !wait_pipeline_idle(500)) {
                ESP_LOGW(TAG, "插话取消（已关闭或新的回复回合已开始）");
                continue;
            }
            ESP_LOGW(TAG, "主动插话：%s", say);
            if (tts_ready()) {
                tts_say(say);
            }
        }
    }
}

void conversation_init(int mode)
{
    conversation_set_mode(mode);
    conversation_load_city();
    if (!s_cloud_lock) {
        s_cloud_lock = xSemaphoreCreateMutex();   /* created before any user */
        if (!s_cloud_lock) {
            ESP_LOGE(TAG, "cannot create the cloud lock - supervisor disabled");
            return;
        }
    }
    if (!s_wake) {
        s_wake = xSemaphoreCreateBinary();
        if (!s_wake) {
            ESP_LOGE(TAG, "cannot create the supervisor wake semaphore");
            return;
        }
    }
    s_supervisor_on = CONFIG_VOICE_SUPERVISOR;
    BaseType_t ok = xTaskCreate(conversation_task, "conv", 16384, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "supervisor task creation failed");
    } else {
        ESP_LOGI(TAG, "conversation monitor ready (mode %s, proactive interjections %s)",
                 conversation_mode_name(mode), s_supervisor_on ? "on" : "off");
    }
}

void conversation_enable_supervisor(bool on)
{
    s_supervisor_on = on;
    if (!on) {
        /* Do not judge a stale claim after the user turned it off. */
        s_claim_pending = false;
        s_new = 0;
    }
    ESP_LOGW(TAG, "proactive interjections %s", on ? "enabled" : "disabled");
}
