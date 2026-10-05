# muse-gadget-206

Custom Hermes gadget firmware port for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
watch-style development board (ESP32-S3R8, 2.06" 410x502 AMOLED, FT3168 touch).

Forked from [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk),
with a new board target plus feature work driven by the Muse Companion phone app.

## Board target

```
firmware/esp32/boards/esp32s3-touch-amoled-206/
```

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8, 240 MHz, 8 MB octal PSRAM, 32 MB flash |
| Display | 2.06" AMOLED, 410x502, CO5300 over QSPI (CS=12, SCK=11, D0-D3=4-7, RST=8, EN=13) |
| Touch | FT3168 capacitive, I2C 0x38 (SDA=8, SCL=9, INT=21) |
| Audio | ES8311 + ES7210, dual mics, speaker |
| Power | AXP2101 PMIC, 3.7V MX1.25 LiPo header (battery fits in case) |
| Extras | PCF85063 RTC, QMI8658 6-axis IMU, TF card slot |

### FT3168 touch note

ESP-IDF has no native FT3168 driver. This port tries the `esp_lcd_touch_ft5x06`
driver first (same FocalTech family, same 0x38 address). If touch doesn't respond
on hardware, `port_touch.cpp` is the place to drop in a native FT3168 register
driver — the `TouchController::Ft3168` enum and board wiring are already in place.

### Audio pins

Codec pins follow the 1.75C layout pending schematic confirmation for the 2.06".
Verify against Waveshare's demo if audio doesn't come up.

## Build

```bash
cd firmware/esp32
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-touch-amoled-206/sdkconfig.defaults" build
# or via PlatformIO:
pio run -e esp32s3-touch-amoled-206 -t upload -t monitor
```

## Features vs upstream

- Full hermes-gadget-sdk protocol: `prompt` / `prompt.close` / `display` /
  `notice` / `status` / `reply.delta` / turn lifecycle
- Approval flow matches the Muse Companion phone app: one question at a time,
  600 ms answer debounce, 60 s TTL auto-deny, exactly-once replies
- Board tag `HGBOARD=esp32s3-touch-amoled-2.06` so `hermes gadget update`
  refuses images built for other boards
