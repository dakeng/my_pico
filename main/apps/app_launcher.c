/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 首页（应用列表页）与轻量化设置/阅读记录入口。
 * 布局严格对应 Figma 应用列表页（书架、阅读记录、传书 Transfer、设置、Demo）。
 *
 * English: MyPico home screen (app list) plus lightweight settings and reading-records views.
 * Layout matches the Figma app list page (Shelf, Reading Records, Transfer, Settings, Demo).
 *
 * 冻结：render 为纯绘制；Demo 入口直接复用官方一级菜单以保留全部官方诊断页。
 * Frozen: render is paint-only; Demo opens the official root menu to keep all stock diagnostic pages intact.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "book_data.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_statusbar.h"

extern const app_desc_t app_mypico_shelf;
extern const app_desc_t app_transfer;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_sleep;
extern const app_desc_t app_sd;
extern const app_desc_t app_mypico_records;
extern const app_desc_t app_mypico_settings;

#define LAUNCHER_LIST_TOP 76
#define LAUNCHER_ROW_H 86
#define LAUNCHER_TOTAL_ROWS 12
#define LAUNCHER_ITEM_PX 34
#define LAUNCHER_PMU_POLL_MS 30000

typedef struct {
    const char* label;
    const app_desc_t* target;
    bool open_demo_menu;
} launcher_item_t;

static const launcher_item_t s_items[] = {
    { "书架", &app_mypico_shelf, false },
    { "阅读记录", &app_mypico_records, false },
    { "传书 Transfer", &app_transfer, false },
    { "设置", &app_mypico_settings, false },
    { "Demo", NULL, true },
};

#define LAUNCHER_ITEM_COUNT ((int)(sizeof(s_items) / sizeof(s_items[0])))

static int s_pressed_row = -1;
static int64_t s_last_pmu_ms;
static uint32_t s_last_shown_min;

static EpdRect launcher_row_rect(int row) {
    return (EpdRect){
        .x = MYPICO_MARGIN,
        .y = LAUNCHER_LIST_TOP + row * LAUNCHER_ROW_H,
        .width = MYPICO_CONTENT_W,
        .height = LAUNCHER_ROW_H,
    };
}

static int launcher_hit_row(uint16_t x, uint16_t y) {
    if ((int)x < MYPICO_MARGIN || (int)x >= MYPICO_MARGIN + MYPICO_CONTENT_W) return -1;
    if ((int)y < LAUNCHER_LIST_TOP) return -1;
    int row = ((int)y - LAUNCHER_LIST_TOP) / LAUNCHER_ROW_H;
    if (row < 0 || row >= LAUNCHER_ITEM_COUNT) return -1;
    return row;
}

static void launcher_on_enter(app_ctx_t* ctx) {
    s_pressed_row = -1;
    ui_statusbar_poll_pmu();
    s_last_pmu_ms = ctx ? ctx->now_ms : 0;
    uint32_t now_u = 0;
    s_last_shown_min = ui_statusbar_time_valid(&now_u) ? now_u / 60 : 0;
}

