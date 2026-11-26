/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：可配置精排分页引擎：支持行距/段距/首行缩进/两端对齐、中文避头尾禁则、英文按词折行、EPUB 块样式与 16 级抖动插图混排。
 * English: Configurable rich-typography pagination engine supporting spacing, indent, justification, CJK kinsoku, Latin word wrap, EPUB block styles and 4bpp dithered inline images.
 *
 * 冻结：原文由调用方持有；字体测量和绘制必须由调用方串行化。不在绘图时读取资源或解码图片。
 * 用户扩展：在保留兼容默认布局路径的同时，提供可配置精排样式与正文插图混排/占位/隐藏切换。
 * Frozen: Caller owns source text and serializes all font measurement and drawing; never read files or decode images during draw.
 * User expansion: Preserves legacy default pagination while adding configurable rich typography and inline/placeholder/hidden image modes.
 */
#include "book_layout.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ttf_font.h"

#define PAGE_MAX 4096u
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define HR_BAND_H 20

static const char* s_text;
static size_t s_len;
static size_t* s_pages;
static size_t s_count;
static size_t s_capacity;
static char* s_line;
static EpdRect s_rect;
static int s_px;
static int s_em_width;
static size_t s_scan;
static int64_t s_used;
static bool s_complete;
static bool s_styled;
static book_layout_style_t s_style;

static const blk_t* s_blocks;
static size_t s_block_count;

/* ---- 字重切换封装（宿主单测兼容）/ Weight switch helper (host-test safe) ---- */

static int layout_push_weight(int wght) {
#ifdef ESP_PLATFORM
    if (wght <= 0) return 0;
    int prev = ttf_get_weight();
    if (prev != wght) ttf_set_weight(wght);
    return prev;
#else
    (void)wght;
    return 0;
#endif
}

static void layout_pop_weight(int prev_wght) {
#ifdef ESP_PLATFORM
    if (prev_wght > 0 && ttf_get_weight() != prev_wght) {
        ttf_set_weight(prev_wght);
    }
#else
    (void)prev_wght;
#endif
}

book_layout_style_t book_layout_style_default(void) {
    return (book_layout_style_t){
        .line_spacing_pct = 50,
        .para_spacing_pct = 35,
        .first_line_indent = true,
        .align = BOOK_ALIGN_JUSTIFY,
        .image_mode = BOOK_IMG_INLINE,
        .embedded_style = true,
        .cjk_kinsoku = true,
    };
}

static book_layout_style_t style_legacy(void) {
    return (book_layout_style_t){
        .line_spacing_pct = 50,
        .para_spacing_pct = 50,
        .first_line_indent = false,
        .align = BOOK_ALIGN_LEFT,
        .image_mode = BOOK_IMG_INLINE,
        .embedded_style = false,
        .cjk_kinsoku = false,
    };
}

/* ---- UTF-8 与中文禁则判断 / UTF-8 & CJK kinsoku helpers ---- */

