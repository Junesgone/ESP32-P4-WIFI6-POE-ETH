#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Mounts the TF card (SDMMC slot 0 on ESP32-P4) and creates the log folders. */
esp_err_t storage_mount(void);
bool storage_ready(void);

/* Card-relative path of the append-only conversation index. */
#define STORAGE_INDEX_PATH "conv/index.jsonl"

/* Writes one mono 16-bit WAV and returns its card-relative path.
 * The JSONL record is written later by storage_append_turn(), once the bridge
 * has produced the transcript and the reply, so every line is complete. */
esp_err_t storage_save_wav(const int16_t *mono, size_t samples, uint32_t sample_rate,
                           uint32_t duration_ms,
                           char *out_rel_path, size_t out_rel_path_len);

/* Appends one complete conversation turn as a single JSON object. */
esp_err_t storage_append_turn(const char *rel_wav, uint32_t duration_ms, int rms_peak,
                              const char *face_id, const char *speaker_id,
                              const char *text, const char *reply);

/* Copies a short human/LLM readable summary of the last few turns into out. */
esp_err_t storage_recent_text(char *out, size_t out_len, int max_records);

/* Prints the last max_lines records of conv/index.jsonl over the console. */
esp_err_t storage_dump_index(int max_lines);

/* ISO-8601 local timestamp, or "uptime+<ms>" before SNTP has synced. */
void storage_timestamp(char *buf, size_t len);
bool storage_time_synced(void);
void storage_set_time_synced(bool synced);
