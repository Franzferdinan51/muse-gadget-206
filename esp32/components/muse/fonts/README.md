# Fonts

`muse_font_12.c`, `muse_font_14.c`, `muse_font_16.c`, `muse_font_20.c` and
`muse_font_28.c` are the fonts the Muse UI speaks: Montserrat Medium baked
at build time with `lv_font_conv` (4bpp, anti-aliased), carrying the
DESIGN_TOKENS.md section 6 charset — ASCII, the tokens extras (degree sign,
dashes, curly quotes, bullet, ellipsis, arrows) — plus the FontAwesome
symbol set LVGL's built-in Montserrat merges. They are wired through
`muse_theme.h`. LVGL's built-in Montserrat 16/20/28 and unscii-16 are no
longer compiled for Muse boards (the built-in 14 LVGL defaults on stays
compiled for LVGL's own examples but is unreferenced, so it is not linked
into the image), and unscii-8 remains only as LVGL's default font, the
fallback for a widget that forgets to set one.

`tools/muse/gen_fonts.sh` regenerates them. It downloads LVGL v9.5.0's own
copies of Montserrat-Medium.ttf and FontAwesome5-Solid+Brands+Regular.woff
(sha256-pinned) and converts them with the same lv_font_conv recipe as
LVGL's built-ins, so the baked fonts' metrics match the built-ins they
replaced. Montserrat is by Julieta Ulanovsky, Sol Matas, Juan Pablo del
Peral and Jacques Le Bailly, licensed under the SIL Open Font License 1.1
(https://openfontlicense.org). The FontAwesome glyphs are Font Awesome
Free, as redistributed by LVGL (the font is SIL OFL 1.1; the icons are
CC BY 4.0).

`muse_font_cjk_16.c` is the CJK fallback for the caption font, built in only
with `CONFIG_MUSE_CJK_FONT`. It holds GNU Unifont 16.0.04's 16x16 bitmaps,
the same cell as unscii-16, for CJK punctuation, kana, every CJK Unified
Ideograph (U+4E00 to U+9FFF) and the fullwidth forms: about 850 KB of flash.
`tools/muse/gen_cjk_font.sh` regenerates it.

GNU Unifont is by Roman Czyborra, Paul Hardy and contributors
(https://unifoundry.com/unifont/). Its compiled fonts are licensed under the
SIL Open Font License, version 1.1 (https://openfontlicense.org), and under
the GNU GPL version 2 or later with the GNU font embedding exception. This
file is a conversion of an unaltered subset of the font.
