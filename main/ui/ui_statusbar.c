/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 状态栏、底部操作栏与纯算法时间/时长格式化实现。
 *
 * English: MyPico status bar, bottom action bar, and pure time/duration formatters.
 *
 * 冻结：绘制函数不访问 I2C 或网络；日期换算采用纯整数 civil-from-days 算法，不依赖 TZ 环境变量。
 * Frozen: Draw functions never touch I2C or networking; date conversion uses integer civil-from-days without TZ env dependencies.
 */

#include "ui_statusbar.h"

#include <stdio.h>
#include <string.h>

#include "read_pico_pmu.h"
#include "settings.h"
#include "ttf_font.h"

// 浅灰底色（匹配黑色 10% 透明度在 4bpp 灰度下的近似阶：0xD0；禁用态保持更浅的 0xE0）。
// Light gray fill approximating 10% black opacity in 4bpp (0xD0); disabled uses lighter 0xE0.
#define MYPICO_PILL_BG 0xD0
#define MYPICO_PILL_BG_DISABLED 0xE0
#define MYPICO_PILL_RADIUS 5
#define MYPICO_STATUS_EM 18
#define MYPICO_BAR_EM 28
// 底部操作栏按钮：四边 padding 16 (×76/65 ≈ 19px)，高 74px，居中于 110px 操作栏内。
// Bottom action bar pills: 16px all-around padding (x76/65 ~= 19px), 74px tall, centered in 110px bar.
#define MYPICO_BAR_BTN_H 74
#define MYPICO_BAR_BTN_Y (MYPICO_ACTIONBAR_Y + (MYPICO_ACTIONBAR_H - MYPICO_BAR_BTN_H) / 2)
#define MYPICO_BAR_HOME_W 94
#define MYPICO_BAR_PAGE_W 122
#define MYPICO_BAR_PAGE_GAP 28

// Howard Hinnant civil_from_days：从 1970-01-01 起的天数换算公历年月日。
// Howard Hinnant's civil_from_days: convert days since 1970-01-01 to calendar Y/M/D.
static void civil_from_days(int64_t z, int* year, int* month, int* day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    *year = (int)y;
    *month = (int)m;
    *day = (int)d;
}

void ui_statusbar_poll_pmu(void) {
    if (!read_pico_pmu_ready()) return;
    read_pico_pmu_poll();
    read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
}

bool ui_statusbar_set_unix_time(uint32_t unix_sec) {
    if (!read_pico_pmu_ready() || unix_sec < MYPICO_TIME_VALID_MIN) return false;
    uint8_t buf[4] = {
        (uint8_t)(unix_sec & 0xFFu),
        (uint8_t)((unix_sec >> 8) & 0xFFu),
        (uint8_t)((unix_sec >> 16) & 0xFFu),
        (uint8_t)((unix_sec >> 24) & 0xFFu),
    };
    if (read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, buf, sizeof(buf)) != ESP_OK) return false;
    read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    return true;
}

bool ui_statusbar_time_valid(uint32_t* out_unix) {
    if (out_unix) *out_unix = 0;
    if (!read_pico_pmu_ready()) return false;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_ok || pmu->unix_sec < MYPICO_TIME_VALID_MIN) return false;
    if (out_unix) *out_unix = pmu->unix_sec;
    return true;
}

void ui_statusbar_format_hm(uint32_t unix_sec, int16_t tz_offset_min, char out[8]) {
    if (!out) return;
    if (unix_sec < MYPICO_TIME_VALID_MIN) {
        strcpy(out, "--:--");
        return;
    }
    int64_t local = (int64_t)unix_sec + (int64_t)tz_offset_min * 60;
    int64_t sec_of_day = local % 86400;
    if (sec_of_day < 0) sec_of_day += 86400;
    int hh = (int)(sec_of_day / 3600);
    int mm = (int)((sec_of_day % 3600) / 60);
    snprintf(out, 8, "%02d:%02d", hh, mm);
}

