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

#include "muse_mdb.h"
#include "muse_audio.h"
#include "muse_voice.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "muse_board.h"
#include "esp_lv_adapter.h"
#include "muse_console.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_settings.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <dirent.h>
#include <sys/stat.h>
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_wifi.h"

/* ------------------------------------------------------------------ */
/* Output (the console is shared with the log; lines interleave,      */
/* exactly as the MDB contract expects the host to tolerate).         */

static void out_raw(const char *s)
{
    muse_console_write(s, strlen(s));
}

static void out_fmt(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0) {
        /* vsnprintf returns the would-be length: clamp to the buffer or
         * a long reply reads (and leaks) stack past b. */
        size_t len = (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1;
        muse_console_write(b, len);
    }
}

/* ------------------------------------------------------------------ */
/* Log ring: an esp_log vprintf hook keeps the last ~32 KB of log     */
/* in PSRAM so `mdb logs` can hand it to the host. The original       */
/* console output is chained, unchanged.                              */

#define RING_SZ (32 * 1024)

static char *s_ring;
static size_t s_ring_head;      /* next write position */
static uint64_t s_ring_total;   /* bytes ever written */
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static int (*s_orig_vprintf)(const char *, va_list);

static int ring_vprintf(const char *fmt, va_list ap)
{
    char tmp[384];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, cp);
    va_end(cp);
    if (n > 0 && s_ring) {
        size_t len = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
        portENTER_CRITICAL(&s_ring_mux);
        for (size_t i = 0; i < len; i++) {
            s_ring[s_ring_head] = tmp[i];
            s_ring_head = (s_ring_head + 1) % RING_SZ;
        }
        s_ring_total += len;
        portEXIT_CRITICAL(&s_ring_mux);
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : 0;
}

/* ------------------------------------------------------------------ */
/* Boot / panic counters (RTC-retained, like the battery state).     */

typedef struct {
    uint32_t magic;
    uint32_t boots;
    uint32_t panics;
} mdb_rtc_t;

#define MDB_RTC_MAGIC 0x4D444231u /* "MDB1" */
static RTC_NOINIT_ATTR mdb_rtc_t s_rtc;

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "other";
    }
}

/* ------------------------------------------------------------------ */

volatile uint32_t muse_mdb_stream_calls;

static void cmd_help(void)
{
    out_raw("OK mdb verbs: help version health logs panic screenshot tap swipe press goto open wake sleep reboot poweroff config settings sd sys nvs loglevel pump chirp ota i2cscan regs loopback\n");
}

static void cmd_version(void)
{
    /* Meta values carry no spaces (PROTOCOL.md S3); the board's full
     * display name lives in `mdb sys info`. */
    out_fmt("OK version=0.2 protocol=2 firmware=%s\n",
            esp_app_get_description()->version);
}

/* Link state as the health JSON's muse_link string. */
static const char *link_name(void)
{
    switch (muse_link_state()) {
    case MUSE_LINK_BOOT:       return "boot";
    case MUSE_LINK_UNPAIRED:   return "advertising";
    case MUSE_LINK_PAIRING:    return "pairing";
    case MUSE_LINK_CONFIRM:    return "confirm";
    case MUSE_LINK_CONNECTING: return "connecting";
    case MUSE_LINK_ONLINE:     return "up";
    case MUSE_LINK_OFFLINE:    return "down";
    default:                   return "error";
    }
}

static const char *mode_name(void)
{
    switch (muse_state_mode(NULL)) {
    case MUSE_MODE_IDLE:      return "idle";
    case MUSE_MODE_LISTENING: return "listening";
    case MUSE_MODE_THINKING:  return "thinking";
    case MUSE_MODE_SPEAKING:  return "speaking";
    case MUSE_MODE_ERROR:     return "error";
    case MUSE_MODE_OFF:       return "off";
    default:                  return "setup";
    }
}

/* OTA is an app-side service (main/ota.c); muse_glue.c provides the
 * strong hooks. The weak defaults keep this bridge buildable and the
 * verbs honest (ERR/unavailable) without the app component. */
__attribute__((weak)) bool muse_mdb_ota_start(const char *url, bool force)
{
    (void)url;
    (void)force;
    return false;
}

__attribute__((weak)) bool muse_mdb_ota_status(char *state, size_t state_len,
                                               char *detail, size_t detail_len)
{
    (void)state;
    (void)state_len;
    (void)detail;
    (void)detail_len;
    return false;
}

/* SD card state, implemented by the glue (MuseOS) where a card slot is
 * wired. Weak default: no card. */
__attribute__((weak)) bool muse_mdb_sd_status(bool *present, unsigned *total_mb,
                                              unsigned *free_mb, bool *key_on_sd)
{
    (void)present; (void)total_mb; (void)free_mb; (void)key_on_sd;
    return false;
}

/* SD mount action; the glue owns the host driver. Weak: unsupported. */
__attribute__((weak)) bool muse_mdb_sd_mount(void)
{
    return false;
}

/* A setting changed over MDB; the glue may need to re-apply live state
 * (keeper kicks). Weak: nothing to re-apply. */
__attribute__((weak)) void muse_mdb_settings_changed(const char *key)
{
    (void)key;
}

