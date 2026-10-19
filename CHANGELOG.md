# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/), and this
project adheres to [Semantic Versioning](https://semver.org/).

## v0.0.5 — 2026-10-19

Voiceprint accuracy release: discriminative weighting, session-balanced
calibration and safer anonymous-role creation.

### Changed

- Voiceprint dimensions are weighted independently by their Fisher ratio over
  hard-to-separate voice pairs (40 independent weights instead of 20 shared
  mean/std weights), and the standardisation tables were refit on a
  session-balanced set of on-device recordings.
- Accept threshold raised to 0.48 to match the new feature scale (relative
  margin stays at 0.10).
- A new anonymous role can only be seeded by utterances of at least 2.5 s, so
  VAD fragments of one sentence cannot accumulate into a duplicate role; short
  fragments can still match existing roles.
- Firmware version bumped to **0.0.5**.

## v0.0.3 — 2026-10-19

Feature release: discriminative voiceprint embedding and refit calibration.

### Changed

- Voiceprint weights are now per-dimension Fisher-ratio weights computed over
  hard-to-separate voice pairs (40 independent dimensions instead of 20 shared
  mean/std weights), and the standardisation tables were refit on an expanded
  on-device recording set.
- Pitch dimensions are no longer separately weighted; the pitch block keeps its
  0.10 mixing weight.
- Firmware version bumped to **0.0.3**.

## v0.0.2 — 2026-10-19

Maintenance release. No functional changes to the firmware.

### Changed

- Firmware version bumped to **0.0.2** (boot banner and release tag).
- Voiceprint defaults confirmed: accept threshold 0.45, relative margin 0.10.
- Repository clean-up and documentation refresh for the public release.

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