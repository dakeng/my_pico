/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：单遍提取章节文字，折叠空白并保留非空块、标题层级、轻量 CSS 样式和图片占位。
 * English: Extract chapter text in one pass, preserving nonempty blocks, heading levels, lightweight CSS styles and image placeholders.
 *
 * 冻结：不执行脚本、不加载外部资源；输出有界，失败释放全部临时分配。
 * 用户扩展：支持外部/内联 CSS 子集与语义块元数据，保持纯文本 UTF-8 偏移与既有行为一致。
 * Frozen: Never execute scripts or load external resources; bound output and release temporary allocations on failure.
 * User expansion: Supports a safe CSS subset and semantic block metadata while keeping UTF-8 text offsets unchanged.
 */
#include "html_text.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CSS_MAX_RULES 128
#define STYLE_STACK_MAX 16

typedef struct {
    uint8_t align;      // 0=unset, 1..4=blk_align_t
    int8_t bold;        // -1=unset, 0=normal, 1=bold
    int8_t indent_zero; // -1=unset, 0=indented, 1=text-indent:0
} css_props_t;

typedef struct {
    char tag[16];
    char cls[32];
    css_props_t props;
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    size_t count;
} css_sheet_t;

typedef struct {
    char tag[16];
    uint8_t heading_level;
    uint8_t align;
    int8_t bold;
    int8_t indent_zero;
    bool quote;
    bool list_item;
} style_frame_t;

typedef struct {
    html_text_t text;
    size_t text_cap, block_cap, start;
    bool active, space;
    bool pending_hr;
    // 当前正在构建的块的样式快照 / Style snapshot for the currently open block
    uint8_t blk_heading_level;
    uint8_t blk_align;
    bool blk_quote;
    bool blk_list_item;
    bool blk_all_bold;
    bool blk_no_indent;
    bool blk_rule_before;
    style_frame_t stack[STYLE_STACK_MAX];
    size_t depth;
} writer_t;

static unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool name_char(unsigned char c) {
    c = lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_';
}

static bool name_equal(const char* name, const char* expected) {
    return strcmp(name, expected) == 0;
}