static void launcher_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_statusbar_draw(fb);

    // 画出与 Figma 一致的等距细横线与列表文字。
    // Draw evenly spaced hairlines and item labels matching the Figma app list.
    ui_hairline(fb, LAUNCHER_LIST_TOP, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    for (int i = 0; i < LAUNCHER_TOTAL_ROWS; ++i) {
        EpdRect r = launcher_row_rect(i);
        if (i < LAUNCHER_ITEM_COUNT) {
            if (i == s_pressed_row) {
                EpdRect fill = { r.x, r.y + 4, r.width, r.height - 8 };
                ui_fill_round_rect(fb, fill, 6, 0xE0);
            }
            ui_text_vc(
                fb, r.x + 2, r.y + r.height / 2, LAUNCHER_ITEM_PX,
                s_items[i].label, EPD_DRAW_ALIGN_LEFT, false
            );
        }
        ui_hairline(fb, r.y + r.height, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    }
}

static app_redraw_t launcher_on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_PRESS) {
        int hit = launcher_hit_row(ev->x0, ev->y0);
        if (hit >= 0) {
            s_pressed_row = hit;
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_CANCEL) {
        s_pressed_row = -1;
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP) {
        int hit = launcher_hit_row(ev->x0, ev->y0);
        s_pressed_row = -1;
        if (hit < 0) return APP_REDRAW_NONE;
        if (s_items[hit].open_demo_menu) {
            ctx->request_menu = true;
            return APP_REDRAW_NONE;
        }
        if (s_items[hit].target) {
            ctx->request_app = s_items[hit].target;
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t launcher_on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;
}

static app_redraw_t launcher_on_key_long(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_2) {
        ctx->request_menu = true;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t launcher_on_tick(app_ctx_t* ctx) {
    if (ctx->now_ms - s_last_pmu_ms < LAUNCHER_PMU_POLL_MS) return APP_REDRAW_NONE;
    s_last_pmu_ms = ctx->now_ms;
    ui_statusbar_poll_pmu();
    uint32_t now_u = 0;
    uint32_t cur_min = ui_statusbar_time_valid(&now_u) ? now_u / 60 : 0;
    if (cur_min != s_last_shown_min) {
        s_last_shown_min = cur_min;
        ui_statusbar_draw(ctx->fb);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect launcher_area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){ 0, 0, UI_LOCK_WIDTH, MYPICO_STATUSBAR_H };
}

const app_desc_t app_launcher = {
    .title = "首页 MyPico",
    .detail = "书架、阅读记录、传书、设置与 Demo",
    .enter_full = true,
    .owns_keys = true,
    .no_menu_handle = true,
    .on_enter = launcher_on_enter,
    .render = launcher_render,
    .on_gesture = launcher_on_gesture,
    .on_key = launcher_on_key,
    .on_key_long = launcher_on_key_long,
    .on_tick = launcher_on_tick,
    .area_hint = launcher_area_hint,
};

/* ---- 轻量化阅读记录页 / Lightweight reading records page ---- */

static book_session_summary_t s_rec_days[10];
static int s_rec_count;
static uint32_t s_rec_total_sec;

static void records_on_enter(app_ctx_t* ctx) {
    (void)ctx;
    ui_statusbar_poll_pmu();
    s_rec_total_sec = 0;
    s_rec_count = book_data_load_recent_days(
        s_rec_days, 10, app_settings_tz_offset_min(), &s_rec_total_sec
    );
}

static void records_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_statusbar_draw(fb);
    ui_text(fb, MYPICO_MARGIN, 74, 38, "阅读记录", EPD_DRAW_ALIGN_LEFT, false);

    char total_buf[48];
    char dur_buf[24];
    ui_statusbar_format_duration(s_rec_total_sec, dur_buf);
    snprintf(total_buf, sizeof(total_buf), "累计阅读时长：%s", dur_buf);
    ui_text(fb, MYPICO_MARGIN, 134, 28, total_buf, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 180, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);

    if (s_rec_count <= 0) {
        ui_text(
            fb, MYPICO_MARGIN, 220, 28,
            "暂无阅读记录，在书架打开书籍阅读后会自动记录。",
            EPD_DRAW_ALIGN_LEFT, false
        );
    } else {
        int y = 200;
        for (int i = 0; i < s_rec_count && y + 72 < MYPICO_ACTIONBAR_Y; ++i) {
            ui_statusbar_format_duration(s_rec_days[i].total_sec, dur_buf);
            ui_text(fb, MYPICO_MARGIN, y + 14, 28, s_rec_days[i].date, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(
                fb, MYPICO_MARGIN + MYPICO_CONTENT_W, y + 14, 28,
                dur_buf, EPD_DRAW_ALIGN_RIGHT, false
            );
            ui_hairline(fb, y + 68, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
            y += 72;
        }
    }

    ui_mypico_draw_action_bar(fb, "首页", false, false);
}

static app_redraw_t records_on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type == UI_GESTURE_TAP && ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
        ctx->request_app = &app_launcher;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t records_on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_1) {
        ctx->request_app = &app_launcher;
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;
}

const app_desc_t app_mypico_records = {
    .title = "阅读记录",
    .detail = "每日与累计阅读时长统计",
    .enter_full = true,
    .owns_keys = true,
    .no_menu_handle = true,
    .on_enter = records_on_enter,
    .render = records_render,
    .on_gesture = records_on_gesture,
    .on_key = records_on_key,
    .on_key_long = launcher_on_key_long,
};

/* ---- 轻量化设置页 / Lightweight settings page ---- */

#define SETTINGS_ROWS 5

static void settings_on_enter(app_ctx_t* ctx) {
    (void)ctx;
    ui_statusbar_poll_pmu();
}

static void settings_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_statusbar_draw(fb);
    ui_text(fb, MYPICO_MARGIN, 74, 38, "设置", EPD_DRAW_ALIGN_LEFT, false);

    char tz_val[24];
    int16_t tz = app_settings_tz_offset_min();
    snprintf(tz_val, sizeof(tz_val), "UTC%+d", (int)(tz / 60));

    const char* labels[SETTINGS_ROWS] = {
        "字体选择",
        "Wi-Fi / 热点配网",
        "睡眠模式",
        "TF 卡管理",
        "本地时区",
    };
    const char* values[SETTINGS_ROWS] = {
        ">",
        ">",
        app_sleep_mode_name(app_settings_sleep_mode()),
        ">",
        tz_val,
    };

    int y = 148;
    ui_hairline(fb, y, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    for (int i = 0; i < SETTINGS_ROWS; ++i) {
        ui_text_vc(fb, MYPICO_MARGIN + 2, y + LAUNCHER_ROW_H / 2, 32, labels[i], EPD_DRAW_ALIGN_LEFT, false);
        ui_text_vc(
            fb, MYPICO_MARGIN + MYPICO_CONTENT_W - 2, y + LAUNCHER_ROW_H / 2, 28,
            values[i], EPD_DRAW_ALIGN_RIGHT, false
        );
        y += LAUNCHER_ROW_H;
        ui_hairline(fb, y, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    }

    ui_mypico_draw_action_bar(fb, "首页", false, false);
}

static app_redraw_t settings_on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (ui_rect_hit(ui_mypico_bar_home_rect(), ev->x0, ev->y0)) {
        ctx->request_app = &app_launcher;
        return APP_REDRAW_NONE;
    }
    if ((int)ev->x0 >= MYPICO_MARGIN && (int)ev->x0 < MYPICO_MARGIN + MYPICO_CONTENT_W
        && (int)ev->y0 >= 148) {
        int row = ((int)ev->y0 - 148) / LAUNCHER_ROW_H;
        if (row == 0) ctx->request_app = &app_font_pick;
        else if (row == 1) ctx->request_app = &app_transfer;
        else if (row == 2) ctx->request_app = &app_sleep;
        else if (row == 3) ctx->request_app = &app_sd;
        else if (row == 4) {
            // 常用时区循环：UTC+8 -> UTC+9 -> UTC+0 -> UTC-5 -> UTC-8 -> UTC+8
            static const int16_t k_tz[] = { 480, 540, 0, -300, -480 };
            int16_t cur = app_settings_tz_offset_min();
            int next_idx = 0;
            for (int i = 0; i < 5; ++i) {
                if (k_tz[i] == cur) { next_idx = (i + 1) % 5; break; }
            }
            app_settings_set_tz_offset_min(k_tz[next_idx]);
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t settings_on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_1) {
        ctx->request_app = &app_launcher;
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;
}

const app_desc_t app_mypico_settings = {
    .title = "设置",
    .detail = "字体、Wi-Fi、睡眠与存储入口",
    .enter_full = true,
    .owns_keys = true,
    .no_menu_handle = true,
    .on_enter = settings_on_enter,
    .render = settings_render,
    .on_gesture = settings_on_gesture,
    .on_key = settings_on_key,
    .on_key_long = launcher_on_key_long,
};