static void cmd_ota(const char *sub, const char *rest)
{
    if (sub && strcmp(sub, "start") == 0) {
        if (!rest || !rest[0]) {
            out_raw("ERR bad_args usage: mdb ota start <url> [force]\n");
            return;
        }
        char url[256];
        strncpy(url, rest, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        bool force = false;
        char *sp = strchr(url, ' ');
        if (sp) {
            *sp = '\0';
            force = strcmp(sp + 1, "force") == 0;
        }
        if (muse_mdb_ota_start(url, force)) {
            out_raw("OK ota=starting\n");
        } else {
            out_raw("ERR ota_unavailable OTA is disabled in this build\n");
        }
    } else if (!sub || strcmp(sub, "status") == 0) {
        char state[16], detail[96];
        if (muse_mdb_ota_status(state, sizeof(state), detail, sizeof(detail))) {
            /* detail is device-generated text; spaces would break k=v */
            for (char *p = detail; *p; p++) {
                if (*p == ' ' || *p == '\n') *p = '_';
            }
            out_fmt("OK state=%s detail=%s\n", state, detail);
        } else {
            out_raw("OK state=unavailable\n");
        }
    } else {
        out_raw("ERR unknown_command usage: mdb ota [status|start <url> [force]]\n");
    }
}

static void cmd_health(void)
{
    muse_power_t p = muse_state_power();
    muse_wifi_status_t ws;
    muse_wifi_status(&ws);
    bool wconn = ws.state == MUSE_WIFI_CONNECTED;
    char ssid[MUSE_SSID_MAX + 1], pass[MUSE_PASS_MAX + 1];
    muse_settings_wifi(ssid, pass);   /* presence only; values never leave the device */
    const char *wifi = ws.state == MUSE_WIFI_OFF ? "off" : wconn ? "connected" : "connecting";
    extern volatile uint32_t muse_bands_queued, muse_bands_chunks_sent,
        muse_bands_chunk_isrs, muse_bands_notifies;
    out_raw("OK\n");
    out_fmt("{\"uptime_ms\":%lld,", (long long)(esp_timer_get_time() / 1000));
    out_fmt("\"boot_count\":%u,", (unsigned)s_rtc.boots);
    out_fmt("\"reset_reason\":\"%s\",", esp_reset_reason() == ESP_RST_SW ? "sw_reset"
        : esp_reset_reason() == ESP_RST_DEEPSLEEP ? "deep_sleep" : reset_name(esp_reset_reason()));
    out_fmt("\"heap_free\":%u,", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    out_fmt("\"heap_min_free\":%u,", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    out_fmt("\"psram_free\":%u,", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (p.battery_pct < 0) {
        out_raw("\"battery_pct\":null,");
    } else {
        out_fmt("\"battery_pct\":%d,", p.battery_pct);
    }
    out_fmt("\"charging\":%s,", p.charging ? "true" : "false");
    out_fmt("\"panel_on\":%s,", muse_state_asleep() ? "false" : "true");
    out_fmt("\"wifi\":\"%s\",", wifi);
    if (wconn) {
        out_fmt("\"wifi_rssi\":%d,", ws.rssi);
        out_fmt("\"ip\":\"%s\",", ws.ip);
    } else {
        out_raw("\"wifi_rssi\":null,\"ip\":null,");
    }
    out_fmt("\"muse_link\":\"%s\",", link_name());
    out_raw("\"hermes_link\":\"down\",\"hermes_via\":null,");
    out_fmt("\"config\":{\"wifi\":%s,\"hatch_token\":%s},",
            ssid[0] ? "true" : "false", muse_settings_hatch_token_len() > 0 ? "true" : "false");
    out_raw("\"last_weather_age_s\":null,");
    out_fmt("\"mode\":\"%s\",", mode_name());
    out_fmt("\"screen\":\"%s\",", muse_ui_current_page());
    out_fmt("\"paired\":%s,", muse_settings_hatch_token_len() > 0 ? "true" : "false");
    out_fmt("\"battery_mv\":%d,", p.battery_mv);
    out_fmt("\"usb_power\":%s,", p.usb ? "true" : "false");
    out_fmt("\"panic_count\":%u,", (unsigned)s_rtc.panics);
    out_fmt("\"frame_ticks\":%u,", (unsigned)muse_ui_frame_ticks);
    out_fmt("\"frame_stage\":%u,", (unsigned)muse_ui_frame_stage);
    out_fmt("\"bands_queued\":%u,", (unsigned)muse_bands_queued);
    out_fmt("\"chunks_sent\":%u,", (unsigned)muse_bands_chunks_sent);
    out_fmt("\"chunk_isrs\":%u,", (unsigned)muse_bands_chunk_isrs);
    out_fmt("\"band_notifies\":%u,", (unsigned)muse_bands_notifies);
    extern volatile uint32_t muse_bands_queue_fails;
    out_fmt("\"band_fails\":%u,", (unsigned)muse_bands_queue_fails);
    extern volatile uint32_t muse_bands_stalls;
    out_fmt("\"band_stalls\":%u,", (unsigned)muse_bands_stalls);
    out_fmt("\"mdb_streams\":%u,", (unsigned)muse_mdb_stream_calls);
    const esp_partition_t *run = esp_ota_get_running_partition();
    out_fmt("\"ota_slot\":\"%s\",", run ? run->label : "?");
    out_fmt("\"fw_version\":\"%s\",", esp_app_get_description()->version);
    {
        bool sd_present = false, sd_key = false;
        unsigned sd_total = 0, sd_free = 0;
        muse_mdb_sd_status(&sd_present, &sd_total, &sd_free, &sd_key);
        out_fmt("\"sd_present\":%s,\"sd_total_mb\":%u,\"sd_free_mb\":%u,\"tts_key_on_sd\":%s,",
                sd_present ? "true" : "false", sd_total, sd_free,
                sd_key ? "true" : "false");
    }
    char ostate[16], odetail[96];
    if (muse_mdb_ota_status(ostate, sizeof(ostate), odetail, sizeof(odetail))) {
        out_fmt("\"ota_state\":\"%s\"}\n", ostate);
    } else {
        out_raw("\"ota_state\":\"unavailable\"}\n");
    }
}

static void cmd_panic(void)
{
    out_raw("OK\n");
    out_fmt("{\"panics\":%u,\"last_reason\":%s,\"last_pc\":null,\"backtrace\":null,\"coredump\":false}\n",
            (unsigned)s_rtc.panics,
            esp_reset_reason() == ESP_RST_PANIC ? "\"panic\"" : "null");
}

static void cmd_logs(void)
{
    if (!s_ring) {
        out_raw("ERR internal log ring unavailable\n");
        return;
    }
    /* Linearise the ring into a scratch copy, then answer with the
     * protocol's counted-lines shape (PROTOCOL.md 4.3): the host reads
     * exactly lines=<n> lines; a partial final line counts and is
     * terminated here so the host never waits on it. */
    static char *lin;
    if (!lin) {
        lin = heap_caps_malloc(RING_SZ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!lin) {
        out_raw("ERR internal log scratch unavailable\n");
        return;
    }
    portENTER_CRITICAL(&s_ring_mux);
    size_t total = s_ring_total < RING_SZ ? (size_t)s_ring_total : RING_SZ;
    size_t head = s_ring_head;
    if (total == RING_SZ) {
        memcpy(lin, s_ring + head, RING_SZ - head);
        memcpy(lin + (RING_SZ - head), s_ring, head);
    } else {
        memcpy(lin, s_ring, total);
    }
    portEXIT_CRITICAL(&s_ring_mux);
    unsigned lines = 0;
    for (size_t i = 0; i < total; i++) {
        if (lin[i] == '\n') {
            lines++;
        }
    }
    bool partial = total > 0 && lin[total - 1] != '\n';
    if (partial) {
        lines++;
    }
    out_fmt("OK lines=%u\n", lines);
    if (total) {
        muse_console_write(lin, total);
    }
    if (partial) {
        out_raw("\n");
    }
}

static void cmd_screenshot(void)
{
#if LV_USE_SNAPSHOT
    /* The take must run in the LVGL task's context (see
     * muse_mdb_stream_snapshot): from here we only ask the UI to do it
     * on its next frame; the OK meta line + payload follow then. */
    muse_ui_request_mdb_snapshot();
#else
    out_raw("ERR not_implemented snapshot off in this build\n");
#endif
}

/* Log sink for the duration of a dump: setting log levels does not
 * mute tags already in the esp_log level cache, so level games leave
 * live log lines interleaving into the binary stream (AIWatchOS hit
 * this with the FT5x06 error pair corrupting its first live dump).
 * Swapping the vprintf is airtight: every log line funnels through it. */
static int dump_sink_vprintf(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

/* Called from the UI's frame timer, i.e. inside the LVGL task, where
 * lv_snapshot_take can drive the draw engine. Taking it from the
 * serial task under the display lock deadlocks: the draw work waits
 * on the very task the lock is held against. No lock needed here -
 * this context is the one the lock protects. */
void muse_mdb_stream_snapshot(void)
{
    muse_mdb_stream_calls++;
#if LV_USE_SNAPSHOT
    /* Own the buffer and pre-fill it: lv_snapshot_take's own allocation
     * came back full of stale PSRAM (last boot's log text), so either
     * the alloc aliases live memory or the render writes nothing.
     * SNAPDBG's a5_left count says which. */
    static lv_draw_buf_t *snap;
    if (!snap) {
        snap = lv_draw_buf_create(410, 502, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    }
    if (!snap) {
        out_raw("ERR internal snapshot alloc failed\n");
        return;
    }
    memset(snap->data, 0xA5, snap->data_size);
    lv_result_t r = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, snap);
    if (r != LV_RESULT_OK) {
        out_raw("ERR internal snapshot failed\n");
        return;
    }
    lv_draw_buf_t *buf = snap;
    uint32_t a5 = 0, sum = 0;
    for (uint32_t i = 0; i + 1 < buf->data_size; i += 2) {
        if (buf->data[i] == 0xA5 && buf->data[i + 1] == 0xA5) {
            a5++;
        }
        sum += buf->data[i] | (buf->data[i + 1] << 8);
    }
    out_fmt("SNAPDBG a5_left=%u sum=%u size=%u\n", (unsigned)a5, (unsigned)sum, (unsigned)buf->data_size);
    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    esp_log_set_vprintf(dump_sink_vprintf);
    out_fmt("OK width=%u height=%u format=rgb565le bytes=%u\n",
            (unsigned)w, (unsigned)h, (unsigned)(w * h * 2));
    static uint8_t chunk[4096];
    size_t used = 0;
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; y++) {
        const uint8_t *rowp = buf->data + y * stride;
        size_t left = w * 2;
        while (left) {
            size_t take = left < sizeof(chunk) - used ? left : sizeof(chunk) - used;
            memcpy(chunk + used, rowp, take);
            used += take;
            rowp += take;
            left -= take;
            if (used == sizeof(chunk)) {
                if (muse_console_write_ms(chunk, used, 3000) != used) {
                    ok = false;
                }
                used = 0;
            }
        }
    }
    if (ok && used) {
        muse_console_write_ms(chunk, used, 3000);
    }
    esp_log_set_vprintf(ring_vprintf);
#endif
}

static void cmd_tap(int x, int y)
{
    if (x < 0 || y < 0 || x >= muse_board->width || y >= muse_board->height) {
        out_fmt("ERR out_of_range x=%d y=%d outside panel %dx%d\n",
                x, y, muse_board->width, muse_board->height);
        return;
    }
    muse_state_poke();
    muse_board->display_lock(-1);
    lv_point_t pt = { .x = (lv_coord_t)x, .y = (lv_coord_t)y };
    lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &pt);
    if (o) {
        lv_obj_send_event(o, LV_EVENT_PRESSED, NULL);
        lv_obj_send_event(o, LV_EVENT_RELEASED, NULL);
        lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    }
    muse_board->display_unlock();
    out_raw("OK\n");
}

static void show_page(const char *page, const char *kind)
{
    if (!page) {
        out_fmt("ERR bad_args usage: mdb %s <face|settings>\n", kind);
        return;
    }
    muse_state_poke();
    muse_board->display_lock(-1);
    if (strcmp(page, "face") == 0 || strcmp(page, "home") == 0) {
        muse_ui_show_face();
        muse_board->display_unlock();
        out_fmt("OK %s=face\n", kind);
    } else if (strcmp(page, "settings") == 0) {
        muse_ui_show_settings();
        muse_board->display_unlock();
        out_fmt("OK %s=settings\n", kind);
    } else {
        muse_board->display_unlock();
        out_fmt("ERR bad_args unknown %s '%s' (face|settings)\n",
                strcmp(kind, "screen") == 0 ? "screen" : "app", page);
    }
}

static void cmd_swipe(int x1, int y1, int x2, int y2, int dur_ms)
{
    if (x1 < 0 || y1 < 0 || x1 >= muse_board->width || y1 >= muse_board->height ||
        x2 < 0 || y2 < 0 || x2 >= muse_board->width || y2 >= muse_board->height) {
        out_raw("ERR out_of_range swipe endpoint outside panel\n");
        return;
    }
    if (dur_ms && (dur_ms < 50 || dur_ms > 5000)) {
        out_fmt("ERR out_of_range duration_ms=%d outside 50..5000\n", dur_ms);
        return;
    }
    int dx = x2 - x1, dy = y2 - y1;
    if ((dx > 60 || dx < -60) && (dy < 40 && dy > -40)) {
        /* A horizontal drag across the tileview: page left/right. */
        muse_state_poke();
        muse_board->display_lock(-1);
        if (dx < 0) {
            muse_ui_show_settings();
        } else {
            muse_ui_show_face();
        }
        muse_board->display_unlock();
        out_raw("OK\n");
        return;
    }
    out_raw("ERR not_implemented only horizontal page swipes in this build\n");
}

static void cmd_press(const char *what)
{
    if (!what) {
        out_raw("ERR bad_args usage: mdb press <boot|pwr>\n");
        return;
    }
    if (strcmp(what, "boot") == 0) {
        /* On this gadget the BOOT key IS the talk button. */
        muse_input_post_ptt(true);
        vTaskDelay(pdMS_TO_TICKS(200));
        muse_input_post_ptt(false);
        out_raw("OK\n");
    } else if (strcmp(what, "ptt") == 0) {
        out_raw("ERR not_implemented ptt is an on-screen target here; use tap or press boot\n");
    } else if (strcmp(what, "pwr") == 0) {
        muse_input_set_asleep(!muse_state_asleep());
        out_raw("OK\n");
    } else {
        out_fmt("ERR bad_args unknown button '%s' (boot|pwr)\n", what);
    }
}

static void cmd_config(const char *sub, const char *rest)
{
    if (sub && strcmp(sub, "get") == 0) {
        char ssid[MUSE_SSID_MAX + 1], pass[MUSE_PASS_MAX + 1];
        /* The link store is the live one (settings delegates to it);
         * lengths only below - values never cross the wire. */
        if (!muse_link_wifi_get(ssid, pass)) {
            muse_settings_wifi(ssid, pass);
        }
        out_raw("OK\n");
        out_fmt("{\"wifi_ssid\":{\"present\":%s,\"len\":%u},"
                "\"wifi_password\":{\"present\":%s,\"len\":%u},"
                "\"hatch_token\":{\"present\":%s,\"len\":%u}}\n",
                ssid[0] ? "true" : "false", (unsigned)strlen(ssid),
                pass[0] ? "true" : "false", (unsigned)strlen(pass),
                muse_settings_hatch_token_len() > 0 ? "true" : "false",
                (unsigned)muse_settings_hatch_token_len());
    } else if (sub && strcmp(sub, "set") == 0) {
        /* Bench provisioning at the owner direction (the Muse app
         * remains the normal route). wifi_ssid stages a value;
         * wifi_password stages and applies. Values are never echoed
         * back and never logged. */
        static char prov_ssid[MUSE_SSID_MAX + 1];
        static char prov_pass[MUSE_PASS_MAX + 1];
        if (!rest || !rest[0]) {
            out_raw("ERR bad_args usage: mdb config set <wifi_ssid|wifi_password> <value>\n");
            return;
        }
        const char *sp = strchr(rest, 32);
        char key[16];
        size_t klen = sp ? (size_t)(sp - rest) : strlen(rest);
        if (klen >= sizeof(key)) {
            out_raw("ERR bad_args unknown config key\n");
            return;
        }
        memcpy(key, rest, klen);
        key[klen] = 0;
        const char *val = sp ? sp + 1 : "";
        if (strcmp(key, "wifi") == 0) {
            /* one-shot: wifi <ssid> <password...> - atomic across the
             * port-open resets that wipe the staged values below. */
            const char *sp2 = strchr(val, 32);
            if (!sp2 || sp2 == val || !sp2[1]) {
                out_raw("ERR bad_args usage: mdb config set wifi <ssid> <password>\n");
                return;
            }
            char ssid[MUSE_SSID_MAX + 1];
            size_t slen = (size_t)(sp2 - val);
            if (slen >= sizeof(ssid)) {
                out_raw("ERR out_of_range ssid too long\n");
                return;
            }
            memcpy(ssid, val, slen);
            ssid[slen] = 0;
            muse_settings_set_wifi_on(true);
            muse_settings_set_wifi(ssid, sp2 + 1);
            out_raw("OK\n");
        } else if (strcmp(key, "wifi_ssid") == 0) {
            strlcpy(prov_ssid, val, sizeof(prov_ssid));
            out_raw("OK\n");
        } else if (strcmp(key, "wifi_password") == 0) {
            strlcpy(prov_pass, val, sizeof(prov_pass));
            muse_settings_set_wifi_on(true);
            muse_settings_set_wifi(prov_ssid, prov_pass);
            memset(prov_pass, 0, sizeof(prov_pass));
            out_raw("OK\n");
        } else {
            out_raw("ERR bad_args only wifi_ssid/wifi_password are settable over mdb\n");
        }
    } else {
        out_raw("ERR bad_args usage: mdb config <get|set>\n");
    }
}

/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* Protocol 2: settings, SD files, OS state, NVS.                     */

static bool parse_bool(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "true") == 0 || strcmp(s, "1") == 0) { *out = true; return true; }
    if (strcmp(s, "off") == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0) { *out = false; return true; }
    return false;
}

static void settings_dump(void)
{
    out_raw("OK\n");
    out_fmt("{\"volume\":%d,\"speaker_on\":%s,\"mic_gain\":%d,\"brightness\":%d,"
            "\"sleep_s\":%d,\"wifi_on\":%s,\"ble_on\":%s,\"avatar\":\"%s\"}\n",
            muse_settings_volume(),
            muse_settings_speaker_on() ? "true" : "false",
            muse_settings_mic_gain(),
            muse_settings_brightness(),
            muse_settings_sleep_s(),
            muse_settings_wifi_on() ? "true" : "false",
            muse_settings_ble_on() ? "true" : "false",
            muse_settings_avatar() == MUSE_AVATAR_PIXEL_GIF ? "pixelgif" : muse_settings_avatar() == MUSE_AVATAR_PIXEL ? "pixel" : muse_settings_avatar() == MUSE_AVATAR_JOLLY ? "jolly" : muse_settings_avatar() == MUSE_AVATAR_INK ? "ink" : muse_settings_avatar() == MUSE_AVATAR_KNIGHT ? "knight" : muse_settings_avatar() == MUSE_AVATAR_BUBBLE ? "bubble" : muse_settings_avatar() == MUSE_AVATAR_HIRES ? "hires" : muse_settings_avatar() == MUSE_AVATAR_STUDIO ? "studio" : "gif");
}

static void cmd_settings(const char *sub, const char *rest)
{
    if (!sub || strcmp(sub, "get") == 0) {
        if (!sub || !rest || !rest[0]) { settings_dump(); return; }
        char key[24];
        size_t n = strcspn(rest, " \t");
        if (n >= sizeof(key)) { out_raw("ERR bad_args unknown setting\n"); return; }
        memcpy(key, rest, n); key[n] = 0;
        if (strcmp(key, "volume") == 0) out_fmt("OK volume=%d\n", muse_settings_volume());
        else if (strcmp(key, "speaker_on") == 0) out_fmt("OK speaker_on=%s\n", muse_settings_speaker_on() ? "on" : "off");
        else if (strcmp(key, "mic_gain") == 0) out_fmt("OK mic_gain=%d\n", muse_settings_mic_gain());
        else if (strcmp(key, "brightness") == 0) out_fmt("OK brightness=%d\n", muse_settings_brightness());
        else if (strcmp(key, "sleep_s") == 0) out_fmt("OK sleep_s=%d\n", muse_settings_sleep_s());
        else if (strcmp(key, "wifi_on") == 0) out_fmt("OK wifi_on=%s\n", muse_settings_wifi_on() ? "on" : "off");
        else if (strcmp(key, "ble_on") == 0) out_fmt("OK ble_on=%s\n", muse_settings_ble_on() ? "on" : "off");
        else if (strcmp(key, "avatar") == 0) out_fmt("OK avatar=%s\n", muse_settings_avatar() == MUSE_AVATAR_PIXEL_GIF ? "pixelgif" : muse_settings_avatar() == MUSE_AVATAR_PIXEL ? "pixel" : muse_settings_avatar() == MUSE_AVATAR_JOLLY ? "jolly" : muse_settings_avatar() == MUSE_AVATAR_INK ? "ink" : muse_settings_avatar() == MUSE_AVATAR_KNIGHT ? "knight" : muse_settings_avatar() == MUSE_AVATAR_BUBBLE ? "bubble" : muse_settings_avatar() == MUSE_AVATAR_HIRES ? "hires" : muse_settings_avatar() == MUSE_AVATAR_STUDIO ? "studio" : "gif");
        else out_fmt("ERR bad_args unknown setting '%s'\n", key);
        return;
    }
    if (strcmp(sub, "set") == 0) {
        if (!rest || !rest[0]) {
            out_raw("ERR bad_args usage: mdb settings set <key> <value>\n");
            return;
        }
        char key[24];
        size_t n = strcspn(rest, " \t");
        if (n >= sizeof(key) || !rest[n]) {
            out_raw("ERR bad_args usage: mdb settings set <key> <value>\n");
            return;
        }
        memcpy(key, rest, n); key[n] = 0;
        const char *val = rest + n + 1;
        bool b; long v = strtol(val, NULL, 10);
        if (strcmp(key, "volume") == 0) {
            if (v < 0 || v > 100) { out_raw("ERR out_of_range volume 0..100\n"); return; }
            muse_settings_set_volume((int)v);
        } else if (strcmp(key, "brightness") == 0) {
            if (v < 10 || v > 100) { out_raw("ERR out_of_range brightness 10..100\n"); return; }
            muse_settings_set_brightness((int)v);
        } else if (strcmp(key, "mic_gain") == 0) {
            if (v < 0 || v > MUSE_MIC_GAIN_MAX) { out_raw("ERR out_of_range mic_gain\n"); return; }
            muse_settings_set_mic_gain((int)v);
        } else if (strcmp(key, "sleep_s") == 0) {
            if (v < 0 || v > 86400) { out_raw("ERR out_of_range sleep_s 0..86400\n"); return; }
            muse_settings_set_sleep_s((int)v);
        } else if (strcmp(key, "speaker_on") == 0) {
            if (!parse_bool(val, &b)) { out_raw("ERR bad_args want on|off\n"); return; }
            muse_settings_set_speaker_on(b);
        } else if (strcmp(key, "wifi_on") == 0) {
            if (!parse_bool(val, &b)) { out_raw("ERR bad_args want on|off\n"); return; }
            muse_settings_set_wifi_on(b);
        } else if (strcmp(key, "ble_on") == 0) {
            if (!parse_bool(val, &b)) { out_raw("ERR bad_args want on|off\n"); return; }
            muse_settings_set_ble_on(b);
        } else if (strcmp(key, "avatar") == 0) {
            if (strcmp(val, "gif") == 0) muse_settings_set_avatar(MUSE_AVATAR_GIF);
            else if (strcmp(val, "pixel") == 0) muse_settings_set_avatar(MUSE_AVATAR_PIXEL);
            else if (strcmp(val, "pixelgif") == 0) muse_settings_set_avatar(MUSE_AVATAR_PIXEL_GIF);
            else if (strcmp(val, "jolly") == 0) muse_settings_set_avatar(MUSE_AVATAR_JOLLY);
            else if (strcmp(val, "ink") == 0) muse_settings_set_avatar(MUSE_AVATAR_INK);
            else if (strcmp(val, "knight") == 0) muse_settings_set_avatar(MUSE_AVATAR_KNIGHT);
            else if (strcmp(val, "bubble") == 0) muse_settings_set_avatar(MUSE_AVATAR_BUBBLE);
            else if (strcmp(val, "hires") == 0) muse_settings_set_avatar(MUSE_AVATAR_HIRES);
            else if (strcmp(val, "studio") == 0) muse_settings_set_avatar(MUSE_AVATAR_STUDIO);
            else { out_raw("ERR bad_args want gif|pixel|pixelgif|jolly|ink|knight|bubble|hires|studio\n"); return; }
        } else {
            out_fmt("ERR bad_args unknown setting '%s'\n", key);
            return;
        }
        muse_mdb_settings_changed(key);
        out_fmt("OK %s set\n", key);
        return;
    }
    out_raw("ERR bad_args usage: mdb settings [get [key]|set <key> <value>]\n");
}

/* Join /sdcard + a user path, refusing escapes and secret key files for
 * reads (writes to them are the owner's own business). */
static bool sd_path(char *out, size_t len, const char *rel, bool for_read)
{
    if (!rel || !rel[0]) rel = "/";
    if (strstr(rel, "..")) return false;
    const char *base = strrchr(rel, '/');
    const char *name = base ? base + 1 : rel;
    if (for_read && (strcmp(name, "tts.key") == 0 ||
                     (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".key") == 0))) {
        return false;   /* secret contents never cross the wire */
    }
    if (rel[0] == '/')
        snprintf(out, len, "/sdcard%s", rel);
    else
        snprintf(out, len, "/sdcard/%s", rel);
    return true;
}

static void sd_status_json(void)
{
    bool present = false, key = false;
    unsigned total = 0, freeb = 0;
    muse_mdb_sd_status(&present, &total, &freeb, &key);
    out_fmt("{\"sd_present\":%s,\"sd_total_mb\":%u,\"sd_free_mb\":%u,\"tts_key_on_sd\":%s}\n",
            present ? "true" : "false", total, freeb, key ? "true" : "false");
}

static void cmd_sd(const char *sub, const char *rest)
{
    char path[160];
    if (!sub || strcmp(sub, "status") == 0) {
        out_raw("OK\n");
        sd_status_json();
    } else if (strcmp(sub, "mount") == 0) {
        if (muse_mdb_sd_mount()) {
            out_raw("OK mounted\n");
            sd_status_json();
        } else {
            out_raw("ERR mount_failed no card, unseated, or not FAT/FAT32\n");
        }
    } else if (strcmp(sub, "ls") == 0) {
        if (!sd_path(path, sizeof(path), rest, false)) { out_raw("ERR bad_args bad path\n"); return; }
        DIR *d = opendir(path);
        if (!d) { out_raw("ERR io cannot open dir (card mounted?)\n"); return; }
        out_raw("OK\n");
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d)) && n < 64) {
            /* stat() only when dir + name provably fits; overlong names
             * list without a type/size rather than statting garbage. */
            char full[192];
            struct stat st;
            bool known = false;
            if (strlen(path) + 1 + strlen(e->d_name) + 1 <= sizeof(full)) {
                strcpy(full, path);
                strcat(full, "/");
                strcat(full, e->d_name);
                known = stat(full, &st) == 0;
            }
            if (known && S_ISDIR(st.st_mode)) {
                out_fmt("D %s\n", e->d_name);
            } else {
                out_fmt("F %s %ld\n", e->d_name, known ? (long)st.st_size : -1L);
            }
            n++;
        }
        closedir(d);
        out_fmt("END ls %d\n", n);
    } else if (strcmp(sub, "cat") == 0) {
        if (!rest || !rest[0]) { out_raw("ERR bad_args usage: mdb sd cat <path>\n"); return; }
        if (!sd_path(path, sizeof(path), rest, true)) { out_raw("ERR protected_or_bad path\n"); return; }
        FILE *f = fopen(path, "rb");
        if (!f) { out_raw("ERR io cannot open file\n"); return; }
        out_raw("OK\n");
        char chunk[512];
        size_t got, total = 0;
        while (total < 8192 && (got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            muse_console_write(chunk, got);
            total += got;
        }
        fclose(f);
        out_fmt("\nEND cat %u bytes\n", (unsigned)total);
    } else if (strcmp(sub, "rm") == 0) {
        if (!rest || !strstr(rest, " confirm")) { out_raw("ERR confirm_required usage: mdb sd rm <path> confirm\n"); return; }
        char arg[128];
        size_t n = strcspn(rest, " ");
        if (n >= sizeof(arg)) { out_raw("ERR bad_args path too long\n"); return; }
        memcpy(arg, rest, n); arg[n] = 0;
        if (!sd_path(path, sizeof(path), arg, false)) { out_raw("ERR bad_args bad path\n"); return; }
        if (remove(path) == 0) out_raw("OK removed\n");
        else out_raw("ERR io remove failed\n");
    } else if (strcmp(sub, "write") == 0) {
        /* mdb sd write <path> <base64> - whole-file write of one chunk
         * (line-length bounded, ~160 bytes decoded): key/config files. */
        if (!rest || !rest[0]) { out_raw("ERR bad_args usage: mdb sd write <path> <base64>\n"); return; }
        const char *sp = strchr(rest, ' ');
        if (!sp) { out_raw("ERR bad_args usage: mdb sd write <path> <base64>\n"); return; }
        char arg[128];
        size_t n = (size_t)(sp - rest);
        if (n >= sizeof(arg)) { out_raw("ERR bad_args path too long\n"); return; }
        memcpy(arg, rest, n); arg[n] = 0;
        if (!sd_path(path, sizeof(path), arg, false)) { out_raw("ERR bad_args bad path\n"); return; }
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint8_t data[192];
        size_t dn = 0;
        uint32_t acc = 0; int bits = 0;
        for (const char *p = sp + 1; *p && *p != '='; p++) {
            const char *q = strchr(T, *p);
            if (!q) { out_raw("ERR bad_args not base64\n"); return; }
            acc = (acc << 6) | (uint32_t)(q - T); bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (dn >= sizeof(data)) { out_raw("ERR too_big one chunk only (<=192 bytes)\n"); return; }
                data[dn++] = (uint8_t)(acc >> bits);
            }
        }
        FILE *f = fopen(path, "wb");
        if (!f) { out_raw("ERR io cannot create file (card mounted?)\n"); return; }
        size_t wrote = fwrite(data, 1, dn, f);
        fclose(f);
        out_fmt("OK wrote %u bytes\n", (unsigned)wrote);
    } else {
        out_raw("ERR bad_args usage: mdb sd <status|mount|ls|cat|rm|write>\n");
    }
}

