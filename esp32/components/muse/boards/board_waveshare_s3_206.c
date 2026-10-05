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
 *   Touch I2C:    SDA=8, SCL=9, INT=21, ADDR=0x38 (FT3168)
 *   IMU:          QMI8658 @ 0x6B (same I2C bus)
 *   PMIC:         AXP2101 @ 0x34 (same I2C bus)
 *   RTC:          PCF85063 @ 0x51 (same I2C bus)
 *   Buttons:      BOOT=GPIO0, PWR=side key via AXP2101
 *
 * NOTE: FT3168 has no ESP-IDF native driver. This board tries the FT5x06
 * driver (same FocalTech family, same 0x38 address). Validate touch on
 * hardware; write a native FT3168 driver if the register map differs.
 * Audio codec pins follow the 1.75C layout pending schematic confirmation.
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
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
#define TOUCH_SDA GPIO_NUM_8
#define TOUCH_SCL GPIO_NUM_9
#define TOUCH_INT GPIO_NUM_21
#define TOUCH_ADDR 0x38

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

    /* I2C bus for touch, IMU, PMU, RTC. */
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

    /* PMU: AXP2101 at 0x34. Only DCDC1 (VCC3V3) and ALDO1 (codecs) feed
     * anything per the 1.75C schematic; verify against 2.06" schematic
     * before trimming rails. */
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

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* QSPI panel IO for CO5300. */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SCK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "QSPI bus");
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io),
                        TAG, "panel IO");

    /* CO5300 panel driver would go here via esp_lcd_new_panel_co5300().
     * Using the generic init sequence pending ESP-BSP CO5300 support. */
    ESP_LOGW(TAG, "CO5300 init: using generic QSPI sequence, validate on hardware");

    /* Touch: try FT5x06 driver for FT3168 (same family, 0x38). */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TOUCH_INT,
    };
    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) == ESP_OK) {
        /* esp_lcd_touch_new_i2c_ft5x06() — may need a native FT3168 driver
         * if the register map differs. Validate touch response on hardware. */
        ESP_LOGW(TAG, "FT3168: trying FT5x06 driver, validate on hardware");
    }

    *touch = NULL;  /* touch init pending hardware validation */
    return NULL;    /* display init pending CO5300 driver validation */
}

static bool display_lock(int timeout_ms)
{
    return true;  /* stub until display_start is complete */
}

static void set_brightness(int pct)
{
    /* CO5300 "write display brightness" (0x51). */
    uint8_t level = (uint8_t)(pct * 255 / 100);
    esp_lcd_panel_io_tx_param(s_io, 0x51, &level, 1);
}

static void panel_sleep(bool sleep)
{
    /* Plain SLPIN (0x10) / SLPOUT (0x11). */
    esp_lcd_panel_io_tx_param(s_io, sleep ? 0x10 : 0x11, NULL, 0);
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
    .audio_init = NULL,  /* pending codec pin confirmation */
    .mic_slot = -1,
    .set_mic_gain = NULL,
    .poll_buttons = poll_buttons,
    .wait_buttons = NULL,
    .read_power = muse_pmu_read_power,
    .power_off = muse_pmu_power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
