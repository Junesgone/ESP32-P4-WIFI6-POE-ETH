# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build (and optionally flash) the 小新 voice_logger firmware.
#
# Linux / macOS sibling of tools/build_voice_logger.ps1.  Secrets stay local:
#   config/deepseek.key            (required, one line, DeepSeek API key)
#   config/wifi_credentials.defaults (required, copied from the .template)
# DeepSeek key also drives ASR and TTS - the full-suite APIs share one key.
#
#   ./tools/build_voice_logger.sh                 build only
#   ./tools/build_voice_logger.sh -f              build + flash app
#   ./tools/build_voice_logger.sh -f -p /dev/ttyACM0 -m   build + flash + monitor
#
# The ESP-SR Chinese TTS voice set lives in the voice_data partition
# (offset 0x810000, see voice_logger/partitions.csv), not in the app image, so
# -f also writes it with esptool.
# ---------------------------------------------------------------------------
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
voice_name="xiaoxin"          # offline fallback voice (CONFIG_VOICE_TTS_CLOUD normally wins)
voice_data="$root/assets/esp_tts_voice_data_${voice_name}.dat"
voice_offset="0x810000"       # must match voice_data in voice_logger/partitions.csv

port="${PORT:-/dev/ttyACM0}"
do_flash=0
do_monitor=0
do_clean=0

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        -f|--flash)   do_flash=1 ;;
        -m|--monitor) do_monitor=1; do_flash=1 ;;
        -p|--port)    port="$2"; shift ;;
        -c|--clean)   do_clean=1 ;;
        -h|--help)    usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
    shift
done

# --- ESP-IDF ---------------------------------------------------------------
if [ -z "${IDF_PATH:-}" ]; then
    for cand in "$root/../v6.1/esp-idf" "$root/../esp-idf" "/opt/esp-idf"; do
        if [ -f "$cand/export.sh" ]; then
            IDF_PATH="$(cd "$cand" && pwd)"
            break
        fi
    done
fi
if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found. Set IDF_PATH to an esp-idf checkout (v6.1)." >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$IDF_PATH/export.sh" >/dev/null 2>&1
export PYTHONUTF8=1

# --- secrets -> generated sdkconfig defaults -------------------------------
key_file="$root/config/deepseek.key"
if [ ! -f "$key_file" ]; then
    echo "config/deepseek.key missing - create it with your DeepSeek API key." >&2
    exit 1
fi
dsk="$(tr -d '\r\n' < "$key_file")"
if [ -z "$dsk" ]; then
    echo "config/deepseek.key is empty." >&2
    exit 1
fi
if [ ! -f "$root/config/wifi_credentials.defaults" ]; then
    echo "config/wifi_credentials.defaults missing - copy the .template and fill it in." >&2
    exit 1
fi

# Generated, never committed (see .gitignore).  ASR/TTS reuse the same key.
cat > "$root/config/deepseek.defaults" <<EOF
# Generated from config/deepseek.key - do not edit by hand.
CONFIG_DEEPSEEK_API_KEY="$dsk"
CONFIG_DEEPSEEK_MODEL="deepseek-flash"
CONFIG_DEEPSEEK_MAX_TOKENS=400
CONFIG_DEEPSEEK_TEMPERATURE_X100=80
EOF

defaults="sdkconfig.defaults"
for f in board_esp32p4_poe_eth_hosted.defaults wifi_credentials.defaults deepseek.defaults; do
    defaults="$defaults;$root/config/$f"
done

# --- build -----------------------------------------------------------------
cd "$root/voice_logger"
# sdkconfig is regenerated on every run (same contract as the PowerShell
# script): the generated defaults carry the API key and the feature switches,
# and SDKCONFIG_DEFAULTS is only consulted when sdkconfig is created.  A stale
# sdkconfig kept a previous vendor's key here once, which silently sent TTS
# requests with the wrong credentials.  For a quick incremental build run
# "idf.py build" directly instead.
rm -f sdkconfig sdkconfig.old

# ESP-IDF's toolchain emits the picolibc spec file path into the gcc command
# line; with a non-ASCII checkout path (e.g. a Chinese home directory) the byte
# is mangled (".../\x0a/esp/.../picolibc.specs: No such file") and every
# translation unit fails.  Keep the build tree on an ASCII path in that case
# while the sources stay where they are.
build_dir="$root/voice_logger/build"
case "$root" in
    *[!\ -~]*)
        build_dir="${TMPDIR:-/tmp}/esp32p4-voice-logger-build"
        echo "note: non-ASCII project path - building in $build_dir"
        ;;
esac
if [ "$do_clean" = 1 ]; then
    rm -rf "$build_dir"
fi
set -- -B "$build_dir" -D "SDKCONFIG_DEFAULTS=$defaults"
idf.py "$@" build

# --- flash -----------------------------------------------------------------
if [ "$do_flash" = 1 ]; then
    idf.py -B "$build_dir" -p "$port" flash
    if [ -f "$voice_data" ]; then
        echo "writing TTS voice set ($voice_name) to $voice_offset ..."
        python -m esptool --chip esp32p4 -p "$port" -b 460800 \
            write_flash "$voice_offset" "$voice_data"
    else
        echo "warning: voice data missing: $voice_data" >&2
    fi
fi

if [ "$do_monitor" = 1 ]; then
    idf.py -B "$build_dir" -p "$port" monitor
fi
