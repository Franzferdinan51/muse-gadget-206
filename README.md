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
- [x] Board driver (`boards/board_waveshare_s3_206.c`)
- [x] CO5300 init sequence (14 cmds, 410x502 windows, from Waveshare BSP v3.0.0)
- [x] FT3168 touch via `esp_lcd_touch_ft5x06` (register-compatible, 2 fingers)
- [x] Touch pins from Waveshare pin_config.h (SDA=15, SCL=14, INT=38, RST=9)
- [x] Audio: ES8311+ES7210 via I2S (MCLK=16, BCLK=41, WS=45, DOUT=42, DIN=40, PA=46)
- [x] AXP2101: all rails used (DCDC1+ALDO1+BLDO1+BLDO2), no trimming
- [x] PWR key via PMU PWRON (polled with `muse_pmu_poll_key()`)
- [ ] Hardware validation when board arrives (display, touch, audio, power)

## Protocol

Full Muse gadget protocol: `prompt` / `prompt.close` / `display` / `notice` /
`status` / `reply.delta`, turn lifecycle, OTA updates. The approval flow matches
the Muse Companion phone app: one question at a time, 600 ms answer debounce,
TTL auto-deny, exactly-once replies.

## 2.06" Watch Commands

Additional Muse-invokable commands for the watch form factor:

| Command | Params | Description |
|---|---|---|
| `companion.set_status` | `text` (string, ≤4000 chars) | Persistent status caption below the face |
| `pocket.set_status` | `text` (string) | Alias for `companion.set_status` |
| `display.show_card` | `title`, `body`, `ttl_s` | Hermes-style card overlay, auto-dismiss |
| `display.show_animation` | — | Clear image/card back to neutral face |
| `device.health` | — | Battery %, charging, model, firmware, status |
| `companion.set_display` | `brightness` (0-100), `theme`, `keep_screen_on` | Remote display prefs |
| `image.start` | `width`, `height` | Begin streamed image upload |
| `image.data` | `rows` (base64 RGB565), `y` | Stream image rows |
| `image.end` | — | Finish streamed image |