static bool ci_equal_n(const char* a, size_t n, const char* b) {
    size_t bl = strlen(b);
    if (n != bl) return false;
    for (size_t i = 0; i < n; ++i) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool ci_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

void html_text_free(html_text_t* text) {
    if (!text) return;
    free(text->utf8);
    html_blocks_free(text->blocks, text->count);
    *text = (html_text_t){0};
}

void html_blocks_free(blk_t* blocks, size_t count) {
    if (!blocks) return;
    for (size_t i = 0; i < count; ++i) { free(blocks[i].image_src); free(blocks[i].image); }
    free(blocks);
}

/* ---- 轻量安全 CSS 解析器 / Bounded safe CSS parser ---- */

static css_props_t css_props_empty(void) {
    return (css_props_t){ .align = 0, .bold = -1, .indent_zero = -1 };
}

static void css_props_merge(css_props_t* dst, const css_props_t* src) {
    if (src->align) dst->align = src->align;
    if (src->bold >= 0) dst->bold = src->bold;
    if (src->indent_zero >= 0) dst->indent_zero = src->indent_zero;
}

static bool css_props_any(const css_props_t* p) {
    return p->align != 0 || p->bold >= 0 || p->indent_zero >= 0;
}

static void parse_css_decls(const char* s, size_t len, css_props_t* out) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (ascii_space((unsigned char)s[i]) || s[i] == ';')) ++i;
        if (i >= len) break;
        if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        size_t p_start = i;
        while (i < len && s[i] != ':' && s[i] != ';') ++i;
        if (i >= len || s[i] != ':') {
            while (i < len && s[i] != ';') ++i;
            continue;
        }
        size_t p_end = i;
        while (p_end > p_start && ascii_space((unsigned char)s[p_end - 1])) --p_end;
        ++i; // skip ':'
        while (i < len && ascii_space((unsigned char)s[i])) ++i;
        size_t v_start = i;
        while (i < len && s[i] != ';') ++i;
        size_t v_end = i;
        while (v_end > v_start && ascii_space((unsigned char)s[v_end - 1])) --v_end;
        // 去掉末尾 !important / Strip trailing !important
        if (v_end - v_start >= 10) {
            size_t bang = v_end;
            while (bang > v_start && s[bang - 1] != '!') --bang;
            if (bang > v_start && ci_equal_n(s + bang, v_end - bang, "important")) {
                v_end = bang - 1;
                while (v_end > v_start && ascii_space((unsigned char)s[v_end - 1])) --v_end;
            }
        }
        size_t pn = p_end - p_start;
        size_t vn = v_end - v_start;
        if (!pn || !vn) continue;

        if (ci_equal_n(s + p_start, pn, "text-align")) {
            if (ci_equal_n(s + v_start, vn, "left") || ci_equal_n(s + v_start, vn, "start")) {
                out->align = BLK_ALIGN_LEFT;
            } else if (ci_equal_n(s + v_start, vn, "center")) {
                out->align = BLK_ALIGN_CENTER;
            } else if (ci_equal_n(s + v_start, vn, "right") || ci_equal_n(s + v_start, vn, "end")) {
                out->align = BLK_ALIGN_RIGHT;
            } else if (ci_equal_n(s + v_start, vn, "justify")) {
                out->align = BLK_ALIGN_JUSTIFY;
            }
        } else if (ci_equal_n(s + p_start, pn, "font-weight")) {
            if (ci_equal_n(s + v_start, vn, "bold") || ci_equal_n(s + v_start, vn, "bolder")) {
                out->bold = 1;
            } else if (ci_equal_n(s + v_start, vn, "normal") || ci_equal_n(s + v_start, vn, "lighter")) {
                out->bold = 0;
            } else if (vn == 3 && s[v_start] >= '1' && s[v_start] <= '9') {
                int w = (s[v_start] - '0') * 100;
                out->bold = w >= 600 ? 1 : 0;
            }
        } else if (ci_equal_n(s + p_start, pn, "text-indent")) {
            // 判断是否为 0（允许 0、0px、0em、0% 等，但不把 0.5em 误判为 0）。
            // Detect zero indent (0, 0px, 0em, 0%) while keeping 0.5em etc. non-zero.
            size_t k = v_start;
            if (k < v_end && (s[k] == '+' || s[k] == '-')) ++k;
            bool has_digit = false;
            bool non_zero = false;
            while (k < v_end && ((s[k] >= '0' && s[k] <= '9') || s[k] == '.')) {
                if (s[k] >= '0' && s[k] <= '9') {
                    has_digit = true;
                    if (s[k] != '0') non_zero = true;
                }
                ++k;
            }
            if (has_digit) out->indent_zero = non_zero ? 0 : 1;
        }
    }
}

static css_sheet_t* ensure_css_sheet(css_sheet_t** sheet_ptr) {
    if (*sheet_ptr) return *sheet_ptr;
    *sheet_ptr = heap_caps_realloc(NULL, sizeof(css_sheet_t), PSRAM_CAPS);
    if (*sheet_ptr) memset(*sheet_ptr, 0, sizeof(css_sheet_t));
    return *sheet_ptr;
}

