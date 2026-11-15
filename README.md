# ESP32-P4-WIFI6-POE-ETH — 小新 (Xiaoxin) Chinese Voice Assistant

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Version](https://img.shields.io/github/v/release/Junesgone/ESP32-P4-WIFI6-POE-ETH?label=version)](https://github.com/Junesgone/ESP32-P4-WIFI6-POE-ETH/releases)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-blue)](https://github.com/espressif/esp-idf)

A self-contained **Chinese voice assistant** running on the
**Waveshare ESP32-P4-WIFI6-POE-ETH** development board. The board listens through
its on-board microphone, transcribes with the **DeepSeek standalone Speech-to-Text
API**, answers with **DeepSeek** (deep thinking, built-in web search, URL reading,
file input and a device function-calling toolset), speaks through its ES8311 +
NS4150B audio path using **DeepSeek streaming Text-to-Speech**, and records every
conversation (audio + text) to a TF card. No wake word, no phone, no PC in the
loop — the board **is** the assistant.

The whole **DeepSeek App "full suite" API** is wired in: `deepseek_web_search`
(联网搜索 / 深度思考搜索), `deepseek_read_url` (阅读链接), file input via the Files
API + document parsing, `safety_check` (模型安全), conversation titles, the
`latest_reminder` user-metadata marker, and the standalone Search / Fetch / OCR /
ASR / TTS tool APIs.

**Keywords:** ESP32-P4, ESP32P4, ESP32-P4-WIFI6-POE-ETH, Waveshare ESP32-P4,
ESP32-P4 dev kit, ESP-IDF, ESP-IDF v6.1, RISC-V, dual-core 400 MHz, 32 MB flash,
32 MB PSRAM, XIP from PSRAM, Wi-Fi 6, Wi-Fi 6 development board, ESP32-C6,
ESP-Hosted, SDIO, ES8311, NS4150B, I2S, embedded AI, edge AI, on-device AI,
TinyML, voice assistant, Chinese voice assistant, 中文语音助手, 小新, Xiaoxin,
wake-word-free, always listening, always-on assistant, speech-to-text, ASR,
cloud ASR, TTS, text-to-speech, streaming TTS, WebSocket TTS, low-latency TTS,
DeepSeek ASR, DeepSeek TTS, DeepSeek OCR, DeepSeek Search API, DeepSeek Fetch API,
deep thinking search, deepseek_web_search, deepseek_read_url, safety_check, Files
API, latest_reminder, DeepSeek full suite, DeepSeek LLM, LLM, chat LLM, function calling, tool calling,
agent, MCP alternative, voice agent, voice UI, speaker recognition, speaker
verification, speaker identification, voiceprint, 声纹识别, speaker diarization,
AEC, acoustic echo cancellation, echo suppression, noise suppression, AGC, VAD,
voice activity detection, barge-in, ESP-SR, esp-dl, Fbank, MFCC, LDA, pitch,
MIPI-CSI, OV5647, camera, face recognition, TF card, SD card, conversation
logging, SNTP, smart speaker, DIY smart speaker, home voice assistant,
home assistant voice, IoT, ESP32 voice assistant.

---

## 1. What it does

```
 you speak
   │
   ├─ ES8311 ADC ─→ I2S ─→ ESP-SR AFE (AEC + NS + AGC) ─→ energy VAD
   │                                                       │
   │                          ┌────────────────────────────┴───────────────────────────┐
   │                          │ one utterance:                                           │
   │                          │  • raw-mic voiceprint (43-dim, on-device) → 主人 / 人N    │
   │                          │  • 16 kHz WAV + JSONL line on the TF card                │
   │                          │  • DeepSeek standalone ASR  POST /v1/tools/asr           │
   │                          │  • DeepSeek chat: 深度思考 + deepseek_web_search           │
   │                          │    (深度思考搜索) + deepseek_read_url + safety_check      │
   │                          │    + conversation title + device tools (12+)              │
   │                          │  • DeepSeek TTS: WebSocket 流式合成 (24 kHz PCM)          │
   │                          │    → 16 kHz → ES8311 → speaker                            │
   │                          │    └─ offline fallback: ESP-SR Chinese TTS               │
   │                          └──────────────────────────────────────────────────────────┘
   │
   └─ a supervisor task fact-checks claims ("今天二十度") against a web search
      and may interject with a one-line correction after the reply
```

Behaviour highlights:

- **No wake word.** "Assistant" mode answers every utterance; "hybrid" answers only
  when addressed and otherwise observes + chips in; "monitor" only records.
- **It waits for the end of the sentence.** 1.1 s trailing silence closes an
  utterance, then a 0.9 s resume window follows: if speech starts again the same
  utterance is extended, so a thinking pause mid-question no longer gets an early
  answer. Proactive interjections are suppressed while the user is talking.
- **Proactive interjections.** After a spoken reply the board can verify claims heard
  in the room (weather, prices, dates, numbers) with a web search and say one short
  correction — gated so it never talks over a reply and never repeats itself.
- **On-device voiceprint.** A 43-dimensional MFCC + pitch embedding is computed from
  the pre-AFE microphone signal, discriminatively weighted per dimension (Fisher
  ratio over hard-to-separate voice pairs), calibrated on session-balanced
  on-device recordings and matched against enrolled people or automatically
  created anonymous roles (人1, 人2, …). Anonymous roles are only seeded by
  reasonably long utterances, so VAD fragments cannot create duplicates.
  Enrolment averages 3 utterances per person.
- **Full logging.** Every turn becomes a 16 kHz WAV plus one JSONL line with speaker
  id, transcript, reply and RMS/duration metadata on the TF card.

---

## 2. Hardware

| Item | Value |
|---|---|
| Board | Waveshare **ESP32-P4-WIFI6-POE-ETH** (P4NRW32, chip rev v3.2) |
| MCU | ESP32-P4, dual-core RISC-V @ 400 MHz + LP core, no radio |
| Memory | **32 MB flash** (GigaDevice, QIO @ 80 MHz) + **32 MB PSRAM** (hex, 200 MHz, XIP) |
| Wi-Fi | on-board **ESP32-C6** (Wi-Fi 6, 2.4 GHz) over ESP-Hosted/SDIO, 40 MHz 4-bit |
| Audio | ES8311 codec (I2C 0x18) + NS4150B class-D amplifier, analog mic, 16 kHz mono |
| Storage | TF card on SDMMC slot 0 (4-bit, 20 MHz) |
| Console | UART0 on GPIO37/38 → CH343 USB serial (115200) |

Pin maps live in `voice_logger/main/board_config.h` and the board fragments under
`config/`.

---

## 3. Features and current state (v0.0.9)

| Area | State |
|---|---|
| Audio path | ES8311 / I2S full duplex, AEC + NS + AGC via ESP-SR AFE, energy VAD |
| Speech recognition | DeepSeek standalone ASR (`POST /v1/tools/asr`, multipart 16 kHz WAV, language auto-detected) |
| Language model | DeepSeek `deepseek-flash` chat, thinking on by default (深度思考), function calling with 12+ device tools |
| Web tools | Built-in `deepseek_web_search` (联网搜索; with thinking on it is 深度思考搜索 over multiple reasoning steps) and `deepseek_read_url` (阅读链接), citations collected from `annotations` |
| File input | Files API upload + server-side parsing (`text_recognition` / `rich_layout_recognition`), then referenced as a `{"type":"file","file_id":...}` content part; OCR available standalone |
| Model safety | `safety_check: {"level":"high"}`; refusals (`finish_reason: content_filter` / 400 "Content Exists Risk") become a short spoken refusal |
| Conversation UX | Conversation title (`title.enabled`), user-metadata marker (`latest_reminder`: date, weekday, city, platform, language) |
| Person recognition | On-board voiceprint decides who is talking (enrolled name or 人N cluster label) and the chat prompt names them |
| Per-person memory | Every turn is remembered per speaker on the TF card and reloaded when that person talks again after a reboot; the model can search it with the `recall_memory` tool |
| Cloud transport | One keep-alive HTTPS connection shared by ASR / chat / TTS / Search / Fetch, with TLS session-ticket resumption (a spoken turn pays ~1 s of handshake instead of ~10.5 s) |
| Speech synthesis | DeepSeek TTS over **WebSocket streaming synthesis** (`wss://api.deepseek.com/v1/tools/tts`, 24 kHz PCM resampled to 16 kHz) with a streaming HTTP POST fallback and ESP-SR offline fallback |
| Voiceprint | On-device 43-dim MFCC+pitch embedding, raw-mic input, LDA-weighted, 3-utterance averaged enrolment, anonymous role clustering with pending/merge/split logic |
| Conversation modes | assistant / hybrid / monitor, switchable at runtime |
| Proactive supervisor | Post-reply fact checking with web search, optional on/off |
| Conversation log | WAV + JSONL per turn on TF card, day folders, SNTP wall-clock timestamps |
| Device tools | Volume, TTS speed, time, reminders, Wi-Fi scan/connect, voiceprint management, recent conversations, system status, reboot |
| Reliability | Bounded cloud timeouts with capture-queue drop accounting; feed-forward AEC reference path |
| Build system | Single PowerShell script that regenerates sdkconfig, builds and flashes app + TTS voice data |

Known limitations (roadmap items): no true barge-in yet (microphone is gated while
the board speaks), anonymous clustering of many very similar voices remains a
feature-space challenge (use `enroll` for identities), cloud latency dominates a
turn (a deep-thinking search turn needs ~4 s before the first answer token),
internal SRAM is the scarce resource so only **one** TLS session is open at a time
(the LLM stream is fully closed before the TTS connection opens), BLE is not
enabled, and no camera is attached yet.

---

## 4. Repository layout

| Path | Contents |
|---|---|
| `voice_logger/` | **main firmware** (ESP-IDF project): audio, AEC/VAD, ASR, DeepSeek, TTS, voiceprint, tools, storage, Wi-Fi |
| `voice_logger/main/` | all application sources: `app_deepseek.c` (chat + standalone Search/Fetch/OCR/Files clients), `app_asr.c` (DeepSeek ASR), `app_tts_cloud.c` (DeepSeek TTS over WebSocket), `app_tools.c` (device function tools), `app_speaker_id.cpp` (voiceprint), `main.c` (audio pipeline + serial console) |
| `c6_slave/` | ESP32-C6 Wi-Fi/BT coprocessor firmware used by the hosted transport |
| `host_ota/` | partition-OTA helper used to update the C6 over SDIO |
| `speaker_test/` `sd_test/` `sd_probe/` `sd_wifi_coexist/` `wifi_scan/` | bring-up projects for each peripheral |
| `tools/` | build + flash script |
| `config/` | board and feature sdkconfig fragments; secrets are **not** committed (see `config/README.md`) |
| `LICENSE` / `NOTICE` | Apache License 2.0 and third-party attribution |

This repository contains **only the board code** plus the build/usage instructions
needed to reproduce it. Development journals, maintenance logs, pitfall records and
private design notes are intentionally kept private.

---

## 5. Getting started

### Prerequisites

- ESP-IDF **v6.1** (tested), with the `idf.py` environment activated. The build
  script assumes the standard Espressif tools install; adjust the paths if yours differ.
- **Windows:** PowerShell 5+ (the legacy `tools/build_voice_logger.ps1`) and the IDF
  Python venv when flashing.
- **Linux / macOS:** bash; `tools/build_voice_logger.sh` does the same job.
- The board on a USB serial port (Windows default `COM5`, Linux `/dev/ttyACM0`;
  override with `-Port` / `-p`).

### 1) Provide your own secrets (never committed)

One DeepSeek key drives the whole full suite (chat, ASR, TTS, search, fetch, OCR,
files); the assistant is fully functional with that one key.

```powershell
cd <repo>/config
Set-Content deepseek.key 'sk-your-deepseek-key'        # required: chat + ASR + TTS + tools
Copy-Item wifi_credentials.defaults.template wifi_credentials.defaults
notepad wifi_credentials.defaults                      # SSID / password
```

`siliconflow.key` is optional and only kept for the legacy fallback path
(`CONFIG_ASR_BASE_URL` / `CONFIG_TTS_API_KEY` pointed at another vendor). The build
script turns the `.key` files into `*.defaults` fragments at build time; those
generated files are git-ignored too. See [config/README.md](config/README.md).

### 2) Provide the offline TTS voice set (not committed)

The ESP-SR Chinese TTS voice data (~3.8 MB binary) is **not** in the repository.
Get a Chinese voice set from the ESP-SR / esp-skainet distribution and place
`esp_tts_voice_data_xiaoxin.dat` at:

```
assets/esp_tts_voice_data_xiaoxin.dat
```

The build script flashes it into the `voice_data` partition at 0x810000. Without it
the firmware still boots, but TTS output is unavailable. See
[assets/README.md](assets/README.md).

### 3) Build + flash

```powershell
# Windows, from the repository root in an ESP-IDF-activated shell
powershell -ExecutionPolicy Bypass -File tools/build_voice_logger.ps1 -Flash -Port COM5
```

```bash
# Linux / macOS
./tools/build_voice_logger.sh          # build only
./tools/build_voice_logger.sh -f -p /dev/ttyACM0       # build + flash
./tools/build_voice_logger.sh -f -m                     # build + flash + monitor
```

Both scripts derive the project root from their own location, read the key, generate
`config/deepseek.defaults`, build the firmware, flash the app and then write the TTS
voice set to the `voice_data` partition.

### 4) First steps on the board

```text
mode                                       print current conversation mode
mode assistant|hybrid|monitor              switch behaviour
say 你好呀                                  speak without the mic (TTS path test)
ask 明天天气怎么样                           full text-driven conversation turn
enroll 主人                                 record 3 utterances, averaged voiceprint
who                                        list enrolled people
recent 5                                   last conversation lines from the TF card
tts                                        TTS engine / voice selection
ds                                         full-suite status + last turn report
vol 50                                     speaker volume 0-100 (boot default is 50)
think on|off   effort low|high|max         深度思考 / 深度思考搜索 (depth)
websearch on|off   readurl on|off          内置联网搜索 / 阅读链接
safety on|off   title on|off|reset         模型安全 / 对话标题
search 上海天气                             standalone Search API
fetch https://api-docs.deepseek.com/       standalone Fetch API
ocr /sdcard/report.pdf [rich]              standalone OCR API
asr /sdcard/rec.wav                        standalone ASR API
askfile /sdcard/report.pdf 总结一下          file input (Files API + parsing) -> answer
```

Additional serial commands (conversation monitor, reminders, AEC calibration,
voiceprint diagnostics) are documented inline in `voice_logger/main/main.c`.

---

## 6. DeepSeek full-suite API integration

The firmware uses the **DeepSeek App "full suite" API** the way the
[Build a Chatbot](https://api-docs.deepseek.com/guides/build_a_chatbot) guide
describes: the service does the heavy lifting, the board only declares the
parameters and renders what comes back. One key (`config/deepseek.key`) drives
chat, ASR, TTS and every tool API.

### 6.1 Chat Completions: App features enabled server-side

`POST https://api.deepseek.com/chat/completions`, SSE streamed. Each request may
declare:

| Parameter | What it enables on the board |
|---|---|
| `thinking: {"type":"enabled"}` + `reasoning_effort: low/high/max` | 深度思考. **On by default**; the thinking text is counted and logged, never spoken. |
| `tools: [{"type":"deepseek_web_search"}]` | 联网搜索. With thinking enabled it becomes **深度思考搜索**: over several reasoning steps the service searches, opens pages and fact-checks before answering. |
| `tools: [{"type":"deepseek_read_url"}]` | 阅读链接: URLs in the user message are fetched and put into context. |
| `safety_check: {"level":"high"}` | 模型内容安全: input and output review. A refusal arrives as `finish_reason: "content_filter"` (or a 400 `Content Exists Risk` for the input) and the board answers with a short spoken refusal instead of the blocked text. |
| `title: {"enabled":true}` | 对话标题, returned in the last chunk's `delta.title`; requested only until the conversation has one. |
| `{"role":"latest_reminder",...}` message | 用户元数据标记: `2026-11-14,星期六,中山,App,中文` (date, weekday, city, platform, language) directly before the user message. |
| `tools: [{"type":"function",...}]` | The device function tools (volume, reminders, Wi-Fi, voiceprint, …) coexist with the built-in web tools. |

Stream handling (see `app_deepseek.c`): `delta.reasoning_content` is the thinking,
`delta.content` the answer; `choices[0].server_tool_event` reports live search /
page-reading progress (`call_started` / `call_completed`); the **last chunk** carries
`delta.annotations` (citations, `url_citation`), `delta.server_tool_calls` (echoed
back in the next request so a page read from a link stays in context),
`delta.title`, `finish_reason` and `usage`. The SSE event can be 10–20 KB on a
search turn, so the line buffer lives in PSRAM.

### 6.2 Standalone tool APIs

| Endpoint | Firmware use | Console |
|---|---|---|
| `POST /v1/tools/search` | client-side 联网搜索 used by the supervisor / function tool | `search <query>` |
| `POST /v1/tools/fetch` | 阅读链接 by URL | `fetch <url>` |
| `POST /v1/tools/ocr` | OCR of a PDF / Word / PPT / image on the TF card | `ocr <path> [rich]` |
| `POST /files` | 文件输入: upload + server-side parsing, then a `{"type":"file","file_id":...}` chat content part | `askfile <path> [question]` |
| `POST /v1/tools/asr` | 语音输入: the captured 16 kHz WAV | automatic + `asr <wav>` |
| `wss://api.deepseek.com/v1/tools/tts` | 朗读: streaming TTS | automatic + `say <text>` |

### 6.3 Streaming speech synthesis (WebSocket)

`app_tts_cloud.c` connects to `wss://api.deepseek.com/v1/tools/tts?voice_id=stella&format=pcm`,
sends the reply as `{"event":"text","text":...}` chunks followed by
`{"event":"finish_text"}`, and plays the binary frames (raw 16-bit mono PCM at
24 kHz, resampled to 16 kHz on the board) through a PSRAM jitter buffer as they
arrive. A streaming `POST /v1/tools/tts` with `format:"pcm"` is the fallback if the
WebSocket cannot be established; the on-board ESP-SR engine remains the offline
fallback. Voices: **stella** (female, default) and **tide** (male).

The streaming **POST is the default**; the WebSocket path is complete but marked
experimental (`CONFIG_VOICE_TTS_WS`, off by default). Measured on this board both
transports deliver audio at the same time - the service does not start sending
audio before `finish_text` - while the esp_websocket_client handshake still fails
intermittently (`esp_transport_read -135` / connect timeout), so the POST avoids
a multi-second stall per reply. With the POST: connect 3.1-3.9 s, first audio
4.3-5.2 s, then playback starts while the rest is still arriving (351 characters
-> 7.3 s of audio, zero playback starvations from the PSRAM jitter buffer). If
the WebSocket is enabled the firmware tries it first, then falls back to POST and
backs off for 5 minutes after two consecutive failures.

### 6.4 Measured voice-turn latency (firmware 0.0.8)

| Stage | Before (0.0.7) | Now (0.0.8) | Note |
|---|---|---|---|
| ASR | 4.2 s | 0.5-1.0 s | shared connection, usually already open |
| Chat first answer token | 3.2-4.6 s | 0.5-1.4 s | TLS session-ticket resume |
| TTS connect | 3.1-3.9 s | 35-50 ms | connection reused from ASR/chat |
| TTS first audio | +1.2-2 s | +1.0-2.0 s | streaming POST, `format:"pcm"` |
| Spoken reply playback | 1-8 s | 1-8 s | PSRAM jitter buffer, 0 starvation |

A spoken question now starts being answered a few seconds after it ends instead of
~20 s. Standalone Search API: 4.7 s for the first call (new TLS connection), then
0.4-0.5 s per call on the reused connection.

### 6.5 TLS session reuse

`app_ds_http.c` keeps **one** `esp_http_client` handle for api.deepseek.com and
serves ASR, TTS, Search, Fetch and the non-streaming chat calls from it. The
DeepSeek edge answers every endpoint with `Connection: keep-alive`, so after the
first handshake the next request goes out on the same TCP+TLS session
(`dshttp` on the console shows requests / handshakes / reuses). The chat SSE
endpoint is the exception - the edge answers it with `Connection: close` and
leaves the chunked body open after `[DONE]` - so the firmware stops reading at
`[DONE]`, closes the socket but keeps the client, and the next handshake resumes
from the TLS session ticket (`CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS`) instead of
redoing the ~3.5 s certificate exchange. Only one TLS session is open at a time
by design.

The persistent client costs ~14 KB of internal RAM, and the SD card needs a
DMA-capable buffer at the same time; both are handled explicitly (the TF card
reserves its DMA buffer after the ESP-Hosted SDIO link is up, and the firmware
logs a `LOW internal RAM` warning when a turn leaves less than ~16 KB free).

### 6.6 Per-person conversation memory

Speaker identification labels every utterance (enrolled name, or 人1/人2… for
unknown-but-consistent voices).  `app_memory.c` stores each completed turn in

    /sdcard/conv/people/<speaker>.txt      (tab separated: timestamp, user, reply)

- When the speaker changes, the in-RAM chat history is cleared and the **last 4
  remembered turns of that person are injected into the system prompt** as
  background, so a recognised person is not a stranger after a reboot.
- The model also gets a `recall_memory` tool: "你还记得我之前说过什么吗" makes it
  search its notes about the current speaker (local substring search, no network).
- Console: `memory` (list speakers + turn counts), `memory show <name>`,
  `memory search <text>`, `memory clear [name]`.
- Files rotate automatically past 48 KB (newest 120 turns are kept). Everything
  stays on the TF card; nothing is uploaded.

Verified on hardware: after a reboot the same voiceprint loaded "2 of 2
remembered turns" and answered *"你刚让我用一句话介绍自己，还问了上海今天的天气"*.

### 6.7 Spoken-answer length and volume

A deep-thinking search turn answers with 300-400 characters -- formatted like the
Chat App would render it. Reading all of that kept the speaker busy for 20-30 s
while the microphone was gated, so follow-up utterances piled up in the 6-slot
capture queue and it felt like the assistant had stopped listening. The firmware
therefore speaks **only the first two sentences (~110 bytes)** of the reply; the
full text still goes to the console and the TF-card JSONL record. The boot volume
is 50 (0-100) because 90 fed the on-board microphone through the on-board
speaker and made the VAD hear the assistant itself.

### 6.8 Configuration

All switches live in `menuconfig → Voice logger configuration → DeepSeek` and
`→ Cloud neural TTS`, and can be flipped at runtime from the serial console
(`think`, `websearch`, `readurl`, `safety`, `title`, `effort`, `tts voice`). The
endpoint URLs are Kconfig strings (`DEEPSEEK_SEARCH_URL`, `DEEPSEEK_FETCH_URL`,
`DEEPSEEK_OCR_URL`, `DEEPSEEK_FILES_URL`, `DEEPSEEK_TTS_URL`,
`DEEPSEEK_TTS_WS_URL`, `ASR_BASE_URL`) so a proxy can be dropped in without
touching the sources.

---

## 7. Voiceprint

- **Input:** the raw microphone block captured in parallel with the AFE output
  (ASR keeps the echo-cancelled / denoised stream). AGC/NS/AEC reshape the spectrum
  and flatten speaker timbre on the processed signal.
- **Feature:** 20 MFCC means + 20 MFCC standard deviations (from esp-dl's 80-band
  log-mel) + 3 pitch statistics = 43 dims, standardised with device-fitted tables,
  LDA-weighted, block-normalised, mixed 90/10 and L2-normalised.
- **Matching:** cosine similarity against existing roles; unmatched voices go through
  a two-hit pending buffer before an anonymous role (人N) is created. Enrolled roles
  are frozen and never auto-merged.
- **Enrolment:** `enroll <name>` collects 3 utterances, averages their embeddings and
  saves the role to the TF card.
- **Configuration:** threshold, margin and enrolment count are Kconfig options
  (`voice_logger/main/Kconfig.projbuild`).

---

## 8. Version and releases

Current version: **v0.0.9** — see [CHANGELOG.md](CHANGELOG.md) and
[GitHub Releases](https://github.com/Junesgone/ESP32-P4-WIFI6-POE-ETH/releases).
The firmware reports its version in the boot banner (`App version`).

---

## 9. License

Released under the **[Apache License, Version 2.0](LICENSE)** — Copyright 2026 Junesgone.
You may use, modify and redistribute the code, including commercially, provided the
license and NOTICE are kept. See [NOTICE](NOTICE) for third-party attribution.

The ESP-Hosted slave example (`c6_slave/`) and the board bring-up projects
(`host_ota/`, `sd_test/`, `sd_probe/`, `sd_wifi_coexist/`, `speaker_test/`,
`wifi_scan/`) are Espressif example code and retain their original Espressif
copyright and Apache-2.0 notices. Runtime services (DeepSeek, SiliconFlow) are
governed by their own terms of service.

---

## 10. Acknowledgements

Built on [ESP-IDF](https://github.com/espressif/esp-idf), ESP-SR, esp-dl and ESP-Hosted
from Espressif. The conversation stack is the [DeepSeek full-suite
API](https://api-docs.deepseek.com/guides/build_a_chatbot): chat with deep thinking
and web tools, standalone ASR, streaming TTS, OCR, Search and Fetch. The old
SiliconFlow SenseVoice / CosyVoice2 path remains available as an optional fallback
(see `config/README.md`). The AEC approach was inspired by
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). "小新" is a personal project for
one living room, not a product.
