#pragma once
/* Direct HTTPS client for the DeepSeek Chat API.
 *
 *   base_url : https://api.deepseek.com        (OpenAI compatible)
 *   model    : deepseek-flash                  (see menuconfig)
 *
 * DeepSeek is a text-only Chat API - it has no ASR and no TTS - so this module
 * only ever moves UTF-8 text.  Audio never leaves the board through it.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Sends one user turn and stores the assistant reply.
 * A short rolling history is kept in RAM so follow-up questions have context. */
esp_err_t deepseek_ask(const char *user_text, char *reply_out, size_t reply_len);

/* Same, but streams the reply and speaks each sentence through ESP-SR TTS as it
 * arrives, so the board starts talking ~1.5 s after the model starts answering
 * instead of waiting for the whole paragraph.  *spoken_out reports whether TTS
 * was actually fed (it is not when TTS is unavailable). */
esp_err_t deepseek_ask_stream(const char *user_text, char *reply_out, size_t reply_len,
                              bool *spoken_out);

/* Stateless fact-checking completion WITH tools (web_search etc.), no history.
 * Used by the conversation supervisor to verify what it just heard. */
esp_err_t deepseek_check(const char *sys_prompt, const char *user_prompt,
                         char *out, size_t out_len);

/* One-shot, stateless completion (no history, no tools).  Used by the
 * conversation supervisor to decide whether to interject. */
esp_err_t deepseek_judge(const char *system_prompt, const char *user_prompt,
                         char *out, size_t out_len);

/* Wipes the in-RAM conversation history. */
void deepseek_reset_history(void);

/* Number of user turns kept so far (for logging / the JSONL record). */
int deepseek_turn_count(void);

/* True when a usable API key is compiled in. */
bool deepseek_configured(void);
