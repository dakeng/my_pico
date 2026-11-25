/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 书架页、单书笔记页与单书热点同步面板。
 * 视觉严格对齐 Figma「书架 (9:178)」与「笔记 (9:264)」：
 * - 书架每页展示 3 本书：封面、书名、作者、阅读进度、阅读时长、最近阅读、划线数、「同步」「查看笔记」
 * - 底部操作栏：首页 / 上一页 / 下一页
 * - 笔记页展示单书划线摘录（左侧浅灰竖条 + 正文）与底部操作栏
 *
 * English: MyPico bookshelf, per-book notes list, and single-book hotspot sync overlay.
 * Matches Figma Shelf (9:178) and Notes (9:264) designs.
 *
 * 冻结：render 为纯绘制；封面与 EPUB 元数据解析仅在 on_enter / 翻页加载阶段执行并落盘缓存。
 * Frozen: render is paint-only; EPUB metadata/cover extraction runs only outside render and caches to TF card.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app.h"
#include "book_data.h"
#include "book_epub.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mypico_reader_state.h"
#include "mypico_sync.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_statusbar.h"

#define TAG "mypico_shelf"

#define SHELF_PER_PAGE 3
#define SHELF_CARD_TOP 75
#define SHELF_CARD_STRIDE 344
#define SHELF_CARD_H 316
#define SHELF_COVER_X MYPICO_MARGIN
#define SHELF_COVER_W BOOK_COVER_W
#define SHELF_COVER_H BOOK_COVER_H
#define SHELF_INFO_X (MYPICO_MARGIN + SHELF_COVER_W + 28)
#define SHELF_INFO_W (UI_LOCK_WIDTH - MYPICO_MARGIN - SHELF_INFO_X)

#define NOTES_TITLE_Y 74
#define NOTES_LIST_TOP 156
#define NOTES_LIST_BOTTOM (MYPICO_ACTIONBAR_Y - 16)
#define NOTES_BAR_W 5
#define NOTES_TEXT_GAP 12
#define NOTES_TEXT_PX 28
#define NOTES_LINE_H 40
#define NOTES_ITEM_GAP 24
#define NOTES_MAX_PAGES 64

typedef enum {
    VIEW_SHELF = 0,
    VIEW_NOTES,
    VIEW_SYNC,
} shelf_view_t;

typedef struct {
    char book_id[BOOK_DATA_ID_LEN];
    char file_path[BOOK_DATA_PATH_MAX];
    uint32_t file_size;
    char title[BOOK_DATA_TITLE_MAX];
    char author[BOOK_DATA_AUTHOR_MAX];
    bool has_cover;
    uint8_t pct;
    uint32_t total_read_sec;
    uint32_t last_read_ts;
    uint32_t highlight_count;
} shelf_item_t;

static shelf_view_t s_view = VIEW_SHELF;
static shelf_item_t* s_items;
static int s_item_count;
static int s_shelf_page;
static bool s_dirty = true;
static unsigned s_store_revision;
static bool s_sd_ready;
static char s_shelf_error[96];

// 当前页 3 个槽位的封面 8bpp 灰度缓存（仅保留当前页 3 张，避免占满 PSRAM）。
// 8bpp grayscale cover cache for the 3 visible slots on the current shelf page.
static uint8_t* s_slot_cover[SHELF_PER_PAGE];
static uint16_t s_slot_cover_w[SHELF_PER_PAGE];
static uint16_t s_slot_cover_h[SHELF_PER_PAGE];
static char s_slot_cover_id[SHELF_PER_PAGE][BOOK_DATA_ID_LEN];

// 笔记页状态（按当前页起始索引分页，适配长短不一的划线卡片）。
// Notes view state (variable-height highlight cards paged by starting index).
static book_record_t s_notes_rec;
static int s_notes_page;
static int s_notes_page_starts[NOTES_MAX_PAGES];
static int s_notes_page_count;

// 同步页状态。
// Sync overlay state.
static mypico_sync_status_t s_sync_status;
static mypico_sync_state_t s_last_sync_state;
static uint32_t s_last_sync_pulls;
static int64_t s_last_poll_ms;

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

void mypico_shelf_mark_dirty(void) {
    s_dirty = true;
}

static void free_slot_covers(void) {
    for (int i = 0; i < SHELF_PER_PAGE; ++i) {
        free(s_slot_cover[i]);
        s_slot_cover[i] = NULL;
        s_slot_cover_w[i] = 0;
        s_slot_cover_h[i] = 0;
        s_slot_cover_id[i][0] = 0;
    }
}

