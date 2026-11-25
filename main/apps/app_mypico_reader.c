/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 阅读页：
 * - 视觉匹配 Figma「阅读页面 (9:302)」，顶栏时间/电量 + 简洁正文 + 划线与「删除」气泡
 * - 长按触发连续 DU 跟手拖选划线，抬手落盘保存；点击已有划线弹出黑色「删除」气泡
 * - 精准记录活跃阅读时长、阅读进度与最近阅读时间，并持久化到 TF 卡 .mypico/
 *
 * English: MyPico reader page:
 * - Matches Figma Reading page (9:302) with status bar, clean body, highlights, and delete bubble
 * - Long-press + continuous-DU finger-following drag selection; tap existing highlight to delete
 * - Tracks active reading duration, whole-book progress, and last-read timestamp on TF card
 *
 * 冻结：render 只做纯绘制；跟手拖选期间仅通过 continuous_du 驱动 1bit 下划线像素，不闪屏。
 * Frozen: render is paint-only; drag selection drives only 1-bit underline rects via continuous_du.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "book_data.h"
#include "book_layout.h"
#include "book_source.h"
#include "continuous_du.h"
#include "esp_log.h"
#include "html_text.h"
#include "mypico_reader_state.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_statusbar.h"

#define TAG "mypico_reader"

#define READER_BODY_TOP (MYPICO_STATUSBAR_H + 8)
#define READER_BODY_BOTTOM (UI_LOCK_HEIGHT - 24)
#define READER_PX_MIN 32
#define READER_PX_MAX 64
#define READER_PX_STEP 4
#define READER_TOC_ROWS 10
#define READER_MAX_BARS 32
#define READER_IDLE_CAP_MS 90000
#define READER_FLUSH_INTERVAL_MS 60000
#define READER_PMU_POLL_MS 30000

// 删除气泡尺寸（Figma 80×58 + 三角 32×16 × 1.17）。
// Delete bubble metrics scaled from Figma (80x58 + triangle 32x16 x 1.17).
#define DEL_BUBBLE_W 96
#define DEL_BUBBLE_H 68
#define DEL_TRI_W 36
#define DEL_TRI_H 18

static char s_target_path[BOOK_DATA_PATH_MAX];
static uint32_t s_target_size;

static book_record_t s_rec;
static bool s_book_open;
static char s_error_msg[128];

static size_t s_chapter_idx;
static html_text_t s_chapter;
static size_t s_page;
static int s_px = 40;

// 视图覆盖层：工具栏、目录、插图预览。
// Overlays: toolbar, TOC, illustration preview.
static bool s_toolbar_open;
static bool s_toc_open;
static int s_toc_page;
static uint8_t* s_preview_img;
static uint16_t s_preview_w;
static uint16_t s_preview_h;

// 已有划线点击后的「删除」气泡状态。
// Delete bubble state after tapping an existing highlight.
static uint32_t s_delete_hl_id;
static EpdRect s_delete_bubble_rect;
static int s_delete_tri_cx;
static bool s_delete_above;

// 长按拖动跟手划线状态。
// Long-press finger-following drag highlight state.
static bool s_selecting;
static size_t s_sel_anchor_s;
static size_t s_sel_anchor_e;
static size_t s_sel_start;
static size_t s_sel_end;
static EpdRect s_prev_bars[READER_MAX_BARS];
static size_t s_prev_bar_count;
static EpdRect s_sel_dirty_logical;
static int64_t s_sel_last_fallback_ms;

// 活跃阅读时长与周期落盘计时。
// Active reading duration and periodic flush timers.
static int64_t s_act_last_ms;
static int64_t s_session_ms_acc;
static uint32_t s_session_logged_sec;
static int64_t s_last_flush_ms;
static int64_t s_last_pmu_ms;

void mypico_reader_set_target(const char* file_path, uint32_t file_size) {
    if (!file_path) return;
    strncpy(s_target_path, file_path, sizeof(s_target_path) - 1);
    s_target_path[sizeof(s_target_path) - 1] = 0;
    s_target_size = file_size;
}

static EpdRect body_rect(void) {
    return (EpdRect){
        .x = MYPICO_MARGIN,
        .y = READER_BODY_TOP,
        .width = MYPICO_CONTENT_W,
        .height = READER_BODY_BOTTOM - READER_BODY_TOP,
    };
}

static void clear_preview(void) {
    free(s_preview_img);
    s_preview_img = NULL;
    s_preview_w = 0;
    s_preview_h = 0;
}

static void clear_delete_bubble(void) {
    s_delete_hl_id = 0;
    memset(&s_delete_bubble_rect, 0, sizeof(s_delete_bubble_rect));
}

static void clear_selection_state(void) {
    s_selecting = false;
    s_prev_bar_count = 0;
    memset(&s_sel_dirty_logical, 0, sizeof(s_sel_dirty_logical));
    continuous_du_reset();
}

