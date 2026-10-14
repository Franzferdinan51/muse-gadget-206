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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MuseOS SD card support. The Waveshare 2.06 TF slot is wired SDMMC
 * 1-bit (CLK=2, CMD=1, D0=3). Mounting is lazy, idempotent and throttled;
 * the firmware NEVER formats the card - a card it cannot mount is simply
 * reported absent. Boards without the slot get inert stubs.
 */

#define MUSE_SD_MOUNT_POINT "/sdcard"

/* Mount (or confirm mounted). ESP_OK when the card is usable. */
esp_err_t muse_sd_mount(void);
bool muse_sd_mounted(void);
/* Filesystem size via the VFS; false when no card is mounted. */
bool muse_sd_info(uint64_t *total_bytes, uint64_t *free_bytes);
/*
 * The cloud TTS key from /sdcard/tts.key (first line, trimmed). Cached
 * once read; NULL while no key file is available. The value is never
 * logged. This is how the speech key ships with the owner, not with the
 * binary: published builds carry no secret at all.
 */
const char *muse_sd_tts_key(void);

#ifdef __cplusplus
}
#endif