void ui_statusbar_format_date(uint32_t unix_sec, int16_t tz_offset_min, char out[16]) {
    if (!out) return;
    if (unix_sec < MYPICO_TIME_VALID_MIN) {
        strcpy(out, "--");
        return;
    }
    int64_t local = (int64_t)unix_sec + (int64_t)tz_offset_min * 60;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int y = 1970, m = 1, d = 1;
    civil_from_days(days, &y, &m, &d);
    if (y < 0 || y > 9999) y = 1970;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    snprintf(out, 16, "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
}

void ui_statusbar_format_duration(uint32_t total_sec, char out[24]) {
    if (!out) return;
    uint32_t total_min = total_sec / 60;
    uint32_t hours = total_min / 60;
    uint32_t mins = total_min % 60;
    if (hours > 0) {
        snprintf(out, 24, "%uh%umin", (unsigned)hours, (unsigned)mins);
    } else {
        snprintf(out, 24, "%umin", (unsigned)mins);
    }
}

int ui_mypico_em_to_px(int target_em_px) {
    // 用汉字宽反推当前字体的 (ascent - descent) / unitsPerEm 比例，消除不同字体的视觉大小偏差。
    // Calibrate pixel_height against a CJK em glyph so visual sizing matches Figma across any loaded font.
    int ref_w = ttf_text_width_px(100, "页");
    if (ref_w <= 20 || ref_w > 120) return target_em_px;
    int px = (target_em_px * 100 + ref_w / 2) / ref_w;
    if (px < 12) px = 12;
    if (px > 120) px = 120;
    return px;
}

void ui_statusbar_draw(uint8_t* fb) {
    if (!fb) return;
    EpdRect bar = {0, 0, UI_LOCK_WIDTH, MYPICO_STATUSBAR_H};
    ui_clear_rect_fast(fb, bar);

    char time_str[8] = "--:--";
    uint32_t now_unix = 0;
    if (ui_statusbar_time_valid(&now_unix)) {
        ui_statusbar_format_hm(now_unix, app_settings_tz_offset_min(), time_str);
    }

    char bat_str[16] = "--%";
    if (read_pico_pmu_ready()) {
        const pmu_snapshot_t* pmu = read_pico_pmu_get();
        if (pmu && pmu->status_ok && pmu->soc_permille <= 1000) {
            int pct = (int)(pmu->soc_permille / 10);
            if (pmu->charge_state == PMU_CHARGE_CHARGING) {
                snprintf(bat_str, sizeof(bat_str), "%d%%+", pct);
            } else {
                snprintf(bat_str, sizeof(bat_str), "%d%%", pct);
            }
        }
    }

    const int cy = MYPICO_STATUSBAR_H / 2;
    const int px = ui_mypico_em_to_px(MYPICO_STATUS_EM);
    const int old_wght = ttf_get_weight();
    ttf_set_weight(MYPICO_WGHT_MEDIUM);
    ui_text_vc(fb, MYPICO_MARGIN, cy, px, time_str, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(
        fb, UI_LOCK_WIDTH - MYPICO_MARGIN, cy, px, bat_str,
        EPD_DRAW_ALIGN_RIGHT, false
    );
    ttf_set_weight(old_wght);
}

EpdRect ui_mypico_bar_home_rect(void) {
    return (EpdRect){
        .x = MYPICO_MARGIN,
        .y = MYPICO_BAR_BTN_Y,
        .width = MYPICO_BAR_HOME_W,
        .height = MYPICO_BAR_BTN_H,
    };
}

EpdRect ui_mypico_bar_prev_rect(void) {
    return (EpdRect){
        .x = UI_LOCK_WIDTH - MYPICO_MARGIN - MYPICO_BAR_PAGE_W - MYPICO_BAR_PAGE_GAP - MYPICO_BAR_PAGE_W,
        .y = MYPICO_BAR_BTN_Y,
        .width = MYPICO_BAR_PAGE_W,
        .height = MYPICO_BAR_BTN_H,
    };
}

EpdRect ui_mypico_bar_next_rect(void) {
    return (EpdRect){
        .x = UI_LOCK_WIDTH - MYPICO_MARGIN - MYPICO_BAR_PAGE_W,
        .y = MYPICO_BAR_BTN_Y,
        .width = MYPICO_BAR_PAGE_W,
        .height = MYPICO_BAR_BTN_H,
    };
}

void ui_mypico_draw_pill(uint8_t* fb, EpdRect rect, int px, const char* label, bool enabled) {
    if (!fb || !label) return;
    uint8_t bg8 = enabled ? MYPICO_PILL_BG : MYPICO_PILL_BG_DISABLED;
    ui_fill_round_rect(fb, rect, MYPICO_PILL_RADIUS, bg8);
    uint8_t fg = enabled ? UI_INK_BLACK : 9;
    const int old_wght = ttf_get_weight();
    ttf_set_weight(MYPICO_WGHT_MEDIUM);
    int above = 0, below = 0;
    ttf_measure_line_px(px, label, &above, &below);
    int baseline = rect.y + rect.height / 2 + (above - below) / 2;
    ttf_draw_text_px(
        fb, rect.x + rect.width / 2, baseline, px, label,
        EPD_DRAW_ALIGN_CENTER, fg, (uint8_t)(bg8 >> 4)
    );
    ttf_set_weight(old_wght);
}

void ui_mypico_draw_action_bar(
    uint8_t* fb, const char* left_label, bool can_prev, bool can_next
) {
    if (!fb) return;
    EpdRect bar = {0, MYPICO_ACTIONBAR_Y, UI_LOCK_WIDTH, MYPICO_ACTIONBAR_H};
    ui_clear_rect_fast(fb, bar);
    const int bar_px = ui_mypico_em_to_px(MYPICO_BAR_EM);
    ui_mypico_draw_pill(fb, ui_mypico_bar_home_rect(), bar_px, left_label ? left_label : "首页", true);
    ui_mypico_draw_pill(fb, ui_mypico_bar_prev_rect(), bar_px, "上一页", can_prev);
    ui_mypico_draw_pill(fb, ui_mypico_bar_next_rect(), bar_px, "下一页", can_next);
}
