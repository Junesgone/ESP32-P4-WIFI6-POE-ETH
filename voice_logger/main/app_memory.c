/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app_storage.h"
#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "memory";

#define MEM_DIR_ABS   BSP_SD_MOUNT_POINT "/" MEMORY_DIR
#define MEM_PATH_MAX  160
#define MEM_READ_MAX  (64 * 1024)     /* one speaker's whole file is read piecewise */

static bool s_ready;
static char s_current[MEMORY_SPK_MAX];

void memory_set_current_speaker(const char *speaker)
{
    snprintf(s_current, sizeof(s_current), "%s",
             (speaker && speaker[0] && strcmp(speaker, "unknown") != 0) ? speaker : "unknown");
}

const char *memory_current_speaker(void)
{
    return s_current[0] ? s_current : "unknown";
}

/* Filesystem-safe copy of a speaker label ("人1" is valid UTF-8 and FAT32
 * accepts it; slashes and control chars are replaced). */
static void speaker_slug(const char *speaker, char *out, size_t out_len)
{
    const char *src = (speaker && speaker[0]) ? speaker : "unknown";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < out_len; p++) {
        unsigned char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20) {
            out[o++] = '_';
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    if (o == 0) {
        snprintf(out, out_len, "unknown");
    }
}

static bool path_for(const char *speaker, char *out, size_t out_len)
{
    char slug[MEMORY_SPK_MAX];
    speaker_slug(speaker, slug, sizeof(slug));
    int n = snprintf(out, out_len, "%s/%s.txt", MEM_DIR_ABS, slug);
    return n > 0 && n < (int)out_len;
}

esp_err_t memory_init(void)
{
    if (!storage_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(MEM_DIR_ABS, &st) != 0) {
        if (mkdir(MEM_DIR_ABS, 0777) != 0) {
            ESP_LOGE(TAG, "cannot create %s", MEM_DIR_ABS);
            return ESP_FAIL;
        }
    }
    s_ready = true;
    ESP_LOGI(TAG, "per-person memory at %s", MEM_DIR_ABS);
    return ESP_OK;
}

bool memory_ready(void)
{
    return s_ready;
}

/* One line = "ts\tuser\treply".  Tabs/newlines inside the texts would break the
 * format, so they are flattened to spaces before writing. */
static void flatten(const char *src, char *out, size_t out_len)
{
    size_t o = 0;
    for (const char *p = src; p && *p && o + 1 < out_len; p++) {
        char c = *p;
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
        out[o++] = c;
    }
    out[o] = 0;
}

esp_err_t memory_append(const char *speaker, const char *user_text, const char *reply)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!user_text || !user_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[MEM_PATH_MAX];
    if (!path_for(speaker, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    static char u[MEMORY_LINE_MAX];
    static char a[MEMORY_LINE_MAX];
    static char ts[40];
    flatten(user_text, u, sizeof(u));
    flatten(reply ? reply : "", a, sizeof(a));
    storage_timestamp(ts, sizeof(ts));
    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "cannot append %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "%s\t%s\t%s\n", ts, u, a);
    long size = ftell(f);
    fclose(f);

    /* Keep the file bounded: when it grows past 48 KB, rewrite it with only the
     * newest 120 lines (the oldest memories rotate out). */
    if (size > 48 * 1024) {
        FILE *in = fopen(path, "r");
        if (in) {
            size_t cap = (size_t)size + 1;
            char *all = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!all) {
                all = malloc(cap);
            }
            if (all) {
                size_t got = fread(all, 1, cap - 1, in);
                all[got] = 0;
                fclose(in);
                /* find the start of the last 120 lines */
                int lines = 0;
                char *p = all + got;
                while (p > all) {
                    p--;
                    if (*p == '\n') {
                        lines++;
                        if (lines > 120) {
                            p++;
                            break;
                        }
                    }
                }
                FILE *out = fopen(path, "w");
                if (out) {
                    fputs(p, out);
                    fclose(out);
                    ESP_LOGI(TAG, "memory file %s rotated (%ld bytes -> newest 120 turns)",
                             path, size);
                }
                free(all);
            } else {
                fclose(in);
            }
        }
    }
    ESP_LOGI(TAG, "remembered turn for %s (%s, %ld bytes)", speaker ? speaker : "?",
             path, size);
    return ESP_OK;
}

/* Reads all lines, calls cb(line) for each; returns number of lines. */
typedef void (*line_cb_t)(const char *line, void *ctx);

static int for_each_line(const char *path, line_cb_t cb, void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    static char line[MEMORY_LINE_MAX];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        if (len) {
            n++;
            if (cb) {
                cb(line, ctx);
            }
        }
    }
    fclose(f);
    return n;
}

typedef struct {
    char *out;
    size_t out_len;
    size_t used;
    int keep;
    int seen;
    int total;
    int copied;
} recent_ctx_t;