static uint8_t compute_progress_pct(void) {
    if (!s_book_open) return s_rec.pct;
    uint32_t total = book_total_bytes();
    if (total == 0) return 0;
    uint32_t ch_base = book_chapter_byte_offset(s_chapter_idx);
    uint32_t ch_next = (s_chapter_idx + 1 < book_chapter_count())
        ? book_chapter_byte_offset(s_chapter_idx + 1)
        : total;
    uint32_t ch_span = ch_next > ch_base ? ch_next - ch_base : 0;
    size_t cur_off = book_layout_page_start_offset(s_page);
    uint32_t in_ch = (s_chapter.len > 0)
        ? (uint32_t)((uint64_t)cur_off * ch_span / s_chapter.len)
        : 0;
    // 末章末页记为 100%。
    // Count the last page of the last chapter as 100%.
    if (s_chapter_idx + 1 >= book_chapter_count()
        && book_layout_complete()
        && s_page + 1 >= book_layout_page_count()) {
        return 100;
    }
    uint64_t pos = (uint64_t)ch_base + in_ch;
    uint32_t pct = (uint32_t)(pos * 100 / total);
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

static void touch_reading_activity(int64_t now_ms) {
    if (!s_book_open || now_ms <= 0) return;
    if (s_act_last_ms > 0 && now_ms > s_act_last_ms) {
        int64_t delta = now_ms - s_act_last_ms;
        if (delta > READER_IDLE_CAP_MS) delta = READER_IDLE_CAP_MS;
        s_session_ms_acc += delta;
    }
    s_act_last_ms = now_ms;
}

static void flush_progress_and_duration(int64_t now_ms, bool log_session) {
    if (!s_book_open) return;
    touch_reading_activity(now_ms);
    uint32_t total_session_sec = (uint32_t)(s_session_ms_acc / 1000);
    uint32_t delta_sec = total_session_sec > s_session_logged_sec
        ? total_session_sec - s_session_logged_sec
        : 0;

    s_rec.chapter = (uint16_t)s_chapter_idx;
    s_rec.byte_off = (uint32_t)book_layout_page_start_offset(s_page);
    s_rec.px = (uint8_t)s_px;
    s_rec.pct = compute_progress_pct();
    s_rec.total_read_sec += delta_sec;
    s_session_logged_sec = total_session_sec;

    uint32_t now_unix = 0;
    if (ui_statusbar_time_valid(&now_unix)) {
        s_rec.last_read_ts = now_unix;
        if (!s_rec.created_ts) s_rec.created_ts = now_unix;
    }

    (void)book_data_save(&s_rec);
    if (log_session && delta_sec >= 5) {
        (void)book_data_append_session(
            s_rec.book_id, now_unix, delta_sec, app_settings_tz_offset_min()
        );
    }
    mypico_shelf_mark_dirty();
    s_last_flush_ms = now_ms;
}

static bool load_chapter(size_t idx, size_t target_off, bool go_last_page) {
    clear_preview();
    clear_delete_bubble();
    clear_selection_state();
    book_layout_free();
    html_text_free(&s_chapter);

    if (idx >= book_chapter_count()) return false;
    esp_err_t err = book_chapter_load_blocks(idx, &s_chapter);
    if (err != ESP_OK) {
        snprintf(s_error_msg, sizeof(s_error_msg), "章节加载失败 (%s)", esp_err_to_name(err));
        return false;
    }

    s_chapter_idx = idx;
    bool ok = book_layout_begin_blocks(
        s_chapter.utf8, s_chapter.len, s_chapter.blocks, s_chapter.count, body_rect(), s_px
    );
    if (!ok) {
        snprintf(s_error_msg, sizeof(s_error_msg), "章节排版失败");
        return false;
    }

    // 推进分页直到覆盖目标偏移或末页。
    // Extend pagination until covering target_off or reaching the last page.
    while (!book_layout_complete()) {
        if (!go_last_page) {
            size_t count = book_layout_page_count();
            if (count > 0 && book_layout_page_start_offset(count) > target_off) break;
        }
        if (!book_layout_extend(4)) {
            snprintf(s_error_msg, sizeof(s_error_msg), "分页超出上限");
            return false;
        }
    }

    if (go_last_page) {
        size_t n = book_layout_page_count();
        s_page = n > 0 ? n - 1 : 0;
    } else {
        s_page = book_layout_page_for_offset(target_off);
    }
    s_error_msg[0] = 0;
    return true;
}

static void close_book_resources(void) {
    clear_preview();
    clear_delete_bubble();
    clear_selection_state();
    book_layout_free();
    html_text_free(&s_chapter);
    book_close();
    book_data_record_free(&s_rec);
    s_book_open = false;
}

/* ---- 划线集合收集与绘制 / Highlight collection and rendering ---- */

static size_t collect_chapter_spans(book_layout_span_t* out, size_t max_spans) {
    size_t n = 0;
    for (size_t i = 0; i < s_rec.highlight_count && n < max_spans; ++i) {
        if (s_rec.highlights[i].chapter == s_chapter_idx) {
            out[n].start_off = s_rec.highlights[i].start_off;
            out[n].end_off = s_rec.highlights[i].end_off;
            ++n;
        }
    }
    return n;
}

static void compute_delete_bubble_position(const book_highlight_t* hl) {
    EpdRect bars[READER_MAX_BARS];
    size_t n = book_layout_span_underlines(
        s_page, body_rect(), hl->start_off, hl->end_off, bars, READER_MAX_BARS
    );
    if (n == 0) {
        clear_delete_bubble();
        return;
    }

    // 优先贴在划线末行正下方居中（与 Figma 16:157 一致）；若触底则翻到首行上方。
    // Anchor centered below the highlight's last line (matching Figma 16:157); flip above if near bottom.
    EpdRect last = bars[n - 1];
    EpdRect first = bars[0];
    int cx = last.x + last.width / 2;
    int bx = cx - DEL_BUBBLE_W / 2;
    if (bx < MYPICO_MARGIN) bx = MYPICO_MARGIN;
    if (bx + DEL_BUBBLE_W > UI_LOCK_WIDTH - MYPICO_MARGIN) {
        bx = UI_LOCK_WIDTH - MYPICO_MARGIN - DEL_BUBBLE_W;
    }
    if (cx < bx + 20) cx = bx + 20;
    if (cx > bx + DEL_BUBBLE_W - 20) cx = bx + DEL_BUBBLE_W - 20;

    int by_below = last.y + last.height + 6 + DEL_TRI_H;
    if (by_below + DEL_BUBBLE_H <= UI_LOCK_HEIGHT - 16) {
        s_delete_above = false;
        s_delete_bubble_rect = (EpdRect){ bx, by_below, DEL_BUBBLE_W, DEL_BUBBLE_H };
    } else {
        s_delete_above = true;
        int by_above = first.y - s_px - DEL_TRI_H - DEL_BUBBLE_H - 8;
        if (by_above < READER_BODY_TOP) by_above = READER_BODY_TOP;
        s_delete_bubble_rect = (EpdRect){ bx, by_above, DEL_BUBBLE_W, DEL_BUBBLE_H };
    }
    s_delete_tri_cx = cx;
    s_delete_hl_id = hl->id;
}

static void draw_delete_bubble(uint8_t* fb) {
    if (s_delete_hl_id == 0) return;
    EpdRect r = s_delete_bubble_rect;
    int half = DEL_TRI_W / 2;
    if (!s_delete_above) {
        int tip_y = r.y - DEL_TRI_H;
        epd_fill_triangle(
            s_delete_tri_cx, tip_y,
            s_delete_tri_cx - half, r.y + 2,
            s_delete_tri_cx + half, r.y + 2,
            UI_GRAY_BLACK, fb
        );
    } else {
        int base_y = r.y + r.height - 2;
        int tip_y = r.y + r.height + DEL_TRI_H;
        epd_fill_triangle(
            s_delete_tri_cx, tip_y,
            s_delete_tri_cx - half, base_y,
            s_delete_tri_cx + half, base_y,
            UI_GRAY_BLACK, fb
        );
    }
    ui_fill_round_rect(fb, r, 6, UI_GRAY_BLACK);
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 32, "删除", EPD_DRAW_ALIGN_CENTER, true);
}