static void css_parse(css_sheet_t** sheet_ptr, const char* css, size_t len) {
    if (!css || !len) return;
    size_t i = 0;
    while (i < len) {
        while (i < len && ascii_space((unsigned char)css[i])) ++i;
        if (i >= len) break;
        if (i + 1 < len && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) ++i;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        // 跳过 @规则（@import/@charset/@page/@media/@font-face）。
        // Skip at-rules safely while respecting balanced braces.
        if (css[i] == '@') {
            while (i < len && css[i] != ';' && css[i] != '{') ++i;
            if (i < len && css[i] == ';') { ++i; continue; }
            if (i < len && css[i] == '{') {
                int depth = 1;
                ++i;
                while (i < len && depth > 0) {
                    if (css[i] == '{') ++depth;
                    else if (css[i] == '}') --depth;
                    ++i;
                }
            }
            continue;
        }
        size_t sel_start = i;
        while (i < len && css[i] != '{' && css[i] != '}') ++i;
        if (i >= len || css[i] != '{') { if (i < len) ++i; continue; }
        size_t sel_end = i++;
        size_t decl_start = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (css[i] == '{') ++depth;
            else if (css[i] == '}') --depth;
            if (depth > 0) ++i;
        }
        size_t decl_end = i;
        if (i < len && css[i] == '}') ++i;

        css_props_t props = css_props_empty();
        parse_css_decls(css + decl_start, decl_end - decl_start, &props);
        if (!css_props_any(&props)) continue;

        css_sheet_t* sheet = ensure_css_sheet(sheet_ptr);
        if (!sheet) return;

        // 按逗号拆分选择器列表，每个取末段简单选择器（tag / .cls / tag.cls）。
        // Split selector list by comma and extract trailing simple selector (tag / .cls / tag.cls).
        size_t s = sel_start;
        while (s < sel_end && sheet->count < CSS_MAX_RULES) {
            size_t c = s;
            while (c < sel_end && css[c] != ',') ++c;
            size_t seg_s = s, seg_e = c;
            s = (c < sel_end) ? c + 1 : sel_end;
            while (seg_s < seg_e && ascii_space((unsigned char)css[seg_s])) ++seg_s;
            while (seg_e > seg_s && ascii_space((unsigned char)css[seg_e - 1])) --seg_e;
            if (seg_s >= seg_e) continue;

            // 忽略伪元素/伪类（如 p:before / a:hover）。/ Ignore pseudo-selectors.
            bool has_pseudo = false;
            for (size_t k = seg_s; k < seg_e; ++k) {
                if (css[k] == ':' || css[k] == '[' || css[k] == '#') { has_pseudo = true; break; }
            }
            if (has_pseudo) continue;

            // 取组合选择器最后一段（如 "div p.center" -> "p.center"）。
            // Take the trailing compound selector in combinators.
            size_t last = seg_e;
            while (last > seg_s && !ascii_space((unsigned char)css[last - 1]) &&
                   css[last - 1] != '>' && css[last - 1] != '+' && css[last - 1] != '~') {
                --last;
            }
            size_t dot = last;
            while (dot < seg_e && css[dot] != '.') ++dot;
            size_t t_len = dot - last;
            size_t c_len = (dot < seg_e) ? (seg_e - dot - 1) : 0;
            if ((t_len == 0 && c_len == 0) || t_len >= 16 || c_len >= 32) continue;
            // 仅支持单类名（忽略 .a.b）。/ Single class only.
            bool multi_dot = false;
            for (size_t k = dot + 1; k < seg_e; ++k) if (css[k] == '.') { multi_dot = true; break; }
            if (multi_dot) continue;

            css_rule_t* r = &sheet->rules[sheet->count++];
            memset(r, 0, sizeof(*r));
            for (size_t k = 0; k < t_len; ++k) r->tag[k] = (char)lower((unsigned char)css[last + k]);
            for (size_t k = 0; k < c_len; ++k) r->cls[k] = (char)lower((unsigned char)css[dot + 1 + k]);
            r->props = props;
        }
    }
}