// 拒绝截断、过长编码、代理项和嵌入零字节。/ Reject truncation, overlong encodings, surrogates and embedded NUL.
static size_t codepoint_size(const char* text, size_t remaining) {
    if (!remaining) return 0;
    const unsigned char* p = (const unsigned char*)text;
    if (p[0] > 0 && p[0] < 0x80) return 1;
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
               p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
               p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (size_t i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

static uint32_t decode_cp(const char* s, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    if (n == 1) return p[0];
    if (n == 2) return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if (n == 3) return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static bool is_ascii_word(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           cp == '\'' || cp == '_';
}

// 中文避头标点（禁止出现在行首）。/ No-line-start punctuation (CJK & Latin closing marks).
static bool is_no_start(uint32_t cp) {
    switch (cp) {
        case ',': case '.': case '!': case '?': case ';': case ':':
        case ')': case ']': case '}': case '%':
        case 0x00B7: // ·
        case 0x2014: // —
        case 0x2019: // ’
        case 0x201D: // ”
        case 0x2026: // …
        case 0x2030: // ‰
        case 0x3001: // 、
        case 0x3002: // 。
        case 0x3009: // 〉
        case 0x300B: // 》
        case 0x300D: // 」
        case 0x300F: // 』
        case 0x3011: // 】
        case 0x3015: // 〕
        case 0xFF01: // ！
        case 0xFF09: // ）
        case 0xFF0C: // ，
        case 0xFF0E: // ．
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0xFF1F: // ？
        case 0xFF3D: // ］
        case 0xFF5D: // ｝
            return true;
        default:
            return false;
    }
}

// 中文避尾标点（禁止孤立出现在行尾）。/ No-line-end opening punctuation.
static bool is_no_end(uint32_t cp) {
    switch (cp) {
        case '(': case '[': case '{':
        case 0x2018: // ‘
        case 0x201C: // “
        case 0x3008: // 〈
        case 0x300A: // 《
        case 0x300C: // 「
        case 0x300E: // 『
        case 0x3010: // 【
        case 0x3014: // 〔
        case 0xFF08: // （
        case 0xFF3B: // ［
        case 0xFF5B: // ｛
            return true;
        default:
            return false;
    }
}

/* ---- 块查找与几何计算 / Block lookup and geometry ---- */

// 块表是有序字节区间，二分查找当前行样式。/ Blocks are ordered byte ranges; binary-search the line style.
static const blk_t* block_at(size_t off) {
    if (!s_block_count) return NULL;
    size_t lo = 0, hi = s_block_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_blocks[mid].offset <= off) lo = mid;
        else hi = mid;
    }
    return &s_blocks[lo];
}

static bool is_block_first_line(const blk_t* block, size_t off) {
    if (block) return off == block->offset;
    return off == 0 || s_text[off - 1] == '\n' || s_text[off - 1] == '\r';
}

static bool block_is_image(const blk_t* block) {
    return block && (block->image || block->image_src);
}

static bool block_has_inline_image(const blk_t* block) {
    return block && block->image && s_style.image_mode == BOOK_IMG_INLINE;
}

static bool block_is_hidden_image(const blk_t* block) {
    return s_styled && block_is_image(block) && s_style.image_mode == BOOK_IMG_HIDE;
}

static bool block_has_placeholder(const blk_t* block) {
    if (!block || !block->image_src) return false;
    if (s_styled && s_style.image_mode == BOOK_IMG_HIDE) return false;
    if (s_styled && s_style.image_mode == BOOK_IMG_PLACEHOLDER) return true;
    return block->image == NULL;
}

typedef struct {
    int line_px;
    int weight;
    int left_inset;
    int right_inset;
    int first_indent;
    int avail_w;
    uint8_t align;
    bool is_heading;
} line_metrics_t;

static line_metrics_t compute_line_metrics(const blk_t* block, size_t off, bool first_line) {
    line_metrics_t m = {0};
    if (!s_styled) {
        m.is_heading = block && block->heading;
        m.line_px = s_px + (m.is_heading ? 8 : 0);
        m.align = BLK_ALIGN_LEFT;
        m.avail_w = s_rect.width;
        return m;
    }

    uint8_t hl = 0;
    if (s_style.embedded_style && block) {
        hl = block->heading_level;
        if (!hl && block->heading) hl = 2;
    } else if (block && block->heading) {
        hl = 2;
    }
    m.is_heading = hl >= 1 && hl <= 3;

    if (s_style.embedded_style && block) {
        if (hl == 1) { m.line_px = s_px + 12; m.weight = 650; }
        else if (hl == 2) { m.line_px = s_px + 8; m.weight = 600; }
        else if (hl == 3) { m.line_px = s_px + 4; m.weight = 600; }
        else if (hl >= 4) { m.line_px = s_px; m.weight = 600; }
        else {
            m.line_px = block->quote && s_px > 24 ? s_px - 2 : s_px;
            if (block->bold) m.weight = 600;
        }
        if (block->quote) { m.left_inset += 26; m.right_inset += 12; }
        if (block->list_item) { m.left_inset += 28; }
        if (m.left_inset + m.right_inset > s_rect.width / 2) {
            m.left_inset = s_rect.width / 4;
            m.right_inset = 0;
        }
        if (block->align != BLK_ALIGN_DEFAULT) {
            m.align = block->align;
        } else {
            m.align = s_style.align == BOOK_ALIGN_JUSTIFY ? BLK_ALIGN_JUSTIFY : BLK_ALIGN_LEFT;
        }
    } else {
        m.line_px = s_px + (m.is_heading ? 8 : 0);
        m.align = s_style.align == BOOK_ALIGN_JUSTIFY ? BLK_ALIGN_JUSTIFY : BLK_ALIGN_LEFT;
    }

    int base_w = s_rect.width - m.left_inset - m.right_inset;
    if (base_w < m.line_px) base_w = s_rect.width;

    // 正文首行缩进（2 个全角字宽），标题/引用/居中/右对齐/text-indent:0/已有全角空格跳过。
    // Body first-line 2-em indent; skipped for headings, quotes, centered/right blocks, text-indent:0, or existing indent.
    if (s_style.first_line_indent && first_line && !block_is_image(block)) {
        bool skip = m.is_heading || hl > 0 ||
                    (s_style.embedded_style && block && (block->quote || block->list_item || block->no_indent)) ||
                    m.align == BLK_ALIGN_CENTER || m.align == BLK_ALIGN_RIGHT;
        if (!skip && off + 3 <= s_len &&
            (unsigned char)s_text[off] == 0xE3 && (unsigned char)s_text[off + 1] == 0x80 &&
            (unsigned char)s_text[off + 2] == 0x80) {
            skip = true;
        }
        if (!skip && off + 2 <= s_len && s_text[off] == ' ' && s_text[off + 1] == ' ') {
            skip = true;
        }
        if (!skip) {
            int ind = s_em_width > 0 ? s_em_width * 2 : s_px * 2;
            if (ind > base_w / 3) ind = base_w / 3;
            m.first_indent = ind;
        }
    }

    m.avail_w = base_w - m.first_indent;
    if (m.avail_w < 1) m.avail_w = 1;
    return m;
}

// 插图等比适配正文区域，不放大；整块换页，不切断图片。
// Fit illustrations proportionally without upscaling; move whole images across page boundaries.
static void image_size(const blk_t* block, int* width, int* height) {
    *width = block->image_width; *height = block->image_height;
    int max_h = s_rect.height;
    if (s_styled && max_h > 64) max_h -= 16;
    if (*width > s_rect.width) { *height = (int)((int64_t)*height * s_rect.width / *width); *width = s_rect.width; }
    if (*height > max_h) { *width = (int)((int64_t)*width * max_h / *height); *height = max_h; }
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

typedef struct {
    int top_pad;
    int body_h;
    int bot_pad;
    bool draw_hr_before;
    bool draw_hr_after;
} row_box_t;

static row_box_t compute_row_box(
    const blk_t* block, bool first_line, bool paragraph_end, int line_px, bool heading, int64_t used_on_page
) {
    row_box_t b = {0};
    if (block_is_hidden_image(block)) return b;

    if (!s_styled) {
        if (block && block->image) {
            int w, h; image_size(block, &w, &h);
            b.body_h = h;
        } else if (block && block->image_src) {
            b.body_h = 2 * s_px < s_rect.height ? 2 * s_px : s_rect.height;
        } else {
            b.body_h = line_px + line_px / 2;
        }
        if (paragraph_end) b.bot_pad = b.body_h / (heading ? 2 : 3);
        return b;
    }

    if (s_style.embedded_style && block && first_line && block->rule_before) {
        b.draw_hr_before = true;
        b.top_pad += HR_BAND_H;
    }
    if (s_style.embedded_style && block && first_line && heading && used_on_page > 0) {
        b.top_pad += s_px / 3;
    }

    if (block_has_inline_image(block)) {
        int w, h; image_size(block, &w, &h);
        b.body_h = h;
        if (b.top_pad + b.body_h + 16 <= s_rect.height) {
            b.top_pad += 8;
            b.bot_pad += 8;
        }
    } else if (block_has_placeholder(block)) {
        b.body_h = 2 * s_px < s_rect.height ? 2 * s_px : s_rect.height;
    } else {
        uint8_t lp = s_style.line_spacing_pct ? s_style.line_spacing_pct : 50;
        b.body_h = line_px + (line_px * (int)lp + 50) / 100;
    }

    if (paragraph_end) {
        int pp = s_style.para_spacing_pct;
        if (heading) {
            b.bot_pad += (line_px * (pp + 25) + 50) / 100;
        } else if (block_has_inline_image(block)) {
            b.bot_pad += (s_px * pp + 50) / 100;
        } else {
            b.bot_pad += (s_px * pp + 50) / 100;
        }
        if (s_style.embedded_style && block && block->rule_after) {
            b.draw_hr_after = true;
            b.bot_pad += HR_BAND_H;
        }
    }
    return b;
}

static EpdRect placeholder_rect(EpdRect body, int top, int height) {
    int pad = body.width > 24 && height > 16 ? 8 : 0;
    return (EpdRect){body.x + pad, body.y + top + pad / 2, body.width - 2 * pad, height - pad};
}

static void draw_placeholder(uint8_t* fb, const blk_t* block, EpdRect rect) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        epd_draw_pixel(x, rect.y, 0, fb); epd_draw_pixel(x, rect.y + rect.height - 1, 0, fb);
    }
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        epd_draw_pixel(rect.x, y, 0, fb); epd_draw_pixel(rect.x + rect.width - 1, y, 0, fb);
    }
    int px = s_px < 32 ? s_px : 32;
    bool repeat = block->image_repeated && !block->image_title;
    const char* label = block->image_title ? "标题图 · 点击查看" : repeat ? "重复图片 · 点击查看" : "点击查看图片";
    if (ttf_text_width_px(px, label) > rect.width - 16) return;
    int lines = repeat ? 2 : 1;
    int top = rect.y + (rect.height - lines * (px + 4)) / 2;
    ttf_draw_text_px(fb, rect.x + rect.width / 2, top + ttf_ascender_px(px), px,
                     label, EPD_DRAW_ALIGN_CENTER, 0, 15);
    if (repeat) {
        char origin[64]; snprintf(origin, sizeof(origin), "已读最早：第 %u 节", (unsigned)block->image_first_chapter + 1);
        int small = px < 24 ? px : 24;
        if (ttf_text_width_px(small, origin) <= rect.width - 16)
            ttf_draw_text_px(fb, rect.x + rect.width / 2, top + px + 4 + ttf_ascender_px(small), small,
                             origin, EPD_DRAW_ALIGN_CENTER, 0, 15);
    }
}

