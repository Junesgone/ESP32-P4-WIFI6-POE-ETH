# ESP32-P4-WIFI6-POE-ETH — 小新 (Xiaoxin) Chinese Voice Assistant

[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

A self-contained **Chinese voice assistant** running on the
**Waveshare ESP32-P4-WIFI6-POE-ETH** development board. The board listens through its
on-board microphone, transcribes with a cloud ASR, answers with a cloud LLM (DeepSeek,
function calling), speaks through its ES8311 + NS4150B audio path, and records every
conversation (audio + text) to a TF card. No wake word, no phone, no PC in the loop —
the board **is** the assistant.

This is an engineering playground for squeezing a real conversation loop out of an
ESP32-P4: dual-core 400 MHz RISC-V, 32 MB flash, 32 MB PSRAM, an on-board ESP32-C6
Wi-Fi 6 coprocessor over ESP-Hosted/SDIO, and an **on-device voiceprint** that runs
entirely on the MCU.

**Keywords:** ESP32-P4, ESP32P4, ESP32-P4-WIFI6-POE-ETH, Waveshare ESP32-P4,
ESP-IDF v6.1, RISC-V, 32 MB PSRAM, XIP from PSRAM, Wi-Fi 6, ESP32-C6, ESP-Hosted,
SDIO, ES8311, NS4150B, embedded AI, edge AI, on-device AI, voice assistant,
Chinese voice assistant, 中文语音助手, 小新, wake-word-free, always-listening,
speech-to-text, ASR, TTS, streaming TTS, SenseVoice, CosyVoice2, SiliconFlow,
DeepSeek, LLM, function calling, tool calling, agent, MCP alternative,
speaker recognition, speaker verification, voiceprint, 声纹识别, AEC,
echo cancellation, noise suppression, VAD, voice activity detection, barge-in,
ESP-SR, esp-dl, MIPI-CSI, OV5647, face recognition, smart speaker, DIY smart speaker.

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
  the **pre-AFE microphone signal** and matched against enrolled people or
  automatically created anonymous roles (人1, 人2, …). Enrolment averages 3 utterances.
- **Full logging.** Every turn becomes a 16 kHz WAV plus one JSONL line with speaker id,
  transcript, reply and RMS/duration metadata on the TF card.

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

Board wiring lives in the firmware sources (`voice_logger/main/board_config.h`) and the
board fragments under `config/`. Development journals and private notes are not
published in this repository.

---

## 3. Current status (2026-10-19)

### Working and measured on the board

| Area | Status |
|---|---|
| Audio path (mic → AFE → VAD → speaker) | ✅ ES8311 / I2S full duplex, AEC + NS + AGC, energy VAD tuned to the room |
| Cloud ASR (SiliconFlow SenseVoice) | ✅ 0.5–1.1 s per utterance, Chinese with punctuation |
| LLM (DeepSeek) + function calling | ✅ 12+ tools: volume, TTS speed, time, reminders, Wi-Fi scan/connect, voiceprint list/forget, recent conversations, system status, reboot |
| Cloud TTS (CosyVoice2, streaming PCM) | ✅ first audio ~1.1–1.4 s after synthesis starts; automatic ESP-SR fallback when offline |
| TF-card conversation log | ✅ WAV + JSONL per turn, day folders, wall-clock timestamps via SNTP |
| Proactive fact-check supervisor | ✅ runs after replies only, sees the last answer to avoid repeats, optional `supervisor on/off` |
| Voiceprint (raw-mic, 43-dim) | ✅ 3-speaker / 12-utterance test: **3/3 roles, 100% mapping, 0 cross-speaker false accepts** (old AFE-input feature: 2/3 roles, 0.52–0.71 false accepts) |
| Multi-utterance enrolment | ✅ 3 sentences averaged; `enroll 主人` → matched 0.845, other voices rejected |
| Per-turn memory snapshot | ✅ internal/PSRAM free + largest block + worker stack high-water mark logged every turn |
| Reliability fixes | ✅ ASR timeout 60 s → 20 s, retry only on fast failures, capture-queue drop warnings, AEC reference-interleave bug fixed |

### Known limitations / next steps

- **Anonymous clustering with 6 similar same-gender voices:** 4/6 roles. The overlap is
  in the feature itself (EER ≈ 9.6 % on the current corpus), not in the threshold;
  formal identities should use `enroll`. LDA-style per-dimension weighting is the next
  candidate (offline: EER 9.6 % → 7.5 %).
- **No true barge-in yet:** while the board speaks the microphone is gated because the
  echo canceller could not cover the I2S/speaker delay. The AEC reference-signal bug is
  fixed now; re-measuring the delay and enabling barge-in is the next step.
- **Cloud latency:** a full turn is roughly 4–10 s end-to-end (ASR + LLM + TTS).
- **Internal SRAM is the scarce resource:** ~64 KB free after init, ~25 KB during a TLS
  session; the design keeps only one cloud (mbedTLS) session at a time.
- **BLE is not enabled:** the on-board C6 currently does not answer HCI over the hosted
  transport; the NimBLE host code is present behind `CONFIG_VOICE_BLE`.
- **Camera not connected yet:** MIPI-CSI OV5647 + ESP-DL face recognition is the planned
  next sensor; the identity seam (`face_id`) already exists in the JSONL schema.
- Wi-Fi is 2.4 GHz only (C6 radio).

---

## 4. Repository layout

| Path | Contents |
|---|---|
| `voice_logger/` | **main firmware** (ESP-IDF project): audio, AEC/VAD, ASR, DeepSeek, TTS, voiceprint, tools, storage, Wi-Fi |
| `voice_logger/main/` | all application sources (audio, AFE, ASR, LLM, TTS, voiceprint, tools, storage) |
| `c6_slave/` | ESP32-C6 Wi-Fi/BT coprocessor firmware used by the hosted transport |
| `host_ota/` | partition-OTA helper used to update the C6 over SDIO |
| `speaker_test/` `sd_test/` `sd_probe/` `sd_wifi_coexist/` `wifi_scan/` | bring-up / validation projects for each peripheral |
| `tools/` | build + flash script and serial capture/ask/send helpers |
| `config/` | board and feature sdkconfig fragments (secrets are **not** committed; see `config/README.md`) |
| `config/README.md` | how to supply API keys / Wi-Fi credentials safely |
| `LICENSE` / `NOTICE` | Apache License 2.0 and third-party attribution |

---

## 5. Getting started

### Prerequisites

- ESP-IDF **v6.1** (tested), with the `idf.py` environment activated. The build script
  assumes the standard Espressif tools install; adjust the paths if yours differ.
- Windows PowerShell (the build script is PowerShell) and the IDF Python venv for the
  serial helpers.
- The board on a USB serial port (default `COM5`; override with `-Port`).

### 1) Provide your own secrets (never committed)

```powershell
cd <repo>/config
Set-Content deepseek.key 'sk-your-deepseek-key'        # required: LLM
Set-Content siliconflow.key 'sk-your-siliconflow-key'  # ASR + cloud TTS
Copy-Item wifi_credentials.defaults.template wifi_credentials.defaults
notepad wifi_credentials.defaults                      # SSID / password
```

The build script turns the `.key` files into `*.defaults` fragments at build time; those
generated files are git-ignored too. See [config/README.md](config/README.md).

### 2) Provide the offline TTS voice set (not committed)

The ESP-SR Chinese TTS voice data (~3.8 MB binary) is **not** in the repository.
Get a Chinese voice set from the ESP-SR / esp-skainet distribution and place
`esp_tts_voice_data_xiaoxin.dat` at:

```
assets/esp_tts_voice_data_xiaoxin.dat
```

The build script flashes it into the `voice_data` partition at 0x810000. Without it the
firmware still boots, but TTS output is unavailable.

### 3) Build + flash

```powershell
# from the repository root, in an ESP-IDF-activated shell
powershell -ExecutionPolicy Bypass -File tools/build_voice_logger.ps1 -Flash -Port COM5
```

The script derives the project root from its own location, reads the keys, regenerates
`sdkconfig`, builds the firmware, flashes the app and then writes the TTS voice set to
the `voice_data` partition. Flashing the voice data only needs to happen once.

After the first boot the board prints `listening...` on the console and is ready.

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

The full command list lives in the console parser in `voice_logger/main/main.c`; the
highlights above are what you need for first use. Voiceprint debug commands:
`vpvec on/off`, `vpmargin 0-100`, `asr on/off`.

---

## 6. Voiceprint: what is actually running

- **Input:** the raw microphone block captured in parallel with the AFE output (ASR keeps
  the echo-cancelled / denoised signal). AGC/NS/AEC reshape the spectrum enough to
  flatten speaker timbre — measured on this board, the old AFE-input feature gave
  cross-speaker cosine 0.52–0.71.
- **Feature:** 20 MFCC means + 20 MFCC standard deviations (from esp-dl's 80-band
  log-mel) + 3 pitch statistics = 43 dims, standardised, block-normalised, mixed 90/10
  and L2-normalised.
- **Calibration:** the standardisation tables in `app_vp_tables.h` were re-fitted on
  **44 board-recorded raw-mic utterances** (6 TTS voices): same speaker 0.643 ± 0.220,
  different speaker −0.142 ± 0.285, d' ≈ 3.1, AUC 0.98, EER ≈ 9.6 %.
- **Enrolment:** `enroll <name>` collects `CONFIG_VOICE_ENROLL_SENTENCES` (default 3)
  utterances, averages their embeddings and freezes the role.
- **Debug:** `vpvec on` prints the unstandardised 43-dim vector per utterance (for
  recalibration), `vpmargin <x100>` overrides the relative top1−top2 margin at runtime,
  `cluster reset` clears auto-created roles.

---

## 7. Repository scope

This repository contains **only the code that runs on the board** (firmware, C6 slave,
bring-up projects, build scripts, config fragments) plus the build/usage instructions
needed to reproduce it. Development journals, maintenance logs, pitfall records and
internal design notes are intentionally kept private, and **no API keys, Wi-Fi
credentials or generated sdkconfig files are published**. See
[config/README.md](config/README.md) for how to supply your own secrets locally.

---

## 8. Security

This repository intentionally ships **no** credentials:

- `config/*.key`, generated `config/*.defaults`, `config/wifi_credentials.defaults`,
  `sdkconfig`, build directories and binaries are excluded by `.gitignore`.
- The build script creates the generated config fragments from local keys at build time;
  only sanitised templates are versioned.
- If you fork/clone and add keys, double-check `git status` before pushing.

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

## 10. Roadmap

1. **Barge-in:** re-measure the I2S/speaker delay now that the AEC reference path is
   fixed; increase `aec_filter_length` accordingly and allow talking over replies.
2. **Voiceprint discrimination:** LDA-style per-dimension weighting (offline evidence:
   EER 9.6 % → 7.5 %), then re-test the 6-voice scenario.
3. **Camera:** MIPI-CSI OV5647 + ESP-DL face detection/recognition, filling `face_id`.
4. **C6 BLE:** instrument the C6 slave HCI path and enable the NimBLE host.
5. **LAN bridges:** optional local ASR/TTS bridges (endpoints are configurable; the
   firmware does not care whether the API is in the cloud or on the LAN).

---

## 11. Acknowledgements

Built on [ESP-IDF](https://github.com/espressif/esp-idf), ESP-SR, esp-dl and ESP-Hosted
from Espressif; Chinese ASR/TTS by [SiliconFlow](https://siliconflow.cn); LLM and web
search by [DeepSeek](https://www.deepseek.com). The AEC approach was inspired by
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). "小新" is a personal project for
one living room, not a product.