// 从 HTML 属性串解析 class、align、style，并结合样式表计算本元素样式。
// Extract class, align, and inline style from HTML attributes and resolve matching CSS rules.
static css_props_t resolve_element_style(
    const css_sheet_t* sheet, const char* tag, const char* attrs, size_t len
) {
    css_props_t res = css_props_empty();
    char classes[128] = "";
    size_t classes_len = 0;
    const char* inline_style = NULL;
    size_t inline_style_len = 0;
    uint8_t attr_align = 0;

    size_t at = 0;
    while (at < len) {
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        size_t start = at;
        while (at < len && name_char((unsigned char)attrs[at])) ++at;
        if (at == start) { ++at; continue; }
        size_t nlen = at - start;
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        if (at >= len || attrs[at] != '=') continue;
        ++at;
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        if (at >= len) break;
        size_t vs = 0, ve = 0;
        if (attrs[at] == '\'' || attrs[at] == '"') {
            char q = attrs[at++];
            vs = at;
            while (at < len && attrs[at] != q) ++at;
            ve = at;
            if (at < len) ++at;
        } else {
            vs = at;
            while (at < len && !ascii_space((unsigned char)attrs[at]) && attrs[at] != '/') ++at;
            ve = at;
        }
        size_t vlen = ve - vs;
        if (ci_equal_n(attrs + start, nlen, "class") && vlen > 0) {
            classes_len = vlen < sizeof(classes) - 1 ? vlen : sizeof(classes) - 1;
            memcpy(classes, attrs + vs, classes_len);
            classes[classes_len] = 0;
        } else if (ci_equal_n(attrs + start, nlen, "style") && vlen > 0) {
            inline_style = attrs + vs;
            inline_style_len = vlen;
        } else if (ci_equal_n(attrs + start, nlen, "align") && vlen > 0) {
            if (ci_equal_n(attrs + vs, vlen, "left")) attr_align = BLK_ALIGN_LEFT;
            else if (ci_equal_n(attrs + vs, vlen, "center")) attr_align = BLK_ALIGN_CENTER;
            else if (ci_equal_n(attrs + vs, vlen, "right")) attr_align = BLK_ALIGN_RIGHT;
            else if (ci_equal_n(attrs + vs, vlen, "justify")) attr_align = BLK_ALIGN_JUSTIFY;
        }
    }

    if (sheet && sheet->count > 0) {
        // 1) 标签选择器 / Tag rules
        for (size_t i = 0; i < sheet->count; ++i) {
            const css_rule_t* r = &sheet->rules[i];
            if (r->tag[0] && !r->cls[0] && name_equal(r->tag, tag)) {
                css_props_merge(&res, &r->props);
            }
        }
        // 2) 类选择器（.cls）和 3) 标签+类选择器（tag.cls）
        // 2) Class rules (.cls) and 3) tag.cls rules
        for (int pass = 0; pass < 2 && classes_len > 0; ++pass) {
            size_t p = 0;
            while (p < classes_len) {
                while (p < classes_len && ascii_space((unsigned char)classes[p])) ++p;
                size_t cs = p;
                while (p < classes_len && !ascii_space((unsigned char)classes[p])) ++p;
                size_t cl = p - cs;
                if (!cl || cl >= 32) continue;
                char cname[32];
                for (size_t k = 0; k < cl; ++k) cname[k] = (char)lower((unsigned char)classes[cs + k]);
                cname[cl] = 0;
                for (size_t i = 0; i < sheet->count; ++i) {
                    const css_rule_t* r = &sheet->rules[i];
                    if (!r->cls[0] || !ci_equal(r->cls, cname)) continue;
                    if (pass == 0 && !r->tag[0]) css_props_merge(&res, &r->props);
                    else if (pass == 1 && r->tag[0] && name_equal(r->tag, tag)) css_props_merge(&res, &r->props);
                }
            }
        }
    }

    if (attr_align) res.align = attr_align;
    if (inline_style && inline_style_len > 0) {
        parse_css_decls(inline_style, inline_style_len, &res);
    }
    return res;
}

/* ---- 样式继承栈 / Style inheritance stack ---- */

static void compute_active_style(
    const writer_t* w, uint8_t* heading_level, uint8_t* align,
    bool* quote, bool* list_item, bool* bold, bool* no_indent
) {
    uint8_t hl = 0;
    uint8_t al = BLK_ALIGN_DEFAULT;
    bool qt = false, li = false;
    int8_t bd = -1, iz = -1;
    for (size_t i = 0; i < w->depth; ++i) {
        const style_frame_t* f = &w->stack[i];
        if (f->heading_level) hl = f->heading_level;
        if (f->align) al = f->align;
        if (f->quote) qt = true;
        if (f->list_item) li = true;
        if (f->bold >= 0) bd = f->bold;
        if (f->indent_zero >= 0) iz = f->indent_zero;
    }
    *heading_level = hl;
    *align = al;
    *quote = qt;
    *list_item = li;
    *bold = (bd == 1);
    *no_indent = (iz == 1) || hl > 0 || qt || li || al == BLK_ALIGN_CENTER || al == BLK_ALIGN_RIGHT;
}