// 16 级灰阶 Floyd-Steinberg 误差扩散绘制插图，减少墨水屏色带。
// Render inline grayscale images with 16-level Floyd-Steinberg dithering to avoid e-ink banding.
static void draw_image_dithered(
    uint8_t* fb, const blk_t* block, int left, int top, int dst_w, int dst_h
) {
    if (!s_styled) {
        for (int y = 0; y < dst_h; ++y) for (int x = 0; x < dst_w; ++x) {
            size_t src = (size_t)((int64_t)y * block->image_height / dst_h) * block->image_width +
                         (size_t)((int64_t)x * block->image_width / dst_w);
            epd_draw_pixel(left + x, top + y, block->image[src], fb);
        }
        return;
    }

    int stride = dst_w + 2;
    int16_t* err_buf = calloc((size_t)stride * 2, sizeof(int16_t));
    if (!err_buf) {
        for (int y = 0; y < dst_h; ++y) for (int x = 0; x < dst_w; ++x) {
            size_t src = (size_t)((int64_t)y * block->image_height / dst_h) * block->image_width +
                         (size_t)((int64_t)x * block->image_width / dst_w);
            epd_draw_pixel(left + x, top + y, block->image[src], fb);
        }
        return;
    }

    int16_t* cur = err_buf;
    int16_t* nxt = err_buf + stride;
    for (int y = 0; y < dst_h; ++y) {
        memset(nxt, 0, (size_t)stride * sizeof(int16_t));
        size_t sy = (size_t)((int64_t)y * block->image_height / dst_h);
        const uint8_t* src_row = block->image + sy * block->image_width;
        for (int x = 0; x < dst_w; ++x) {
            size_t sx = (size_t)((int64_t)x * block->image_width / dst_w);
            int val = (int)src_row[sx] + cur[x + 1];
            if (val < 0) val = 0;
            else if (val > 255) val = 255;
            // 纯白保持 0xFF 避免底噪，其余按 16 级量化并扩散误差。
            // Preserve pure white to avoid background speckle; diffuse 16-level quantization error elsewhere.
            int q = (src_row[sx] >= 252) ? 15 : (val + 8) / 17;
            if (q > 15) q = 15;
            int out_g = q * 17;
            epd_draw_pixel(left + x, top + y, (uint8_t)out_g, fb);
            if (src_row[sx] < 252) {
                int err = val - out_g;
                cur[x + 2] = (int16_t)(cur[x + 2] + (err * 7) / 16);
                nxt[x]     = (int16_t)(nxt[x]     + (err * 3) / 16);
                nxt[x + 1] = (int16_t)(nxt[x + 1] + (err * 5) / 16);
                nxt[x + 2] = (int16_t)(nxt[x + 2] + (err * 1) / 16);
            }
        }
        int16_t* tmp = cur; cur = nxt; nxt = tmp;
    }
    free(err_buf);
}

