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
#include "mypico_sync.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ttf_font.h"
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
// 目标视觉字高（em 像素），经 ui_mypico_em_to_px 校准后在 86px 行高内饱满清晰。
// Target visual em size calibrated via ui_mypico_em_to_px for crisp legibility in 86px rows.
#define LAUNCHER_ITEM_EM 38
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
    const int item_px = ui_mypico_em_to_px(LAUNCHER_ITEM_EM);
    const int old_wght = ttf_get_weight();
    ttf_set_weight(MYPICO_WGHT_MEDIUM);
    ui_hairline(fb, LAUNCHER_LIST_TOP, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    for (int i = 0; i < LAUNCHER_TOTAL_ROWS; ++i) {
        EpdRect r = launcher_row_rect(i);
        if (i < LAUNCHER_ITEM_COUNT) {
            if (i == s_pressed_row) {
                EpdRect fill = { r.x, r.y + 4, r.width, r.height - 8 };
                ui_fill_round_rect(fb, fill, 6, 0xE0);
            }
            ui_text_vc(
                fb, r.x + 2, r.y + r.height / 2, item_px,
                s_items[i].label, EPD_DRAW_ALIGN_LEFT, false
            );
        }
        ui_hairline(fb, r.y + r.height, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    }
    ttf_set_weight(old_wght);
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
    const int title_px = ui_mypico_em_to_px(38);
    const int body_px = ui_mypico_em_to_px(28);
    const int old_wght = ttf_get_weight();
    ttf_set_weight(MYPICO_WGHT_MEDIUM);
    ui_text_vc(fb, MYPICO_MARGIN, 96, title_px, "阅读记录", EPD_DRAW_ALIGN_LEFT, false);
    ttf_set_weight(MYPICO_WGHT_REGULAR);

    char total_buf[48];
    char dur_buf[24];
    ui_statusbar_format_duration(s_rec_total_sec, dur_buf);
    snprintf(total_buf, sizeof(total_buf), "累计阅读时长：%s", dur_buf);
    ui_text_vc(fb, MYPICO_MARGIN, 152, body_px, total_buf, EPD_DRAW_ALIGN_LEFT, false);
    ui_hairline(fb, 186, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);

    if (s_rec_count <= 0) {
        ui_text_vc(
            fb, MYPICO_MARGIN, 238, body_px,
            "暂无阅读记录，在书架打开书籍阅读后会自动记录。",
            EPD_DRAW_ALIGN_LEFT, false
        );
    } else {
        int y = 200;
        for (int i = 0; i < s_rec_count && y + 72 < MYPICO_ACTIONBAR_Y; ++i) {
            ui_statusbar_format_duration(s_rec_days[i].total_sec, dur_buf);
            ui_text_vc(fb, MYPICO_MARGIN, y + 34, body_px, s_rec_days[i].date, EPD_DRAW_ALIGN_LEFT, false);
            ui_text_vc(
                fb, MYPICO_MARGIN + MYPICO_CONTENT_W, y + 34, body_px,
                dur_buf, EPD_DRAW_ALIGN_RIGHT, false
            );
            ui_hairline(fb, y + 68, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
            y += 72;
        }
    }
    ttf_set_weight(old_wght);

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

#define SETTINGS_ROWS 9

static mypico_sntp_status_t s_sntp_status;
static mypico_sntp_state_t s_last_sntp_state;
static bool s_last_wifi_conn;
static int64_t s_sntp_poll_ms;

static void draw_wrapped_hint(uint8_t* fb, int x, int y, int max_w, int px, int line_h, const char* text) {
    if (!fb || !text || !*text) return;
    const char* p = text;
    int line_idx = 0;
    char buf[160];
    while (*p && line_idx < 3) {
        size_t len = 0;
        int w = 0;
        while (*p && *p != '\n') {
            unsigned char c = (unsigned char)*p;
            size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 :
                       (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 1;
            if (len + n >= sizeof(buf)) break;
            char glyph[5] = {0};
            memcpy(glyph, p, n);
            int gw = ttf_text_width_px(px, glyph);
            if (len > 0 && w + gw > max_w) break;
            memcpy(buf + len, p, n);
            len += n;
            w += gw;
            p += n;
        }
        buf[len] = 0;
        if (len > 0) {
            ui_text_vc(fb, x, y + line_idx * line_h + line_h / 2, px, buf, EPD_DRAW_ALIGN_LEFT, false);
        }
        line_idx++;
        if (*p == '\n') ++p;
    }
}

static void settings_on_enter(app_ctx_t* ctx) {
    ui_statusbar_poll_pmu();
    memset(&s_sntp_status, 0, sizeof(s_sntp_status));
    s_last_sntp_state = MYPICO_SNTP_IDLE;
    s_last_wifi_conn = mypico_wifi_is_connected(NULL, 0);
    s_sntp_poll_ms = ctx ? ctx->now_ms : 0;
}

static void settings_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    mypico_sntp_stop();
    memset(&s_sntp_status, 0, sizeof(s_sntp_status));
    s_last_sntp_state = MYPICO_SNTP_IDLE;
}

static void settings_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    ui_statusbar_draw(fb);
    const int title_px = ui_mypico_em_to_px(38);
    const int label_px = ui_mypico_em_to_px(32);
    const int val_px = ui_mypico_em_to_px(28);
    const int hint_px = ui_mypico_em_to_px(24);
    const int old_wght = ttf_get_weight();
    ttf_set_weight(MYPICO_WGHT_MEDIUM);
    ui_text_vc(fb, MYPICO_MARGIN, 96, title_px, "设置", EPD_DRAW_ALIGN_LEFT, false);

    char tz_val[24];
    int16_t tz = app_settings_tz_offset_min();
    snprintf(tz_val, sizeof(tz_val), "UTC%+d", (int)(tz / 60));

    const char* sntp_val = "立即校时";
    switch (s_sntp_status.state) {
        case MYPICO_SNTP_CONNECTING: sntp_val = "连 Wi-Fi 中…"; break;
        case MYPICO_SNTP_SYNCING: sntp_val = "对时中…"; break;
        case MYPICO_SNTP_DONE: sntp_val = "已同步"; break;
        case MYPICO_SNTP_NO_WIFI: sntp_val = "未配 Wi-Fi"; break;
        case MYPICO_SNTP_ERROR: sntp_val = "失败，点重试"; break;
        default: break;
    }

    const bool keep_wifi = app_settings_keep_wifi();
    const bool wifi_usb_only = app_settings_keep_wifi_usb_only();

    const char* labels[SETTINGS_ROWS] = {
        "字体选择",
        "Wi-Fi / 热点配网",
        "保持 Wi-Fi 连接",
        "仅充电/USB 保持",
        "睡眠模式",
        "充电/USB 不休眠",
        "TF 卡管理",
        "本地时区",
        "网络校时 (SNTP)",
    };
    const char* values[SETTINGS_ROWS] = {
        ">",
        ">",
        keep_wifi ? "开启" : "关闭",
        (keep_wifi && wifi_usb_only) ? "开启" : "关闭",
        app_sleep_mode_name(app_settings_sleep_mode()),
        app_settings_keep_awake_usb() ? "开启" : "关闭",
        ">",
        tz_val,
        sntp_val,
    };

    int y = 148;
    ui_hairline(fb, y, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    for (int i = 0; i < SETTINGS_ROWS; ++i) {
        ttf_set_weight(MYPICO_WGHT_MEDIUM);
        ui_text_vc(fb, MYPICO_MARGIN + 2, y + LAUNCHER_ROW_H / 2, label_px, labels[i], EPD_DRAW_ALIGN_LEFT, false);
        ttf_set_weight(MYPICO_WGHT_REGULAR);
        ui_text_vc(
            fb, MYPICO_MARGIN + MYPICO_CONTENT_W - 2, y + LAUNCHER_ROW_H / 2, val_px,
            values[i], EPD_DRAW_ALIGN_RIGHT, false
        );
        y += LAUNCHER_ROW_H;
        ui_hairline(fb, y, MYPICO_MARGIN, MYPICO_CONTENT_W, UI_GRAY_LIGHT);
    }

    char wifi_hint[72] = {0};
    const char* hint = s_sntp_status.message;
    if (!hint[0]) {
        char ssid[33] = {0};
        if (mypico_wifi_is_connected(ssid, sizeof(ssid))) {
            snprintf(wifi_hint, sizeof(wifi_hint), "Wi-Fi 已保持连接：%s", ssid);
            hint = wifi_hint;
        }
    }
    if (hint[0]) {
        ttf_set_weight(MYPICO_WGHT_REGULAR);
        draw_wrapped_hint(fb, MYPICO_MARGIN + 2, y + 18, MYPICO_CONTENT_W - 4, hint_px, 36, hint);
    }
    ttf_set_weight(old_wght);

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
        else if (row == 2) {
            app_settings_set_keep_wifi(!app_settings_keep_wifi());
            mypico_wifi_policy_changed(ctx->now_ms);
            s_last_wifi_conn = mypico_wifi_is_connected(NULL, 0);
            return APP_REDRAW_PAGE;
        }
        else if (row == 3) {
            bool cur_effective = app_settings_keep_wifi() && app_settings_keep_wifi_usb_only();
            if (!cur_effective) {
                app_settings_set_keep_wifi(true);
                app_settings_set_keep_wifi_usb_only(true);
            } else {
                app_settings_set_keep_wifi_usb_only(false);
            }
            mypico_wifi_policy_changed(ctx->now_ms);
            s_last_wifi_conn = mypico_wifi_is_connected(NULL, 0);
            return APP_REDRAW_PAGE;
        }
        else if (row == 4) ctx->request_app = &app_sleep;
        else if (row == 5) {
            app_settings_set_keep_awake_usb(!app_settings_keep_awake_usb());
            return APP_REDRAW_PAGE;
        }
        else if (row == 6) ctx->request_app = &app_sd;
        else if (row == 7) {
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
        else if (row == 8) {
            if (s_sntp_status.state == MYPICO_SNTP_CONNECTING
                || s_sntp_status.state == MYPICO_SNTP_SYNCING) {
                return APP_REDRAW_NONE;
            }
            (void)mypico_sntp_start();
            mypico_sntp_get_status(&s_sntp_status);
            s_last_sntp_state = s_sntp_status.state;
            s_sntp_poll_ms = ctx->now_ms;
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t settings_on_tick(app_ctx_t* ctx) {
    if (ctx->now_ms - s_sntp_poll_ms < 300) return APP_REDRAW_NONE;
    s_sntp_poll_ms = ctx->now_ms;
    bool redraw = false;

    if (s_sntp_status.state == MYPICO_SNTP_CONNECTING
        || s_sntp_status.state == MYPICO_SNTP_SYNCING) {
        mypico_sntp_poll(ctx->now_ms);
        mypico_sntp_get_status(&s_sntp_status);
        if (s_sntp_status.state != s_last_sntp_state) {
            s_last_sntp_state = s_sntp_status.state;
            if (s_sntp_status.state == MYPICO_SNTP_DONE) {
                ui_statusbar_poll_pmu();
            }
            redraw = true;
        }
    }

    bool wifi_conn = mypico_wifi_is_connected(NULL, 0);
    if (wifi_conn != s_last_wifi_conn) {
        s_last_wifi_conn = wifi_conn;
        ui_statusbar_poll_pmu();
        redraw = true;
    }
    return redraw ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
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
    .on_exit = settings_on_exit,
    .render = settings_render,
    .on_gesture = settings_on_gesture,
    .on_key = settings_on_key,
    .on_key_long = launcher_on_key_long,
    .on_tick = settings_on_tick,
};