static void cmd_sys(const char *sub)
{
    if (!sub || strcmp(sub, "info") == 0) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        uint32_t flash = 0;
        esp_flash_get_size(NULL, &flash);
        out_raw("OK\n");
        out_fmt("{\"chip\":\"esp32s3\",\"model\":%d,\"rev\":%d,\"cores\":%d,"
                "\"idf\":\"%s\",\"flash_mb\":%u,\"fw\":\"%s\",\"board\":\"%s\","
                "\"reset_reason\":\"%s\",\"boots\":%u,\"panics\":%u,\"uptime_s\":%llu}\n",
                chip.model, chip.revision, chip.cores, esp_get_idf_version(),
                (unsigned)(flash >> 20), esp_app_get_description()->version,
                muse_board ? muse_board->name : "?",
                reset_name(esp_reset_reason()), (unsigned)s_rtc.boots,
                (unsigned)s_rtc.panics,
                (unsigned long long)(esp_timer_get_time() / 1000000));
        out_raw("{\"partitions\":[");
        bool first = true;
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it; it = esp_partition_next(it)) {
            const esp_partition_t *p = esp_partition_get(it);
            out_fmt("%s{\"label\":\"%s\",\"type\":%u,\"subtype\":%u,\"offset\":%u,\"size\":%u}",
                    first ? "" : ",", p->label, p->type, p->subtype,
                    (unsigned)p->address, (unsigned)p->size);
            first = false;
        }
        out_raw("]}\n");
    } else if (strcmp(sub, "heap") == 0) {
        out_raw("OK\n");
        out_fmt("{\"internal\":{\"free\":%u,\"largest\":%u,\"min_free\":%u},"
                "\"psram\":{\"free\":%u,\"largest\":%u,\"min_free\":%u},"
                "\"dma_free\":%u}\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    } else if (strcmp(sub, "tasks") == 0) {
#if configUSE_TRACE_FACILITY
        TaskStatus_t *arr = heap_caps_malloc(sizeof(TaskStatus_t) * 40,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!arr) { out_raw("ERR no_mem\n"); return; }
        uint32_t total = 0;
        UBaseType_t n = uxTaskGetSystemState(arr, 40, &total);
        out_raw("OK\n");
        for (UBaseType_t i = 0; i < n; i++) {
            static const char cs[] = {'R', 'r', 'B', 'S', 'D', '?'};
            unsigned st = (unsigned)arr[i].eCurrentState;
            out_fmt("%s %c prio=%u stack_hwm=%u\n", arr[i].pcTaskName,
                    cs[st < 5 ? st : 5], (unsigned)arr[i].uxCurrentPriority,
                    (unsigned)arr[i].usStackHighWaterMark);
        }
        out_fmt("END tasks %u\n", (unsigned)n);
        free(arr);
#else
        out_raw("ERR unsupported trace facility off in this build\n");
#endif
    } else {
        out_raw("ERR bad_args usage: mdb sys <info|heap|tasks>\n");
    }
}