static void draw_hr_line(uint8_t* fb, EpdRect rect, int y_center) {
    int w = (rect.width * 50) / 100;
    if (w < 40) w = rect.width;
    int x0 = rect.x + (rect.width - w) / 2;
    for (int x = x0; x < x0 + w; ++x) {
        epd_draw_pixel(x, y_center, 0x80, fb);
        epd_draw_pixel(x, y_center + 1, 0x80, fb);
    }
}

void book_layout_free(void) {
    free(s_pages);
    free(s_line);
    s_pages = NULL;
    s_line = NULL;
    s_text = NULL;
    s_count = s_capacity = s_len = 0;
    s_px = 0;
    s_em_width = 0;
    s_scan = 0;
    s_used = 0;
    s_complete = false;
    s_styled = false;
    s_blocks = NULL;
    s_block_count = 0;
}

static bool append_page(size_t off) {
    if (s_count == PAGE_MAX) return false;
    if (s_count == s_capacity) {
        size_t cap = s_capacity ? s_capacity * 2 : 16;
        size_t* pages = heap_caps_realloc(s_pages, cap * sizeof(*pages), PSRAM_CAPS);
        if (!pages) return false;
        s_pages = pages;
        s_capacity = cap;
    }
    s_pages[s_count++] = off;
    return true;
}

/* ---- 折行与行内对齐/两端对齐布局 / Wrapping & per-line placement ---- */

typedef struct {
    size_t vis_end;
    size_t next;
    bool paragraph_end;
    bool first_line;
    line_metrics_t m;
} wrapped_line_t;

// 折行时保留原文字节位置；CRLF 算一个段落边界；精排模式支持中文禁则与英文按词折行。
// Preserve source offsets while wrapping; rich mode adds CJK kinsoku and Latin word wrapping.
static bool take_line(size_t off, wrapped_line_t* out) {
    memset(out, 0, sizeof(*out));
    const blk_t* block = block_at(off);
    out->first_line = is_block_first_line(block, off);
    out->m = compute_line_metrics(block, off, out->first_line);
    size_t limit = block ? block->offset + block->len : s_len;
    s_line[0] = 0;

    if (block_is_image(block)) {
        out->m.is_heading = false;
        out->vis_end = off;
        out->next = limit + (limit < s_len && s_text[limit] == '\n');
        out->paragraph_end = true;
        return out->next > off;
    }

    int prev_w = layout_push_weight(out->m.weight);
    size_t end = off;
    size_t prev_end = off;
    size_t prev_prev_end = off;
    uint32_t last_cp = 0;
    uint32_t prev_last_cp = 0;
    size_t last_word_break = off;
    int64_t last_word_break_w = 0;
    int64_t width = 0;
    bool overflowed = false;

    while (end < limit && s_text[end] != '\r' && s_text[end] != '\n') {
        size_t n = codepoint_size(s_text + end, s_len - end);
        if (!n) { layout_pop_weight(prev_w); return false; }
        uint32_t cp = decode_cp(s_text + end, n);
        char glyph[5];
        memcpy(glyph, s_text + end, n);
        glyph[n] = 0;
        int64_t gw = ttf_text_width_px(out->m.line_px, glyph);
        if (gw < 0) { layout_pop_weight(prev_w); return false; }
        int64_t candidate = width + gw;
        if (candidate > out->m.avail_w) {
            if (end == off) { layout_pop_weight(prev_w); return false; }
            overflowed = true;
            if (s_styled && s_style.cjk_kinsoku) {
                // 1) 英文单词不断词：在词边界回退（前提是不让本行过短）。
                // 1) Latin word wrap: backtrack to last break opportunity if line stays >= 35% full.
                if (is_ascii_word(cp) && is_ascii_word(last_cp) &&
                    last_word_break > off && last_word_break_w * 100 >= (int64_t)out->m.avail_w * 35) {
                    end = last_word_break;
                    // 重新定位 last_cp 供禁尾检查 / Recompute last_cp for no-end check
                    last_cp = 0;
                    prev_end = off;
                    for (size_t p = off; p < end;) {
                        size_t pn = codepoint_size(s_text + p, end - p);
                        if (!pn) break;
                        prev_end = p;
                        last_cp = decode_cp(s_text + p, pn);
                        p += pn;
                    }
                } else {
                    // 2) 避头禁则：下一行首为闭标点时回退一字，让前字与标点同入下行。
                    // 2) No-line-start kinsoku: push preceding char to next line with closing punctuation.
                    if (is_no_start(cp) && prev_end > off && !is_no_end(prev_last_cp)) {
                        end = prev_end;
                        last_cp = prev_last_cp;
                        prev_end = prev_prev_end;
                    }
                }
                // 3) 避尾禁则：本行末为开括号/左引号时推入下一行。
                // 3) No-line-end kinsoku: push lone opening punctuation to next line.
                if (end > off && is_no_end(last_cp) && prev_end > off) {
                    end = prev_end;
                }
            }
            break;
        }
        prev_prev_end = prev_end;
        prev_last_cp = last_cp;
        prev_end = end;
        last_cp = cp;
        width = candidate;
        end += n;
        if (!is_ascii_word(cp)) {
            last_word_break = end;
            last_word_break_w = width;
        }
    }
    layout_pop_weight(prev_w);

    size_t vis_end = end;
    if (s_styled && overflowed) {
        // 折行处的尾部空白不计入可见宽度。/ Trim trailing ASCII spaces at wrapped line break.
        while (vis_end > off && s_text[vis_end - 1] == ' ') --vis_end;
    }

    size_t copy_len = vis_end - off;
    if (copy_len > 0) memcpy(s_line, s_text + off, copy_len);
    s_line[copy_len] = 0;

    out->vis_end = vis_end;
    out->next = end;
    if (s_styled && overflowed && out->next < limit && s_text[out->next] == ' ') {
        // 吞掉折行处单个空格分隔符，避免下一行行首多出空格。
        // Consume a single space separator at a wrapped break so the next line starts flush.
        out->next++;
    }
    if (out->next < s_len && (s_text[out->next] == '\r' || s_text[out->next] == '\n')) {
        size_t nl = out->next;
        out->next = nl + 1;
        if (s_text[nl] == '\r' && out->next < s_len && s_text[out->next] == '\n') out->next++;
        out->paragraph_end = true;
    } else if (out->next >= limit) {
        out->paragraph_end = true;
    }
    return out->next > off;
}

