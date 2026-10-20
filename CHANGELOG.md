# Changelog

## Unreleased

- **Waveshare AMOLED 2.16**: the screen no longer times out while a long TTS
  answer is still playing (the idle check now also waits for the announcement
  and the queued audio, not only the voice-assistant run).
- **sh_radio**: a radio that was paused or stopped (on-screen pause button, HA,
  media player entity) is no longer restarted after a wake-word interaction;
  only a station that was actually playing is resumed.

## v1.0.0 — 2026-10-19

First public release.

- **Waveshare ESP32-S3-Touch-AMOLED-2.16** device package:
  - duplex I2S (ES7210 TDM 4-slot + ES8311), hardware AEC reference (MIC3),
    ESP-SR AFE `"MR"` with NSNet2 models from a 2 MB `model` partition
  - `fd` echo canceller with aggressive NLP, energy barge-in (2.5 s guard)
  - tuned defaults that persist across reboots (AEC gain 8.0, NLP aggressive)
  - LVGL UI with clock/weather/apps/settings, internet-radio app, screenshot
    endpoint on `:8080`
  - AXP2101 PMU: charger defaults, `Charging Enabled` switch, 10 % safety floor
  - optional `advanced-audio.yaml` package exposing 30 tuning/diagnostic entities
  - `partitions.csv` + `srmodels.bin` + one-time USB flash script and guide
- **GL-S10 Bluetooth proxy** device package (LEDs, buttons, active proxy).
- Shared `packages/common` (wifi/api/ota/logger/time/uptime/captive portal) and
  the six custom components the devices need, all installed from this repo.