static void stack_push(writer_t* w, const char* tag, const css_props_t* p, uint8_t hl, bool quote, bool li) {
    if (w->depth > 0) {
        // 容错：未闭合的同类 <p>/<li> 自动弹出上一层。/ Auto-close unclosed <p> or <li>.
        if ((name_equal(tag, "p") || name_equal(tag, "li")) &&
            name_equal(w->stack[w->depth - 1].tag, tag)) {
            w->depth--;
        }
    }
    if (w->depth >= STYLE_STACK_MAX) return;
    style_frame_t* f = &w->stack[w->depth++];
    memset(f, 0, sizeof(*f));
    strncpy(f->tag, tag, sizeof(f->tag) - 1);
    f->heading_level = hl;
    f->align = p->align;
    f->bold = p->bold;
    f->indent_zero = p->indent_zero;
    f->quote = quote;
    f->list_item = li;
}

static void stack_pop(writer_t* w, const char* tag) {
    for (size_t i = w->depth; i > 0; --i) {
        if (name_equal(w->stack[i - 1].tag, tag)) {
            w->depth = i - 1;
            return;
        }
    }
}

/* ---- 文本输出与块构建 / Text output and block emission ---- */

static esp_err_t reserve_text(writer_t* w, size_t extra) {
    if (extra > HTML_TEXT_MAX_BYTES - w->text.len) return ESP_ERR_INVALID_SIZE;
    size_t need = w->text.len + extra + 1;
    if (need <= w->text_cap) return ESP_OK;
    size_t cap = w->text_cap ? w->text_cap : 256;
    while (cap < need) {
        if (cap > (HTML_TEXT_MAX_BYTES + 1) / 2) { cap = HTML_TEXT_MAX_BYTES + 1; break; }
        cap *= 2;
    }
    char* text = heap_caps_realloc(w->text.utf8, cap, PSRAM_CAPS);
    if (!text) return ESP_ERR_NO_MEM;
    w->text.utf8 = text;
    w->text_cap = cap;
    return ESP_OK;
}

static esp_err_t finish_block(writer_t* w) {
    w->space = false;
    if (!w->active) return ESP_OK;
    if (w->text.count == HTML_TEXT_MAX_BLOCKS) return ESP_ERR_INVALID_SIZE;
    if (w->text.count == w->block_cap) {
        size_t cap = w->block_cap ? w->block_cap * 2 : 32;
        if (cap > HTML_TEXT_MAX_BLOCKS) cap = HTML_TEXT_MAX_BLOCKS;
        blk_t* blocks = heap_caps_realloc(w->text.blocks, cap * sizeof(*blocks), PSRAM_CAPS);
        if (!blocks) return ESP_ERR_NO_MEM;
        w->text.blocks = blocks;
        w->block_cap = cap;
    }
    w->text.blocks[w->text.count++] = (blk_t){
        .offset = w->start,
        .len = w->text.len - w->start,
        .heading = w->blk_heading_level >= 1 && w->blk_heading_level <= 3,
        .heading_level = w->blk_heading_level,
        .align = w->blk_align,
        .quote = w->blk_quote,
        .list_item = w->blk_list_item,
        .bold = w->blk_all_bold,
        .no_indent = w->blk_no_indent,
        .rule_before = w->blk_rule_before,
    };
    w->active = false;
    return ESP_OK;
}

