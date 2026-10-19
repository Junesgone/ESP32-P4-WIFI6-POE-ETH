# assets/ — binary assets (not committed)

The large binary assets are intentionally **not** in this repository.

## ESP-SR Chinese TTS voice set

The offline fallback voice is a ~3.8 MB `.dat` blob that is memory-mapped from the
`voice_data` flash partition. Put one of the ESP-SR Chinese voice sets here as:

```
assets/esp_tts_voice_data_xiaoxin.dat
```

Where to get it: the ESP-SR / esp-skainet distribution (ESP-SR Chinese TTS voice sets
such as `xiaole`, `xiaoxin`). The build script flashes it to offset 0x810000 of the
`voice_data` partition and warns if the file is missing.

The firmware still boots without it, but speech output (cloud and local TTS both go
through the same TTS engine object) will be unavailable.
