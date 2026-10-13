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

/*
 * Status line and card overlay for the 2.06" watch (410x502).
 *
 * Layout: the face occupies the top ~410px; the status line sits below it
 * in the remaining ~92px. Cards overlay the face area with title + body.
 */

#include "muse_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "lvgl.h"

#include "muse_board.h"
#include "muse_theme.h"

#define STATUS_MAX_CHARS 4000
#define CARD_MAX_TITLE 120
#define CARD_MAX_BODY 2000

static const char *TAG = "status";

static SemaphoreHandle_t s_lock;
static char s_status[STATUS_MAX_CHARS + 1];
static lv_obj_t *s_status_label;

static char s_card_title[CARD_MAX_TITLE + 1];
static char s_card_body[CARD_MAX_BODY + 1];
static lv_obj_t *s_card;
static lv_obj_t *s_card_title_lbl;
static lv_obj_t *s_card_body_lbl;
static TimerHandle_t s_card_timer;

static void ensure_lock(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* Runs on the LVGL task. */
static void status_create_ui(lv_timer_t *t) {
    (void)t;
    if (s_status_label) return;
    lv_obj_t *scr = lv_scr_act();
    if (!scr) return;

    /* Status label: bottom area, centered, word-wrapped. */
    s_status_label = lv_label_create(scr);
    lv_obj_set_width(s_status_label, 390);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(MUSE_TEXT), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(s_status_label, s_status);
}

/* Runs on the LVGL task. */
static void card_dismiss_timer_cb(TimerHandle_t t) {
    (void)t;
    muse_card_hide();
}

static void card_create_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    if (!scr || s_card) return;

    /* Card: centered overlay, 360x300, rounded, above the face. */
    s_card = lv_obj_create(scr);
    lv_obj_set_size(s_card, 360, 300);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_radius(s_card, MUSE_R_DIALOG, 0);
    muse_theme_bubble(s_card);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_90, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    /* The gloss band (tokens G2), inset 12px a side; the labels are
     * created after it and draw over it. */
    lv_obj_set_width(muse_theme_gloss(s_card), 360 - 24);

    s_card_title_lbl = lv_label_create(s_card);
    lv_obj_set_width(s_card_title_lbl, 320);
    lv_label_set_long_mode(s_card_title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_card_title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_card_title_lbl, lv_color_hex(MUSE_TEXT), 0);
    lv_obj_set_style_text_align(s_card_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_card_title_lbl, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(s_card_title_lbl, s_card_title);

    s_card_body_lbl = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_card_body_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_card_body_lbl, lv_color_hex(MUSE_TEXT), 0);
    lv_obj_set_width(s_card_body_lbl, 320);
    lv_label_set_long_mode(s_card_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_card_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_card_body_lbl, LV_ALIGN_TOP_MID, 0, 60);
    lv_label_set_text(s_card_body_lbl, s_card_body);
}

/* lv_async_call callbacks take exactly one user-data argument. The
 * call sites below used to cast (lv_async_cb_t) onto functions with
 * other signatures (and one passed a third argument), which LVGL 9
 * rejects at compile time. These trampolines read the file-static
 * state instead; it is copied under s_lock before the async call is
 * queued, so the latest values are what get applied. */
static void status_label_apply_cb(void *user_data) {
    (void)user_data;
    if (s_status_label) lv_label_set_text(s_status_label, s_status);
}

static void card_create_ui_cb(void *user_data) {
    (void)user_data;
    card_create_ui();
}

void muse_status_set(const char *text) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!text) text = "";
    strncpy(s_status, text, STATUS_MAX_CHARS);
    s_status[STATUS_MAX_CHARS] = '\0';
    xSemaphoreGive(s_lock);

    /* Marshal to LVGL task. */
    if (s_status_label) {
        lv_async_call(status_label_apply_cb, NULL);
    } else {
        /* UI not up yet; create on next tick. */
        lv_timer_create(status_create_ui, 100, NULL);
    }
}

const char *muse_status_get(void) {
    return s_status;
}

void muse_card_show(const char *title, const char *body, int ttl_s) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_card_title, title ? title : "", CARD_MAX_TITLE);
    s_card_title[CARD_MAX_TITLE] = '\0';
    strncpy(s_card_body, body ? body : "", CARD_MAX_BODY);
    s_card_body[CARD_MAX_BODY] = '\0';
    xSemaphoreGive(s_lock);

    /* Create/show on LVGL task. For simplicity, do it directly if we're
     * already on the LVGL task; otherwise async. */
    if (!s_card) {
        /* Defer creation to LVGL task via async call. */
        lv_async_call(card_create_ui_cb, NULL);
    } else {
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_card_title_lbl, s_card_title);
        lv_label_set_text(s_card_body_lbl, s_card_body);
    }

    /* (Re)start the auto-dismiss timer. */
    if (s_card_timer) {
        xTimerStop(s_card_timer, 0);
        xTimerDelete(s_card_timer, 0);
        s_card_timer = NULL;
    }
    if (ttl_s > 0) {
        s_card_timer = xTimerCreate("card_ttl", pdMS_TO_TICKS((uint32_t)ttl_s * 1000),
                                    pdFALSE, NULL, card_dismiss_timer_cb);
        if (s_card_timer) xTimerStart(s_card_timer, 0);
    }
}

void muse_card_hide(void) {
    if (s_card_timer) {
        xTimerStop(s_card_timer, 0);
        xTimerDelete(s_card_timer, 0);
        s_card_timer = NULL;
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
}

bool muse_card_visible(void) {
    return s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}
