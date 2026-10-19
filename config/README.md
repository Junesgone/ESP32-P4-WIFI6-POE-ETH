# config/ — build-time configuration fragments (no secrets committed)

The build script (`tools/build_voice_logger.ps1`) regenerates `sdkconfig` from the
fragments in this folder, then appends the key-derived fragments it writes itself.

## What you must create locally

| File | Required | Contents |
|---|---|---|
| `deepseek.key` | yes (LLM) | DeepSeek API key, one line |
| `siliconflow.key` | recommended (ASR + cloud TTS) | SiliconFlow API key, one line |
| `wifi_credentials.defaults` | yes | copy from `wifi_credentials.defaults.template` and fill SSID/password |

The build script writes `deepseek.defaults`, `asr.defaults` and `tts.defaults` from the
`.key` files every time it runs. **All of these files are git-ignored** — never commit
them and never paste real keys into the templates.

## Versioned fragments (safe to commit)

| File | Purpose |
|---|---|
| `board_esp32p4_poe_eth.defaults` | flash/PSRAM/cache/bootloader settings for the board |
| `board_esp32p4_poe_eth_audio.defaults` | I2S / ES8311 audio path |
| `board_esp32p4_poe_eth_hosted.defaults` | ESP-Hosted (ESP32-C6) transport |
| `host_ota.defaults` | partition-OTA helper for the C6 firmware |
| `deepseek.defaults.template` | example of the generated DeepSeek fragment (placeholder key) |
| `wifi_credentials.defaults.template` | copy this to `wifi_credentials.defaults` |

## Example

```powershell
cd config
Set-Content deepseek.key 'sk-...'
Set-Content siliconflow.key 'sk-...'
Copy-Item wifi_credentials.defaults.template wifi_credentials.defaults
notepad wifi_credentials.defaults
```
