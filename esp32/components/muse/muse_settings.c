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

#include "muse_settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "muse_link.h"

static const char *TAG = "muse_settings";

#define NS "muse"
#define DEFAULT_HOST "hatch.metaaivm.com"

static struct {
    uint8_t volume;
    bool speaker_on;
    uint8_t mic_gain;
    uint8_t brightness;
    uint16_t sleep_s;
    bool wifi_on;
    bool ble_on;
    uint8_t avatar;
    uint8_t power_mode;
    uint8_t tts_model;
    uint8_t tts_voice_mode;
    char ssid[MUSE_SSID_MAX + 1];
    char pass[MUSE_PASS_MAX + 1];
    char host[MUSE_HOST_MAX + 1];
    char vm[MUSE_VM_MAX + 1];
    char token[MUSE_TOKEN_MAX + 1];
    char tts_voice[MUSE_TTS_VOICE_MAX + 1];
    char tts_local[MUSE_TTS_LOCAL_MAX + 1];
} s = {
    .volume = CONFIG_MUSE_DEFAULT_VOLUME,
    .speaker_on = true,
    .mic_gain = 30,
    .brightness = 100,
    .sleep_s = 120,
    .wifi_on = true,
    .avatar = MUSE_AVATAR_GIF,
    .power_mode = MUSE_POWER_AUTO,
    .tts_model = MUSE_TTS_MODEL_FREE,
    .tts_local = "http://192.168.1.96:8799",
    .host = DEFAULT_HOST,
};

static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static muse_setting_cb_t s_listener;

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Missing keys leave the default in place. */
static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    nvs_get_str(s_nvs, key, out, &n);
}