typedef struct {
    int x_start;
    int extra_per_gap;
    int extra_rem;
    bool justify_all_glyphs; // true: CJK 逐字距均分; false: 仅空格均分
    bool justified;
} line_placement_t;

// 计算单行的水平起点与两端对齐字距参数（绘制/命中/下划线共用，保证像素级一致）。
// Compute horizontal start and justification spacing (shared by draw, hit-test, and underlines).
static line_placement_t compute_line_placement(const wrapped_line_t* ln, size_t off, EpdRect rect) {
    line_placement_t pl = {0};
    int base_x = rect.x + ln->m.left_inset + ln->m.first_indent;
    pl.x_start = base_x;
    if (!s_styled || ln->vis_end <= off) return pl;

    uint8_t al = ln->m.align;
    if (al == BLK_ALIGN_LEFT) return pl;

    // 测量可见文字自然宽与可分配间隙数。/ Measure natural width and justifiable gaps.
    int prev_w = layout_push_weight(ln->m.weight);
    int nat_w = 0;
    int glyph_count = 0;
    int space_count = 0;
    bool has_cjk = false;
    for (size_t cur = off; cur < ln->vis_end;) {
        size_t n = codepoint_size(s_text + cur, ln->vis_end - cur);
        if (!n) break;
        uint32_t cp = decode_cp(s_text + cur, n);
        if (cp >= 0x2E80) has_cjk = true;
        if (cp == ' ') ++space_count;
        char glyph[5];
        memcpy(glyph, s_text + cur, n);
        glyph[n] = 0;
        int gw = ttf_text_width_px(ln->m.line_px, glyph);
        if (gw > 0) nat_w += gw;
        ++glyph_count;
        cur += n;
    }
    layout_pop_weight(prev_w);

    int avail = ln->m.avail_w;
    if (al == BLK_ALIGN_CENTER) {
        int dx = (avail - nat_w) / 2;
        if (dx > 0) pl.x_start = base_x + dx;
        return pl;
    }
    if (al == BLK_ALIGN_RIGHT) {
        int dx = avail - nat_w;
        if (dx > 0) pl.x_start = base_x + dx;
        return pl;
    }
    if (al == BLK_ALIGN_JUSTIFY && !ln->paragraph_end && avail > nat_w) {
        int slack = avail - nat_w;
        // 余量超过行宽 35% 时不强行拉伸，避免字距过稀。/ Skip stretching if slack exceeds 35% of line width.
        if (slack * 100 <= avail * 35) {
            int gaps = has_cjk ? (glyph_count - 1) : space_count;
            if (gaps >= 2 || (!has_cjk && gaps >= 1)) {
                pl.justified = true;
                pl.justify_all_glyphs = has_cjk;
                pl.extra_per_gap = slack / gaps;
                pl.extra_rem = slack % gaps;
            }
        }
    }
    return pl;
}

static int gap_extra_after(const line_placement_t* pl, uint32_t cp, int* gap_idx) {
    if (!pl->justified) return 0;
    if (!pl->justify_all_glyphs && cp != ' ') return 0;
    int idx = (*gap_idx)++;
    return pl->extra_per_gap + (idx < pl->extra_rem ? 1 : 0);
}

/* ---- 公开分页接口 / Public pagination API ---- */

bool book_layout_build(const char* utf8, size_t len, EpdRect rect, int px) {
    return book_layout_build_blocks(utf8, len, NULL, 0, rect, px);
}