static const char *nvs_type_name(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8: return "u8";
    case NVS_TYPE_I8: return "i8";
    case NVS_TYPE_U16: return "u16";
    case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U32: return "u32";
    case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U64: return "u64";
    case NVS_TYPE_I64: return "i64";
    case NVS_TYPE_STR: return "str";
    case NVS_TYPE_BLOB: return "blob";
    default: return "?";
    }
}

static void cmd_nvs(const char *sub, const char *rest)
{
    if (!sub || strcmp(sub, "list") == 0) {
        const char *want_ns = rest && rest[0] ? rest : NULL;
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find("nvs", want_ns, NVS_TYPE_ANY, &it);
        if (err != ESP_OK) { out_raw("OK\nEND nvs 0\n"); return; }
        out_raw("OK\n");
        nvs_handle_t h = 0;
        int entry_count = 0;
        char cur_ns[16] = "";
        while (it) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            entry_count++;
            if (strcmp(cur_ns, info.namespace_name) != 0) {
                if (h) nvs_close(h);
                h = 0;
                if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
                    strlcpy(cur_ns, info.namespace_name, sizeof(cur_ns));
                } else {
                    cur_ns[0] = 0;
                }
            }
            /* Values: numbers print (they are state, not secrets);
             * strings and blobs report length only. */
            if (info.type == NVS_TYPE_STR || info.type == NVS_TYPE_BLOB) {
                size_t len = 0;
                if (h && info.type == NVS_TYPE_STR)
                    nvs_get_str(h, info.key, NULL, &len);
                if (h && info.type == NVS_TYPE_BLOB)
                    nvs_get_blob(h, info.key, NULL, &len);
                out_fmt("%s %s %s len=%u\n", info.namespace_name, info.key,
                        nvs_type_name(info.type), (unsigned)len);
                if (nvs_entry_next(&it) != ESP_OK) break;
                continue;
            }
            long long val = 0;
            bool have = false;
            if (h) {
                uint8_t u8; int8_t i8; uint16_t u16; int16_t i16;
                uint32_t u32; int32_t i32; uint64_t u64; int64_t i64;
                switch (info.type) {
                case NVS_TYPE_U8: have = nvs_get_u8(h, info.key, &u8) == ESP_OK; val = u8; break;
                case NVS_TYPE_I8: have = nvs_get_i8(h, info.key, &i8) == ESP_OK; val = i8; break;
                case NVS_TYPE_U16: have = nvs_get_u16(h, info.key, &u16) == ESP_OK; val = u16; break;
                case NVS_TYPE_I16: have = nvs_get_i16(h, info.key, &i16) == ESP_OK; val = i16; break;
                case NVS_TYPE_U32: have = nvs_get_u32(h, info.key, &u32) == ESP_OK; val = u32; break;
                case NVS_TYPE_I32: have = nvs_get_i32(h, info.key, &i32) == ESP_OK; val = i32; break;
                case NVS_TYPE_U64: have = nvs_get_u64(h, info.key, &u64) == ESP_OK; val = (long long)u64; break;
                case NVS_TYPE_I64: have = nvs_get_i64(h, info.key, &i64) == ESP_OK; val = (long long)i64; break;
                default: break;
                }
            }
            if (have) out_fmt("%s %s %s %lld\n", info.namespace_name, info.key,
                              nvs_type_name(info.type), val);
            else out_fmt("%s %s %s\n", info.namespace_name, info.key,
                         nvs_type_name(info.type));
            if (nvs_entry_next(&it) != ESP_OK) break;
        }
        if (h) nvs_close(h);
        out_fmt("END nvs %d\n", entry_count);
    } else if (strcmp(sub, "erase") == 0 || strcmp(sub, "erase-ns") == 0) {
        bool whole = strcmp(sub, "erase-ns") == 0;
        if (!rest || !strstr(rest, " confirm")) {
            out_raw("ERR confirm_required usage: mdb nvs erase <ns> [key] confirm\n");
            return;
        }
        char ns[16], key[16] = "";
        if (whole) {
            size_t n = strcspn(rest, " ");
            if (n >= sizeof(ns)) { out_raw("ERR bad_args ns too long\n"); return; }
            memcpy(ns, rest, n); ns[n] = 0;
        } else {
            if (sscanf(rest, "%15s %15s", ns, key) < 2) {
                out_raw("ERR bad_args usage: mdb nvs erase <ns> <key> confirm\n");
                return;
            }
        }
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
            out_raw("ERR io no such namespace\n");
            return;
        }
        esp_err_t err = whole ? nvs_erase_all(h) : nvs_erase_key(h, key);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        out_raw(err == ESP_OK ? "OK erased\n" : "ERR io erase failed\n");
    } else {
        out_raw("ERR bad_args usage: mdb nvs <list [ns]|erase <ns> <key> confirm|erase-ns <ns> confirm>\n");
    }
}