/* ---- 连续 DU 跟手拖选下划线差分 / Continuous-DU drag underline diff ---- */

// 将某条水平下划线中与另一组下划线不重叠的片段标记到 continuous_du（正=压黑，负=擦白）。
// Mark non-overlapping horizontal segments of one bar into continuous_du (+darken, -erase).
static void mark_bar_diff_against(
    EpdRect bar, const EpdRect* other, size_t other_count, int phases, EpdRect* dirty
) {
    int x0 = bar.x;
    int x1 = bar.x + bar.width;
    for (size_t i = 0; i < other_count; ++i) {
        if (other[i].y != bar.y) continue;
        int ox0 = other[i].x;
        int ox1 = other[i].x + other[i].width;
        if (ox1 <= x0 || ox0 >= x1) continue;
        if (ox0 <= x0 && ox1 >= x1) {
            return;
        }
        if (ox0 > x0 && ox1 < x1) {
            EpdRect left = { x0, bar.y, ox0 - x0, bar.height };
            EpdRect right = { ox1, bar.y, x1 - ox1, bar.height };
            continuous_du_mark_rect(continuous_du_rect_from_logical(left), phases);
            continuous_du_mark_rect(continuous_du_rect_from_logical(right), phases);
            *dirty = ui_rect_union(*dirty, left);
            *dirty = ui_rect_union(*dirty, right);
            return;
        }
        if (ox0 <= x0) x0 = ox1;
        else x1 = ox0;
    }
    if (x1 > x0) {
        EpdRect seg = { x0, bar.y, x1 - x0, bar.height };
        continuous_du_mark_rect(continuous_du_rect_from_logical(seg), phases);
        *dirty = ui_rect_union(*dirty, seg);
    }
}

