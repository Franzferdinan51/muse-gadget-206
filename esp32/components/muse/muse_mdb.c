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
        muse_console_write(b, (size_t)n);
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
    out_raw("OK mdb verbs: help version health logs panic screenshot tap swipe press goto open wake sleep reboot config pump\n");
}

static void cmd_version(void)
{
    out_raw("OK version=0.1 protocol=1 firmware=museos-bringup\n");
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
    out_fmt("\"mdb_streams\":%u,", (unsigned)muse_mdb_stream_calls);
    const esp_partition_t *run = esp_ota_get_running_partition();
    out_fmt("\"ota_slot\":\"%s\",", run ? run->label : "?");
    out_fmt("\"fw_version\":\"%s\",", esp_app_get_description()->version);
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