static void load_u8(const char *key, uint8_t *out)
{
    nvs_get_u8(s_nvs, key, out);
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_set_u8(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void save_str(const char *key, const char *v)
{
    nvs_set_str(s_nvs, key, v);
    nvs_commit(s_nvs);
}

static void notify(muse_setting_t what)
{
    if (s_listener) {
        s_listener(what);
    }
}

esp_err_t muse_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t b;
    load_u8("volume", &s.volume);
    if (nvs_get_u8(s_nvs, "speaker", &b) == ESP_OK) {
        s.speaker_on = b;
    }
    load_u8("mic_gain", &s.mic_gain);
    load_u8("bright", &s.brightness);
    load_u8("avatar", &s.avatar);
    if (s.avatar > MUSE_AVATAR_STUDIO) { s.avatar = MUSE_AVATAR_GIF; }
    load_u8("power_mode", &s.power_mode);
    if (s.power_mode > MUSE_POWER_OFF) { s.power_mode = MUSE_POWER_AUTO; }
    load_u8("tts_model", &s.tts_model);
    if (s.tts_model > MUSE_TTS_MODEL_QWEN3) { s.tts_model = MUSE_TTS_MODEL_FREE; }
    load_u8("tts_vmode", &s.tts_voice_mode);
    if (s.tts_voice_mode >= muse_settings_tts_voice_count()) { s.tts_voice_mode = MUSE_TTS_VOICE_DEFAULT; }
    load_str("tts_voice", s.tts_voice, sizeof(s.tts_voice));
    load_str("tts_local", s.tts_local, sizeof(s.tts_local));
    nvs_get_u16(s_nvs, "sleep_s", &s.sleep_s);
    if (nvs_get_u8(s_nvs, "wifi_on", &b) == ESP_OK) {
        s.wifi_on = b;
    }
    if (nvs_get_u8(s_nvs, "ble_on", &b) == ESP_OK) {
        s.ble_on = b;
    }
    load_str("ssid", s.ssid, sizeof(s.ssid));
    load_str("pass", s.pass, sizeof(s.pass));
    load_str("host", s.host, sizeof(s.host));
    load_str("vm", s.vm, sizeof(s.vm));
    load_str("token", s.token, sizeof(s.token));

    s.volume = clampi(s.volume, 0, 100);
    s.mic_gain = clampi(s.mic_gain, 0, MUSE_MIC_GAIN_MAX);
    s.brightness = clampi(s.brightness, 10, 100);
    s.sleep_s = clampi(s.sleep_s, 0, 3600);   /* corrupt NVS must not kill auto-sleep */
    ESP_LOGI(TAG, "vol %d%s, mic %d dB, bright %d, sleep %ds, wifi %s (%s), ble %s, muse %s",
             s.volume, s.speaker_on ? "" : " (speaker off)", s.mic_gain, s.brightness, s.sleep_s, s.wifi_on ? "on" : "off",
             "network saved by Link", s.ble_on ? "on" : "off", s.token[0] ? "token set" : "no token");
    return ESP_OK;
}

void muse_settings_set_listener(muse_setting_cb_t cb)
{
    s_listener = cb;
}

int muse_settings_volume(void) { int v; LOCKED(v = s.volume); return v; }
bool muse_settings_speaker_on(void) { bool v; LOCKED(v = s.speaker_on); return v; }
int muse_settings_mic_gain(void) { int v; LOCKED(v = s.mic_gain); return v; }
int muse_settings_brightness(void) { int v; LOCKED(v = s.brightness); return v; }
int muse_settings_avatar(void) { int v; LOCKED(v = s.avatar); return v; }
int muse_settings_power_mode(void) { int v; LOCKED(v = s.power_mode); return v; }
int muse_settings_tts_model(void) { int v; LOCKED(v = s.tts_model); return v; }
int muse_settings_tts_voice_mode(void) { int v; LOCKED(v = s.tts_voice_mode); return v; }

void muse_settings_tts_voice_id(char out[MUSE_TTS_VOICE_MAX + 1])
{
    LOCKED(strlcpy(out, s.tts_voice, MUSE_TTS_VOICE_MAX + 1));
}

void muse_settings_tts_local_url(char out[MUSE_TTS_LOCAL_MAX + 1])
{
    LOCKED(strlcpy(out, s.tts_local, MUSE_TTS_LOCAL_MAX + 1));
}

void muse_settings_set_tts_local_url(const char *url)
{
    if (!url) { url = ""; }
    LOCKED(strlcpy(s.tts_local, url, sizeof(s.tts_local)));
    save_str("tts_local", url);
    notify(MUSE_SETTING_TTS_MODEL);
}

/* Built-in Fish Audio voices (public catalog reference ids). */
static const muse_tts_voice_t kTtsVoices[] = {
    { "Default", "" },
    { "Sarah", "933563129e564b19a115bedd57b7406a" },
    { "Selene", "b347db033a6549378b48d00acb0d06cd" },
    { "Hannah", "9a9cf47702da476aa4629e2506d4a857" },
    { "Friendly", "b545c585f631496c914815291da4e893" },
    { "Polo", "f48d143a59a946ab87c0130fd081f349" },
    { "Adrian", "bf322df2096a46f18c579d0baa36f41d" },
    { "Ethan", "536d3a5e000945adb7038665781a4aca" },
    { "Elite", "d8a1340984ee4b63ad1ffae27a6a4339" },
    { "Jordan", "79d0bd3e4e5444b18f7b6d89b5927bf1" },
    { "Energetic", "802e3bc2b27e49c2995d23ef70e6ac89" },
    { "Atomic", "4c6a6762e4ac4bdebdb4fa8525d054a2" },
    { "Announcer", "90e65eaaf50e4470b8e6d43ee6afd7d5" },
    { "Custom", "" },
};

int muse_settings_tts_voice_count(void)
{
    return (int)(sizeof(kTtsVoices) / sizeof(kTtsVoices[0]));
}

const muse_tts_voice_t *muse_settings_tts_voice_at(int idx)
{
    if (idx < 0 || idx >= muse_settings_tts_voice_count()) { return &kTtsVoices[0]; }
    return &kTtsVoices[idx];
}
int muse_settings_sleep_s(void) { int v; LOCKED(v = s.sleep_s); return v; }
bool muse_settings_wifi_on(void) { bool v; LOCKED(v = s.wifi_on); return v; }
bool muse_settings_ble_on(void) { bool v; LOCKED(v = s.ble_on); return v; }

/* Home Link owns the saved networks (this is the first); the local copy is only a fallback. */
void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1])
{
    if (muse_link_wifi_get(ssid, pass)) {
        return;
    }
    LOCKED({
        strlcpy(ssid, s.ssid, MUSE_SSID_MAX + 1);
        if (pass) {
            strlcpy(pass, s.pass, MUSE_PASS_MAX + 1);
        }
    });
}

void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1])
{
    LOCKED(strlcpy(out, s.host, MUSE_HOST_MAX + 1));
}

void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1])
{
    LOCKED(strlcpy(out, s.vm, MUSE_VM_MAX + 1));
}

void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1])
{
    LOCKED(strlcpy(out, s.token, MUSE_TOKEN_MAX + 1));
}

size_t muse_settings_hatch_token_len(void)
{
    size_t n;
    LOCKED(n = strlen(s.token));
    return n;
}

void muse_settings_set_volume(int pct)
{
    int v = clampi(pct, 0, 100);
    LOCKED(s.volume = v);
    save_u8("volume", (uint8_t)v);
    notify(MUSE_SETTING_VOLUME);
}

void muse_settings_set_speaker_on(bool on)
{
    LOCKED(s.speaker_on = on);
    save_u8("speaker", on);
    notify(MUSE_SETTING_SPEAKER);
}

