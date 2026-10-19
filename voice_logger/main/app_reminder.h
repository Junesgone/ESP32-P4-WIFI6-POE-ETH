/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* Proactive reminders.
 *
 * The board is always listening (no wake word), so it must also be able to
 * speak up on its own initiative.  A reminder is "say this text on the device
 * at time T", scheduled from a tool call like
 *
 *     user: "十分钟后提醒我喝水"
 *       -> set_reminder{minutes:10, text:"喝水"}
 *
 * Reminders live in RAM, so they do not survive a reboot. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t reminder_init(void);

/* Schedule text to be spoken in `seconds`.  Returns id (>0) or <0 when full. */
int reminder_add(uint32_t seconds, const char *text);

/* Human readable list; returns the number of pending alerts. */
int reminder_list(char *out, size_t out_len);

/* Cancel everything; returns how many were pending. */
int reminder_cancel_all(void);

int reminder_pending_count(void);