static esp_err_t emit(writer_t* w, uint32_t cp) {
    if (cp == 0xa0 || (cp < 128 && ascii_space((unsigned char)cp))) {
        if (w->active) w->space = true;
        return ESP_OK;
    }
    char bytes[4];
    size_t n;
    if (cp < 0x80) { bytes[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        bytes[0] = (char)(0xc0 | (cp >> 6)); bytes[1] = (char)(0x80 | (cp & 63)); n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char)(0xe0 | (cp >> 12)); bytes[1] = (char)(0x80 | ((cp >> 6) & 63));
        bytes[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        bytes[0] = (char)(0xf0 | (cp >> 18)); bytes[1] = (char)(0x80 | ((cp >> 12) & 63));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 63)); bytes[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    bool separator = !w->active && w->text.count > 0;
    esp_err_t err = reserve_text(w, n + (separator || w->space ? 1 : 0));
    if (err != ESP_OK) return err;
    uint8_t hl, al;
    bool qt, li, bd, ni;
    compute_active_style(w, &hl, &al, &qt, &li, &bd, &ni);
    if (!w->active) {
        if (separator) w->text.utf8[w->text.len++] = '\n';
        w->start = w->text.len;
        w->active = true;
        w->blk_heading_level = hl;
        w->blk_align = al;
        w->blk_quote = qt;
        w->blk_list_item = li;
        w->blk_all_bold = bd;
        w->blk_no_indent = ni;
        w->blk_rule_before = w->pending_hr;
        w->pending_hr = false;
    } else {
        if (w->space) w->text.utf8[w->text.len++] = ' ';
        if (!bd) w->blk_all_bold = false;
    }
    w->space = false;
    memcpy(w->text.utf8 + w->text.len, bytes, n);
    w->text.len += n;
    return ESP_OK;
}

// 实体查找有固定上限；未知名称原样保留，非法数值替换为 U+FFFD。
// Bound entity lookahead; preserve unknown names and replace invalid numeric values with U+FFFD.
static size_t entity(const char* s, size_t len, uint32_t* cp) {
    size_t end = 1;
    while (end < len && end <= 32 && s[end] != ';' && !ascii_space((unsigned char)s[end]) && s[end] != '&' && s[end] != '<') end++;
    if (end >= len || end > 32 || s[end] != ';') return 0;
    static const struct { const char* name; uint32_t cp; } names[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"middot", 0x00B7}, {"bull", 0x2022}, {"copy", 0x00A9}, {"reg", 0x00AE},
        {"trade", 0x2122}, {"times", 0x00D7}, {"divide", 0x00F7},
        {"laquo", 0x00AB}, {"raquo", 0x00BB}, {"ensp", 0x2002}, {"emsp", 0x3000},
        {"thinsp", 0x2009}, {"sect", 0x00A7}, {"para", 0x00B6}, {"deg", 0x00B0},
    };
    if (end > 2 && s[1] == '#') {
        size_t i = 2;
        unsigned base = 10;
        if (i < end && (s[i] == 'x' || s[i] == 'X')) { base = 16; i++; }
        if (i == end) return 0;
        uint32_t value = 0;
        bool overflow = false;
        for (; i < end; i++) {
            unsigned char c = lower((unsigned char)s[i]);
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 255;
            if (digit >= base) return 0;
            if (value > (0x10ffffu - digit) / base) overflow = true;
            else if (!overflow) value = value * base + digit;
        }
        *cp = overflow || !value || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : value;
        return end + 1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i].name) == end - 1 && memcmp(s + 1, names[i].name, end - 1) == 0) {
            *cp = names[i].cp;
            return end + 1;
        }
    }
    return 0;
}

static size_t utf8(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (p[0] && p[0] < 0x80) { *cp = p[0]; return 1; }
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 : p[0] >= 0xe0 && p[0] <= 0xef ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > len) return 0;
    uint32_t value = p[0] & (0x7f >> n);
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 63);
    }
    if ((n == 2 && value < 0x80) || (n == 3 && value < 0x800) || (n == 4 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
    *cp = value;
    return n;
}

static bool block_tag(const char* name) {
    static const char* const tags[] = {
        "p", "div", "li", "tr", "br", "hr", "blockquote", "pre",
        "section", "article", "header", "footer", "figcaption", "center",
    };
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) return true;
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) if (name_equal(name, tags[i])) return true;
    return false;
}

static bool inline_style_tag(const char* name) {
    return name_equal(name, "b") || name_equal(name, "strong") || name_equal(name, "span");
}

static bool image_tag(const char* name) {
    const char* local = strrchr(name, ':');
    if (local) name = local + 1;
    return name_equal(name, "img") || name_equal(name, "image");
}

