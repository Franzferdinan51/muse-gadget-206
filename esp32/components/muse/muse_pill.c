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

#include "muse_pill.h"

const char *muse_pill_label(muse_pill_t pill)
{
    switch (pill) {
    case MUSE_PILL_CONNECTED:
        return "Connected";
    case MUSE_PILL_CONNECTING:
        return "Connecting…";
    case MUSE_PILL_WAITING:
        return "Waiting to retry";
    case MUSE_PILL_UNPAIRED:
        return "Not paired";
    case MUSE_PILL_STOPPED:
        return "Stopped";
    default:
        return "Unknown";
    }
}

uint32_t muse_pill_dot(muse_pill_t pill)
{
    switch (pill) {
    case MUSE_PILL_CONNECTED:
        return MUSE_PILL_DOT_CONNECTED;
    case MUSE_PILL_CONNECTING:
        return MUSE_PILL_DOT_CONNECTING;
    case MUSE_PILL_WAITING:
        return MUSE_PILL_DOT_WAITING;
    case MUSE_PILL_UNPAIRED:
    case MUSE_PILL_STOPPED:
    default:
        return MUSE_PILL_DOT_UNPAIRED;
    }
}

muse_pill_t muse_pill_from_link(muse_hatch_state_t hatch, bool linked)
{
    switch (hatch) {
    case MUSE_HATCH_REACHABLE:
        return MUSE_PILL_CONNECTED;
    case MUSE_HATCH_TESTING:
    case MUSE_HATCH_UNTESTED:
        return MUSE_PILL_CONNECTING;
    case MUSE_HATCH_NOT_SET:
        /* A pairing via the Muse link is still working towards a session. */
        return linked ? MUSE_PILL_CONNECTING : MUSE_PILL_UNPAIRED;
    case MUSE_HATCH_OFFLINE:
    case MUSE_HATCH_UNREACHABLE:
    default:
        return MUSE_PILL_WAITING;
    }
}
