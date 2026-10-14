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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used.
 */

#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define MUSE_HOST_MAX 63
#define MUSE_VM_MAX 63
#define MUSE_TOKEN_MAX 1023

#define MUSE_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */

typedef enum {
    MUSE_SETTING_VOLUME,
    MUSE_SETTING_SPEAKER,
    MUSE_SETTING_MIC_GAIN,
    MUSE_SETTING_BRIGHTNESS,
    MUSE_SETTING_SLEEP,
    MUSE_SETTING_WIFI,          /* on/off or credentials */
    MUSE_SETTING_BLE,
    MUSE_SETTING_AVATAR,
    MUSE_SETTING_POWER,
    MUSE_SETTING_TTS_MODEL,
    MUSE_SETTING_TTS_VOICE,
    MUSE_SETTING_HATCH,
} muse_setting_t;

typedef void (*muse_setting_cb_t)(muse_setting_t what);

esp_err_t muse_settings_init(void);
void muse_settings_set_listener(muse_setting_cb_t cb);

int muse_settings_volume(void);         /* 0..100 */
bool muse_settings_speaker_on(void);    /* off: replies are shown, not played */
int muse_settings_mic_gain(void);       /* dB, 0..MUSE_MIC_GAIN_MAX */
int muse_settings_brightness(void);     /* 10..100 */
int muse_settings_sleep_s(void);        /* 0 = never */
bool muse_settings_wifi_on(void);
bool muse_settings_ble_on(void);

#define MUSE_AVATAR_GIF   0       /* the animated Juno clips (default) */
#define MUSE_AVATAR_PIXEL 1       /* the original pixel character */
#define MUSE_AVATAR_PIXEL_GIF 2   /* the animated pixel Juno clips (Muse-Avatar) */
#define MUSE_AVATAR_JOLLY 3       /* Jollybot, the original Muse (Muse-Avatar) */
#define MUSE_AVATAR_INK 4         /* Hermes ink, white variant (collection) */
#define MUSE_AVATAR_KNIGHT 5      /* Hermes knight (collection) */
#define MUSE_AVATAR_BUBBLE 6      /* Juno bubble (collection) */
#define MUSE_AVATAR_HIRES 7       /* Juno hi-res (collection) */
#define MUSE_AVATAR_STUDIO 8      /* Juno studio render (collection) */
int muse_settings_avatar(void);         /* MUSE_AVATAR_* */

#define MUSE_POWER_AUTO 0   /* save when the battery runs low (default) */
#define MUSE_POWER_ON   1   /* always saving */
#define MUSE_POWER_OFF  2   /* never (a critical battery still protects) */
int muse_settings_power_mode(void);     /* MUSE_POWER_* */

#define MUSE_TTS_MODEL_FREE   0   /* fish-audio/s2.1-pro-free:free (default) */
#define MUSE_TTS_MODEL_DRAMA3 1   /* Drama 3 on the local Mac server */
#define MUSE_TTS_MODEL_QWEN3  2   /* Qwen3-TTS on the local Mac server */
int muse_settings_tts_model(void);      /* MUSE_TTS_MODEL_* */
#define MUSE_TTS_LOCAL_MAX 95
void muse_settings_tts_local_url(char out[MUSE_TTS_LOCAL_MAX + 1]);

#define MUSE_TTS_VOICE_MAX     63
#define MUSE_TTS_VOICE_DEFAULT 0   /* the model's built-in voice */
/* Voice selection indexes: 0 = Default, 1..count-2 = built-in Fish
 * Audio presets (display name + reference id), count-1 = Custom
 * (the stored reference/voice id). */
typedef struct { const char *name; const char *fish_id; } muse_tts_voice_t;
int muse_settings_tts_voice_count(void);
const muse_tts_voice_t *muse_settings_tts_voice_at(int idx);
#define MUSE_TTS_VOICE_CUSTOM (muse_settings_tts_voice_count() - 1)
int muse_settings_tts_voice_mode(void); /* voice selection index */
void muse_settings_tts_voice_id(char out[MUSE_TTS_VOICE_MAX + 1]);

void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1]);
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]);
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]);
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_settings_hatch_token_len(void);

void muse_settings_set_volume(int pct);
void muse_settings_set_speaker_on(bool on);
void muse_settings_set_mic_gain(int db);
void muse_settings_set_brightness(int pct);
void muse_settings_set_sleep_s(int secs);
void muse_settings_set_wifi_on(bool on);
void muse_settings_set_ble_on(bool on);
void muse_settings_set_avatar(int which);   /* MUSE_AVATAR_* */
void muse_settings_set_power_mode(int mode);   /* MUSE_POWER_* */
void muse_settings_set_tts_model(int which);   /* MUSE_TTS_MODEL_* */
void muse_settings_set_tts_local_url(const char *url);
void muse_settings_set_tts_voice_mode(int mode);   /* MUSE_TTS_VOICE_* */
void muse_settings_set_tts_voice_id(const char *id);
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void muse_settings_set_wifi(const char *ssid, const char *pass);
void muse_settings_set_hatch_host(const char *host);
void muse_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t muse_settings_set_hatch_token(const char *token, bool append);
