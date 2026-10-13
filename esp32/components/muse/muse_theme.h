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
 * Muse Gloss theme for the LVGL UI: the named tokens of DESIGN_TOKENS.md
 * ("Muse Gloss" v1.0, taken from the Muse Companion App's muse_theme.dart).
 * UI code takes its colors from here, never raw hex.
 *
 * Chrome is always the blue family. The state accents are a spice that may
 * appear ONLY on state elements — the state word, the ring indicator, the
 * level meter, the avatar glow, an 8px status dot — never as a surface.
 * (The avatar's own per-mode ramps live in avatar/muse_pixel.c and already
 * match tokens section 3; MUSE_STATE_* below mirror its accents for LVGL
 * elements that report state without being the avatar.)
 *
 * A surface is a bubble: a 3-stop vertical gradient (the Flutter original
 * is diagonal; vertical reads identically at watch size, tokens G1), a 1px
 * white border, a blue-tinted shadow (tokens G4: LVGL renders its own
 * shadow from its partial draw buffers — no runtime blur, no extra frame
 * buffer), and the gloss band (tokens G2) where a child object can be
 * spared. The gradient descriptors are built lazily, the caption_font()
 * pattern: every caller is on the LVGL task.
 */

#ifndef MUSE_THEME_H
#define MUSE_THEME_H

#include "lvgl.h"

/* ---- Color tokens (DESIGN_TOKENS.md section 1) ---- */
#define MUSE_BLUE 0x1877F2       /* primary chrome: bezel, selection, links */
#define MUSE_BLUE_DEEP 0x0A3F86  /* gradient foot, pressed states */
#define MUSE_BLUE_TOP 0x4B95F5   /* lerp(blue, white, .22): filled top stop */
#define MUSE_BLUE_BOT 0x1467D4   /* lerp(blue, blueDeep, .28): filled bottom stop */
#define MUSE_BUBBLE 0x14305A     /* THE surface color */
#define MUSE_BUBBLE_TOP 0x3A5174 /* lerp(bubble, white, .16): surface top stop */
#define MUSE_BUBBLE_BOT 0x123465 /* lerp(bubble, blueDeep, .25): surface bottom stop */
#define MUSE_INK 0x07101C        /* page background base */
#define MUSE_NIGHT 0x0C1828      /* app background over ink */
#define MUSE_NIGHT_DEEP 0x0C1D33 /* lerp(night, blueDeep, .12): backdrop foot (G5) */
#define MUSE_INPUT 0x13233C      /* input / keyboard field fill */
#define MUSE_RING_TRACK 0x0A2343 /* lerp(ink, blue, .18): bezel / progress track */
#define MUSE_TEXT 0xF2EFFF       /* primary text (warm white, not #FFF) */
#define MUSE_TEXT_NUM 0xFFFFFF   /* headline numerals only */
#define MUSE_CAPTION 0xD8D2FF    /* caption / reply text — always this, never the accent */
#define MUSE_DIM 0x8B84A8        /* secondary text, inactive icons */
#define MUSE_FAINT 0x3A3358      /* tertiary text, off segments */
#define MUSE_OK 0x6FF0BF         /* success / connected */
#define MUSE_WARN 0xFFB45C       /* warning */
#define MUSE_DANGER 0xFF5C5C     /* error / destructive */
#define MUSE_MIST 0xF4F8FF       /* text on filled blue bubbles */

/* ---- State accents (tokens section 3): state elements ONLY ---- */
#define MUSE_STATE_READY 0xA77DFF
#define MUSE_STATE_LISTENING 0x5CB8FF
#define MUSE_STATE_THINKING 0xE07BFF
#define MUSE_STATE_SPEAKING 0x6FF0BF
#define MUSE_STATE_ERROR 0xFF5C5C
#define MUSE_BOOT_ACCENT 0xA9C0FF /* the pairing code: pairing happens in BOOT */

/* ---- Geometry (tokens section 5): the only radii ---- */
#define MUSE_R_CHIP 10
#define MUSE_R_CARD 16
#define MUSE_R_BUBBLE 22
#define MUSE_R_DIALOG 24

/* ---- Gloss constants (tokens section 1), opacities out of 255 ---- */
#define MUSE_OPA_BORDER 56        /* white border, surface bubbles (0.22) */
#define MUSE_OPA_BORDER_FILLED 87 /* white border, filled blue bubbles (0.34) */
#define MUSE_OPA_GLOSS 56         /* gloss band peak (0.22) */
#define MUSE_OPA_SHADOW 72        /* blue shadow (0.28; tokens G4 LVGL note) */
#define MUSE_SHADOW_W 18
#define MUSE_SHADOW_Y 8
#define MUSE_GLOSS_H 10

/* ---- Gradient descriptors (tokens section 4) ---- */

/* G1 surface gradient: bubbleTop -> bubble -> bubbleBot, vertical. */
static inline const lv_grad_dsc_t *muse_grad_surface(void)
{
    static lv_grad_dsc_t dsc;
    if (!dsc.stops_count) {
        dsc.dir = LV_GRAD_DIR_VER;
        dsc.stops_count = 3;
        dsc.stops[0] = (lv_grad_stop_t){ lv_color_hex(MUSE_BUBBLE_TOP), LV_OPA_COVER, 0 };
        dsc.stops[1] = (lv_grad_stop_t){ lv_color_hex(MUSE_BUBBLE), LV_OPA_COVER, 128 };
        dsc.stops[2] = (lv_grad_stop_t){ lv_color_hex(MUSE_BUBBLE_BOT), LV_OPA_COVER, 255 };
    }
    return &dsc;
}

/* The surface pressed: the gradient foot takes over (tokens section 8). */
static inline const lv_grad_dsc_t *muse_grad_surface_pressed(void)
{
    static lv_grad_dsc_t dsc;
    if (!dsc.stops_count) {
        dsc.dir = LV_GRAD_DIR_VER;
        dsc.stops_count = 3;
        dsc.stops[0] = (lv_grad_stop_t){ lv_color_hex(MUSE_BUBBLE), LV_OPA_COVER, 0 };
        dsc.stops[1] = (lv_grad_stop_t){ lv_color_hex(MUSE_BUBBLE_BOT), LV_OPA_COVER, 128 };
        dsc.stops[2] = (lv_grad_stop_t){ lv_color_hex(MUSE_BLUE_DEEP), LV_OPA_COVER, 255 };
    }
    return &dsc;
}

/* Filled (own / primary) bubble gradient: blueTop -> blue -> blueBot. */
static inline const lv_grad_dsc_t *muse_grad_filled(void)
{
    static lv_grad_dsc_t dsc;
    if (!dsc.stops_count) {
        dsc.dir = LV_GRAD_DIR_VER;
        dsc.stops_count = 3;
        dsc.stops[0] = (lv_grad_stop_t){ lv_color_hex(MUSE_BLUE_TOP), LV_OPA_COVER, 0 };
        dsc.stops[1] = (lv_grad_stop_t){ lv_color_hex(MUSE_BLUE), LV_OPA_COVER, 128 };
        dsc.stops[2] = (lv_grad_stop_t){ lv_color_hex(MUSE_BLUE_BOT), LV_OPA_COVER, 255 };
    }
    return &dsc;
}

/* G5 page backdrop: ink -> night -> a whisper of blueDeep at the foot. */
static inline const lv_grad_dsc_t *muse_grad_backdrop(void)
{
    static lv_grad_dsc_t dsc;
    if (!dsc.stops_count) {
        dsc.dir = LV_GRAD_DIR_VER;
        dsc.stops_count = 3;
        dsc.stops[0] = (lv_grad_stop_t){ lv_color_hex(MUSE_INK), LV_OPA_COVER, 0 };
        dsc.stops[1] = (lv_grad_stop_t){ lv_color_hex(MUSE_NIGHT), LV_OPA_COVER, 170 };
        dsc.stops[2] = (lv_grad_stop_t){ lv_color_hex(MUSE_NIGHT_DEEP), LV_OPA_COVER, 255 };
    }
    return &dsc;
}

/* G2 gloss band: white at 0.22 fading to 0 over the band's own height. */
static inline const lv_grad_dsc_t *muse_grad_gloss(void)
{
    static lv_grad_dsc_t dsc;
    if (!dsc.stops_count) {
        dsc.dir = LV_GRAD_DIR_VER;
        dsc.stops_count = 2;
        dsc.stops[0] = (lv_grad_stop_t){ lv_color_white(), MUSE_OPA_GLOSS, 0 };
        dsc.stops[1] = (lv_grad_stop_t){ lv_color_white(), LV_OPA_TRANSP, 255 };
    }
    return &dsc;
}

/* ---- Surface recipes ---- */

static inline void muse_theme_shadow(lv_obj_t *obj)
{
    lv_obj_set_style_shadow_color(obj, lv_color_hex(MUSE_BLUE), 0);
    lv_obj_set_style_shadow_opa(obj, MUSE_OPA_SHADOW, 0);
    lv_obj_set_style_shadow_width(obj, MUSE_SHADOW_W, 0);
    lv_obj_set_style_shadow_offset_y(obj, MUSE_SHADOW_Y, 0);
}

/* A surface bubble: gradient + 1px white-22% border + blue shadow.
 * The caller sets the radius (tokens section 5) and the size. Children
 * are clipped to the rounded rect so a gloss band's corners follow it. */
static inline void muse_theme_bubble(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(obj, muse_grad_surface(), 0);
    lv_obj_set_style_bg_grad(obj, muse_grad_surface_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(obj, MUSE_OPA_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    muse_theme_shadow(obj);
}

/* A filled blue bubble (primary actions): brighter border, same shadow. */
static inline void muse_theme_filled(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(obj, muse_grad_filled(), 0);
    lv_obj_set_style_bg_grad(obj, muse_grad_surface_pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_white(), 0);
    lv_obj_set_style_border_opa(obj, MUSE_OPA_BORDER_FILLED, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    muse_theme_shadow(obj);
}

/* The page backdrop (G5) for screens and full-screen overlays. */
static inline void muse_theme_backdrop(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(obj, muse_grad_backdrop(), 0);
}

/*
 * The gloss band (G2): a 10px white fade starting 1px below the top edge.
 * The band ignores layouts, so it can live inside flex rows and panels
 * without moving anything; the caller sets its width (parent width minus
 * the 12px insets) and aligns it to the top. Create it before the
 * surface's other children so they draw over it.
 */
static inline lv_obj_t *muse_theme_gloss(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_add_flag(g, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(g, MUSE_GLOSS_H);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(g, muse_grad_gloss(), 0);
    lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 1);
    return g;
}

#endif /* MUSE_THEME_H */
