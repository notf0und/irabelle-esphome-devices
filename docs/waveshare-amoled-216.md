# Waveshare ESP32-S3-Touch-AMOLED-2.16

A full-duplex voice satellite for Home Assistant Assist:

- **Microphones**: dual MEMS mics via the ES7210 ADC (TDM 4-slot). The board
  also routes the speaker output back into the ADC (MIC3) as a **hardware AEC
  reference**.
- **Speaker**: ES8311 DAC + 3 W amplifier. Both directions share one I2S port,
  which lets the wake word keep listening while the answer plays.
- **Audio front-end**: ESP-SR AFE fed `"MR"` (microphone + hardware reference),
  NSNet2 noise suppression loaded from a dedicated flash partition, `fd` echo
  canceller with aggressive NLP, energy barge-in so you can talk over an answer.
- **Display**: 2.16" 480×480 AMOLED with a custom LVGL UI (clock, weather, apps,
  settings, internet radio, volume overlay).
- **Power**: AXP2101 PMU with charger defaults that match the vendor firmware, a
  `Charging Enabled` switch, and a firmware safety floor at 10 % battery.

Entities you get in Home Assistant (beyond the standard diagnostics):

| Entity | Purpose |
|--------|---------|
| `media_player.*_media_player` | the speaker (TTS, music, radio) |
| `assist_satellite.*_assist_satellite` | the voice assistant satellite |
| `switch.*_wake_word` / `binary_sensor.*_wake_word_listening` | wake word on/off and "is it listening" |
| `select.*_wake_word` | wake word model/phrase selection |
| `number.*_mic_analog_gain` | ES7210 analog PGA (input level) |
| `light.*_display_brightness` | backlight |
| `sensor.*_battery_level` / `_battery_voltage` / `switch.*_charging_enabled` | battery and charging |
| `button.*` | restart, shutdown, power off, calibrate IMU, factory reset |

The LVGL framebuffer is also served as a JPEG on `http://<device-ip>:8080/screenshot`.

## Install

