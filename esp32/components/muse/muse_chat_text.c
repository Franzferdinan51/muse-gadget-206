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

/* Shared by both chat backends: the voice note's encoding, and reply and
 * transcript text cut to fit the caption. Also the serial console's "@chat"
 * lines for typed turns. */
#include "muse_chat_priv.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "muse_state.h"
#include "muse_text.h"

#define CAPTION_CHARS 32   /* what fits across the round screen */
#define CONSOLE_LINE 400   /* one "@chat" line */
#define CONSOLE_TEXT 240   /* escaped text per line */

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

void muse_hatch_wav_header(uint8_t h[MUSE_HATCH_WAV_HEADER], uint32_t rate)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, UINT32_MAX, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);             /* PCM */
    put_le(h + 22, 1, 2);             /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);
    put_le(h + 32, 2, 2);
    put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, UINT32_MAX, 4);
}

size_t muse_hatch_base64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = in[i] << 16 | in[i + 1] << 8 | in[i + 2];
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = A[v >> 6 & 63];
        *o++ = A[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0);
        *o++ = A[v >> 18];
        *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    return o - out;
}

/* Last CAPTION_CHARS characters of `src`, starting on a word boundary when possible. */
void muse_hatch_tail_words(const char *src, char *out, size_t cap)
{
    size_t len = strlen(src);
    const char *p = src;
    if (len > CAPTION_CHARS) {
        p = src + len - CAPTION_CHARS;
        const char *sp = strchr(p, ' ');
        if (sp && sp[1] && sp - p < 12) {
            p = sp + 1;
        }
        while ((*p & 0xC0) == 0x80) {
            p++;
        }
    }
    while (*p == ' ') {
        p++;
    }
    strlcpy(out, p, cap);
}

/*
 * The next line of `text` wrapped to `cols` characters as the caption shows
 * them (an ellipsis as three dots, muse_text.h), splitting only words longer
 * than a line. CJK has no spaces: it breaks between characters, but never
 * before closing punctuation.
 */
static bool next_line(const char **text, int cols, const char **start, size_t *len)
{
    const char *p = *text;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    const char *end = p, *brk = NULL, *movable = NULL;
    int n = 0;
    muse_text_cjk_t prev = MUSE_TEXT_NOT_CJK;
    while (*end && *end != '\n') {
        size_t bytes;
        char shown[4];
        int w = muse_text_ascii(end, &bytes, shown);
        w = w < 0 ? 1 : w;
        muse_text_cjk_t cjk = muse_text_cjk(end);
        bool cjk_break = n && cjk != MUSE_TEXT_CJK_CLOSE && (cjk || prev);
        if (n + w > cols && n) {
            if (cjk_break) {
                brk = end;
            } else if (cjk == MUSE_TEXT_CJK_CLOSE && !brk) {
                brk = movable;   /* a full word: its last character goes with the punctuation */
            }
            break;
        }
        if (n && cjk != MUSE_TEXT_CJK_CLOSE) {
            movable = end;
        }
        if (*end == ' ' || cjk_break) {
            brk = end;
        }
        n += w;
        end += bytes;
        prev = cjk;
    }
    if (*end && *end != ' ' && *end != '\n' && brk) {
        end = brk;   /* don't split a word */
    }
    *start = p;
    *len = end - p;
    *text = end;
    return end != p;
}

/*
 * Wraps `text` to the screen's page (muse_state_page) and puts the page holding
 * byte `at` in `out`. Pages overlap by a line: a page's last line starts the
 * next one, so the page turns as that line is reached and nothing is skipped.
 */
bool muse_hatch_caption_at(const char *text, size_t at, char *out, size_t cap)
{
    int cols, lines;
    muse_state_page(muse_text_has_cjk(text), &cols, &lines);
    const char *p = text, *start;
    size_t len;
    int line = -1, n = 0;
    while (next_line(&p, cols, &start, &len)) {
        line = n++;
        if ((size_t)(start + len - text) > at) {
            break;
        }
    }
    if (line < 0) {
        return false;
    }
    int first = lines > 1 ? line / (lines - 1) * (lines - 1) : line;

    size_t o = 0;
    out[0] = '\0';
    p = text;
    for (n = 0; n < first + lines && next_line(&p, cols, &start, &len); n++) {
        if (n < first) {
            continue;
        }
        if (o + 1 + len >= cap) {
            break;   /* whole lines only */
        }
        o += snprintf(out + o, cap - o, "%s%.*s", o ? "\n" : "", (int)len, start);
    }
    return true;
}