bool book_layout_build_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px) {
    return book_layout_begin_blocks(utf8, len, blocks, count, rect, px) && book_layout_extend(PAGE_MAX);
}

bool book_layout_begin_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px) {
    return book_layout_begin_blocks_styled(utf8, len, blocks, count, rect, px, NULL);
}

bool book_layout_begin_blocks_styled(
    const char* utf8, size_t len, const blk_t* blocks, size_t count,
    EpdRect rect, int px, const book_layout_style_t* style
) {
    book_layout_free();
    if ((!utf8 && len) || len == SIZE_MAX || px <= 0 || px > INT_MAX / 3 ||
        rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) return false;

    s_styled = (style != NULL);
    s_style = style ? *style : style_legacy();
    if (s_style.line_spacing_pct < 20 || s_style.line_spacing_pct > 150) s_style.line_spacing_pct = 50;
    if (s_style.para_spacing_pct > 150) s_style.para_spacing_pct = 35;

    int line_height = s_styled
        ? (px + (px * (int)s_style.line_spacing_pct + 50) / 100)
        : (px + px / 2);
    if (line_height > rect.height) return false;

    for (size_t off = 0; off < len;) {
        size_t n = codepoint_size(utf8 + off, len - off);
        if (!n) return false;
        off += n;
    }
    if (count) {
        if (!blocks || count > HTML_TEXT_MAX_BLOCKS) return false;
        size_t expected = 0;
        for (size_t i = 0; i < count; ++i) {
            const blk_t* b = &blocks[i];
            if (b->image && (!b->image_width || !b->image_height)) return false;
            if (b->offset != expected || b->offset >= len || !b->len || b->len > len - b->offset ||
                ((unsigned char)utf8[b->offset] & 0xc0) == 0x80) return false;
            size_t end = b->offset + b->len;
            if (i + 1 < count) {
                if (end >= len || utf8[end] != '\n') return false;
                expected = end + 1;
            } else if (end != len) return false;
        }
    }
    s_blocks = blocks;
    s_block_count = count;
    s_text = utf8;
    s_len = len;
    s_px = px;
    s_rect = rect;
    s_em_width = s_styled ? ttf_text_width_px(px, "\xe4\xb8\xad") : px; // "中"
    if (s_em_width <= 0) s_em_width = px;
    s_line = heap_caps_malloc(len + 1, PSRAM_CAPS);
    if (!s_line || !append_page(0)) goto fail;
    return book_layout_extend(2);
fail:
    book_layout_free();
    return false;
}

bool book_layout_extend(size_t pages) {
    if (!s_count) return false;
    if (!pages || s_complete) return true;
    size_t target = pages > PAGE_MAX - book_layout_page_count() ? PAGE_MAX : book_layout_page_count() + pages;
    while (s_scan < s_len) {
        wrapped_line_t ln;
        if (!take_line(s_scan, &ln)) goto fail;
        const blk_t* block = block_at(s_scan);
        if (block_is_hidden_image(block)) {
            s_scan = ln.next;
            continue;
        }
        row_box_t box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, s_used);
        int need_h = box.top_pad + box.body_h;
        if (need_h > s_rect.height) {
            // 行首重算 top_pad 后再校验一次。/ Recompute without mid-page top_pad before rejecting.
            box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, 0);
            need_h = box.top_pad + box.body_h;
            if (need_h > s_rect.height) goto fail;
        }
        if (s_used > 0 && s_used + need_h > s_rect.height) {
            if (!append_page(s_scan)) goto fail;
            s_used = 0;
            if (book_layout_page_count() >= target) return true;
            box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, 0);
            need_h = box.top_pad + box.body_h;
        }
        s_used += need_h + box.bot_pad;
        s_scan = ln.next;
    }
    s_complete = true;
    return true;
fail:
    book_layout_free();
    return false;
}

size_t book_layout_page_count(void) { return s_count ? s_count - !s_complete : 0; }
bool book_layout_complete(void) { return s_complete; }

size_t book_layout_page_start_offset(size_t page) {
    return page < s_count ? s_pages[page] : s_len;
}

size_t book_layout_page_for_offset(size_t off) {
    size_t lo = 0, hi = book_layout_page_count();
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_pages[mid] <= off) lo = mid;
        else hi = mid;
    }
    return lo;
}

/* ---- 正文、块装饰与插图绘制 / Body, block decoration, and image drawing ---- */

