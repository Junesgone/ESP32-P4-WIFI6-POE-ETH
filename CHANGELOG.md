# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/), and this
project adheres to [Semantic Versioning](https://semver.org/).

## v0.0.1 — 2026-10-19

Initial public release of the ESP32-P4-WIFI6-POE-ETH voice assistant ("小新").

### Added

- Full-duplex audio path: ES8311 codec + I2S, ESP-SR AFE (AEC / NS / AGC), energy VAD.
- Cloud speech recognition (SiliconFlow SenseVoice) and DeepSeek chat with function calling.
- Streaming cloud text-to-speech (CosyVoice2, raw 16 kHz PCM) with ESP-SR offline fallback.
- On-device speaker recognition: 43-dim MFCC + pitch embedding from the pre-AFE
  microphone signal, LDA-weighted, with 3-utterance averaged enrolment and anonymous
  role clustering (pending / merge / split state machine).
- Conversation modes: assistant, hybrid, monitor; optional post-reply fact-checking
  supervisor with web search.
- Device tools: volume, TTS speed, time, reminders, Wi-Fi scan/connect, voiceprint
  management, recent conversations, system status, reboot.
- WAV + JSONL conversation logging on the TF card with day folders and SNTP timestamps.
- Build/flash PowerShell script with per-board sdkconfig fragments and TTS voice-data
  partition flashing.
- Apache License 2.0, NOTICE, SPDX headers and English README.

### Fixed / hardened

- Correct AEC reference interleaving (microphone + playback reference pairing).
- Bounded cloud ASR timeouts with fast-failure-only retries and capture-queue drop
  accounting, so a bad network cannot stall the utterance pipeline.
- Supervisor gating that never talks over a reply and never repeats the last answer.
- Firmware version reporting (`App version`) in the boot banner.