static bool is_supported_book_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".epub") == 0 || strcasecmp(dot, ".txt") == 0;
}

static int compare_shelf_items(const void* a, const void* b) {
    const shelf_item_t* x = (const shelf_item_t*)a;
    const shelf_item_t* y = (const shelf_item_t*)b;
    if (x->last_read_ts != y->last_read_ts) {
        return y->last_read_ts > x->last_read_ts ? 1 : -1;
    }
    return strcasecmp(x->title, y->title);
}

static void load_visible_covers(void) {
    int base = s_shelf_page * SHELF_PER_PAGE;
    for (int slot = 0; slot < SHELF_PER_PAGE; ++slot) {
        int idx = base + slot;
        if (idx >= s_item_count) {
            free(s_slot_cover[slot]);
            s_slot_cover[slot] = NULL;
            s_slot_cover_id[slot][0] = 0;
            continue;
        }
        shelf_item_t* it = &s_items[idx];
        if (s_slot_cover[slot] && strcmp(s_slot_cover_id[slot], it->book_id) == 0) {
            continue;
        }
        free(s_slot_cover[slot]);
        s_slot_cover[slot] = NULL;
        s_slot_cover_w[slot] = 0;
        s_slot_cover_h[slot] = 0;
        s_slot_cover_id[slot][0] = 0;

        if (!it->has_cover) continue;
        uint16_t w = 0, h = 0;
        uint8_t* gray = NULL;
        if (book_data_cover_load(it->book_id, &w, &h, &gray) == ESP_OK) {
            s_slot_cover[slot] = gray;
            s_slot_cover_w[slot] = w;
            s_slot_cover_h[slot] = h;
            strcpy(s_slot_cover_id[slot], it->book_id);
        }
    }
}

// 扫描当前页尚未解析过元数据的 EPUB，按需提取书名、作者与封面并落盘缓存。
// Lazily extract title, author and cover for unparsed EPUBs on the visible shelf page.
static void hydrate_visible_metadata(void) {
    int base = s_shelf_page * SHELF_PER_PAGE;
    for (int slot = 0; slot < SHELF_PER_PAGE; ++slot) {
        int idx = base + slot;
        if (idx >= s_item_count) break;
        shelf_item_t* it = &s_items[idx];
        const char* ext = strrchr(it->file_path, '.');
        if (!ext || strcasecmp(ext, ".epub") != 0) continue;

        book_record_t rec;
        book_data_record_init(&rec);
        if (book_data_load(it->file_path, it->file_size, &rec) != ESP_OK) {
            book_data_record_free(&rec);
            continue;
        }
        if (!rec.meta_parsed || (!rec.author[0] && !rec.has_cover)) {
            book_epub_meta_t meta;
            if (book_epub_read_meta(it->file_path, &meta) == ESP_OK) {
                if (meta.title[0]) copy_str(rec.title, sizeof(rec.title), meta.title);
                if (meta.author[0]) copy_str(rec.author, sizeof(rec.author), meta.author);
                if (meta.has_cover) {
                    uint8_t* thumb = NULL;
                    if (book_epub_load_cover_thumb(
                            it->file_path, SHELF_COVER_W, SHELF_COVER_H, &thumb) == ESP_OK) {
                        if (book_data_cover_save(rec.book_id, SHELF_COVER_W, SHELF_COVER_H, thumb) == ESP_OK) {
                            rec.has_cover = true;
                        }
                        free(thumb);
                    }
                }
                rec.meta_parsed = true;
                (void)book_data_save(&rec);
                copy_str(it->title, sizeof(it->title), rec.title);
                copy_str(it->author, sizeof(it->author), rec.author);
                it->has_cover = rec.has_cover;
            }
        }
        book_data_record_free(&rec);
    }
    load_visible_covers();
}

