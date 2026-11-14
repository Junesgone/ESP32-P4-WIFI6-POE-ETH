# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

<#
  Build (and optionally flash) the voice_logger firmware.

  One key is enough: config\deepseek.key drives the full DeepSeek suite
  (chat, ASR, TTS, search, fetch, OCR, files).  config\siliconflow.key is
  optional and only feeds the legacy fallback backend.

  With -Flash this also writes the ESP-SR Chinese TTS voice set into the
  voice_data partition (offset 0x810000, see voice_logger/partitions.csv).
  The app does not contain the voice audio; it mmaps that partition.

  Linux / macOS users: tools/build_voice_logger.sh does the same job.
#>
param(
    [switch]$Flash,
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"
# Project root is the parent of this script's folder, so the repository can be
# cloned anywhere (no absolute paths baked in).
$root = Split-Path -Parent $PSScriptRoot
# Voice set flashed into the voice_data partition.  This is only the offline
# fallback: normally 小新 speaks with the DeepSeek neural voice
# (CONFIG_VOICE_TTS_CLOUD, WebSocket streaming).  xiaoxin is the least robotic
# of the two concatenative ESP-SR sets, so it is what you hear when Wi-Fi or the
# TTS API is unavailable.
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
if (-not $dsk) { Write-Warning "config\deepseek.key missing - chat, ASR, TTS and the tool APIs will fail." }
$wifiDefaults = Join-Path $root "config\wifi_credentials.defaults"
if (-not (Test-Path $wifiDefaults)) {
    throw "config\wifi_credentials.defaults missing - copy config\wifi_credentials.defaults.template and fill in your SSID/password."
}

# Generated file, git-ignored (see .gitignore).  ASR and TTS reuse this key.
@"
# Generated from config/deepseek.key - do not edit by hand.
CONFIG_DEEPSEEK_API_KEY="$dsk"
CONFIG_DEEPSEEK_MODEL="deepseek-flash"
CONFIG_DEEPSEEK_MAX_TOKENS=900
CONFIG_DEEPSEEK_TEMPERATURE_X100=80
"@ | Set-Content -Encoding ascii (Join-Path $root "config\deepseek.defaults")

# Adjust this line if your ESP-IDF installation lives elsewhere.
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' *> $null
$env:PYTHONUTF8 = '1'

$c = $root + "\config"
$defaults = "sdkconfig.defaults;" +
            "$c\board_esp32p4_poe_eth_hosted.defaults;" +
            "$c\wifi_credentials.defaults;" +
            "$c\deepseek.defaults"

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
