# ESP32-P4-WIFI6-POE-ETH — 小新 (Xiaoxin) Chinese Voice Assistant

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Version](https://img.shields.io/github/v/release/Junesgone/ESP32-P4-WIFI6-POE-ETH?label=version)](https://github.com/Junesgone/ESP32-P4-WIFI6-POE-ETH/releases)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-blue)](https://github.com/espressif/esp-idf)

A self-contained **Chinese voice assistant** running on the
**Waveshare ESP32-P4-WIFI6-POE-ETH** development board. The board listens through
its on-board microphone, transcribes with a cloud ASR, answers with a cloud LLM
(DeepSeek, function calling), speaks through its ES8311 + NS4150B audio path, and
records every conversation (audio + text) to a TF card. No wake word, no phone,
no PC in the loop — the board **is** the assistant.

**Keywords:** ESP32-P4, ESP32P4, ESP32-P4-WIFI6-POE-ETH, Waveshare ESP32-P4,
ESP32-P4 dev kit, ESP-IDF, ESP-IDF v6.1, RISC-V, dual-core 400 MHz, 32 MB flash,
32 MB PSRAM, XIP from PSRAM, Wi-Fi 6, Wi-Fi 6 development board, ESP32-C6,
ESP-Hosted, SDIO, ES8311, NS4150B, I2S, embedded AI, edge AI, on-device AI,
TinyML, voice assistant, Chinese voice assistant, 中文语音助手, 小新, Xiaoxin,
wake-word-free, always listening, always-on assistant, speech-to-text, ASR,
cloud ASR, TTS, text-to-speech, streaming TTS, low-latency TTS, SenseVoice,
CosyVoice2, SiliconFlow, DeepSeek, LLM, chat LLM, function calling, tool calling,
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
   │                          │  • cloud ASR (SiliconFlow SenseVoice)                    │
   │                          │  • DeepSeek chat (function calling, 12+ tools)           │
   │                          │  • cloud TTS (CosyVoice2, streaming) → ES8311 → speaker  │
   │                          │    └─ offline fallback: ESP-SR Chinese TTS               │
   │                          └──────────────────────────────────────────────────────────┘
   │
   └─ a supervisor task fact-checks claims ("今天二十度") against a web search
      and may interject with a one-line correction after the reply
```

Behaviour highlights:

- **No wake word.** "Assistant" mode answers every utterance; "hybrid" answers only
  when addressed and otherwise observes + chips in; "monitor" only records.
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

## 3. Features and current state (v0.0.5)

| Area | State |
|---|---|
| Audio path | ES8311 / I2S full duplex, AEC + NS + AGC via ESP-SR AFE, energy VAD |
| Speech recognition | Cloud ASR (SiliconFlow SenseVoice, Chinese with punctuation) |
| Language model | DeepSeek chat with OpenAI-compatible function calling, 12+ device tools |
| Speech synthesis | Streaming cloud TTS (CosyVoice2, raw 16 kHz PCM) with ESP-SR offline fallback |
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
turn, internal SRAM is the scarce resource (one TLS session at a time), BLE is not
enabled, and no camera is attached yet.

---

## 4. Repository layout

| Path | Contents |
|---|---|
| `voice_logger/` | **main firmware** (ESP-IDF project): audio, AEC/VAD, ASR, DeepSeek, TTS, voiceprint, tools, storage, Wi-Fi |
| `voice_logger/main/` | all application sources |
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
- Windows PowerShell (the build script is PowerShell) and the IDF Python venv when
  flashing.
- The board on a USB serial port (default `COM5`; override with `-Port`).

### 1) Provide your own secrets (never committed)

```powershell
cd <repo>/config
Set-Content deepseek.key 'sk-your-deepseek-key'        # required: LLM
Set-Content siliconflow.key 'sk-your-siliconflow-key'  # ASR + cloud TTS
Copy-Item wifi_credentials.defaults.template wifi_credentials.defaults
notepad wifi_credentials.defaults                      # SSID / password
```

The build script turns the `.key` files into `*.defaults` fragments at build time;
those generated files are git-ignored too. See [config/README.md](config/README.md).

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
# from the repository root, in an ESP-IDF-activated shell
powershell -ExecutionPolicy Bypass -File tools/build_voice_logger.ps1 -Flash -Port COM5
```

The script derives the project root from its own location, reads the keys,
regenerates `sdkconfig`, builds the firmware, flashes the app and then writes the TTS
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
```

Additional serial commands (conversation monitor, reminders, AEC calibration,
voiceprint diagnostics) are documented inline in `voice_logger/main/main.c`.

---

## 6. Voiceprint

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

## 7. Version and releases

Current version: **v0.0.5** — see [CHANGELOG.md](CHANGELOG.md) and
[GitHub Releases](https://github.com/Junesgone/ESP32-P4-WIFI6-POE-ETH/releases).
The firmware reports its version in the boot banner (`App version`).

---

## 8. License

Released under the **[Apache License, Version 2.0](LICENSE)** — Copyright 2026 Junesgone.
You may use, modify and redistribute the code, including commercially, provided the
license and NOTICE are kept. See [NOTICE](NOTICE) for third-party attribution.

The ESP-Hosted slave example (`c6_slave/`) and the board bring-up projects
(`host_ota/`, `sd_test/`, `sd_probe/`, `sd_wifi_coexist/`, `speaker_test/`,
`wifi_scan/`) are Espressif example code and retain their original Espressif
copyright and Apache-2.0 notices. Runtime services (DeepSeek, SiliconFlow) are
governed by their own terms of service.

---

## 9. Acknowledgements

Built on [ESP-IDF](https://github.com/espressif/esp-idf), ESP-SR, esp-dl and ESP-Hosted
from Espressif; Chinese ASR/TTS by [SiliconFlow](https://siliconflow.cn); LLM and web
search by [DeepSeek](https://www.deepseek.com). The AEC approach was inspired by
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). "小新" is a personal project for
one living room, not a product.
