/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* Minimal line-oriented console on the UART, so the DeepSeek -> TTS -> SD path
 * can be exercised before any speech recognition exists. */
typedef void (*console_line_handler_t)(const char *line);
void console_start(console_line_handler_t handler);
