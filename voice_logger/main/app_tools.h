#pragma once
/* Tool calling for 小新.
 *
 * DeepSeek is a text model with no access to the board.  Ask it to "turn the
 * volume up" without tools and it will cheerfully answer "已经调到最大啦" while
 * nothing changes - it is describing an action, not performing one.
 *
 * So the board advertises what it can really do (OpenAI-compatible "tools"),
 * the model replies with tool_calls, this module executes them locally, and the
 * results go back to the model so its final sentence is truthful.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Freshly allocated "tools" array for the chat request; caller owns it. */
cJSON *tools_spec(void);

/* Restricted set for the conversation supervisor: only the tools it needs to
 * fact-check a claim (web_search + get_time).  A supervisor that misreads a
 * sentence must never be able to reboot the board, change the volume or set
 * reminders, so it does not see those tools at all. */
cJSON *tools_spec_supervisor(void);

/* Executes one call.  Returns true when the name is known; result receives a
 * short Chinese sentence that is sent back to the model. */
bool tools_execute(const char *name, const cJSON *args, char *result, size_t result_len);

/* Direct web search (DeepSeek /v1/tools/search), exposed so the serial console
 * can verify the search bridge without going through a model round trip.
 * Returns true on success and fills out with model-readable text. */
bool tools_web_search(const char *query, char *out, size_t outlen);

/* Microseconds timestamp at which a reboot() tool call asked to restart, or 0.
 * The main loop honours it so the reply is spoken before the chip goes down. */
int64_t tools_pending_reboot_us(void);
