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

#include "muse_chat.h"

/*
 * Connection pill: the face header's link dot and its label.
 *
 * Ported from the Muse Companion App's connection bubble
 * (app/lib/src/gadget/service.dart's ConnectionState,
 * app/lib/app/model.dart's connectionStatusLabel, and the _StatusDot in
 * app/lib/ui/companion_screen.dart), with the roles reversed: the states
 * describe this gadget's own link to Hatch, not a phone's link to it.
 *
 * Host-clean (no ESP/LVGL includes): the mapping is pure so the host
 * tests can pin the labels and colours.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Same order as the app's ConnectionState. */
typedef enum {
    MUSE_PILL_UNPAIRED,
    MUSE_PILL_CONNECTING,
    MUSE_PILL_CONNECTED,
    MUSE_PILL_WAITING,
    MUSE_PILL_STOPPED,
    MUSE_PILL_COUNT,
} muse_pill_t;

/* Dot colours as 0xRRGGBB: the app's green/amber/orange/grey fitted to
 * Muse Gloss tokens (muse_theme.h). */
#define MUSE_PILL_DOT_CONNECTED 0x6FF0BFu /* MUSE_OK */
#define MUSE_PILL_DOT_CONNECTING 0xFFC107u /* MUSE_AMBER */
#define MUSE_PILL_DOT_WAITING 0xFFB45Cu   /* MUSE_WARN */
#define MUSE_PILL_DOT_UNPAIRED 0x8B84A8u  /* MUSE_DIM */
#define MUSE_PILL_DOT_STOPPED 0x8B84A8u   /* MUSE_DIM */

/* Short link label, verbatim from the app ("Waiting to retry", ...).
 * Out-of-range pills read "Unknown". */
const char *muse_pill_label(muse_pill_t pill);

/* Dot colour for a pill; out-of-range pills get the grey. */
uint32_t muse_pill_dot(muse_pill_t pill);

/*
 * This gadget's link as a pill: the Hatch session state plus whether a
 * pairing exists (a local token or one via the Muse link). STOPPED has
 * no producer on the gadget yet — nothing here stops the link — it
 * exists so the ported tables stay complete.
 *
 * REACHABLE reads connected; TESTING/UNTESTED (and a pairing with no
 * local session yet) read connecting; OFFLINE/UNREACHABLE read waiting
 * to retry; no pairing at all reads unpaired. Unknown states read
 * waiting rather than claiming connected.
 */
muse_pill_t muse_pill_from_link(muse_hatch_state_t hatch, bool linked);

#ifdef __cplusplus
}
#endif
