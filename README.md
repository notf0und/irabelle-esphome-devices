# Irabelle ESPHome devices

Ready-to-install ESPHome configurations for the devices I run at home.
You point ESPHome at this repository and it builds the firmware for you —
no copying of YAML packages, custom components, sounds or model files, and
you pick up my updates by reinstalling.

| Device | Package to include | First flash |
|--------|--------------------|-------------|
| **Waveshare ESP32-S3-Touch-AMOLED-2.16** (voice satellite: mic array, speaker, 2.16" AMOLED UI, internet radio) | `devices/waveshare-amoled-216/waveshare-amoled-216.yaml` | **USB once** — custom partition table + ESP-SR model blob ([guide](docs/waveshare-amoled-216.md)) |
| **GL-S10 Bluetooth proxy** (BLE gateway, 3 LEDs) | `devices/gl-s10/gl-s10-bt-proxy.yaml` | **UART once** — stock Tuya firmware ([guide](docs/gl-s10.md)) |

## Install (90 seconds)

### 1. Add the secrets ESPHome needs

In your ESPHome `secrets.yaml` (same directory as your device config). ESPHome
usually already has `wifi_ssid` / `wifi_password`; add the rest:

```yaml
wifi_ssid: "your-ssid"
wifi_password: "your-password"

# generate with: openssl rand -base64 32
api_key: "REPLACE_WITH_32_BYTES_BASE64"
ota_password: "REPLACE_WITH_SOMETHING_LONG"

# password of the fallback access point the device opens when WiFi fails
ap_password: "REPLACE_WITH_SOMETHING_LONG"
```

`api_key` must be 32 bytes encoded as base64 (`openssl rand -base64 32`).
Both devices read these by name, so the same secrets file works for all of them.

### 2. Create the device config

In the ESPHome dashboard: **+ New device** → *Skip* the wizard → paste this,
change the three substitutions, save.

**Waveshare AMOLED 2.16:**

```yaml
substitutions:
  name: waveshare-amoled-216              # hostname (unique per device)
  friendly_name: Waveshare AMOLED 2.16    # name in Home Assistant
  friendly_name_slug: waveshare_amoled_2_16  # entity_id prefix

packages:
  device: github://notf0und/irabelle-esphome-devices/devices/waveshare-amoled-216/waveshare-amoled-216.yaml@main
```

**GL-S10:**

```yaml
substitutions:
  name: gl-s10-bt-proxy
  friendly_name: GL-S10 Bluetooth Proxy
  friendly_name_slug: gl_s10_bt_proxy

packages:
  device: github://notf0und/irabelle-esphome-devices/devices/gl-s10/gl-s10-bt-proxy.yaml@main
```

The copies in [`examples/`](examples/) also show the optional hardening
(API/OTA encryption is already on by default through the secrets above).

> `@main` tracks this repository's main branch — that is how you get updates.
> If you prefer reproducible builds, pin a tag or commit: `...@v1.0.0`.

### 3. First flash (once per board)

Follow the device guide — the Waveshare needs a one-time USB flash of the
partition table and the ESP-SR models, the GL-S10 needs its case opened and a
UART flash:

- [Waveshare ESP32-S3-Touch-AMOLED-2.16](docs/waveshare-amoled-216.md)
- [GL-S10 Bluetooth proxy](docs/gl-s10.md)

After that, everything is OTA from the ESPHome dashboard.

### 4. Add it to Home Assistant

The device announces itself over mDNS; **Settings → Devices & Services** shows a
discovered ESPHome device. Add it with the `api_key` from `secrets.yaml`.

## How updates work

ESPHome clones this repository when it compiles your device and caches the
checkout under `.esphome/packages/…`. Git sources declare `refresh: 1d`, so the
next build after that picks up whatever changed here (packages, components,
sound files, and the model blob for the Waveshare — the blob itself is only
flashed over USB, see its guide).

- **To update a device:** open it in the dashboard and hit **Install** again.
  Nothing in your own file changes — you keep your substitutions.
- **To pin instead of tracking:** change `@main` to a release tag, e.g.
  `@v1.0.0`.
- **To go back:** pin the tag you came from and reinstall.

I try to keep `main` working at all times; if something is in flux it is called
out in the device guide. Releases are tagged when a device configuration
changes in a way worth noting. The sync/release workflow (and the sanitizing
checklist for anything added here) is in [`docs/maintenance.md`](docs/maintenance.md).

## Repository layout

```
components/                 custom ESPHome components used by the devices
packages/common/            shared wifi / api / ota / logger / time packages
packages/devices/           hardware definitions (Waveshare, GL-S10)
devices/waveshare-amoled-216/   device package + advanced audio + partitions + models + sounds + scripts
devices/gl-s10/             device package
examples/                   copy-paste configs for your ESPHome directory
docs/                       per-device first-flash and HA guides
```

## The devices

### Waveshare ESP32-S3-Touch-AMOLED-2.16

A full-duplex voice satellite built around dual microphones, the ES7210 ADC /
ES8311 DAC pair, a hardware echo-cancellation reference tapped from the speaker
output, and the on-board 2.16" AMOLED running a custom LVGL UI (clock, weather,
apps, settings, internet radio).

Audio pipeline highlights:

- ESP-SR AFE fed `"MR"` (microphone + hardware AEC reference) with NSNet2
  noise suppression served from a dedicated 2 MB `model` flash partition.
- `fd` AEC profile with aggressive NLP; `vc` is refused because it crashes
  ESP-SR on this hardware.
- Energy barge-in on the echo-cancelled output, so you can talk over an answer
  to stop it.
- Post-AEC gain and NLP settings persist across reboots (`AEC Gain` 8.0,
  aggressive NLP by default).

Extra features documented in [its guide](docs/waveshare-amoled-216.md):
internet radio app, microphone streaming to a TCP host, LVGL screenshot
endpoint (`http://<ip>:8080/screenshot`), battery band automation, and the
optional AEC/AFE tuning entity package.

### GL-S10 Bluetooth proxy

Turned a stock GL-S10 (Tuya) BLE gateway into a modern ESPHome Bluetooth proxy:
`bluetooth_proxy` with active connections, `esp32_ble_tracker`, the three front
LEDs mapped as lights (power / bluetooth / network), reset button and safe-mode
button. Ethernet is available on v2.3 boards (commented in the package).

## License

MIT — see [LICENSE](LICENSE). The device configuration is my own work; the
custom components are derived from ESPHome (MIT) and Espressif's ESP-SR, and the
GL-S10 layout follows [blakadder's bluetooth-proxies](https://github.com/blakadder/bluetooth-proxies)
(MIT). The audio prompts in `devices/waveshare-amoled-216/sounds/` come from the
[Home Assistant Voice PE](https://github.com/esphome/home-assistant-voice-pe)
(MIT/Apache-2.0); they are included so the build is reproducible offline.