void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px) {
    if (!fb || page >= book_layout_page_count() || px != s_px || rect.width != s_rect.width ||
        rect.height != s_rect.height || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) return;
    size_t off = s_pages[page];
    size_t end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;
    while (off < end) {
        wrapped_line_t ln;
        if (!take_line(off, &ln)) return;
        const blk_t* block = block_at(off);
        if (block_is_hidden_image(block)) {
            off = ln.next;
            continue;
        }
        row_box_t box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, used);
        if (used + box.top_pad + box.body_h > rect.height) return;

        if (box.draw_hr_before) {
            draw_hr_line(fb, rect, rect.y + (int)used + HR_BAND_H / 2);
        }

        int line_top = rect.y + (int)used + box.top_pad;
        if (block_has_inline_image(block)) {
            int img_w = 0, img_h = 0;
            image_size(block, &img_w, &img_h);
            int left = rect.x + (rect.width - img_w) / 2;
            draw_image_dithered(fb, block, left, line_top, img_w, img_h);
        } else if (block_has_placeholder(block)) {
            draw_placeholder(fb, block, placeholder_rect(rect, (int)used + box.top_pad, box.body_h));
        } else if (s_line[0]) {
            // 引用块左侧竖条 / Vertical accent bar for blockquotes
            if (s_styled && s_style.embedded_style && block && block->quote) {
                int bar_h = ln.paragraph_end ? box.body_h : box.body_h + 2;
                if (line_top + bar_h > rect.y + rect.height) bar_h = rect.y + rect.height - line_top;
                for (int y = line_top + 2; y < line_top + bar_h - 2; ++y) {
                    for (int x = rect.x + 8; x < rect.x + 12; ++x) {
                        epd_draw_pixel(x, y, 0x70, fb);
                    }
                }
            }
            // 列表项圆点标记 / Bullet marker on the first line of a list item
            if (s_styled && s_style.embedded_style && block && block->list_item && ln.first_line) {
                int bx = rect.x + 10;
                int by = line_top + ttf_ascender_px(ln.m.line_px) - ln.m.line_px / 3;
                for (int dy = -3; dy <= 3; ++dy) {
                    for (int dx = -3; dx <= 3; ++dx) {
                        if (dx * dx + dy * dy <= 10) epd_draw_pixel(bx + dx, by + dy, 0, fb);
                    }
                }
            }

            line_placement_t pl = compute_line_placement(&ln, off, rect);
            int prev_w = layout_push_weight(ln.m.weight);
            int baseline_y = line_top + ttf_ascender_px(ln.m.line_px);
            if (!pl.justified) {
                ttf_draw_text_px(fb, pl.x_start, baseline_y, ln.m.line_px,
                                 s_line, EPD_DRAW_ALIGN_LEFT, 0, 15);
            } else {
                int gx = pl.x_start;
                int gap_idx = 0;
                for (size_t cur = off; cur < ln.vis_end;) {
                    size_t n = codepoint_size(s_text + cur, ln.vis_end - cur);
                    if (!n) break;
                    uint32_t cp = decode_cp(s_text + cur, n);
                    char glyph[5];
                    memcpy(glyph, s_text + cur, n);
                    glyph[n] = 0;
                    ttf_draw_text_px(fb, gx, baseline_y, ln.m.line_px,
                                     glyph, EPD_DRAW_ALIGN_LEFT, 0, 15);
                    int gw = ttf_text_width_px(ln.m.line_px, glyph);
                    if (gw > 0) gx += gw;
                    if (cur + n < ln.vis_end) gx += gap_extra_after(&pl, cp, &gap_idx);
                    cur += n;
                }
            }
            layout_pop_weight(prev_w);
        }

        if (box.draw_hr_after) {
            int hr_y = line_top + box.body_h + box.bot_pad - HR_BAND_H / 2;
            if (hr_y + 2 <= rect.y + rect.height) draw_hr_line(fb, rect, hr_y);
        }

        used += box.top_pad + box.body_h + box.bot_pad;
        off = ln.next;
    }
}

size_t book_layout_image_at(size_t page, EpdRect rect, int x, int y, EpdRect* hit) {
    if (page >= book_layout_page_count() || rect.width != s_rect.width || rect.height != s_rect.height ||
        rect.x < 0 || rect.y < 0 || rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height ||
        x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) return SIZE_MAX;
    size_t off = s_pages[page], end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;
    while (off < end) {
        wrapped_line_t ln;
        if (!take_line(off, &ln)) return SIZE_MAX;
        const blk_t* block = block_at(off);
        if (block_is_hidden_image(block)) {
            off = ln.next;
            continue;
        }
        row_box_t box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, used);
        if (used + box.top_pad + box.body_h > rect.height) return SIZE_MAX;
        if (block_has_placeholder(block)) {
            EpdRect area = placeholder_rect(rect, (int)used + box.top_pad, box.body_h);
            if (x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height) {
                if (hit) *hit = area;
                return (size_t)(block - s_blocks);
            }
        } else if (s_styled && block_has_inline_image(block)) {
            int img_w = 0, img_h = 0;
            image_size(block, &img_w, &img_h);
            EpdRect area = { rect.x + (rect.width - img_w) / 2, rect.y + (int)used + box.top_pad, img_w, img_h };
            if (x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height) {
                if (hit) *hit = area;
                return (size_t)(block - s_blocks);
            }
        }
        used += box.top_pad + box.body_h + box.bot_pad;
        off = ln.next;
    }
    return SIZE_MAX;
}

/* ---- 字符级坐标反查与下划线绘制 / Character hit-test and highlight underlines ---- */