static void apply_drag_bars_diff(
    app_ctx_t* ctx, const EpdRect* new_bars, size_t new_count
) {
    EpdRect dirty = s_sel_dirty_logical;
    for (size_t i = 0; i < s_prev_bar_count; ++i) {
        mark_bar_diff_against(s_prev_bars[i], new_bars, new_count, -continuous_du_light_phases(), &dirty);
    }
    for (size_t i = 0; i < new_count; ++i) {
        mark_bar_diff_against(new_bars[i], s_prev_bars, s_prev_bar_count, continuous_du_dark_phases(), &dirty);
    }
    s_prev_bar_count = new_count;
    if (new_count > 0) memcpy(s_prev_bars, new_bars, new_count * sizeof(*new_bars));
    s_sel_dirty_logical = dirty;

    if (ctx->continuous_ready && continuous_du_busy() && dirty.width > 0 && dirty.height > 0) {
        EpdRect phys = continuous_du_rect_from_logical(dirty);
        (void)continuous_du_scan(ctx->hl, phys);
    }
}

static void extract_clean_excerpt(size_t start_off, size_t end_off, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!s_chapter.utf8 || end_off <= start_off || start_off >= s_chapter.len) return;
    if (end_off > s_chapter.len) end_off = s_chapter.len;
    size_t len = end_off - start_off;
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)s_chapter.utf8[start_off + len] & 0xC0) == 0x80) --len;
    }
    memcpy(out, s_chapter.utf8 + start_off, len);
    out[len] = 0;
}

static void commit_drag_selection(app_ctx_t* ctx) {
    if (!s_selecting) return;
    // 把剩余连续 DU 相位补扫完，确保下划线达到满黑度。
    // Finish draining leftover continuous DU phases so the underline reaches full darkness.
    if (ctx->continuous_ready && s_sel_dirty_logical.width > 0 && s_sel_dirty_logical.height > 0) {
        EpdRect phys = continuous_du_rect_from_logical(s_sel_dirty_logical);
        for (int i = 0; i < 12 && continuous_du_busy(); ++i) {
            (void)continuous_du_scan(ctx->hl, phys);
        }
    }

    char excerpt[BOOK_DATA_HL_TEXT_MAX];
    extract_clean_excerpt(s_sel_start, s_sel_end, excerpt, sizeof(excerpt));
    if (excerpt[0]) {
        uint32_t now_unix = 0;
        (void)ui_statusbar_time_valid(&now_unix);
        (void)book_data_add_highlight(
            &s_rec, (uint16_t)s_chapter_idx,
            (uint32_t)s_sel_start, (uint32_t)s_sel_end,
            now_unix, excerpt, NULL
        );
        // 把已确认的下划线写入 framebuffer，保证后续局部刷新与物理屏一致。
        // Mirror finalized underline bars into fb so later partial updates stay consistent.
        for (size_t i = 0; i < s_prev_bar_count; ++i) {
            epd_fill_rect(s_prev_bars[i], 0, ctx->fb);
        }
        flush_progress_and_duration(ctx->now_ms, false);
    }
    clear_selection_state();
}

/* ---- 工具栏与目录几何 / Toolbar and TOC geometry ---- */

#define TOOLBAR_Y (UI_LOCK_HEIGHT - 216)
#define TOOLBAR_H 216
#define TOOLBAR_BTN_COUNT 6

static EpdRect toolbar_btn_rect(int idx) {
    int col = idx % 3;
    int row = idx / 3;
    int gap = 14;
    int w = (MYPICO_CONTENT_W - 2 * gap) / 3;
    int h = 72;
    return (EpdRect){
        .x = MYPICO_MARGIN + col * (w + gap),
        .y = TOOLBAR_Y + 26 + row * (h + 16),
        .width = w,
        .height = h,
    };
}

static void render_toolbar(uint8_t* fb) {
    EpdRect panel = { 0, TOOLBAR_Y, UI_LOCK_WIDTH, TOOLBAR_H };
    ui_clear_rect_fast(fb, panel);
    ui_hairline(fb, TOOLBAR_Y, 0, UI_LOCK_WIDTH, UI_GRAY_BLACK);

    char px_dec[24], px_inc[24];
    snprintf(px_dec, sizeof(px_dec), "字号- (%d)", s_px);
    snprintf(px_inc, sizeof(px_inc), "字号+ (%d)", s_px);
    const char* labels[TOOLBAR_BTN_COUNT] = {
        "返回书架", "目录", "整屏强刷",
        px_dec, px_inc, "收起工具栏",
    };
    for (int i = 0; i < TOOLBAR_BTN_COUNT; ++i) {
        bool en = true;
        if (i == 3) en = s_px > READER_PX_MIN;
        if (i == 4) en = s_px < READER_PX_MAX;
        ui_mypico_draw_pill(fb, toolbar_btn_rect(i), 26, labels[i], en);
    }
}