// 只保留有界引用；资源解析由 EPUB 后端处理，不在 HTML 转换器内打开文件。
// Retain a bounded reference only; EPUB resolves resources without file access in the HTML converter.
static char* image_source(const char* attrs, size_t len) {
    size_t at = 0;
    while (at < len) {
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        size_t start = at;
        while (at < len && name_char((unsigned char)attrs[at])) ++at;
        if (at == start) break;
        size_t name_len = at - start;
        bool wanted = (name_len == 3 && !memcmp(attrs + start, "src", 3)) ||
                      (name_len == 4 && !memcmp(attrs + start, "href", 4)) ||
                      (name_len == 10 && !memcmp(attrs + start, "xlink:href", 10));
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        if (at == len || attrs[at++] != '=') break;
        while (at < len && ascii_space((unsigned char)attrs[at])) ++at;
        if (at == len || (attrs[at] != '\'' && attrs[at] != '"')) break;
        char quote = attrs[at++]; start = at;
        while (at < len && attrs[at] != quote) ++at;
        if (at == len) break;
        size_t end = at++;
        if (!wanted) continue;
        char value[512]; size_t used = 0;
        for (size_t i = start; i < end;) {
            uint32_t cp;
            size_t n = attrs[i] == '&' ? entity(attrs + i, end - i, &cp) : 0;
            if (!n) n = utf8(attrs + i, end - i, &cp);
            if (!n || cp < 32 || used + 4 >= sizeof(value)) return NULL;
            i += n;
            if (cp < 0x80) value[used++] = (char)cp;
            else if (cp < 0x800) { value[used++] = 0xc0 | (cp >> 6); value[used++] = 0x80 | (cp & 63); }
            else if (cp < 0x10000) { value[used++] = 0xe0 | (cp >> 12); value[used++] = 0x80 | ((cp >> 6) & 63); value[used++] = 0x80 | (cp & 63); }
            else { value[used++] = 0xf0 | (cp >> 18); value[used++] = 0x80 | ((cp >> 12) & 63); value[used++] = 0x80 | ((cp >> 6) & 63); value[used++] = 0x80 | (cp & 63); }
        }
        if (!used) return NULL;
        char* result = heap_caps_realloc(NULL, used + 1, PSRAM_CAPS);
        if (result) { memcpy(result, value, used); result[used] = 0; }
        return result;
    }
    return NULL;
}