static void scan_shelf(void) {
    free_slot_covers();
    free(s_items);
    s_items = NULL;
    s_item_count = 0;
    s_shelf_error[0] = 0;

    // 复用 book_store_roots()：TF 卡优先，未插卡时兼容内置 /flash/books。
    // Reuse book_store_roots(): prefer TF card, and also support internal /flash/books when no card is present.
    read_pico_sd_info_t sd = {0};
    esp_err_t sd_err = read_pico_sd_get_info(&sd);
    if (sd_err == ESP_ERR_INVALID_STATE) {
        (void)read_pico_sd_start_probe();
    }

    book_store_root_t store_roots[2];
    int store_n = 0;
    esp_err_t root_err = book_store_roots(store_roots, &store_n);
    if (root_err != ESP_OK || store_n <= 0) {
        s_sd_ready = false;
        snprintf(s_shelf_error, sizeof(s_shelf_error), "未检测到可用存储，请插卡或在「设置」中挂载");
        s_store_revision = book_store_revision();
        s_dirty = false;
        return;
    }

    s_sd_ready = true;
    bool has_sd = !store_roots[0].is_flash;
    book_data_set_root(has_sd ? BOOK_DATA_DEFAULT_ROOT : "/flash/.mypico");
    (void)book_data_ensure_dirs();

    // 扫描所有可用书源目录（TF 卡 /sdcard/books、内置 /flash/books，以及可选的 /sdcard 根目录）。
    // Scan all available book roots (/sdcard/books, /flash/books, and fallback /sdcard).
    const char* dirs[3];
    int dir_n = 0;
    for (int i = 0; i < store_n && dir_n < 3; ++i) {
        dirs[dir_n++] = store_roots[i].path;
    }
    if (has_sd && dir_n < 3) {
        dirs[dir_n++] = "/sdcard";
    }

    int cap = 0;
    for (int r = 0; r < dir_n; ++r) {
        DIR* dir = opendir(dirs[r]);
        if (!dir) continue;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_supported_book_ext(ent->d_name)) continue;

            char full[BOOK_DATA_PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", dirs[r], ent->d_name) >= (int)sizeof(full)) {
                continue;
            }
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            if (s_item_count >= cap) {
                int next = cap ? cap * 2 : 16;
                shelf_item_t* grown = heap_caps_realloc(
                    s_items, (size_t)next * sizeof(*s_items), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                );
                if (!grown) break;
                s_items = grown;
                cap = next;
            }

            book_record_t rec;
            book_data_record_init(&rec);
            (void)book_data_load(full, (uint32_t)st.st_size, &rec);

            shelf_item_t* it = &s_items[s_item_count++];
            memset(it, 0, sizeof(*it));
            strcpy(it->book_id, rec.book_id);
            copy_str(it->file_path, sizeof(it->file_path), full);
            it->file_size = (uint32_t)st.st_size;
            copy_str(it->title, sizeof(it->title), rec.title);
            copy_str(it->author, sizeof(it->author), rec.author);
            it->has_cover = rec.has_cover;
            it->pct = rec.pct;
            it->total_read_sec = rec.total_read_sec;
            it->last_read_ts = rec.last_read_ts;
            it->highlight_count = (uint32_t)rec.highlight_count;
            book_data_record_free(&rec);
        }
        closedir(dir);
    }

    if (s_item_count > 1) {
        qsort(s_items, (size_t)s_item_count, sizeof(*s_items), compare_shelf_items);
    }
    int max_page = s_item_count > 0 ? (s_item_count - 1) / SHELF_PER_PAGE : 0;
    if (s_shelf_page > max_page) s_shelf_page = max_page;
    s_store_revision = book_store_revision();
    s_dirty = false;
    ESP_LOGI(TAG, "shelf scanned: %d books (has_sd=%d)", s_item_count, (int)has_sd);
    hydrate_visible_metadata();
}

/* ---- 书架卡片几何与绘制 / Shelf card geometry and rendering ---- */

static EpdRect shelf_card_rect(int slot) {
    return (EpdRect){
        .x = MYPICO_MARGIN,
        .y = SHELF_CARD_TOP + slot * SHELF_CARD_STRIDE,
        .width = MYPICO_CONTENT_W,
        .height = SHELF_CARD_H,
    };
}

static EpdRect shelf_sync_btn_rect(int slot) {
    int y = SHELF_CARD_TOP + slot * SHELF_CARD_STRIDE + 260;
    return (EpdRect){ .x = SHELF_INFO_X, .y = y, .width = 76, .height = 48 };
}

static EpdRect shelf_notes_btn_rect(int slot) {
    int y = SHELF_CARD_TOP + slot * SHELF_CARD_STRIDE + 260;
    return (EpdRect){ .x = SHELF_INFO_X + 76 + 12, .y = y, .width = 126, .height = 48 };
}

