# config/ — build-time configuration fragments (no secrets committed)

Both build scripts (`tools/build_voice_logger.ps1` on Windows,
`tools/build_voice_logger.sh` on Linux/macOS) regenerate `sdkconfig` from the
versioned fragments in this folder, plus one generated fragment derived from your
local key file.

## What you must create locally

| File | Required | Contents |
|---|---|---|
| `deepseek.key` | **yes** | DeepSeek API key, one line. Drives chat (with deep thinking, web search, URL reading, safety, titles), ASR, TTS, Search, Fetch, OCR and the Files API. |
| `wifi_credentials.defaults` | **yes** | copy from `wifi_credentials.defaults.template` and fill SSID/password |
| `siliconflow.key` | optional | only for the legacy fallback ASR/TTS backend (menuconfig: `ASR_BASE_URL`, `TTS_API_KEY`) |

The build script writes `deepseek.defaults` from `deepseek.key` on every run.
**Every `.key` file and every generated `*.defaults` is git-ignored** — never
commit them and never paste real keys into the templates.

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
Copy-Item wifi_credentials.defaults.template wifi_credentials.defaults
notepad wifi_credentials.defaults
```

```bash
cd config
printf '%s' 'sk-...' > deepseek.key
cp wifi_credentials.defaults.template wifi_credentials.defaults
${EDITOR:-nano} wifi_credentials.defaults
```

The DeepSeek full-suite features (thinking, `deepseek_web_search`,
`deepseek_read_url`, `safety_check`, conversation titles, TTS WebSocket) and all
their endpoint URLs are Kconfig options under
`menuconfig → Voice logger configuration`; the defaults are sane and the serial
console can flip them at runtime.