esp_err_t html_to_blocks_css(const char* html, size_t len, const char* css, size_t css_len, html_text_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = (html_text_t){0};
    if (!html && len) return ESP_ERR_INVALID_ARG;
    if (len > HTML_TEXT_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    writer_t w = {0};
    css_sheet_t* sheet = NULL;
    if (css && css_len > 0) css_parse(&sheet, css, css_len);
    esp_err_t err = ESP_OK;
    char skip[16] = "";
    bool resume_head = false;
    size_t style_body_start = SIZE_MAX;
    size_t pos = len >= 3 && memcmp(html, "\xef\xbb\xbf", 3) == 0 ? 3 : 0;
    while (pos < len) {
        if ((!skip[0] || name_equal(skip, "head")) && len - pos >= 4 && memcmp(html + pos, "<!--", 4) == 0) {
            pos += 4;
            while (len - pos >= 3 && memcmp(html + pos, "-->", 3)) pos++;
            pos = len - pos >= 3 ? pos + 3 : len;
            continue;
        }
        if (html[pos] == '<') {
            size_t tag_open_pos = pos;
            size_t at = pos + 1;
            bool closing = at < len && html[at] == '/';
            if (closing) at++;
            size_t start = at;
            while (at < len && name_char((unsigned char)html[at])) at++;
            char name[16] = "";
            size_t name_len = at - start;
            if (name_len < sizeof(name)) {
                for (size_t i = 0; i < name_len; i++) name[i] = (char)lower((unsigned char)html[start + i]);
            }
            bool head_child = name_equal(skip, "head") && !closing &&
                              (name_equal(name, "script") || name_equal(name, "style"));
            if (skip[0] && !head_child && (!closing || !name_equal(name, skip))) { pos++; continue; }
            bool declaration = start < len && (html[start] == '!' || html[start] == '?');
            unsigned char initial = start < len ? lower((unsigned char)html[start]) : 0;
            if ((name_len && initial >= 'a' && initial <= 'z') || declaration) {
                char quote = 0;
                size_t end = at;
                for (; end < len; end++) {
                    char c = html[end];
                    if (quote) { if (c == quote) quote = 0; }
                    else if (c == '\'' || c == '"') quote = c;
                    else if (c == '>') break;
                }
                if (end == len) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
                bool self_closing = end > at && html[end - 1] == '/';
                pos = end + 1;
                if (skip[0]) {
                    if (head_child) {
                        if (!self_closing) {
                            memcpy(skip, name, sizeof(skip));
                            resume_head = true;
                            if (name_equal(name, "style")) style_body_start = pos;
                        }
                    } else {
                        if (name_equal(skip, "style") && style_body_start != SIZE_MAX && tag_open_pos > style_body_start) {
                            css_parse(&sheet, html + style_body_start, tag_open_pos - style_body_start);
                        }
                        style_body_start = SIZE_MAX;
                        if (resume_head) {
                            memcpy(skip, "head", 5);
                            resume_head = false;
                        } else skip[0] = 0;
                    }
                    continue;
                }
                if (!closing && !self_closing && (name_equal(name, "head") || name_equal(name, "style") || name_equal(name, "script"))) {
                    memcpy(skip, name, sizeof(skip));
                    if (name_equal(name, "style")) style_body_start = pos;
                } else if (!closing && image_tag(name)) {
                    // 图片独立成块；后端可替换占位，原文字节位置保持稳定。
                    // Isolate images so the backend can replace placeholders without changing text offsets.
                    err = finish_block(&w);
                    if (err != ESP_OK) goto fail;
                    size_t saved_depth = w.depth;
                    w.depth = 0;
                    const char* label = "[图片]";
                    for (size_t i = 0; label[i];) {
                        uint32_t cp;
                        size_t n = utf8(label + i, strlen(label + i), &cp);
                        err = emit(&w, cp);
                        if (err != ESP_OK) goto fail;
                        i += n;
                    }
                    err = finish_block(&w);
                    if (err != ESP_OK) goto fail;
                    w.text.blocks[w.text.count - 1].image_src = image_source(html + at, end - at);
                    w.depth = saved_depth;
                } else if (block_tag(name)) {
                    err = finish_block(&w);
                    if (err != ESP_OK) goto fail;
                    if (name_equal(name, "hr")) {
                        if (!closing) w.pending_hr = true;
                    } else if (!name_equal(name, "br")) {
                        if (closing) {
                            stack_pop(&w, name);
                        } else if (!self_closing) {
                            css_props_t p = resolve_element_style(sheet, name, html + at, end - at);
                            uint8_t hl = (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2])
                                ? (uint8_t)(name[1] - '0') : 0;
                            if (name_equal(name, "center") && !p.align) p.align = BLK_ALIGN_CENTER;
                            bool is_quote = name_equal(name, "blockquote") || name_equal(name, "pre");
                            bool is_li = name_equal(name, "li");
                            stack_push(&w, name, &p, hl, is_quote, is_li);
                        }
                    }
                } else if (inline_style_tag(name)) {
                    if (closing) {
                        stack_pop(&w, name);
                    } else if (!self_closing) {
                        css_props_t p = resolve_element_style(sheet, name, html + at, end - at);
                        if (name_equal(name, "b") || name_equal(name, "strong")) p.bold = 1;
                        stack_push(&w, name, &p, 0, false, false);
                    }
                }
                continue;
            }
        }
        if (skip[0]) { pos++; continue; }
        uint32_t cp;
        size_t n = html[pos] == '&' ? entity(html + pos, len - pos, &cp) : 0;
        if (!n) n = utf8(html + pos, len - pos, &cp);
        if (!n) { err = ESP_ERR_INVALID_RESPONSE; goto fail; }
        err = emit(&w, cp);
        if (err != ESP_OK) goto fail;
        pos += n;
    }
    err = finish_block(&w);
    if (err == ESP_OK && w.pending_hr && w.text.count > 0) {
        w.text.blocks[w.text.count - 1].rule_after = true;
    }
    if (err == ESP_OK) err = reserve_text(&w, 0);
    if (err != ESP_OK) goto fail;
    w.text.utf8[w.text.len] = 0;
    *out = w.text;
    free(sheet);
    return ESP_OK;
fail:
    free(sheet);
    html_text_free(&w.text);
    return err;
}

esp_err_t html_to_blocks(const char* html, size_t len, html_text_t* out) {
    return html_to_blocks_css(html, len, NULL, 0, out);
}