/* ---- Reply captions (ported from the CompanionApp captions.dart) ---- */

/* Strips markdown: fenced code blocks become a space, links keep their
 * text, and [*_~`#>-] runs go. Collapses whitespace runs to one space. */
static size_t clean_reply(const char *text, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;   /* collapse leading whitespace too */
    const char *p = text;
    while (*p && o + 1 < cap) {
        if (!strncmp(p, "```", 3)) {
            const char *end = strstr(p + 3, "```");
            p = end ? end + 3 : p + strlen(p);
            if (!space && o + 1 < cap) {
                out[o++] = ' ';
                space = true;
            }
            continue;
        }
        if (*p == '[') {
            const char *mid = strchr(p, ']');
            const char *open = mid ? strchr(mid, '(') : NULL;
            const char *close = open ? strchr(open, ')') : NULL;
            if (mid && open && close && open == mid + 1) {
                p++;   /* emit the link text, skip the URL below */
                while (p < mid && o + 1 < cap) {
                    out[o++] = *p++;
                    space = false;
                }
                p = close + 1;
                continue;
            }
        }
        if (*p == '*' || *p == '_' || *p == '~' || *p == '`' || *p == '#' || *p == '>') {
            while (*p == '*' || *p == '_' || *p == '~' || *p == '`' || *p == '#' || *p == '>') {
                p++;
            }
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            if (!space && o + 1 < cap) {
                out[o++] = ' ';
                space = true;
            }
            p++;
            continue;
        }
        /* Whole UTF-8 characters, so a clip never splits one. */
        size_t n = 1;
        while (n < 4 && (p[n] & 0xC0) == 0x80) {
            n++;
        }
        if (o + n >= cap) {
            break;
        }
        memcpy(out + o, p, n);
        o += n;
        p += n;
        space = false;
    }
    while (o && out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';
    return o;
}

/* Clips to `limit` bytes with a UTF-8 ellipsis, like _clip in captions.dart. */
static void clip_ellipsis(char *out, size_t cap, size_t limit)
{
    size_t len = strlen(out);
    if (len <= limit) {
        return;
    }
    size_t at = limit > 3 ? limit - 3 : 0;
    while (at && (out[at] & 0xC0) == 0x80) {
        at--;
    }
    while (at && out[at - 1] == ' ') {
        at--;
    }
    if (at + 3 < cap) {
        memcpy(out + at, "\xE2\x80\xA6", 3);
        out[at + 3] = '\0';
    } else {
        out[at] = '\0';
    }
}

void muse_caption_from_reply(const char *text, char *out, size_t cap)
{
    if (!cap) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    clean_reply(text, out, cap);
    clip_ellipsis(out, cap, MUSE_CAPTION_LIMIT);
}

void muse_speakable_reply(const char *text, char *out, size_t cap)
{
    if (!cap) {
        return;
    }
    out[0] = '\0';
    if (!text) {
        return;
    }
    clean_reply(text, out, cap);
    clip_ellipsis(out, cap, MUSE_SPEAKABLE_LIMIT);
}

const char *muse_activity_caption(const char *code)
{
    static char generic[48];
    if (!code) {
        return NULL;
    }
    while (*code == ' ' || *code == '\t') {
        code++;
    }
    char key[32];
    size_t i = 0;
    for (const char *p = code; *p && i + 1 < sizeof(key); p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        } else if (c == ' ') {
            c = '_';
        }
        key[i++] = c;
    }
    while (i && (key[i - 1] == '_' || key[i - 1] == '\t')) {
        i--;
    }
    key[i] = '\0';
    if (!key[0]) {
        return NULL;
    }
    static const struct {
        const char *code;
        const char *label;
    } known[] = {
        { "thinking", "Thinking…" },
        { "working", "Working…" },
        { "searching", "Searching…" },
        { "reading", "Reading…" },
        { "writing", "Writing…" },
        { "listening", "Listening…" },
        { "speaking", "Speaking…" },
        { "browsing", "Looking something up…" },
        { "using_tool", "Using a tool…" },
        { "tool", "Using a tool…" },
        { "waiting", "Waiting…" },
    };
    for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        if (!strcmp(key, known[k].code)) {
            return known[k].label;
        }
    }
    size_t o = 0;
    bool word = true;
    for (size_t k = 0; key[k] && o + 1 < sizeof(generic); k++) {
        char c = key[k] == '_' ? ' ' : key[k];
        if (word && c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        word = (c == ' ');
        generic[o++] = c;
    }
    generic[o] = '\0';
    return o ? generic : NULL;
}

