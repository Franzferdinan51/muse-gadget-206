/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-AMOLED-2.06: watch-style 410x502 CO5300 AMOLED
 * with FT3168 touch, ES8311 speaker + ES7210 dual mic, AXP2101 PMU,
 * PCF85063 RTC, TF slot. Ships with watch straps and a 3.7V MX1.25 LiPo
 * that fits inside the case.
 *
 * Pinout (Waveshare demo + community ESPHome configs):
 *   Display QSPI: CS=12, SCK=11, D0=4, D1=5, D2=6, D3=7, RST=8, EN=13
 *   Touch I2C:    SDA=15, SCL=14, INT=38, RST=9, ADDR=0x38 (FT3168)
 *   IMU:          QMI8658 @ 0x6B (same I2C bus)
 *   PMIC:         AXP2101 @ 0x34 (same I2C bus)
 *   RTC:          PCF85063 @ 0x51 (same I2C bus)
 *   Buttons:      BOOT=GPIO0, PWR=side key via AXP2101
 *
 * NOTE: FT3168 has no ESP-IDF native driver. This board uses the FT5x06
 * driver (same FocalTech family, same 0x38 address) — hardware-proven on
 * this exact unit by AIWatchOS: the chip answers at 0x38 on this bus
 * alongside AXP2101 0x34, QMI8658 0x6B, PCF85063 0x51 and the codecs at
 * 0x18 (ES8311) / 0x40 (ES7210), and raw + burst reads are clean. Known
 * quirk from the AIWatchOS soak: the production read path intermittently
 * returns ESP_ERR_INVALID_STATE in bursts on this shared bus (periodic
 * co-client suspected) — treat occasional read errors as bus contention,
 * not a dead chip.
 * Audio codec pins (the AUDIO_* defines below) are from the 2.06
 * schematic and are cross-validated by HermesGadget's independent
 * port of this board.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch_ft5x06.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"  /* audio_codec_new_gpio (was audio_codec_gpio.h pre-1.5) */
#include "es8311_codec.h"
#include "es7210_adc.h"  /* ES7210 factory header in esp_codec_dev 1.5.x */
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "muse_pmu.h"

static const char *TAG = "board-206";

/* 2.06" panel: 410x502, CO5300 over QSPI. */
#define LCD_H_RES 410
#define LCD_V_RES 502
#define LCD_CS GPIO_NUM_12
#define LCD_SCK GPIO_NUM_11
#define LCD_D0 GPIO_NUM_4
#define LCD_D1 GPIO_NUM_5
#define LCD_D2 GPIO_NUM_6
#define LCD_D3 GPIO_NUM_7
#define LCD_RST GPIO_NUM_8
#define LCD_EN GPIO_NUM_13

/* Touch: FT3168 on I2C. */
#define TOUCH_SDA GPIO_NUM_15
#define TOUCH_SCL GPIO_NUM_14
#define TOUCH_INT GPIO_NUM_38
#define TOUCH_RST GPIO_NUM_9
#define TOUCH_ADDR 0x38

/* Audio: ES8311 + ES7210. From 2.06 schematic (Waveshare). */
#define AUDIO_MCLK GPIO_NUM_16
#define AUDIO_BCLK GPIO_NUM_41
#define AUDIO_WS GPIO_NUM_45
#define AUDIO_DOUT GPIO_NUM_42   /* to ES8311 (speaker) */
#define AUDIO_DIN GPIO_NUM_40    /* from ES7210 (mics) */
#define AUDIO_PA GPIO_NUM_46     /* power amp enable */

#define DRAW_BUF_LINES 118
#define LCD_CHUNK_BYTES (LCD_H_RES * 8 * 2)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_boot;
static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t init(void)
{
    /* Display enable pin: power the panel. */
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << LCD_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&en), TAG, "display EN gpio");
    gpio_set_level(LCD_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Touch reset: HIGH -> 1ms -> LOW -> 20ms -> HIGH -> 50ms (Waveshare init). */
    gpio_config_t tp_rst = {
        .pin_bit_mask = 1ULL << TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tp_rst), TAG, "touch RST gpio");
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* I2C bus for touch, IMU, PMU, RTC, codecs. */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = TOUCH_SCL,
        .sda_io_num = TOUCH_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "boot button");

    /* PMU: AXP2101 at 0x34. The 2.06" schematic wires DCDC1 (VCC3V3),
     * ALDO1 (A3V3 audio), BLDO1 (1.2V), and BLDO2 (2.8V) — all rails are
     * used, so no trimming (unlike the 1.75C which only needed DCDC1+ALDO1).
     * PWR key goes to the PMU's PWRON pin; polled via muse_pmu_poll_key(). */
    esp_err_t err = muse_pmu_init(s_i2c_bus, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMU unavailable (%s): battery status disabled", esp_err_to_name(err));
    }
    return ESP_OK;
}