void muse_mdb_init(void)
{
    /* Quiet the touch driver's NACK chatter (see flash10 note): two
     * error lines per failed poll otherwise flood the ring. */
    esp_log_level_set("FT5x06", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    if (s_rtc.magic != MDB_RTC_MAGIC) {
        s_rtc.magic = MDB_RTC_MAGIC;
        s_rtc.boots = 0;
        s_rtc.panics = 0;
    }
    s_rtc.boots++;
    if (esp_reset_reason() == ESP_RST_PANIC) {
        s_rtc.panics++;
    }
    s_ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring) {
        memset(s_ring, 0, RING_SZ);
        s_orig_vprintf = esp_log_set_vprintf(ring_vprintf);
    }
    ESP_LOGI("mdb", "MDB ready - 'mdb help'");
}

void muse_mdb_handle_line(const char *line)
{
    char buf[320];   /* room for an OTA URL */
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);   /* "mdb" */
    char *verb = strtok_r(NULL, " \t", &save);
    if (!tok || strcmp(tok, "mdb") != 0) {
        return;
    }
    if (!verb) {
        cmd_help();
        return;
    }
    if (strcmp(verb, "help") == 0) {
        cmd_help();
    } else if (strcmp(verb, "version") == 0) {
        cmd_version();
    } else if (strcmp(verb, "health") == 0) {
        cmd_health();
    } else if (strcmp(verb, "panic") == 0) {
        cmd_panic();
    } else if (strcmp(verb, "logs") == 0) {
        cmd_logs();
    } else if (strcmp(verb, "screenshot") == 0) {
        cmd_screenshot();
    } else if (strcmp(verb, "goto") == 0) {
        show_page(strtok_r(NULL, " \t", &save), "screen");
    } else if (strcmp(verb, "open") == 0) {
        show_page(strtok_r(NULL, " \t", &save), "app");
    } else if (strcmp(verb, "config") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        cmd_config(sub, sub ? strtok_r(NULL, "", &save) : NULL);
    } else if (strcmp(verb, "tap") == 0) {
        char *xs = strtok_r(NULL, " \t", &save);
        char *ys = strtok_r(NULL, " \t", &save);
        if (xs && ys) {
            cmd_tap(atoi(xs), atoi(ys));
        } else {
            out_raw("ERR bad_args usage: mdb tap <x> <y>\n");
        }
    } else if (strcmp(verb, "swipe") == 0) {
        char *a = strtok_r(NULL, " \t", &save);
        char *b = strtok_r(NULL, " \t", &save);
        char *c = strtok_r(NULL, " \t", &save);
        char *d = strtok_r(NULL, " \t", &save);
        char *e = strtok_r(NULL, " \t", &save);
        if (a && b && c && d) {
            cmd_swipe(atoi(a), atoi(b), atoi(c), atoi(d), e ? atoi(e) : 0);
        } else {
            out_raw("ERR bad_args usage: mdb swipe <x1> <y1> <x2> <y2> [duration_ms]\n");
        }
    } else if (strcmp(verb, "press") == 0) {
        cmd_press(strtok_r(NULL, " \t", &save));
    } else if (strcmp(verb, "pump") == 0) {
        /* Diagnostic: do by hand what the adapter's worker does - take
         * the lock (timed, so a held lock answers instead of hanging)
         * and run one timer-handler pass. */
        if (esp_lv_adapter_lock(500) == ESP_OK) {
            uint32_t next = lv_timer_handler();
            esp_lv_adapter_unlock();
            out_fmt("OK pumped next=%u ticks=%u\n", (unsigned)next, (unsigned)muse_ui_frame_ticks);
        } else {
            out_raw("ERR busy adapter lock held >500ms\n");
        }
    } else if (strcmp(verb, "chirp") == 0) {
        /* Speaker test at a gentle, capped volume. Runtime-only: a test
         * must not rewrite the user's saved volume. */
        char *vs = strtok_r(NULL, " \t", &save);
        int vol = vs ? atoi(vs) : 35;
        if (vol < 1) vol = 1;
        if (vol > 80) vol = 80;
        muse_audio_set_volume(vol);
        muse_voice_request_chirp();
        out_fmt("OK chirp vol=%d\n", vol);
    } else if (strcmp(verb, "settings") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        cmd_settings(sub, sub ? strtok_r(NULL, "", &save) : NULL);
    } else if (strcmp(verb, "sd") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        cmd_sd(sub, sub ? strtok_r(NULL, "", &save) : NULL);
    } else if (strcmp(verb, "sys") == 0) {
        cmd_sys(strtok_r(NULL, " \t", &save));
    } else if (strcmp(verb, "nvs") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        cmd_nvs(sub, sub ? strtok_r(NULL, "", &save) : NULL);
    } else if (strcmp(verb, "loglevel") == 0) {
        char *tag = strtok_r(NULL, " \t", &save);
        char *lvl = strtok_r(NULL, " \t", &save);
        if (tag && lvl) {
            esp_log_level_t l = ESP_LOG_NONE;
            if (strcmp(lvl, "error") == 0) l = ESP_LOG_ERROR;
            else if (strcmp(lvl, "warn") == 0) l = ESP_LOG_WARN;
            else if (strcmp(lvl, "info") == 0) l = ESP_LOG_INFO;
            else if (strcmp(lvl, "debug") == 0) l = ESP_LOG_DEBUG;
            else if (strcmp(lvl, "verbose") == 0) l = ESP_LOG_VERBOSE;
            else if (strcmp(lvl, "none") != 0) {
                out_raw("ERR bad_args level: none|error|warn|info|debug|verbose\n");
                tag = NULL;
            }
            if (tag) {
                esp_log_level_set(tag, l);
                out_raw("OK log level set\n");
            }
        } else {
            out_raw("ERR bad_args usage: mdb loglevel <tag|*> <level>\n");
        }
    } else if (strcmp(verb, "poweroff") == 0) {
        /* Via the input task, like the Power page: goodbye caption plus
         * the USB-aware wait (a bare PMU bit just reboots on USB). */
        out_raw("OK powering off\n");
        muse_input_request_power_off();
    } else if (strcmp(verb, "i2cscan") == 0) {
        char report[256];
        if (muse_board && muse_board->scan_i2c && muse_board->scan_i2c(report, sizeof(report))) {
            out_fmt("OK %s\n", report);
        } else {
            out_raw("ERR unsupported no I2C scan on this board\n");
        }
    } else if (strcmp(verb, "regs") == 0) {
        /* Codec register dump (audio diagnostic): what state did the
         * codec drivers actually leave the chips in? */
        static char report[1280];
        if (muse_board && muse_board->audio_regs && muse_board->audio_regs(report, sizeof(report))) {
            /* Too long for one out_fmt (256 B formatter): stream it raw. */
            out_raw("OK ");
            out_raw(report);
        } else {
            out_raw("ERR unsupported no codec register access on this board\n");
        }
    } else if (strcmp(verb, "loopback") == 0) {
        /* Speaker->mic acoustic loopback test; per-tone levels land in
         * the log (see muse_audio_loopback_test). */
        muse_voice_request_loopback();
        out_raw("OK loopback requested, results in log\n");
    } else if (strcmp(verb, "ota") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        cmd_ota(sub, sub ? strtok_r(NULL, "", &save) : NULL);
    } else if (strcmp(verb, "wake") == 0) {
        muse_input_set_asleep(false);
        out_raw("OK panel=on\n");
    } else if (strcmp(verb, "sleep") == 0) {
        muse_input_set_asleep(true);
        out_raw("OK panel=off\n");
    } else if (strcmp(verb, "reboot") == 0) {
        out_raw("OK rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(60));
        esp_restart();
    } else {
        out_fmt("ERR unknown_command '%s'\n", verb);
    }
}
