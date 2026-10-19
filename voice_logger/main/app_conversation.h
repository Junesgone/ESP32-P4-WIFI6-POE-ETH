#pragma once
/* Conversation monitor: make the board a participant, not a Q&A box.
 *
 * Every transcribed utterance is appended to a rolling transcript with a
 * speaker label.  A supervisor task periodically shows that transcript to the
 * model and asks one question: "should you speak up right now?".  If yes it
 * talks over the conversation - the same TTS path as a normal reply, just
 * started on its own initiative.
 *
 * Modes:
 *   ASSISTANT - answer every utterance (boot default); the supervisor still
 *               fact-checks new claims once the reply is done.
 *   HYBRID    - answer when addressed, otherwise observe + interject.
 *   MONITOR   - never answer, only observe + interject. */
#include <stdbool.h>
#include <stddef.h>

#define CONV_MODE_ASSISTANT 0
#define CONV_MODE_MONITOR   1   /* never answer, only observe + interject */
#define CONV_MODE_HYBRID    2   /* answer when addressed, otherwise observe */

void conversation_init(int mode);
int  conversation_mode(void);
void conversation_set_mode(int mode);
const char *conversation_mode_name(int mode);

/* Serialise cloud/TLS work: the board only has ~74 KB of internal RAM after
 * init, and two concurrent mbedTLS sessions fail with -0x008D (SSL alloc
 * failed).  Every ASR / LLM / search / cloud-TTS call must be bracketed by
 * these.  The lock is not recursive, so helpers that may be called from inside
 * an already-locked turn (e.g. cloud TTS during a reply) use the holder check
 * below to avoid taking it twice. */
void conversation_lock_cloud(void);
void conversation_unlock_cloud(void);
bool conversation_cloud_lock_held_by_current(void);

/* City used when checking weather-like claims (default 中山, stored in NVS). */
const char *conversation_city(void);
void conversation_set_city(const char *city);

/* Should this utterance get a normal spoken reply in the current mode? */
bool conversation_should_reply(const char *text);

/* Append one transcribed utterance. */
void conversation_add(const char *speaker, const char *text);

/* Human readable transcript tail (console). */
void conversation_snapshot(char *out, size_t out_len);

/* Run one supervisor evaluation now.  When force is true the cooldown is
 * ignored.  Returns 1 when it decided to interject (say receives the sentence),
 * 0 when it judged without interjecting, -1 when the judgement itself failed
 * (network/heap; the caller may retry later).  Exposed for the console. */
int conversation_eval_once(bool force, char *say, size_t say_len);

/* Voice-pipeline bookkeeping.  The supervisor must never take the only TLS
 * lock while a reply is being produced or played, and it must know what the
 * board just said so it does not repeat the same correction.  main.c calls
 * these around every utterance turn. */
void conversation_note_turn_start(void);
void conversation_note_turn_end(void);
void conversation_note_reply(const char *text);

/* Proactive interjections ("主动插话").  The boot value comes from
 * CONFIG_VOICE_SUPERVISOR; serial command "supervisor on|off" toggles it. */
bool conversation_supervisor_enabled(void);
void conversation_enable_supervisor(bool on);
