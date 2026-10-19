# GL-S10 Bluetooth proxy

The GL-S10 is a small Tuya-branded ESP32 BLE gateway. With ESPHome it becomes a
Bluetooth proxy for Home Assistant: your HA instance (or any other ESPHome
Bluetooth proxy) can use it to reach BLE devices that are out of range, and it
forwards advertisements for passive sensors.

This package is based on [blakadder's bluetooth-proxies](https://github.com/blakadder/bluetooth-proxies)
(MIT) with the LED/button handling from the stock device.

## What you get

| Entity | Type | Meaning |
|--------|------|---------|
| `light.gl_s10_bt_proxy_power_led` | light | power LED (strobe effects `blink_slow` / `blink_fast`) |
| `light.gl_s10_bt_proxy_bluetooth_led` | light | flickers on every BLE advertisement |
| `light.gl_s10_bt_proxy_network_led` | light | solid while Home Assistant is connected, blinks otherwise |
| `binary_sensor.gl_s10_bt_proxy_reset_button` | diagnostic | the case button |
| `button.gl_s10_bt_proxy_safe_mode_boot` | diagnostic | reboot into safe mode |

Plus the usual ESPHome diagnostics (WiFi, uptime, internal temperature, log
level).

## Install

1. Add the [required secrets](../../README.md#1-add-the-secrets-esphome-needs)
   (`wifi_ssid`, `wifi_password`, `api_key`, `ota_password`, `ap_password`).
2. Create the device config from
   [`examples/gl-s10.yaml`](../examples/gl-s10.yaml) and set `name` /
   `friendly_name`.
3. Flash it over UART (below).
4. In Home Assistant: **Settings → Devices & Services → ESPHome** discovers it;
   add it with `api_key`.
5. Make sure the Bluetooth integration uses it: **Settings → Devices & Services
   → Bluetooth → Configure → Bluetooth adapters**, or leave the default (HA uses
   every proxy automatically once `bluetooth_proxy` is active).

## First flash (UART, once)

The stock firmware does not allow OTA, so the first flash is over the serial
pads. Follow these instructions for opening and wiring the board:

**<https://blakadder.com/gl-s10>**

Short version:

1. Open the case (four clips along the sides).
2. Solder/connect a 3.3 V USB-UART adapter to the labelled pads
   (`3V3`, `GND`, `TX`, `RX`) — **do not power it from 5 V** while the case is
   open without the adapter's 3.3 V regulator.
3. Hold the `GPIO0`/boot pad to ground while powering up (download mode).
4. In the ESPHome dashboard: **Install → Connect the board to a USB port of
   your computer** (Web Serial, Chrome/Edge) or **Manual download** and flash
   with `esptool`:

   ```bash
   esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
     write-flash 0x0 firmware.factory.bin
   ```

5. Remove the boot strap, power-cycle. The network LED starts blinking, joins
   WiFi and goes solid once Home Assistant connects.

After that, every update is OTA from the dashboard — no case opening again.

## Ethernet (v2.3 boards only)

Newer GL-S10 revisions expose an Ethernet PHY (IP101) instead of the second
WiFi-only variant. If your board has the RJ45 jack, uncomment the `ethernet:`
block in [`packages/devices/gl-s10.yaml`](../packages/devices/gl-s10.yaml):

```yaml
ethernet:
  type: IP101
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk_mode: GPIO0_IN
  phy_addr: 1
  power_pin: GPIO5
```

Keep `wifi:` as a fallback; ESPHome picks whichever link is up. For Ethernet
you probably also want `use_address` on your device config so OTA/API use the
Ethernet IP.

## Notes

- **2.4 GHz only**, like every ESP32.
- The board has a 4 MB flash; the default ESPHome partition table is fine —
  there is no custom partition table for this device, so no special first-flash
  steps beyond UART.
- BLE proxy performance: keep `active: True` in `bluetooth_proxy` (the package
  does) so HA can open GATT connections, not just listen to advertisements.
- If you use another Bluetooth proxy nearby, keep them ~10 m apart to avoid
  duplicate/conflicting advertisement reports.