/* The CO5300 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/* CO5300 init sequence for 410x502 panel.
 * Adapted from Waveshare's esp32_s3_touch_amoled_1_75c BSP v3.0.0
 * (lcd_init_cmds[]). Same CO5300 die, different glass: only the address
 * windows change (0x2A/0x2B). The column window is 22..431, NOT 0..409:
 * hardware-proven on this exact unit by AIWatchOS — the visible glass
 * starts at GRAM column 22, matching the vendor BSP (waveshare/
 * esp32_s3_touch_amoled_2_06 v2.0.0: CASET 22..431, gap (22, 0)). */
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },   /* page select -> page 2 */
    { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },   /* page-2 power/analog trim */
    { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },   /* page-2 register */
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },   /* page select -> page 0 */
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },   /* enable QSPI interface mode */
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },   /* pixel format = 16bpp RGB565 */
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },   /* tearing effect ON */
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },   /* brightness control block ON */
    { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },   /* brightness = max (UI takes over) */
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },   /* CO5300 vendor register */
    { 0x2A, (uint8_t[]){ 0x00, 0x16, 0x01, 0xAF }, 4, 0 },  /* cols 22-431: 410px of glass */
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xF5 }, 4, 600 },/* rows 0-501 (502px) */
    { 0x11, NULL, 0, 600 },                /* sleep out */
    { 0x29, NULL, 0, 0 },                  /* display on */
};

static esp_lcd_panel_handle_t s_panel;

/* display_start returns lv_display_t*, so ESP_RETURN_ON_ERROR (which
 * returns an esp_err_t) cannot be used inside it; log and bail with
 * NULL instead, matching the lv_display_t* failure paths below. */
#define DISPLAY_CHECK(expr, msg) do { \
        esp_err_t err_ = (expr); \
        if (err_ != ESP_OK) { \
            ESP_LOGE(TAG, "%s: %s", msg, esp_err_to_name(err_)); \
            return NULL; \
        } \
    } while (0)

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* QSPI bus for CO5300. Panel must be powered (LCD_EN) before any traffic. */
    const spi_bus_config_t bus_cfg =
        CO5300_PANEL_BUS_QSPI_CONFIG(LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3,
                                     LCD_CHUNK_BYTES);
    DISPLAY_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO), "QSPI bus");
    const esp_lcd_panel_io_spi_config_t io_cfg =
        CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    DISPLAY_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io), "panel IO");

    co5300_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    DISPLAY_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), "CO5300 panel");
    /* Gap stays (0, 0) through init so the boot clear below addresses
     * GRAM coordinates directly; the window moves onto the glass with
     * set_gap(22, 0) right after (AIWatchOS-proven on this unit). */
    DISPLAY_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0), "set gap");
    DISPLAY_CHECK(esp_lcd_panel_reset(s_panel), "panel reset");
    DISPLAY_CHECK(esp_lcd_panel_init(s_panel), "panel init");
    DISPLAY_CHECK(esp_lcd_panel_disp_on_off(s_panel, true), "display on");

    /* Boot clear of the full vendor GRAM span (cols 0..431 x rows
     * 0..501) to black, mirroring AIWatchOS's display_driver: the panel
     * wakes with stale GRAM content (a solid green field) that must not
     * survive anywhere, the columns outside the visible window
     * included. Banded through a small DMA buffer; the panel IO above
     * registered no transfer-done callback, so draw_bitmap completes
     * synchronously here. A failed clear is cosmetic only (LVGL's
     * first flush repaints the visible window) — log, don't fail. */
    {
        enum { kGramCols = 432, kClearRows = 12 };
        uint8_t *clear_buf = heap_caps_calloc(1, kGramCols * kClearRows * 2,
                                              MALLOC_CAP_DMA);
        if (clear_buf) {
            for (int y = 0; y < LCD_V_RES; y += kClearRows) {
                int y_end = y + kClearRows;
                if (y_end > LCD_V_RES) {
                    y_end = LCD_V_RES;
                }
                esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y,
                                                          kGramCols, y_end,
                                                          clear_buf);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "GRAM clear band y=%d: %s", y,
                             esp_err_to_name(err));
                    break;
                }
            }
            free(clear_buf);
        } else {
            ESP_LOGW(TAG, "GRAM clear skipped: no DMA memory");
        }
    }
    /* Every flush from here on lands at CASET 22..431 — on the glass. */
    DISPLAY_CHECK(esp_lcd_panel_set_gap(s_panel, 22, 0), "set gap 22");

    /* LVGL via Meta's adapter + banded flush (fixed internal DMA buffers). */
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    DISPLAY_CHECK(esp_lv_adapter_init(&adapter_cfg), "LVGL adapter");
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        ESP_LOGE(TAG, "display register failed");
        return NULL;
    }
    /* CO5300 needs even-aligned update windows. */
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    if (esp_lv_adapter_start() != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed");
        return NULL;
    }
    /* Touch: FT3168 via esp_lcd_touch_ft5x06 (register-compatible).
     * FT3168 returns chip ID 0x03; max 2 fingers. */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.dev_addr = TOUCH_ADDR;
    tp_io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io;
    DISPLAY_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), "touch IO");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST,
        .int_gpio_num = TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    DISPLAY_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_tp), "FT3168 touch");
    const esp_lv_adapter_touch_config_t lv_tp_cfg =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch) {
        ESP_LOGE(TAG, "touch register failed");
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return true;  /* stub until display_start is complete */
}