/* Keep the last `keep` turns: first pass counts, second copies the tail. */
static void recent_cb(const char *line, void *vctx)
{
    recent_ctx_t *c = vctx;
    c->seen++;
    if (c->seen <= c->total - c->keep) {
        return;
    }
    char *u = strdup(line);
    if (!u) {
        return;
    }
    /* line = "<ts>\t<user>\t<reply>" */
    char *ts_sep = strchr(u, '\t');
    if (!ts_sep) {
        free(u);
        return;
    }
    *ts_sep = 0;
    char *u_text = ts_sep + 1;
    char *reply_sep = strchr(u_text, '\t');
    char *reply = "";
    if (reply_sep) {
        *reply_sep = 0;
        reply = reply_sep + 1;
    }
    int n = snprintf(c->out + c->used, c->out_len - c->used,
                     "之前他说：%.240s\n你答：%.240s\n", u_text, reply);
    if (n > 0 && c->used + (size_t)n < c->out_len) {
        c->used += (size_t)n;
        c->copied++;
    }
    free(u);
}

int memory_recent(const char *speaker, int max_turns, char *out, size_t out_len)
{
    if (out && out_len) {
        out[0] = 0;
    }
    if (!s_ready || !out || out_len < 8 || max_turns <= 0) {
        return 0;
    }
    char path[MEM_PATH_MAX];
    if (!path_for(speaker, path, sizeof(path))) {
        return 0;
    }
    recent_ctx_t c = { .out = out, .out_len = out_len, .keep = max_turns };
    c.total = for_each_line(path, NULL, NULL);
    if (c.total == 0) {
        return 0;
    }
    for_each_line(path, recent_cb, &c);
    ESP_LOGI(TAG, "loaded %d of %d remembered turns for %s", c.copied, c.total,
             speaker ? speaker : "?");
    return c.copied;
}

typedef struct {
    const char *query;
    char *out;
    size_t out_len;
    size_t used;
    int hits;
} search_ctx_t;

static void search_cb(const char *line, void *vctx)
{
    search_ctx_t *c = vctx;
    if (c->hits >= 5 || c->used + 300 >= c->out_len) {
        return;
    }
    if (!c->query || !c->query[0] || !strstr(line, c->query)) {
        return;
    }
    char *u = strdup(line);
    if (!u) {
        return;
    }
    /* line = "<ts>\t<user>\t<reply>" */
    char *ts_sep = strchr(u, '\t');
    char *u_text = ts_sep ? ts_sep + 1 : u;
    char *reply_sep = strchr(u_text, '\t');
    char *a_text = "";
    if (reply_sep) {
        *reply_sep = 0;
        a_text = reply_sep + 1;
    }
    int n = snprintf(c->out + c->used, c->out_len - c->used,
                     "他说：%.200s -> 你答：%.200s\n", u_text, a_text);
    free(u);
    if (n > 0 && c->used + (size_t)n < c->out_len) {
        c->used += (size_t)n;
        c->hits++;
    }
}

int memory_search(const char *speaker, const char *query, char *out, size_t out_len)
{
    if (out && out_len) {
        out[0] = 0;
    }
    if (!s_ready || !out || out_len < 16 || !query || !query[0]) {
        return 0;
    }
    char path[MEM_PATH_MAX];
    if (!path_for(speaker, path, sizeof(path))) {
        return 0;
    }
    search_ctx_t c = { .query = query, .out = out, .out_len = out_len };
    for_each_line(path, search_cb, &c);
    ESP_LOGI(TAG, "memory search \"%.40s\" for %s: %d hits", query,
             speaker ? speaker : "?", c.hits);
    return c.hits;
}

esp_err_t memory_clear(const char *speaker)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speaker && speaker[0]) {
        char path[MEM_PATH_MAX];
        if (!path_for(speaker, path, sizeof(path))) {
            return ESP_ERR_INVALID_ARG;
        }
        return (remove(path) == 0) ? ESP_OK : ESP_FAIL;
    }
    DIR *d = opendir(MEM_DIR_ABS);
    if (!d) {
        return ESP_FAIL;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[MEM_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%.60s", MEM_DIR_ABS, e->d_name);
        remove(path);
    }
    closedir(d);
    return ESP_OK;
}

typedef struct {
    char *out;
    size_t out_len;
    size_t used;
} list_ctx_t;

static void list_cb(const char *line, void *vctx)
{
    list_ctx_t *c = vctx;
    (void)line;
    c->used++;      /* reused as a counter */
}

int memory_list(char *out, size_t out_len)
{
    if (out && out_len) {
        out[0] = 0;
    }
    if (!s_ready || !out || out_len < 16) {
        return 0;
    }
    DIR *d = opendir(MEM_DIR_ABS);
    if (!d) {
        snprintf(out, out_len, "（还没有任何记忆）");
        return 0;
    }
    int speakers = 0;
    size_t used = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && used + 80 < out_len) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char path[MEM_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%.60s", MEM_DIR_ABS, e->d_name);
        list_ctx_t c = { .out = out, .out_len = out_len, .used = 0 };
        int turns = for_each_line(path, list_cb, &c);
        char name[64];
        snprintf(name, sizeof(name), "%.60s", e->d_name);
        char *dot = strrchr(name, '.');
        if (dot) {
            *dot = 0;
        }
        int n = snprintf(out + used, out_len - used, "- %s：%d 轮\n", name, turns);
        if (n > 0) {
            used += (size_t)n;
        }
        speakers++;
    }
    closedir(d);
    if (speakers == 0) {
        snprintf(out, out_len, "（还没有任何记忆）");
    }
    return speakers;
}
