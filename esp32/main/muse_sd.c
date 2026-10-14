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

#include "muse_sd.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#if defined(CONFIG_MUSE_BOARD_WAVESHARE_S3_206)
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#define SD_SUPPORTED 1
#define SD_PIN_CLK 2
#define SD_PIN_CMD 1
#define SD_PIN_D0  3
#else
#define SD_SUPPORTED 0
#endif

static const char *TAG = "link.sd";

static bool s_mounted;
#if SD_SUPPORTED
static sdmmc_card_t *s_card;
static TickType_t s_last_try;
#endif

#define SD_RETRY_TICKS pdMS_TO_TICKS(10000)

esp_err_t muse_sd_mount(void)
{
#if !SD_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mounted) return ESP_OK;
    TickType_t now = xTaskGetTickCount();
    if (s_last_try && now - s_last_try < SD_RETRY_TICKS) return ESP_ERR_TIMEOUT;
    s_last_try = now;

    /* DAT3 doubles as the card's mode select at power-up: low/floating
     * boots the card into SPI mode and SDMMC init never gets an answer.
     * Hold it high (the board routes it to GPIO17) before clocking. */
    gpio_config_t d3 = {
        .pin_bit_mask = 1ULL << 17,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&d3);

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,   /* never format the owner's card */
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    /* Slot 1 only: the S3 has no SDMMC slot 0, so probing it first just
     * wastes a failure's worth of time and log lines on every attempt. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_PIN_CLK;
    slot.cmd = (gpio_num_t)SD_PIN_CMD;
    slot.d0 = (gpio_num_t)SD_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MUSE_SD_MOUNT_POINT, &host, &slot,
                                            &mnt, &s_card);
    if (err == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "card mounted at %s", MUSE_SD_MOUNT_POINT);
    } else {
        ESP_LOGW(TAG, "mount failed: %s (no card, unseated, or not"
                 " FAT/FAT32 - ESP-IDF FatFs cannot mount exFAT)",
                 esp_err_to_name(err));
    }
    return err;
#endif
}

bool muse_sd_mounted(void)
{
    return s_mounted;
}

bool muse_sd_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) return false;
    /* esp_vfs_fat_info, not statvfs: the FatFs VFS statvfs path fails on
     * this mount while plain file I/O works fine (found live, 1.7.15). */
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MUSE_SD_MOUNT_POINT, &total, &freeb) != ESP_OK) return false;
    if (total_bytes) *total_bytes = total;
    if (free_bytes) *free_bytes = freeb;
    return true;
}

const char *muse_sd_tts_key(void)
{
    static char s_key[192];
    static bool s_loaded;
    if (s_loaded) return s_key;
    if (!s_mounted && muse_sd_mount() != ESP_OK) return NULL;

    FILE *f = fopen(MUSE_SD_MOUNT_POINT "/tts.key", "r");
    if (!f) return NULL;
    char line[sizeof(s_key)];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got) return NULL;
    /* Trim surrounding whitespace/newlines; the key itself is opaque. */
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n < 8 || n >= sizeof(s_key)) return NULL;   /* implausible: not a key */
    memcpy(s_key, p, n);
    s_key[n] = '\0';
    s_loaded = true;
    ESP_LOGI(TAG, "tts key loaded from sd (%u chars)", (unsigned)n);
    return s_key;
}
