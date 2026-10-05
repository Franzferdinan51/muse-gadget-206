# muse-gadget-206

Muse gadget firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
watch-style development board (ESP32-S3R8, 2.06" 410x502 AMOLED, FT3168 touch).

Based on Meta's official
[muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(`esp32/` tree), with a new board target for the 2.06".

## Board

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8, 240 MHz, 8 MB octal PSRAM, 32 MB flash |
| Display | 2.06" AMOLED, 410x502, CO5300 over QSPI (CS=12, SCK=11, D0-D3=4-7, RST=8, EN=13) |
| Touch | FT3168 capacitive, I2C 0x38 (SDA=8, SCL=9, INT=21) |
| Audio | ES8311 + ES7210, dual mics, speaker (pins pending schematic confirmation) |
| Power | AXP2101 PMIC, 3.7V MX1.25 LiPo header (battery included, fits in case) |
| Extras | PCF85063 RTC, QMI8658 6-axis IMU, TF card slot, watch straps |

## Build

```bash
cd esp32
idf.py build -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse-waveshare-s3-206"
```

Or select **Waveshare ESP32-S3-Touch-AMOLED-2.06** in `idf.py menuconfig`
under Component config -> Muse -> Board.

## Board port status

- [x] Kconfig board option (`MUSE_BOARD_WAVESHARE_S3_206`)
- [x] sdkconfig with Meta's PSRAM/power tuning
- [x] Board driver skeleton (`boards/board_waveshare_s3_206.c`)
- [ ] CO5300 init sequence validation on hardware
- [ ] FT3168 touch driver (no ESP-IDF native driver; trying FT5x06 first)
- [ ] Audio codec pin confirmation against schematic
- [ ] AXP2101 rail trim (verify which rails are wired before trimming)
- [ ] PWR key wiring confirmation

## Protocol

Full Muse gadget protocol: `prompt` / `prompt.close` / `display` / `notice` /
`status` / `reply.delta`, turn lifecycle, OTA updates. The approval flow matches
the Muse Companion phone app: one question at a time, 600 ms answer debounce,
TTL auto-deny, exactly-once replies.