static void draw_truncated_text(uint8_t* fb, int x, int y, int px, int max_w, const char* src) {
    char buf[160];
    strncpy(buf, src ? src : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    bool clipped = false;
    while (buf[0] && ttf_text_width_px(px, buf) > max_w) {
        size_t n = strlen(buf) - 1;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) --n;
        buf[n] = 0;
        clipped = true;
    }
    if (clipped && strlen(buf) + 4 < sizeof(buf)) {
        size_t n = strlen(buf);
        if (n >= 3) {
            size_t m = n - 1;
            while (m > 0 && ((unsigned char)buf[m] & 0xC0) == 0x80) --m;
            buf[m] = 0;
        }
        strcat(buf, "…");
    }
    ui_text(fb, x, y, px, buf, EPD_DRAW_ALIGN_LEFT, false);
}

static int wrap_note_text(
    uint8_t* fb, int x, int y, int max_w, int px, int line_h, const char* text, int max_lines
);

static void draw_cover_slot(uint8_t* fb, int slot, const shelf_item_t* it, int x, int y) {
    if (s_slot_cover[slot] && s_slot_cover_w[slot] && s_slot_cover_h[slot]) {
        uint16_t cw = s_slot_cover_w[slot];
        uint16_t ch = s_slot_cover_h[slot];
        const uint8_t* pix = s_slot_cover[slot];
        for (int py = 0; py < SHELF_COVER_H && py < ch; ++py) {
            for (int px = 0; px < SHELF_COVER_W && px < cw; ++px) {
                epd_draw_pixel(x + px, y + py, pix[(size_t)py * cw + px], fb);
            }
        }
        // 细浅灰描边使浅色封面也有清晰边界。
        // Light border so bright covers keep a crisp frame.
        epd_draw_rect((EpdRect){ x, y, SHELF_COVER_W, SHELF_COVER_H }, UI_GRAY_LIGHT, fb);
        return;
    }

    // 无封面图时绘制简洁的纸书占位封面（内嵌书名）。
    // Minimal book cover placeholder showing the book title when no image is available.
    EpdRect box = { x, y, SHELF_COVER_W, SHELF_COVER_H };
    epd_fill_rect(box, 0xE8, fb);
    epd_draw_rect(box, UI_GRAY_LIGHT, fb);
    ui_hairline(fb, y + 20, x + 14, SHELF_COVER_W - 28, UI_GRAY_LIGHT);
    ui_hairline(fb, y + SHELF_COVER_H - 22, x + 14, SHELF_COVER_W - 28, UI_GRAY_LIGHT);
    wrap_note_text(
        fb, x + 14, y + 36, SHELF_COVER_W - 28, 22, 30,
        it && it->title[0] ? it->title : "EPUB", 4
    );
}

