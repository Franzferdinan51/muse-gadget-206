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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Status line and card overlay for the 2.06" watch.
 *
 * The status line is a persistent caption below the face, set by
 * `companion.set_status` / `pocket.set_status`. It survives until replaced.
 *
 * Cards are Hermes-style title/body overlays with auto-dismiss, shown via
 * the `display.show_card` command. A card covers the face until dismissed
 * by tap, timeout, or `display.show_animation`.
 *
 * All functions are safe from any task; they marshal to the LVGL task.
 */

/* Set the persistent status text (UTF-8, up to 4000 chars). Empty clears it. */
void muse_status_set(const char *text);
/* Get the current status text (for `device.health` etc). */
const char *muse_status_get(void);

/* Show a card overlay. ttl_s <= 0 means no auto-dismiss. */
void muse_card_show(const char *title, const char *body, int ttl_s);
/* Hide the card overlay if shown. */
void muse_card_hide(void);
/* True if a card is currently shown. */
bool muse_card_visible(void);

#ifdef __cplusplus
}
#endif
