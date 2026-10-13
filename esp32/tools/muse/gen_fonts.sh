#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Regenerates components/muse/fonts/muse_font_{12,14,16,20,28}.c, the baked
# Montserrat the Muse UI speaks (DESIGN_TOKENS.md section 6): Montserrat
# Medium at 4bpp with the section 6 charset -- ASCII 32-126, the tokens
# extras (degree sign, dashes, curly quotes, bullet, ellipsis, arrows) and
# the FontAwesome symbol set LVGL's own built-in Montserrat merges (its
# LV_SYMBOL_* codepoints). The recipe mirrors LVGL's built-ins exactly
# (lv_font_conv --no-compress --no-prefilter --force-fast-kern-format), so
# metrics match the built-ins they replace; the sources are LVGL v9.5.0's
# own copies, sha256-pinned like gen_cjk_font.sh pins Unifont.
# U+26A1 (the tokens list's lightning bolt) is not in Montserrat or
# FontAwesome at that codepoint; the charge glyph the UI uses is
# FontAwesome's U+F0E7, which the symbol merge includes.
# Needs curl and npx (Node.js).
set -eu

LVGL_TAG=v9.5.0
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$HERE/components/muse/fonts"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

MONTSERRAT_SHA256=421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53
FONTAWESOME_SHA256=f4e42f6cd69e5dbdcccc0f2f5be136cebde0e427641e45403bf9173a92da95f4
BASE="https://raw.githubusercontent.com/lvgl/lvgl/$LVGL_TAG/scripts/built_in_font"
curl -fsSL -o "$TMP/Montserrat-Medium.ttf" "$BASE/Montserrat-Medium.ttf"
curl -fsSL -o "$TMP/FontAwesome5-Solid+Brands+Regular.woff" \
    "$BASE/FontAwesome5-Solid+Brands+Regular.woff"
echo "$MONTSERRAT_SHA256  $TMP/Montserrat-Medium.ttf" | shasum -a 256 -c - >/dev/null
echo "$FONTAWESOME_SHA256  $TMP/FontAwesome5-Solid+Brands+Regular.woff" | shasum -a 256 -c - >/dev/null

TEXT_RANGES="0x20-0x7E,0xB0,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2022,0x2026,0x2190-0x2193"
# The FontAwesome codepoints of LVGL's built-in Montserrat (decimal, from
# lv_font_montserrat_16.c's Opts line in the LVGL tree).
FA_RANGES="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

for SIZE in 12 14 16 20 28; do
    npx -y lv_font_conv@1.5.3 \
        --font "$TMP/Montserrat-Medium.ttf" -r "$TEXT_RANGES" \
        --font "$TMP/FontAwesome5-Solid+Brands+Regular.woff" -r "$FA_RANGES" \
        --size "$SIZE" --bpp 4 --format lvgl --lv-font-name "muse_font_$SIZE" \
        --no-compress --no-prefilter --force-fast-kern-format \
        -o "$TMP/font_$SIZE.c"
    # The project includes LVGL as "lvgl.h"; the header names no temp paths.
    sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" \
        "$TMP/font_$SIZE.c" > "$OUT_DIR/muse_font_$SIZE.c"
    echo "wrote $OUT_DIR/muse_font_$SIZE.c"
done
ls -la "$OUT_DIR"/muse_font_*.c