static void render_shelf(uint8_t* fb) {
    ui_clear_page(fb);
    ui_statusbar_draw(fb);

    if (!s_sd_ready) {
        ui_text(fb, MYPICO_MARGIN, 180, 30, s_shelf_error, EPD_DRAW_ALIGN_LEFT, false);
        ui_mypico_draw_action_bar(fb, "首页", false, false);
        return;
    }

    if (s_item_count == 0) {
        ui_text(fb, MYPICO_MARGIN, 180, 32, "书架还是空的", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(
            fb, MYPICO_MARGIN, 240, 26,
            "请通过「传书 Transfer」或将 EPUB 放入 TF 卡 /books 目录。",
            EPD_DRAW_ALIGN_LEFT, false
        );
        ui_mypico_draw_action_bar(fb, "首页", false, false);
        return;
    }

    int base = s_shelf_page * SHELF_PER_PAGE;
    int16_t tz = app_settings_tz_offset_min();

    for (int slot = 0; slot < SHELF_PER_PAGE; ++slot) {
        int idx = base + slot;
        if (idx >= s_item_count) break;
        const shelf_item_t* it = &s_items[idx];
        int top = SHELF_CARD_TOP + slot * SHELF_CARD_STRIDE;

        draw_cover_slot(fb, slot, it, SHELF_COVER_X, top);

        // 右侧信息列严格匹配 Figma 书 item 字段与顺序。
        // Right metadata column strictly matches the Figma book item fields and order.
        draw_truncated_text(fb, SHELF_INFO_X, top, 36, SHELF_INFO_W, it->title);
        draw_truncated_text(
            fb, SHELF_INFO_X, top + 54, 24, SHELF_INFO_W,
            it->author[0] ? it->author : "未知作者"
        );

        char line[64];
        snprintf(line, sizeof(line), "阅读进度：%u%%", (unsigned)it->pct);
        ui_text(fb, SHELF_INFO_X, top + 92, 24, line, EPD_DRAW_ALIGN_LEFT, false);

        char dur[24];
        ui_statusbar_format_duration(it->total_read_sec, dur);
        snprintf(line, sizeof(line), "阅读时长：%s", dur);
        ui_text(fb, SHELF_INFO_X, top + 130, 24, line, EPD_DRAW_ALIGN_LEFT, false);

        char date[16];
        ui_statusbar_format_date(it->last_read_ts, tz, date);
        snprintf(line, sizeof(line), "最近阅读：%s", date);
        ui_text(fb, SHELF_INFO_X, top + 168, 24, line, EPD_DRAW_ALIGN_LEFT, false);

        snprintf(line, sizeof(line), "划线：%u", (unsigned)it->highlight_count);
        ui_text(fb, SHELF_INFO_X, top + 206, 24, line, EPD_DRAW_ALIGN_LEFT, false);

        ui_mypico_draw_pill(fb, shelf_sync_btn_rect(slot), 24, "同步", true);
        ui_mypico_draw_pill(fb, shelf_notes_btn_rect(slot), 24, "查看笔记", true);
    }

    bool can_prev = s_shelf_page > 0;
    bool can_next = base + SHELF_PER_PAGE < s_item_count;
    ui_mypico_draw_action_bar(fb, "首页", can_prev, can_next);
}

/* ---- 单书笔记页排版与绘制 / Per-book notes pagination and rendering ---- */

// 估算一段 UTF-8 摘录在指定宽度内占几行（同时供分页与绘制复用）。
// Measure or draw wrapped UTF-8 lines inside a width budget.
static int wrap_note_text(
    uint8_t* fb, int x, int y, int max_w, int px, int line_h, const char* text, int max_lines
) {
    if (!text || !*text) return 0;
    int lines = 0;
    const char* p = text;
    char line[256];

    while (*p && (max_lines <= 0 || lines < max_lines)) {
        if (*p == '\r') { ++p; continue; }
        if (*p == '\n') { ++p; lines++; continue; }

        size_t len = 0;
        int w = 0;
        while (*p && *p != '\n' && *p != '\r') {
            unsigned char c = (unsigned char)*p;
            size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 :
                       (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
            if (len + n >= sizeof(line)) break;
            char glyph[5] = {0};
            memcpy(glyph, p, n);
            int gw = ttf_text_width_px(px, glyph);
            if (gw < 0) gw = 0;
            if (len > 0 && w + gw > max_w) break;
            memcpy(line + len, p, n);
            len += n;
            w += gw;
            p += n;
        }
        line[len] = 0;
        if (fb && len > 0) {
            ui_text(fb, x, y + lines * line_h, px, line, EPD_DRAW_ALIGN_LEFT, false);
        }
        lines++;
        if (*p == '\r') ++p;
        if (*p == '\n') ++p;
    }
    return lines > 0 ? lines : 1;
}

static void paginate_notes(void) {
    s_notes_page = 0;
    s_notes_page_count = 1;
    s_notes_page_starts[0] = 0;
    if (s_notes_rec.highlight_count == 0) return;

    const int text_w = MYPICO_CONTENT_W - NOTES_BAR_W - NOTES_TEXT_GAP;
    int y = NOTES_LIST_TOP;
    for (size_t i = 0; i < s_notes_rec.highlight_count; ++i) {
        int lines = wrap_note_text(
            NULL, 0, 0, text_w, NOTES_TEXT_PX, NOTES_LINE_H, s_notes_rec.highlights[i].text, 12
        );
        int h = lines * NOTES_LINE_H;
        if (y > NOTES_LIST_TOP && y + h > NOTES_LIST_BOTTOM) {
            if (s_notes_page_count < NOTES_MAX_PAGES) {
                s_notes_page_starts[s_notes_page_count++] = (int)i;
            }
            y = NOTES_LIST_TOP;
        }
        y += h + NOTES_ITEM_GAP;
    }
}

static void open_notes_for_item(const shelf_item_t* it) {
    book_data_record_free(&s_notes_rec);
    book_data_record_init(&s_notes_rec);
    (void)book_data_load(it->file_path, it->file_size, &s_notes_rec);
    paginate_notes();
    s_view = VIEW_NOTES;
}

static void render_notes(uint8_t* fb) {
    ui_clear_page(fb);
    ui_statusbar_draw(fb);

    char header[192];
    snprintf(header, sizeof(header), "笔记：%s", s_notes_rec.title);
    draw_truncated_text(fb, MYPICO_MARGIN, NOTES_TITLE_Y, 36, MYPICO_CONTENT_W, header);

    if (s_notes_rec.highlight_count == 0) {
        ui_text(
            fb, MYPICO_MARGIN, NOTES_LIST_TOP + 20, 28,
            "暂无划线笔记。在阅读页面长按并拖动即可划线。",
            EPD_DRAW_ALIGN_LEFT, false
        );
    } else {
        int start = s_notes_page_starts[s_notes_page];
        int end = (s_notes_page + 1 < s_notes_page_count)
            ? s_notes_page_starts[s_notes_page + 1]
            : (int)s_notes_rec.highlight_count;
        const int text_x = MYPICO_MARGIN + NOTES_BAR_W + NOTES_TEXT_GAP;
        const int text_w = MYPICO_CONTENT_W - NOTES_BAR_W - NOTES_TEXT_GAP;
        int y = NOTES_LIST_TOP;

        for (int i = start; i < end; ++i) {
            int lines = wrap_note_text(
                NULL, 0, 0, text_w, NOTES_TEXT_PX, NOTES_LINE_H, s_notes_rec.highlights[i].text, 12
            );
            int h = lines * NOTES_LINE_H;
            if (y + h > NOTES_LIST_BOTTOM && y > NOTES_LIST_TOP) break;
            // Figma 左侧竖条引用样式 (#d9d9d9 -> 0xD0)。
            // Left vertical quote bar matching Figma (#d9d9d9 -> 0xD0).
            epd_fill_rect((EpdRect){ MYPICO_MARGIN, y + 4, NOTES_BAR_W, h - 8 }, 0xD0, fb);
            wrap_note_text(
                fb, text_x, y, text_w, NOTES_TEXT_PX, NOTES_LINE_H,
                s_notes_rec.highlights[i].text, 12
            );
            y += h + NOTES_ITEM_GAP;
        }
    }

    bool can_prev = s_notes_page > 0;
    bool can_next = s_notes_page + 1 < s_notes_page_count;
    ui_mypico_draw_action_bar(fb, "返回", can_prev, can_next);
}

/* ---- 单书热点同步界面 / Single-book hotspot sync view ---- */

static EpdRect sync_close_btn_rect(void) {
    return (EpdRect){ MYPICO_MARGIN, MYPICO_ACTIONBAR_Y + 24, 180, 58 };
}

static EpdRect sync_wifi_btn_rect(void) {
    return (EpdRect){ UI_LOCK_WIDTH - MYPICO_MARGIN - 240, MYPICO_ACTIONBAR_Y + 24, 240, 58 };
}

static void start_sync_for_item(const shelf_item_t* it) {
    book_record_t rec;
    book_data_record_init(&rec);
    (void)book_data_load(it->file_path, it->file_size, &rec);
    (void)mypico_sync_start(&rec, app_settings_tz_offset_min());
    book_data_record_free(&rec);
    mypico_sync_get_status(&s_sync_status);
    s_last_sync_state = s_sync_status.state;
    s_last_sync_pulls = s_sync_status.pull_count;
    s_view = VIEW_SYNC;
}

static void render_sync(uint8_t* fb) {
    ui_clear_page(fb);
    ui_statusbar_draw(fb);

    char title_line[192];
    snprintf(title_line, sizeof(title_line), "同步：%s", s_sync_status.book_title);
    draw_truncated_text(fb, MYPICO_MARGIN, 74, 36, MYPICO_CONTENT_W, title_line);
    ui_hairline(fb, 134, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);

    const char* state_label = "准备中";
    switch (s_sync_status.state) {
        case MYPICO_SYNC_NO_WIFI: state_label = "未配置热点 Wi-Fi"; break;
        case MYPICO_SYNC_CONNECTING: state_label = "正在连接手机热点…"; break;
        case MYPICO_SYNC_READY: state_label = "已就绪，请点击 iPhone 快捷指令"; break;
        case MYPICO_SYNC_DONE: state_label = "同步完成！"; break;
        case MYPICO_SYNC_ERROR: state_label = "连接失败"; break;
        default: break;
    }

    ui_text(fb, MYPICO_MARGIN, 168, 34, state_label, EPD_DRAW_ALIGN_LEFT, false);

    int y = 240;
    char line[192];
    if (s_sync_status.ssid[0]) {
        snprintf(line, sizeof(line), "热点名称：%s", s_sync_status.ssid);
        draw_truncated_text(fb, MYPICO_MARGIN, y, 26, MYPICO_CONTENT_W, line);
        y += 48;
    }
    if (s_sync_status.ip[0]) {
        snprintf(line, sizeof(line), "同步地址：http://%s/api/sync", MYPICO_SYNC_HOSTNAME);
        draw_truncated_text(fb, MYPICO_MARGIN, y, 26, MYPICO_CONTENT_W, line);
        y += 44;
        snprintf(line, sizeof(line), "备用 IP：http://%s/api/sync", s_sync_status.ip);
        draw_truncated_text(fb, MYPICO_MARGIN, y, 24, MYPICO_CONTENT_W, line);
        y += 48;
    }
    if (s_sync_status.note_filename[0]) {
        snprintf(line, sizeof(line), "笔记文件：%s", s_sync_status.note_filename);
        draw_truncated_text(fb, MYPICO_MARGIN, y, 24, MYPICO_CONTENT_W, line);
        y += 48;
    }
    if (s_sync_status.message[0]) {
        wrap_note_text(fb, MYPICO_MARGIN, y + 12, MYPICO_CONTENT_W, 24, 38, s_sync_status.message, 4);
        y += 120;
    }

    if (s_sync_status.state == MYPICO_SYNC_READY || s_sync_status.state == MYPICO_SYNC_CONNECTING) {
        ui_hairline(fb, y + 20, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
        wrap_note_text(
            fb, MYPICO_MARGIN, y + 44, MYPICO_CONTENT_W, 24, 38,
            "提示：请确认 iPhone 个人热点已开启「最大兼容性」(2.4GHz)，连上后直接点击桌面快捷指令图标一键同步到 iCloud Obsidian。",
            5
        );
    }

    if (s_sync_status.state == MYPICO_SYNC_DONE && s_sync_status.time_calibrated) {
        ui_text(fb, MYPICO_MARGIN, y + 20, 24, "设备时钟已同步至手机时间。", EPD_DRAW_ALIGN_LEFT, false);
    }

    ui_mypico_draw_pill(fb, sync_close_btn_rect(), 28, "完成返回", true);
    if (s_sync_status.state == MYPICO_SYNC_NO_WIFI || s_sync_status.state == MYPICO_SYNC_ERROR) {
        ui_mypico_draw_pill(fb, sync_wifi_btn_rect(), 26, "去配置热点 Wi-Fi", true);
    }
}

/* ---- app_desc_t 生命周期与交互 / Page lifecycle and input ---- */

static void shelf_on_enter(app_ctx_t* ctx) {
    ui_statusbar_poll_pmu();
    s_last_poll_ms = ctx ? ctx->now_ms : 0;
    s_view = VIEW_SHELF;
    if (s_dirty || !s_items || s_store_revision != book_store_revision()) {
        scan_shelf();
    } else {
        hydrate_visible_metadata();
    }
}

static void shelf_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    mypico_sync_stop();
    free_slot_covers();
    book_data_record_free(&s_notes_rec);
}

static void shelf_on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    mypico_sync_stop();
    free_slot_covers();
    free(s_items);
    s_items = NULL;
    s_item_count = 0;
    s_sd_ready = false;
    s_dirty = true;
    s_view = VIEW_SHELF;
    snprintf(s_shelf_error, sizeof(s_shelf_error), "TF 卡已断开，请重新插卡挂载");
}

static void shelf_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (s_view == VIEW_NOTES) render_notes(fb);
    else if (s_view == VIEW_SYNC) render_sync(fb);
    else render_shelf(fb);
}