static void render_toc(uint8_t* fb) {
    ui_clear_page(fb);
    ui_statusbar_draw(fb);
    ui_text(fb, MYPICO_MARGIN, 74, 36, "目录", EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 134, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);

    size_t total = book_chapter_count();
    int base = s_toc_page * READER_TOC_ROWS;
    int row_h = 92;
    for (int r = 0; r < READER_TOC_ROWS; ++r) {
        size_t idx = (size_t)(base + r);
        if (idx >= total) break;
        int y = 144 + r * row_h;
        char title[160];
        if (book_chapter_title(idx, title, sizeof(title)) != ESP_OK) {
            snprintf(title, sizeof(title), "第 %u 章", (unsigned)idx + 1);
        }
        if (idx == s_chapter_idx) {
            ui_fill_round_rect(fb, (EpdRect){ MYPICO_MARGIN, y + 6, MYPICO_CONTENT_W, row_h - 12 }, 6, 0xE4);
        }
        // 截断超长章节标题。
        // Truncate overly long chapter titles to content width.
        while (title[0] && ttf_text_width_px(28, title) > MYPICO_CONTENT_W - 16) {
            size_t n = strlen(title) - 1;
            while (n > 0 && ((unsigned char)title[n] & 0xC0) == 0x80) --n;
            title[n] = 0;
        }
        ui_text_vc(fb, MYPICO_MARGIN + 8, y + row_h / 2, 28, title, EPD_DRAW_ALIGN_LEFT, false);
        ui_hairline(fb, y + row_h, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    }

    bool can_prev = s_toc_page > 0;
    bool can_next = (size_t)(base + READER_TOC_ROWS) < total;
    ui_mypico_draw_action_bar(fb, "返回", can_prev, can_next);
}

static void render_preview(uint8_t* fb) {
    ui_clear_page(fb);
    ui_statusbar_draw(fb);
    if (!s_preview_img || !s_preview_w || !s_preview_h) return;
    int ox = (UI_LOCK_WIDTH - s_preview_w) / 2;
    int oy = MYPICO_STATUSBAR_H + (UI_LOCK_HEIGHT - MYPICO_STATUSBAR_H - 60 - s_preview_h) / 2;
    if (ox < 0) ox = 0;
    if (oy < MYPICO_STATUSBAR_H) oy = MYPICO_STATUSBAR_H;
    for (int y = 0; y < s_preview_h && oy + y < UI_LOCK_HEIGHT; ++y) {
        for (int x = 0; x < s_preview_w && ox + x < UI_LOCK_WIDTH; ++x) {
            epd_draw_pixel(ox + x, oy + y, s_preview_img[(size_t)y * s_preview_w + x], fb);
        }
    }
    ui_text_vc(fb, UI_LOCK_WIDTH / 2, UI_LOCK_HEIGHT - 32, 24, "轻点任意处返回正文", EPD_DRAW_ALIGN_CENTER, false);
}

/* ---- 翻页辅助 / Page turn helpers ---- */

static app_redraw_t turn_next_page(app_ctx_t* ctx) {
    clear_delete_bubble();
    if (!book_layout_complete() && s_page + 2 >= book_layout_page_count()) {
        (void)book_layout_extend(4);
    }
    if (s_page + 1 < book_layout_page_count()) {
        s_page++;
        touch_reading_activity(ctx->now_ms);
        return APP_REDRAW_PAGE;
    }
    if (s_chapter_idx + 1 < book_chapter_count()) {
        if (load_chapter(s_chapter_idx + 1, 0, false)) {
            touch_reading_activity(ctx->now_ms);
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t turn_prev_page(app_ctx_t* ctx) {
    clear_delete_bubble();
    if (s_page > 0) {
        s_page--;
        touch_reading_activity(ctx->now_ms);
        return APP_REDRAW_PAGE;
    }
    if (s_chapter_idx > 0) {
        if (load_chapter(s_chapter_idx - 1, 0, true)) {
            touch_reading_activity(ctx->now_ms);
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

/* ---- app_desc_t 回调 / Page callbacks ---- */

static void reader_on_enter(app_ctx_t* ctx) {
    close_book_resources();
    ui_statusbar_poll_pmu();
    s_toolbar_open = false;
    s_toc_open = false;
    s_error_msg[0] = 0;
    s_act_last_ms = ctx ? ctx->now_ms : 0;
    s_session_ms_acc = 0;
    s_session_logged_sec = 0;
    s_last_flush_ms = s_act_last_ms;
    s_last_pmu_ms = s_act_last_ms;

    if (!s_target_path[0]) {
        snprintf(s_error_msg, sizeof(s_error_msg), "未选择书籍");
        return;
    }

    book_data_record_init(&s_rec);
    (void)book_data_load(s_target_path, s_target_size, &s_rec);
    s_px = s_rec.px >= READER_PX_MIN && s_rec.px <= READER_PX_MAX ? s_rec.px : 40;

    ESP_LOGI(TAG, "opening %s (%u bytes)", s_target_path, (unsigned)s_target_size);
    esp_err_t err = book_open(s_target_path);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "book_open failed: %s", esp_err_to_name(err));
        snprintf(s_error_msg, sizeof(s_error_msg), "无法打开书籍 (%s)", esp_err_to_name(err));
        return;
    }
    s_book_open = true;
    ESP_LOGI(TAG, "opened kind=%d chapters=%u", (int)book_kind(), (unsigned)book_chapter_count());

    size_t start_ch = s_rec.chapter < book_chapter_count() ? s_rec.chapter : 0;
    if (!load_chapter(start_ch, s_rec.byte_off, false)) {
        ESP_LOGW(TAG, "load_chapter(%u) failed: %s", (unsigned)start_ch, s_error_msg);
        if (start_ch != 0) (void)load_chapter(0, 0, false);
    }
}

static void reader_on_exit(app_ctx_t* ctx) {
    if (s_book_open) {
        flush_progress_and_duration(ctx ? ctx->now_ms : s_act_last_ms, true);
    }
    close_book_resources();
}

static void reader_on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    close_book_resources();
    snprintf(s_error_msg, sizeof(s_error_msg), "TF 卡已断开");
}

static void reader_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (s_preview_img) {
        render_preview(fb);
        return;
    }
    if (s_toc_open) {
        render_toc(fb);
        return;
    }

    ui_clear_page(fb);
    ui_statusbar_draw(fb);

    if (!s_book_open || s_error_msg[0]) {
        ui_text(
            fb, MYPICO_MARGIN, 200, 32,
            s_error_msg[0] ? s_error_msg : "打开书籍失败",
            EPD_DRAW_ALIGN_LEFT, false
        );
        ui_mypico_draw_action_bar(fb, "返回", false, false);
        return;
    }

    book_layout_span_t spans[BOOK_DATA_MAX_HIGHLIGHTS];
    size_t span_n = collect_chapter_spans(spans, BOOK_DATA_MAX_HIGHLIGHTS);
    book_layout_draw_page_highlights(fb, s_page, body_rect(), s_px, spans, span_n);

    if (s_delete_hl_id != 0) {
        draw_delete_bubble(fb);
    }
    if (s_toolbar_open) {
        render_toolbar(fb);
    }
}

static app_redraw_t reader_on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    touch_reading_activity(ctx->now_ms);

    if (!s_book_open || s_error_msg[0]) {
        if (ev->type == UI_GESTURE_TAP && ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
            ctx->request_app = &app_mypico_shelf;
        }
        return APP_REDRAW_NONE;
    }

    // 插图预览：轻点返回正文。
    // Image preview: tap anywhere to return to text.
    if (s_preview_img) {
        if (ev->type == UI_GESTURE_TAP) {
            clear_preview();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    // 目录视图。
    // TOC view.
    if (s_toc_open) {
        size_t total = book_chapter_count();
        int max_toc = total > 0 ? (int)((total - 1) / READER_TOC_ROWS) : 0;
        if (ev->type == UI_GESTURE_SWIPE_L && s_toc_page < max_toc) {
            s_toc_page++;
            return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R && s_toc_page > 0) {
            s_toc_page--;
            return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_TAP) {
            if (ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
                s_toc_open = false;
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(ui_mypico_bar_prev_rect(), ev->x0, ev->y0) && s_toc_page > 0) {
                s_toc_page--;
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(ui_mypico_bar_next_rect(), ev->x0, ev->y0) && s_toc_page < max_toc) {
                s_toc_page++;
                return APP_REDRAW_PAGE;
            }
            if ((int)ev->x0 >= MYPICO_MARGIN && (int)ev->x0 < MYPICO_MARGIN + MYPICO_CONTENT_W
                && (int)ev->y0 >= 144 && (int)ev->y0 < MYPICO_ACTIONBAR_Y) {
                int r = ((int)ev->y0 - 144) / 92;
                size_t target = (size_t)(s_toc_page * READER_TOC_ROWS + r);
                if (r >= 0 && r < READER_TOC_ROWS && target < total) {
                    s_toc_open = false;
                    (void)load_chapter(target, 0, false);
                    return APP_REDRAW_PAGE;
                }
            }
        }
        return APP_REDRAW_NONE;
    }

    // 长按正文：进入跟手拖选划线模式（continuous_du）。
    // Long-press on body text: enter finger-following drag highlight mode.
    if (ev->type == UI_GESTURE_LONG_PRESS && !s_toolbar_open && s_delete_hl_id == 0) {
        size_t cs = 0, ce = 0;
        if (book_layout_char_at(s_page, body_rect(), ev->x0, ev->y0, &cs, &ce, NULL)) {
            clear_selection_state();
            s_selecting = true;
            s_sel_anchor_s = cs;
            s_sel_anchor_e = ce;
            s_sel_start = cs;
            s_sel_end = ce;
            s_sel_last_fallback_ms = ctx->now_ms;

            EpdRect bars[READER_MAX_BARS];
            size_t n = book_layout_span_underlines(s_page, body_rect(), cs, ce, bars, READER_MAX_BARS);
            apply_drag_bars_diff(ctx, bars, n);
            return APP_REDRAW_DONE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_selecting) {
        if (ev->type == UI_GESTURE_CANCEL) {
            // 手指抬起时会由 ui_gesture 发出 CANCEL（因 long_fired=true），在此提交划线。
            // ui_gesture emits CANCEL on lift after long_fired=true; commit the highlight here.
            commit_drag_selection(ctx);
            return APP_REDRAW_DONE;
        }
        return APP_REDRAW_NONE;
    }

    // 底部工具栏打开时的点击处理。
    // Toolbar tap dispatch when open.
    if (s_toolbar_open) {
        if (ev->type == UI_GESTURE_TAP) {
            for (int i = 0; i < TOOLBAR_BTN_COUNT; ++i) {
                if (!ui_rect_hit(toolbar_btn_rect(i), ev->x0, ev->y0)) continue;
                if (i == 0) {
                    s_toolbar_open = false;
                    ctx->request_app = &app_mypico_shelf;
                    return APP_REDRAW_NONE;
                }
                if (i == 1) {
                    s_toolbar_open = false;
                    s_toc_open = true;
                    s_toc_page = (int)(s_chapter_idx / READER_TOC_ROWS);
                    return APP_REDRAW_PAGE;
                }
                if (i == 2) {
                    s_toolbar_open = false;
                    return APP_REDRAW_FULL;
                }
                if (i == 3 && s_px > READER_PX_MIN) {
                    size_t off = book_layout_page_start_offset(s_page);
                    s_px -= READER_PX_STEP;
                    app_settings_set_book_px((uint8_t)s_px);
                    (void)load_chapter(s_chapter_idx, off, false);
                    return APP_REDRAW_PAGE;
                }
                if (i == 4 && s_px < READER_PX_MAX) {
                    size_t off = book_layout_page_start_offset(s_page);
                    s_px += READER_PX_STEP;
                    app_settings_set_book_px((uint8_t)s_px);
                    (void)load_chapter(s_chapter_idx, off, false);
                    return APP_REDRAW_PAGE;
                }
                if (i == 5) {
                    s_toolbar_open = false;
                    return APP_REDRAW_PAGE;
                }
            }
            if ((int)ev->y0 < TOOLBAR_Y) {
                s_toolbar_open = false;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }

    // 删除气泡打开时：点气泡内删除划线，点外部关闭气泡。
    // When delete bubble is visible: tap inside deletes highlight, tap outside dismisses.
    if (s_delete_hl_id != 0) {
        if (ev->type == UI_GESTURE_TAP) {
            if (ui_rect_hit(s_delete_bubble_rect, ev->x0, ev->y0)) {
                (void)book_data_delete_highlight(&s_rec, s_delete_hl_id);
                clear_delete_bubble();
                flush_progress_and_duration(ctx->now_ms, false);
                return APP_REDRAW_PAGE;
            }
            clear_delete_bubble();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    // 正常阅读：左右滑翻页、点击已有划线弹删除气泡、点插图预览、点左中右翻页/唤起工具栏。
    // Normal reading: swipe page turns, tap highlight for delete bubble, tap image, or tap zones.
    if (ev->type == UI_GESTURE_SWIPE_L) return turn_next_page(ctx);
    if (ev->type == UI_GESTURE_SWIPE_R) return turn_prev_page(ctx);

    if (ev->type == UI_GESTURE_TAP) {
        // 1) 优先判断是否点中已有划线。
        // 1) Prioritize tapping an existing highlight to pop up the delete bubble.
        size_t cs = 0, ce = 0;
        if (book_layout_char_at(s_page, body_rect(), ev->x0, ev->y0, &cs, &ce, NULL)) {
            int hl_idx = book_data_find_highlight_at(&s_rec, (uint16_t)s_chapter_idx, (uint32_t)cs);
            if (hl_idx >= 0) {
                compute_delete_bubble_position(&s_rec.highlights[hl_idx]);
                if (s_delete_hl_id != 0) {
                    return APP_REDRAW_PAGE;
                }
            }
        }

        // 2) 检查 EPUB 插图占位。
        // 2) Check EPUB image placeholder tap.
        size_t blk_idx = book_layout_image_at(s_page, body_rect(), ev->x0, ev->y0, NULL);
        if (blk_idx != SIZE_MAX && blk_idx < s_chapter.count && s_chapter.blocks[blk_idx].image_src) {
            uint8_t* pix = NULL;
            uint16_t w = 0, h = 0;
            if (book_chapter_load_image(s_chapter_idx, s_chapter.blocks[blk_idx].image_src, &pix, &w, &h) == ESP_OK) {
                clear_preview();
                s_preview_img = pix;
                s_preview_w = w;
                s_preview_h = h;
                return APP_REDRAW_PAGE;
            }
        }

        // 3) 左/中/右分区：左翻上页、右翻下页、中唤工具栏。
        // 3) Left/center/right tap zones: prev page, toolbar, next page.
        if (ev->x0 < (UI_LOCK_WIDTH * 28) / 100) return turn_prev_page(ctx);
        if (ev->x0 > (UI_LOCK_WIDTH * 72) / 100) return turn_next_page(ctx);
        s_toolbar_open = true;
        return APP_REDRAW_PAGE;
    }

    return APP_REDRAW_NONE;
}

static app_redraw_t reader_on_key(app_ctx_t* ctx, int key) {
    touch_reading_activity(ctx->now_ms);
    if (!s_book_open || s_error_msg[0]) {
        if (key == UI_KEY_1) ctx->request_app = &app_mypico_shelf;
        if (key == UI_KEY_2) return APP_REDRAW_FULL;
        return APP_REDRAW_NONE;
    }
    if (s_preview_img) {
        clear_preview();
        return APP_REDRAW_PAGE;
    }
    if (s_toc_open) {
        size_t total = book_chapter_count();
        int max_toc = total > 0 ? (int)((total - 1) / READER_TOC_ROWS) : 0;
        if (key == UI_KEY_1) {
            if (s_toc_page > 0) { s_toc_page--; return APP_REDRAW_PAGE; }
            s_toc_open = false;
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_2) return APP_REDRAW_FULL;
        if (key == UI_KEY_3 && s_toc_page < max_toc) { s_toc_page++; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_1) return turn_prev_page(ctx);
    if (key == UI_KEY_3) return turn_next_page(ctx);
    if (key == UI_KEY_2) {
        clear_delete_bubble();
        s_toolbar_open = !s_toolbar_open;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t reader_on_key_long(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key == UI_KEY_2) {
        s_toolbar_open = false;
        return APP_REDRAW_FULL;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t reader_on_tick(app_ctx_t* ctx) {
    // 1) 跟手拖选模式：实时追踪手指位置并用 continuous_du 驱动下划线相位。
    // 1) Drag selection mode: track live finger coordinates and scan continuous_du underline phases.
    if (s_selecting) {
        if (!ctx->touch || !ctx->touch->touched || ctx->released) {
            commit_drag_selection(ctx);
            return APP_REDRAW_DONE;
        }
        size_t cs = 0, ce = 0;
        if (book_layout_char_at(s_page, body_rect(), ctx->touch->x, ctx->touch->y, &cs, &ce, NULL)) {
            size_t ns = cs < s_sel_anchor_s ? cs : s_sel_anchor_s;
            size_t ne = ce > s_sel_anchor_e ? ce : s_sel_anchor_e;
            if (ns != s_sel_start || ne != s_sel_end) {
                s_sel_start = ns;
                s_sel_end = ne;
                EpdRect bars[READER_MAX_BARS];
                size_t n = book_layout_span_underlines(s_page, body_rect(), ns, ne, bars, READER_MAX_BARS);
                apply_drag_bars_diff(ctx, bars, n);
            }
        }
        if (ctx->continuous_ready && continuous_du_busy()
            && s_sel_dirty_logical.width > 0 && s_sel_dirty_logical.height > 0) {
            EpdRect phys = continuous_du_rect_from_logical(s_sel_dirty_logical);
            (void)continuous_du_scan(ctx->hl, phys);
        } else if (!ctx->continuous_ready && ctx->now_ms - s_sel_last_fallback_ms >= 90) {
            s_sel_last_fallback_ms = ctx->now_ms;
            for (size_t i = 0; i < s_prev_bar_count; ++i) epd_fill_rect(s_prev_bars[i], 0, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    // 2) 后台增量补全当前章分页（每次 2 页，不阻塞交互）。
    // 2) Incrementally finish chapter pagination (2 pages per tick, non-blocking).
    if (s_book_open && !book_layout_complete()) {
        (void)book_layout_extend(2);
    }

    // 3) 定期落盘进度与时长，防止意外断电丢数据。
    // 3) Periodically flush reading progress and duration.
    if (s_book_open && ctx->now_ms - s_last_flush_ms >= READER_FLUSH_INTERVAL_MS) {
        flush_progress_and_duration(ctx->now_ms, false);
    }

    // 4) 定期轮询 PMU 状态栏时间/电量。
    // 4) Periodically refresh PMU status bar snapshot.
    if (ctx->now_ms - s_last_pmu_ms >= READER_PMU_POLL_MS) {
        s_last_pmu_ms = ctx->now_ms;
        ui_statusbar_poll_pmu();
    }

    return APP_REDRAW_NONE;
}

static EpdRect reader_area_hint(app_ctx_t* ctx) {
    (void)ctx;
    if (s_sel_dirty_logical.width > 0 && s_sel_dirty_logical.height > 0) {
        return s_sel_dirty_logical;
    }
    return body_rect();
}

const app_desc_t app_mypico_reader = {
    .title = "阅读",
    .detail = "MyPico 阅读器与跟手划线",
    .enter_full = true,
    .owns_keys = true,
    .no_menu_handle = true,
    .on_enter = reader_on_enter,
    .on_exit = reader_on_exit,
    .on_media_lost = reader_on_media_lost,
    .render = reader_render,
    .on_gesture = reader_on_gesture,
    .on_key = reader_on_key,
    .on_key_long = reader_on_key_long,
    .on_tick = reader_on_tick,
    .area_hint = reader_area_hint,
};