static const char *skip_blank(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return s;
}

bool muse_repeat_pick(const char *reply, const char *status, char *out, size_t cap)
{
    if (cap) {
        out[0] = '\0';
    } else {
        return false;
    }
    const char *src = skip_blank(reply ? reply : "");
    if (!*src) {
        src = skip_blank(status ? status : "");
    }
    if (!*src || !strcmp(src, "Listening…")) {
        return false;
    }
    strlcpy(out, src, cap);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\n' || out[n - 1] == '\r')) {
        out[--n] = '\0';
    }
    return out[0] != '\0';
}

static bool has_image_ext(const char *path, size_t len)
{
    static const char *exts[] = { ".png", ".jpg", ".jpeg", ".webp", ".gif" };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        size_t n = strlen(exts[e]);
        if (len < n) {
            continue;
        }
        size_t i = 0;
        while (i < n) {
            char a = path[len - n + i], b = exts[e][i];
            if (a >= 'A' && a <= 'Z') {
                a += 'a' - 'A';
            }
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

bool muse_reply_image_url(const char *text, char *out, size_t cap)
{
    if (cap) {
        out[0] = '\0';
    }
    if (!text || !cap) {
        return false;
    }
    static const char scheme[] = "https://";
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (scheme[i]) {
            char c = p[i];
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
            }
            if (c != scheme[i]) {
                break;
            }
            i++;
        }
        if (scheme[i]) {
            continue;
        }
        const char *end = p;
        while (*end && (unsigned char)*end > 32 && *end != '"' && *end != '\'' && *end != '<' && *end != '>') {
            end++;
        }
        const char *t = end;
        while (t > p) {
            char c = t[-1];
            if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
                t--;
                continue;
            }
            if (c == ')' || c == ']') {
                char open = c == ')' ? '(' : '[';
                int depth = 0;
                for (const char *q = p; q < t; q++) {
                    if (*q == open) {
                        depth++;
                    } else if (*q == c) {
                        depth--;
                    }
                }
                if (depth < 0) {
                    t--;
                    continue;
                }
            }
            break;
        }
        const char *host = p + sizeof(scheme) - 1;
        const char *ps = host;
        while (ps < t && *ps != '/' && *ps != '?' && *ps != '#') {
            ps++;
        }
        if (ps == host) {
            continue;   /* no host */
        }
        const char *pe = ps;
        while (pe < t && *pe != '?' && *pe != '#') {
            pe++;
        }
        if (!has_image_ext(ps, (size_t)(pe - ps))) {
            continue;
        }
        size_t n = (size_t)(t - p);
        if (n + 1 > cap) {
            continue;   /* a URL is exact or useless; try the next one */
        }
        memcpy(out, p, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

static bool is_avatar_key(const char *key, size_t len)
{
    static const char *keys[] = {
        "avatar_url", "avatar", "image_url", "image", "portrait_url", "portrait",
        "character_url", "character_image", "picture_url", "picture",
        "photo_url", "photo", "icon_url",
    };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t n = strlen(keys[k]);
        if (len != n) {
            continue;
        }
        size_t i = 0;
        while (i < n) {
            char c = key[i];
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
            }
            if (c != keys[k][i]) {
                break;
            }
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool is_avatar_url(const char *v, size_t len)
{
    static const char *schemes[] = { "https://", "http://", "data:image/", "data:model/" };
    for (size_t s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++) {
        size_t n = strlen(schemes[s]);
        if (len >= n && !memcmp(v, schemes[s], n)) {
            return true;
        }
    }
    return false;
}

bool muse_identity_avatar_url(const char *json, char *out, size_t cap)
{
    if (cap) {
        out[0] = '\0';
    }
    if (!json || !cap) {
        return false;
    }
    for (const char *p = json; *p; p++) {
        if (*p != '"') {
            continue;
        }
        const char *ke = p + 1;
        while (*ke && *ke != '"') {
            ke++;
        }
        if (!*ke) {
            return false;
        }
        bool want = is_avatar_key(p + 1, (size_t)(ke - p - 1));
        const char *v = skip_blank(ke + 1);
        if (*v != ':') {
            continue;
        }
        v = skip_blank(v + 1);
        if (*v != '"') {
            continue;
        }
        const char *ve = v + 1;
        while (*ve && (*ve != '"' || ve[-1] == '\\')) {
            ve++;
        }
        if (want && *ve && is_avatar_url(v + 1, (size_t)(ve - v - 1))) {
            size_t n = (size_t)(ve - v - 1);
            if (n + 1 <= cap) {
                memcpy(out, v + 1, n);
                out[n] = '\0';
                return true;
            }
        }
        p = *ve ? ve : ve - 1;
    }
    return false;
}

uint32_t muse_speak_claim(uint32_t *slot)
{
    if (!slot) {
        return 0;
    }
    if (++*slot == 0) {
        ++*slot;   /* zero is no generation in flight */
    }
    return *slot;
}

bool muse_speak_is_latest(const uint32_t *slot, uint32_t gen)
{
    return slot && gen && *slot == gen;
}

/* ---- Typed turns on the serial console ---- */

/* JSON-escapes whole characters of *src into out, up to cap - 1 bytes; moves *src past them. */
static size_t escape_some(const char **src, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)*src;
    size_t o = 0;
    while (*p) {
        char esc[8];
        const char *put = esc;
        size_t n = 1, put_len = 2;
        esc[0] = '\\';
        if (*p == '"' || *p == '\\') {
            esc[1] = (char)*p;
        } else if (*p == '\n') {
            esc[1] = 'n';
        } else if (*p == '\r') {
            esc[1] = 'r';
        } else if (*p == '\t') {
            esc[1] = 't';
        } else if (*p < 0x20) {
            put_len = snprintf(esc, sizeof(esc), "\\u%04x", *p);
        } else {
            while (n < 4 && (p[n] & 0xC0) == 0x80) {
                n++;   /* the rest of a UTF-8 character */
            }
            put = (const char *)p;
            put_len = n;
        }
        if (o + put_len >= cap) {
            break;
        }
        memcpy(out + o, put, put_len);
        o += put_len;
        p += n;
    }
    out[o] = '\0';
    *src = (const char *)p;
    return o;
}

/* Each line goes out in one write, so other tasks' log lines land between lines, not inside them. */
void muse_hatch_console(const char *type, const char *text, const char *fields, ...)
{
    static unsigned seq;
    char line[CONSOLE_LINE];
    do {
        size_t n = snprintf(line, sizeof(line), "@chat {\"seq\":%u,\"type\":\"%s\"",
                            __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED), type);
        if (fields) {
            va_list ap;
            va_start(ap, fields);
            line[n++] = ',';
            n += vsnprintf(line + n, sizeof(line) - n, fields, ap);
            va_end(ap);
        }
        if (n > sizeof(line) - CONSOLE_TEXT - 16) {
            n = sizeof(line) - CONSOLE_TEXT - 16;   /* never: fields are short */
        }
        if (text) {
            const char *was = text;
            n += snprintf(line + n, sizeof(line) - n, ",\"text\":\"");
            n += escape_some(&text, line + n, CONSOLE_TEXT);
            line[n++] = '"';
            if (text == was) {
                text = NULL;
            }
        }
        line[n++] = '}';
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
        fflush(stdout);
    } while (text && *text);
}

size_t muse_hatch_unescape(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return o - s;
}