static app_redraw_t shelf_on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view == VIEW_SYNC) {
        if (ev->type == UI_GESTURE_TAP) {
            if (ui_rect_hit(sync_close_btn_rect(), ev->x0, ev->y0)) {
                mypico_sync_stop();
                s_view = VIEW_SHELF;
                return APP_REDRAW_PAGE;
            }
            if ((s_sync_status.state == MYPICO_SYNC_NO_WIFI || s_sync_status.state == MYPICO_SYNC_ERROR)
                && ui_rect_hit(sync_wifi_btn_rect(), ev->x0, ev->y0)) {
                mypico_sync_stop();
                s_view = VIEW_SHELF;
                ctx->request_app = &app_transfer;
                return APP_REDRAW_NONE;
            }
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == VIEW_NOTES) {
        if (ev->type == UI_GESTURE_SWIPE_L && s_notes_page + 1 < s_notes_page_count) {
            s_notes_page++;
            return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R && s_notes_page > 0) {
            s_notes_page--;
            return APP_REDRAW_PAGE;
        }
        if (ev->type == UI_GESTURE_TAP) {
            if (ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
                s_view = VIEW_SHELF;
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(ui_mypico_bar_prev_rect(), ev->x0, ev->y0) && s_notes_page > 0) {
                s_notes_page--;
                return APP_REDRAW_PAGE;
            }
            if (ui_rect_hit(ui_mypico_bar_next_rect(), ev->x0, ev->y0)
                && s_notes_page + 1 < s_notes_page_count) {
                s_notes_page++;
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }

    // VIEW_SHELF 交互：支持左右滑动翻页、底部按钮和卡片点击。
    // VIEW_SHELF input: swipe paging, bottom bar buttons, and card actions.
    int max_page = s_item_count > 0 ? (s_item_count - 1) / SHELF_PER_PAGE : 0;
    if (ev->type == UI_GESTURE_SWIPE_L && s_shelf_page < max_page) {
        s_shelf_page++;
        hydrate_visible_metadata();
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R && s_shelf_page > 0) {
        s_shelf_page--;
        hydrate_visible_metadata();
        return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;

    if (ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
        ctx->request_app = &app_launcher;
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(ui_mypico_bar_prev_rect(), ev->x0, ev->y0) && s_shelf_page > 0) {
        s_shelf_page--;
        hydrate_visible_metadata();
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(ui_mypico_bar_next_rect(), ev->x0, ev->y0) && s_shelf_page < max_page) {
        s_shelf_page++;
        hydrate_visible_metadata();
        return APP_REDRAW_PAGE;
    }

    int base = s_shelf_page * SHELF_PER_PAGE;
    for (int slot = 0; slot < SHELF_PER_PAGE; ++slot) {
        int idx = base + slot;
        if (idx >= s_item_count) break;
        if (ui_rect_hit(shelf_sync_btn_rect(slot), ev->x0, ev->y0)) {
            start_sync_for_item(&s_items[idx]);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(shelf_notes_btn_rect(slot), ev->x0, ev->y0)) {
            open_notes_for_item(&s_items[idx]);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(shelf_card_rect(slot), ev->x0, ev->y0)) {
            mypico_reader_set_target(s_items[idx].file_path, s_items[idx].file_size);
            ctx->request_app = &app_mypico_reader;
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t shelf_on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (s_view == VIEW_SYNC) {
        if (key == UI_KEY_1) {
            mypico_sync_stop();
            s_view = VIEW_SHELF;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == VIEW_NOTES) {
        if (key == UI_KEY_1) {
            if (s_notes_page > 0) { s_notes_page--; return APP_REDRAW_PAGE; }
            s_view = VIEW_SHELF;
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3 && s_notes_page + 1 < s_notes_page_count) {
            s_notes_page++;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    int max_page = s_item_count > 0 ? (s_item_count - 1) / SHELF_PER_PAGE : 0;
    if (key == UI_KEY_1) {
        if (s_shelf_page > 0) {
            s_shelf_page--;
            hydrate_visible_metadata();
            return APP_REDRAW_PAGE;
        }
        ctx->request_app = &app_launcher;
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3 && s_shelf_page < max_page) {
        s_shelf_page++;
        hydrate_visible_metadata();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t shelf_on_key_long(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_2) {
        mypico_sync_stop();
        ctx->request_app = &app_launcher;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t shelf_on_tick(app_ctx_t* ctx) {
    if (s_view == VIEW_SYNC) {
        if (ctx->now_ms - s_last_poll_ms >= 300) {
            s_last_poll_ms = ctx->now_ms;
            mypico_sync_poll(ctx->now_ms);
            mypico_sync_get_status(&s_sync_status);
            if (s_sync_status.state != s_last_sync_state
                || s_sync_status.pull_count != s_last_sync_pulls) {
                s_last_sync_state = s_sync_status.state;
                s_last_sync_pulls = s_sync_status.pull_count;
                if (s_sync_status.time_calibrated) ui_statusbar_poll_pmu();
                return APP_REDRAW_PAGE;
            }
        }
    }
    return APP_REDRAW_NONE;
}

const app_desc_t app_mypico_shelf = {
    .title = "书架",
    .detail = "MyPico 书架、笔记与热点同步",
    .enter_full = true,
    .owns_keys = true,
    .blocks_idle_sleep = false,
    .no_menu_handle = true,
    .on_enter = shelf_on_enter,
    .on_exit = shelf_on_exit,
    .on_media_lost = shelf_on_media_lost,
    .render = shelf_render,
    .on_gesture = shelf_on_gesture,
    .on_key = shelf_on_key,
    .on_key_long = shelf_on_key_long,
    .on_tick = shelf_on_tick,
};