void muse_settings_set_mic_gain(int db)
{
    int v = clampi(db, 0, MUSE_MIC_GAIN_MAX);
    LOCKED(s.mic_gain = v);
    save_u8("mic_gain", (uint8_t)v);
    notify(MUSE_SETTING_MIC_GAIN);
}

void muse_settings_set_brightness(int pct)
{
    int v = clampi(pct, 10, 100);
    LOCKED(s.brightness = v);
    save_u8("bright", (uint8_t)v);
    notify(MUSE_SETTING_BRIGHTNESS);
}

void muse_settings_set_avatar(int which)
{
    int v = clampi(which, MUSE_AVATAR_GIF, MUSE_AVATAR_STUDIO);
    LOCKED(s.avatar = (uint8_t)v);
    save_u8("avatar", (uint8_t)v);
    notify(MUSE_SETTING_AVATAR);
}

void muse_settings_set_power_mode(int mode)
{
    int v = clampi(mode, MUSE_POWER_AUTO, MUSE_POWER_OFF);
    LOCKED(s.power_mode = (uint8_t)v);
    save_u8("power_mode", (uint8_t)v);
    notify(MUSE_SETTING_POWER);
}

void muse_settings_set_tts_model(int which)
{
    int v = clampi(which, MUSE_TTS_MODEL_FREE, MUSE_TTS_MODEL_QWEN3);
    LOCKED(s.tts_model = (uint8_t)v);
    save_u8("tts_model", (uint8_t)v);
    notify(MUSE_SETTING_TTS_MODEL);
}

void muse_settings_set_tts_voice_mode(int mode)
{
    int v = clampi(mode, MUSE_TTS_VOICE_DEFAULT, muse_settings_tts_voice_count() - 1);
    LOCKED(s.tts_voice_mode = (uint8_t)v);
    save_u8("tts_vmode", (uint8_t)v);
    notify(MUSE_SETTING_TTS_VOICE);
}

void muse_settings_set_tts_voice_id(const char *id)
{
    char buf[MUSE_TTS_VOICE_MAX + 1];
    LOCKED(strlcpy(s.tts_voice, id ? id : "", sizeof(s.tts_voice)));
    LOCKED(strlcpy(buf, s.tts_voice, sizeof(buf)));
    save_str("tts_voice", buf);
    notify(MUSE_SETTING_TTS_VOICE);
}

void muse_settings_set_sleep_s(int secs)
{
    int v = clampi(secs, 0, 3600);
    LOCKED(s.sleep_s = (uint16_t)v);
    nvs_set_u16(s_nvs, "sleep_s", (uint16_t)v);
    nvs_commit(s_nvs);
    notify(MUSE_SETTING_SLEEP);
}

void muse_settings_set_wifi_on(bool on)
{
    LOCKED(s.wifi_on = on);
    save_u8("wifi_on", on);
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_ble_on(bool on)
{
    LOCKED(s.ble_on = on);
    save_u8("ble_on", on);
    notify(MUSE_SETTING_BLE);
}

void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    if (muse_link_wifi_set(ssid ? ssid : "", ssid && ssid[0] && pass ? pass : "")) {
        ESP_LOGI(TAG, "wifi network: %s", ssid && ssid[0] ? ssid : "(all forgotten)");
        notify(MUSE_SETTING_WIFI);
        return;
    }
    LOCKED({
        strlcpy(s.ssid, ssid ? ssid : "", sizeof(s.ssid));
        strlcpy(s.pass, s.ssid[0] && pass ? pass : "", sizeof(s.pass));
        save_str("ssid", s.ssid);
        save_str("pass", s.pass);
    });
    ESP_LOGI(TAG, "wifi network: %s", s.ssid[0] ? s.ssid : "(forgotten)");
    notify(MUSE_SETTING_WIFI);
}

void muse_settings_set_hatch_host(const char *host)
{
    LOCKED({
        strlcpy(s.host, host && host[0] ? host : DEFAULT_HOST, sizeof(s.host));
        save_str("host", s.host);
    });
    notify(MUSE_SETTING_HATCH);
}

void muse_settings_set_hatch_vm(const char *vm)
{
    LOCKED({
        strlcpy(s.vm, vm ? vm : "", sizeof(s.vm));
        save_str("vm", s.vm);
    });
    notify(MUSE_SETTING_HATCH);
}

esp_err_t muse_settings_set_hatch_token(const char *token, bool append)
{
    esp_err_t err = ESP_OK;
    LOCKED({
        size_t have = append ? strlen(s.token) : 0;
        size_t add = strlen(token ? token : "");
        if (have + add > MUSE_TOKEN_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(s.token + have, token, add);
            s.token[have + add] = '\0';
            save_str("token", s.token);
        }
    });
    if (err == ESP_OK) {
        notify(MUSE_SETTING_HATCH);
    }
    return err;
}