bool book_layout_char_at(
    size_t page, EpdRect rect, int x, int y,
    size_t* out_start, size_t* out_end, EpdRect* out_glyph_rect
) {
    if (page >= book_layout_page_count() || rect.width != s_rect.width || rect.height != s_rect.height ||
        rect.x < 0 || rect.y < 0 || rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) {
        return false;
    }
    size_t off = s_pages[page];
    size_t end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;

    // 若手指略超出上下边界，夹到正文区首行或末行，提升拖选连贯性。
    // Clamp y inside the body rect to keep drag selection smooth near edges.
    int cy = y < rect.y ? rect.y : (y >= rect.y + rect.height ? rect.y + rect.height - 1 : y);
    int cx = x < rect.x ? rect.x : (x >= rect.x + rect.width ? rect.x + rect.width - 1 : x);

    bool have_best = false;
    size_t best_s = 0, best_e = 0;
    EpdRect best_r = {0};

    while (off < end) {
        wrapped_line_t ln;
        if (!take_line(off, &ln)) break;
        const blk_t* block = block_at(off);
        if (block_is_hidden_image(block)) {
            off = ln.next;
            continue;
        }
        row_box_t box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, used);
        if (used + box.top_pad + box.body_h > rect.height) break;
        int line_top = rect.y + (int)used + box.top_pad;
        int line_bot = line_top + box.body_h + box.bot_pad;

        if (!block_is_image(block) && ln.vis_end > off) {
            line_placement_t pl = compute_line_placement(&ln, off, rect);
            int prev_w = layout_push_weight(ln.m.weight);
            int gx = pl.x_start;
            int gap_idx = 0;
            size_t cur = off;
            size_t line_best_s = off, line_best_e = off;
            EpdRect line_best_r = { pl.x_start, line_top, 1, box.body_h };

            while (cur < ln.vis_end) {
                size_t n = codepoint_size(s_text + cur, ln.vis_end - cur);
                if (!n) break;
                uint32_t cp = decode_cp(s_text + cur, n);
                char glyph[5];
                memcpy(glyph, s_text + cur, n);
                glyph[n] = 0;
                int gw = ttf_text_width_px(ln.m.line_px, glyph);
                if (gw < 1) gw = 1;
                int gap = (cur + n < ln.vis_end) ? gap_extra_after(&pl, cp, &gap_idx) : 0;
                line_best_s = cur;
                line_best_e = cur + n;
                line_best_r = (EpdRect){ gx, line_top, gw, box.body_h };
                if (cx < gx + gw + gap) break;
                gx += gw + gap;
                cur += n;
            }
            layout_pop_weight(prev_w);

            best_s = line_best_s;
            best_e = line_best_e;
            best_r = line_best_r;
            have_best = true;
            if (cy < line_bot) break;
        }
        used += box.top_pad + box.body_h + box.bot_pad;
        off = ln.next;
    }

    if (!have_best) return false;
    if (out_start) *out_start = best_s;
    if (out_end) *out_end = best_e;
    if (out_glyph_rect) *out_glyph_rect = best_r;
    return true;
}

size_t book_layout_span_underlines(
    size_t page, EpdRect rect, size_t start_off, size_t end_off,
    EpdRect* out_rects, size_t max_rects
) {
    if (!out_rects || !max_rects || end_off <= start_off || page >= book_layout_page_count() ||
        rect.width != s_rect.width || rect.height != s_rect.height) {
        return 0;
    }
    size_t off = s_pages[page];
    size_t end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    if (end_off <= off || start_off >= end) return 0;

    int64_t used = 0;
    size_t count = 0;
    while (off < end && count < max_rects) {
        wrapped_line_t ln;
        if (!take_line(off, &ln)) break;
        const blk_t* block = block_at(off);
        if (block_is_hidden_image(block)) {
            off = ln.next;
            continue;
        }
        row_box_t box = compute_row_box(block, ln.first_line, ln.paragraph_end, ln.m.line_px, ln.m.is_heading, used);
        if (used + box.top_pad + box.body_h > rect.height) break;

        if (!block_is_image(block) && ln.vis_end > off) {
            size_t seg_s = start_off > off ? start_off : off;
            size_t seg_e = end_off < ln.vis_end ? end_off : ln.vis_end;
            if (seg_e > seg_s) {
                line_placement_t pl = compute_line_placement(&ln, off, rect);
                int prev_w = layout_push_weight(ln.m.weight);
                int x0 = pl.x_start;
                int x1 = pl.x_start;
                int gx = pl.x_start;
                int gap_idx = 0;
                size_t cur = off;
                while (cur < ln.vis_end) {
                    size_t n = codepoint_size(s_text + cur, ln.vis_end - cur);
                    if (!n) break;
                    uint32_t cp = decode_cp(s_text + cur, n);
                    char glyph[5];
                    memcpy(glyph, s_text + cur, n);
                    glyph[n] = 0;
                    int gw = ttf_text_width_px(ln.m.line_px, glyph);
                    if (gw < 0) gw = 0;
                    if (cur <= seg_s) x0 = gx;
                    gx += gw;
                    if (cur < seg_e) x1 = gx;
                    if (cur + n < ln.vis_end) gx += gap_extra_after(&pl, cp, &gap_idx);
                    cur += n;
                }
                layout_pop_weight(prev_w);
                if (x1 > x0) {
                    int line_top = rect.y + (int)used + box.top_pad;
                    int uy = line_top + ttf_ascender_px(ln.m.line_px) + 6;
                    if (uy + 3 > line_top + box.body_h) uy = line_top + box.body_h - 3;
                    out_rects[count++] = (EpdRect){ x0, uy, x1 - x0, 3 };
                }
            }
        }
        used += box.top_pad + box.body_h + box.bot_pad;
        off = ln.next;
    }
    return count;
}

void book_layout_draw_page_highlights(
    uint8_t* fb, size_t page, EpdRect rect, int px,
    const book_layout_span_t* spans, size_t span_count
) {
    book_layout_draw_page(fb, page, rect, px);
    if (!fb || !spans || !span_count) return;
    EpdRect bars[32];
    for (size_t i = 0; i < span_count; ++i) {
        size_t n = book_layout_span_underlines(
            page, rect, spans[i].start_off, spans[i].end_off, bars, 32
        );
        for (size_t j = 0; j < n; ++j) {
            for (int y = bars[j].y; y < bars[j].y + bars[j].height; ++y) {
                for (int x = bars[j].x; x < bars[j].x + bars[j].width; ++x) {
                    epd_draw_pixel(x, y, 0, fb);
                }
            }
        }
    }
}