1. Add the [required secrets](../../README.md#1-add-the-secrets-esphome-needs).
2. Create the device config from
   [`examples/waveshare-amoled-216.yaml`](../examples/waveshare-amoled-216.yaml),
   set `name`, `friendly_name` and `friendly_name_slug`, then **Install** once
   over USB (see below).
3. In Home Assistant: **Settings → Devices & Services → ESPHome** discovers it —
   add it with `api_key` from your `secrets.yaml`.
4. Assign it to a voice assistant: **Settings → Voice assistants** → pick your
   pipeline for the new satellite (the wake word runs on the device).

## First flash (once per board)

This device is **not** a stock ESPHome target: it needs a custom partition table
and a 2 MB `model` partition for the ESP-SR models. That cannot be done over
OTA, so every board needs one USB flash. After it, everything is OTA.

Why:

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| `app0` | `0x10000` | 6912K | firmware, slot A |
| `app1` | `0x6D0000` | 6912K | firmware, slot B (OTA) |
| `model` | `0xD90000` | 2M | `srmodels.bin` (NSNet2 noise suppression), mmap'd by ESP-SR |
| `nvs` | `0xF90000` | 448K | WiFi credentials + restored settings (unchanged offset) |

The partition table itself ships in this repository
(`devices/waveshare-amoled-216/partitions.csv`) and the device package points
ESPHome at it, so you do not have to copy it — see the two flashes below.

### 1. Flash the factory image over USB

Either:

- **ESPHome dashboard (browser Web Serial)**: plug the board into the computer
  running the browser, then **Install → Connect the board to a USB port of your
  computer**, pick the serial port and wait. The dashboard builds and writes the
  factory image (bootloader + partition table + app).
- **Manual**: **Install → Manual download**, pick the **Factory** format, then
  flash the `.bin` with [ESPHome Web](https://web.esphome.io/) or:

  ```bash
  esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
    write-flash 0x0 firmware.factory.bin
  ```

At this point the device boots, joins WiFi and can be added to Home Assistant —
but the `model` partition only contains a 4-byte placeholder, so noise
suppression is off. Do step 2.

### 2. Write the ESP-SR model blob

No UI can write a data partition, so this is one `esptool` command with the blob
from this repository:

```bash
# download the blob (626 KB) from
#   https://raw.githubusercontent.com/notf0und/irabelle-esphome-devices/main/devices/waveshare-amoled-216/srmodels.bin
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write-flash 0xD90000 srmodels.bin
```

> ⚠️ Always write the blob **after** the factory image. The factory image is
> `0xD90004` bytes, i.e. it ends 4 bytes inside the `model` partition; flashing
> it after the blob would overwrite the blob header.

> If you run ESPHome in Docker/compose on your own host, the bundled script does
> both steps (and reads the image list from the build): see
> [`devices/waveshare-amoled-216/scripts/flash-usb.sh`](../devices/waveshare-amoled-216/scripts/flash-usb.sh)
> (`--build-root /config/.esphome/build --models …/srmodels.bin`).

### 3. Verify

```bash
esphome logs your-device.yaml --device <ip>
```

- `ES7210 for AEC: 0x01 0x..->0x.. (ADC clocks on …) — slot order MIC1, MIC3, MIC2, MIC4`
  → the echo reference works.
- `AFE ready: kind=fd, … ref=hw, agc=off, ns=on, se=off, models=yes` → tuned
  defaults applied.
- `AFE: loaded esp-sr models from the 'model' partition (2 models, 2048 KiB)`
  → step 2 worked.

If you instead see `AFE: 'model' partition present but empty/invalid`, repeat
step 2.

## Updates

OTA only: reinstall from the dashboard, or `esphome upload <file>.yaml`. OTA
never touches `model` or `nvs`, so the models and your WiFi credentials survive.
Only redo the USB flash if the partition table or the model version changes
(that is called out in the release notes when it happens).

## Tuning the audio front-end

The device ships with the tuned defaults (AEC gain 8.0, NLP aggressive, `fd`
canceller, hardware reference, barge-in threshold 200 with a 2.5 s guard).
Everything is readable/writable from Home Assistant if you include the extra
package — add this line under `packages:` in your config:

```yaml
  advanced_audio: github://notf0und/irabelle-esphome-devices/devices/waveshare-amoled-216/advanced-audio.yaml@main
```

That exposes 30 entities:

- `AEC Gain`, `AEC NLP Level`, `AEC Mode`, `AEC Filter Length`, `AEC Ref Source`,
  `AFE Type`, `AFE NS`, `AFE AGC`, `Barge-in Level`, `Chunk Interval`
- diagnostics: `AEC In/Out RMS`, `AEC Suppress dB`, `AEC Ref Fill`,
  `Mic Ch0..3 RMS`, `Mic RX Frames/Empty`, `Mic TDM Mode`, `AFE Models`,
  `AFE Type Active`, `Audio Frontend Active`
- `AEC Test Tone` button (4 s 1 kHz straight into the I2S output) — the fastest
  way to check the echo path.

`AFE Type = vc` is refused on purpose: it crashes ESP-SR on this hardware.

## Optional extras

### Battery band automation

This firmware prevents charging from being disabled at or below 10 % (hard
floor, works even when Home Assistant is offline). For day-to-day battery
longevity, keep the pack in a 65–75 % band with an automation: switch
`switch.<device>_charging_enabled` **on** below 65 %, **off** above 75 %.

### Microphone streaming

`switch.*_stream_audio` pushes raw 16 kHz mono PCM to `stream_ip:stream_port`
(default `127.0.0.1:5005`, both editable as `text` entities in HA). Useful for
debugging/voice tooling; leave the switch off otherwise.

### Internet radio

The radios app plays stream URLs on the device itself. Two gotchas:

- The speaker pipeline only plays formats ESPHome can *identify* (MP3, FLAC,
  WAV; Ogg/Opus only with an `.opus` URL or `codecs: opus`). A wrong format
  fails with `Media reader encountered an error` while the media player still
  says `playing`.
- Playing a URL that HA must transcode (AAC/AAC+/HLS) goes through Home
  Assistant (`homeassistant.action`), which requires the ESPHome integration
  option **“Allow the device to make Home Assistant service calls”** for this
  device (off by default). The radio app uses that for some stations.

### Screenshot

`curl -o screen.jpg http://<device-ip>:8080/screenshot`. Only meaningful right
after a full-screen refresh (LVGL renders partial frames); the UI has a
`Radio Test Redraw` button that forces one when you need to capture.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| Build fails on `partitions.csv` | ESPHome did not find the file. It ships in this repo; if you cloned the config by hand instead of using the `github://` package, copy `devices/waveshare-amoled-216/partitions.csv` next to your YAML, or set `partitions: /config/partitions.csv` in your `esp32:` block after copying it to `/config`. |
| `AFE: 'model' partition present but empty/invalid` | The model blob is missing (or the factory image was flashed last). Write `srmodels.bin` at `0xD90000`. |
| `AFE: no esp-sr models (no 'model' partition, no embedded blob)` | The board still has the default ESPHome partition table: the USB factory flash did not happen. |
| Wake word dead, display shows a red line | The wake word is muted (long press of PWRON toggles it). Check the `Wake Word` switch in HA, or press the power button briefly. |
| Audio sounds slow / chipmunk after switching radio stations | Fixed by the bundled `resampler` component; make sure `external_components` was not stripped from the package. |
| Device answers the API and then drops | Too many `esphome logs` clients against the board (24-socket pool). Keep one. |
| `AFE_TYPE_VC` keeps reverting to `fd` | Intentional: `vc` panics ESP-SR here. |

## How the pieces fit

```
devices/waveshare-amoled-216/
├── waveshare-amoled-216.yaml   device package (this is the file you include)
├── advanced-audio.yaml         optional tuning entities + diagnostics
├── apps/                       LVGL apps (radio, settings pages)
├── sounds/                     wake word / boot / timer prompts
├── partitions.csv              6-partition table (2 MB model partition)
├── srmodels.bin                ESP-SR models (NSNet2 + VADNet), 626 KB
└── scripts/                    flash-usb.sh, pack-models.sh, pack_model.py
```

The heavy lifting lives in [`components/`](../components):

- `waveshare_duplex_audio` — single I2S port used in both directions (TDM
  4-slot), ES7210 TDM setup including the ADC3/ADC4 clock un-gating that makes
  the AEC reference audible, ESP-SR AFE integration, energy barge-in.
- `axp2101` — PMU: charger defaults, `Charging Enabled` switch, 10 % floor.
- `lvgl_screenshot` — JPEG snapshot of the LVGL draw buffer over HTTP.
- `sh_radio`, `resampler`, `stream_microphone` — radio directory, patched
  resampler for stream rate changes, raw mic streaming.

`packages/` holds the shared base (`wifi`, `api`, `ota`, `time`, `logger`, …)
and the AMOLED hardware definition with its LVGL pages.
