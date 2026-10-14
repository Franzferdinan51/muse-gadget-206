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

/*
 * MDB — the Muse Developer Bridge, MuseOS edition.
 *
 * The same line protocol as AIWatchOS's bridge (see that repo's
 * docs/mdb.md, the contract): commands arrive on the shared serial
 * console as lines starting with "mdb ", replies are "OK ..."/"ERR ..."
 * lines or framed blocks (FBDUMP_BEGIN..., MDB_HEALTH_BEGIN...,
 * MDB_LOGS_BEGIN..., MDB_PANIC_BEGIN...). It exists so the host can see
 * the watch (screenshot, health, logs) and drive it (tap, swipe,
 * press, goto, wake, sleep, reboot) without a human finger.
 *
 * Bench tooling, like the 'p' snapshot: it can read the screen and
 * inject input, so it does not belong in a build that leaves the bench.
 *
 * muse_mdb_init() installs the log ring (an esp_log vprintf hook) and
 * the boot/panic counters; call it before the serial task starts.
 * muse_mdb_handle_line() runs in the serial task's context.
 */
void muse_mdb_init(void);
void muse_mdb_handle_line(const char *line);
/* Streams one FBDUMP for a pending `mdb screenshot`; called from the
 * UI's frame timer (LVGL task context) - see muse_mdb.c for why. */
void muse_mdb_stream_snapshot(void);