static void set_brightness(int pct)
{
    /* CO5300 "write display brightness" (0x51). */
    /* CO5300 "write display brightness" (0x51), QSPI-framed. */
    uint8_t level = (uint8_t)(pct * 255 / 100);
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), &level, 1);
}

static void panel_sleep(bool sleep)
{
    /* Plain SLPIN (0x10) / SLPOUT (0x11). */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((sleep ? 0x10 : 0x11) << 8), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static unsigned poll_buttons(void)
{
    /* BOOT talks; PWR via PMU is aux. */
    unsigned ev = muse_gpio_button_poll(&s_boot);
    unsigned key = muse_pmu_poll_key();
    ev |= (key & MUSE_PMU_KEY_PRESS ? MUSE_BTN_AUX_PRESS : 0) |
          (key & MUSE_PMU_KEY_RELEASE ? MUSE_BTN_AUX_RELEASE : 0);
    return ev;
}

/* Audio: ES8311 speaker + ES7210 dual mics via I2S.
 * Pins from the 2.06 schematic: MCLK=16, BCLK=41, WS=45, DOUT=42, DIN=40.
 * PA_CTRL (GPIO46) enables the speaker amp. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    /* Enable the power amp. */
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << AUDIO_PA,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "PA enable gpio");
    gpio_set_level(AUDIO_PA, 1);

    /* I2S standard mode: ES8311 (TX) + ES7210 (RX) share BCLK/WS/MCLK. */
    i2s_chan_handle_t tx_chan, rx_chan;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan), TAG, "I2S channels");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_WS,
            .dout = AUDIO_DOUT,
            .din = AUDIO_DIN,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &std_cfg), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_chan, &std_cfg), TAG, "I2S RX");

    /* Codec interfaces over the shared buses, esp_codec_dev 1.5.x API.
     * (The first draft of this function used another API generation's
     * factory names, config fields and a two-argument esp_codec_dev_new
     * referencing undefined variables, plus an ES7210 address of 0x10;
     * none of it could ever have compiled or run. Addresses below are
     * the parts' 7-bit defaults.) The devices are returned created but
     * unopened; muse_audio.c opens them with its sample info. */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_chan,
        .tx_handle = tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!gpio_if || !data_if) {
        ESP_LOGE(TAG, "codec interface init failed");
        return ESP_FAIL;
    }

    /* ES8311 speaker codec on the shared I2C bus. */
    audio_codec_i2c_cfg_t es8311_i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR >> 1,
        .bus_handle = s_i2c_bus,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&es8311_i2c_cfg),
        .gpio_if = gpio_if,
        .pa_pin = AUDIO_PA,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .use_mclk = true,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) {
        ESP_LOGE(TAG, "ES8311 init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t spk_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    *spk = esp_codec_dev_new(&spk_cfg);
    if (!*spk) {
        ESP_LOGE(TAG, "speaker dev failed");
        return ESP_FAIL;
    }

    /* ES7210 dual-mic ADC on the shared I2C bus. Mic bias/gain are not
     * config fields in 1.5.x; gain is applied via esp_codec_dev (see
     * set_mic_gain) and the driver default bias is used. */
    audio_codec_i2c_cfg_t es7210_i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR >> 1,
        .bus_handle = s_i2c_bus,
    };
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&es7210_i2c_cfg),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *es7210_if = es7210_codec_new(&es7210_cfg);
    if (!es7210_if) {
        ESP_LOGE(TAG, "ES7210 init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t mic_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_if,
        .data_if = data_if,
    };
    *mic = esp_codec_dev_new(&mic_cfg);
    if (!*mic) {
        ESP_LOGE(TAG, "mic dev failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, (float)db);
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-2.06",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 2.06f,
    .talk_button = "boot",
    .aux_button = "pwr",
    .talk_hint = { LV_ALIGN_CENTER, 0, 180 },
    .aux_hint = { LV_ALIGN_CENTER, 0, -180 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = NULL,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = NULL,
    .audio_init = audio_init,
    .mic_slot = -1,
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = NULL,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
