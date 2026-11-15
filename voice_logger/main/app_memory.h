/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* Per-person conversation memory, persisted on the TF card.
 *
 * Speaker identification already tells the board WHO is talking (enrolled
 * name or 人N cluster label).  This module remembers WHAT was said with that
 * person, across reboots, in /sdcard/conv/people/<speaker>.jsonl-style files
 * (tab separated, one turn per line: timestamp, user text, reply text).
 *
 * It serves three purposes:
 *   - the chat request gets the last few turns of THIS speaker as context, so
 *     a recognised person is not a stranger on every boot;
 *   - the model can call the recall_memory tool to search its notes about the
 *     current speaker;
 *   - the serial console can inspect / clear / search what is remembered.
 *
 * Privacy: everything stays on the TF card, files are named by the on-board
 * speaker label, and clearing is a console command ("memory clear").
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define MEMORY_DIR        "conv/people"
#define MEMORY_TURNS_MAX  200     /* per speaker, oldest lines rotate out */
#define MEMORY_SPK_MAX    40      /* max UTF-8 bytes of a speaker label */
#define MEMORY_LINE_MAX   1024

esp_err_t memory_init(void);
bool memory_ready(void);

/* Append one completed turn for this speaker.  The speaker label is sanitised
 * for the filesystem (unknown -> "unknown"). */
esp_err_t memory_append(const char *speaker, const char *user_text, const char *reply);

/* Formatted recent turns (oldest first) for the LLM system message.
 * Returns the number of turns copied; out is always NUL-terminated. */
int memory_recent(const char *speaker, int max_turns, char *out, size_t out_len);

/* Substring search inside one speaker's remembered turns (Chinese-safe byte
 * search).  Returns the number of matching turns, formatted into out. */
int memory_search(const char *speaker, const char *query, char *out, size_t out_len);

/* Wipe one speaker's memory (speaker == NULL/"" clears everybody). */
esp_err_t memory_clear(const char *speaker);

/* Human readable list of speakers with their turn counts. */
int memory_list(char *out, size_t out_len);

/* The speaker whose memory is currently in use (set by the conversation
 * layer), for the recall_memory tool. */
void memory_set_current_speaker(const char *speaker);
const char *memory_current_speaker(void);
