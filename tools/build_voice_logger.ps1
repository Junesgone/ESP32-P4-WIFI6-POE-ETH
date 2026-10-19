<#
  Build (and optionally flash) the voice_logger firmware.

  Keys, one per line, kept out of the source tree:
      config\deepseek.key       (required)
      config\siliconflow.key    (speech -> text; optional)

  With -Flash this also writes the ESP-SR Chinese TTS voice set into the
  voice_data partition (offset 0x810000, see voice_logger/partitions.csv).
  The app does not contain the voice audio; it mmaps that partition.
#>
param(
    [switch]$Flash,
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"
# Project root is the parent of this script's folder, so the repository can be
# cloned anywhere (no absolute paths baked in).
$root = Split-Path -Parent $PSScriptRoot
# Voice set flashed into the voice_data partition.  This is now only the offline
# fallback: normally 小新 speaks with the SiliconFlow CosyVoice2 neural voice
# (CONFIG_VOICE_TTS_CLOUD).  xiaoxin is the least robotic of the two concatenative
# ESP-SR sets, so it is what you hear when Wi-Fi or the TTS API is unavailable.
$voiceName = "xiaoxin"
$voiceData = Join-Path $root ("assets\esp_tts_voice_data_" + $voiceName + ".dat")
# Must match the voice_data offset in voice_logger/partitions.csv.
$voiceOffset = "0x810000"

function Read-Key([string[]]$names) {
    foreach ($n in $names) {
        $f = Join-Path $root ("config\" + $n + ".key")
        if (Test-Path $f) {
            $v = (Get-Content $f -Raw).Trim()
            if ($v) { return $v }
        }
    }
    return ""
}

$dsk = Read-Key @("deepseek")
$asr = Read-Key @("siliconflow", "asr")
if (-not $dsk) { Write-Warning "config\deepseek.key missing - DeepSeek calls will fail." }
if (-not $asr) { Write-Warning "config\siliconflow.key missing - speech will not be transcribed." }
$wifiDefaults = Join-Path $root "config\wifi_credentials.defaults"
if (-not (Test-Path $wifiDefaults)) {
    throw "config\wifi_credentials.defaults missing - copy config\wifi_credentials.defaults.template and fill in your SSID/password."
}

@"
# Generated from config/deepseek.key - do not edit by hand.
CONFIG_DEEPSEEK_API_KEY="$dsk"
CONFIG_DEEPSEEK_MODEL="deepseek-flash"
CONFIG_DEEPSEEK_MAX_TOKENS=400
CONFIG_DEEPSEEK_TEMPERATURE_X100=80
"@ | Set-Content -Encoding ascii (Join-Path $root "config\deepseek.defaults")

@"
# Generated from config/siliconflow.key - do not edit by hand.
CONFIG_ASR_API_KEY="$asr"
CONFIG_ASR_BASE_URL="https://api.siliconflow.cn/v1/audio/transcriptions"
CONFIG_ASR_MODEL="FunAudioLLM/SenseVoiceSmall"
"@ | Set-Content -Encoding ascii (Join-Path $root "config\asr.defaults")

# Cloud neural TTS reuses the SiliconFlow key.  Raw PCM at 16 kHz keeps the
# board-side code trivial (no MP3 decoder, no resampler).
@"
# Generated from config/siliconflow.key - do not edit by hand.
CONFIG_VOICE_TTS_CLOUD=y
CONFIG_TTS_API_KEY="$asr"
CONFIG_TTS_BASE_URL="https://api.siliconflow.cn/v1/audio/speech"
CONFIG_TTS_MODEL="FunAudioLLM/CosyVoice2-0.5B"
CONFIG_TTS_VOICE="FunAudioLLM/CosyVoice2-0.5B:claire"
CONFIG_TTS_SPEED_X100=100
CONFIG_TTS_GAIN_DB=0
CONFIG_TTS_MAX_SECONDS=30
"@ | Set-Content -Encoding ascii (Join-Path $root "config\tts.defaults")

# Adjust this line if your ESP-IDF installation lives elsewhere.
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' *> $null
$env:PYTHONUTF8 = '1'

$c = $root + "\config"
$defaults = "sdkconfig.defaults;" +
            "$c\board_esp32p4_poe_eth_hosted.defaults;" +
            "$c\wifi_credentials.defaults;" +
            "$c\deepseek.defaults;" +
            "$c\asr.defaults;" +
            "$c\tts.defaults"

Push-Location (Join-Path $root "voice_logger")
try {
    # sdkconfig.defaults only applies when sdkconfig is generated.
    Remove-Item sdkconfig -Force -ErrorAction SilentlyContinue
    idf.py -D SDKCONFIG_DEFAULTS="$defaults" build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    if ($Flash) {
        idf.py -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw "flash failed" }
        if (Test-Path $voiceData) {
            Write-Host "writing TTS voice set ($voiceName) to $voiceOffset ..."
            python -m esptool --chip esp32p4 -p $Port -b 460800 write_flash $voiceOffset $voiceData
            if ($LASTEXITCODE -ne 0) { throw "voice data flash failed" }
        } else {
            Write-Warning "voice data missing: $voiceData"
        }
    }
} finally {
    Pop-Location
}