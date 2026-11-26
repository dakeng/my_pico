/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 通用顶部状态栏（时间与电量）、底部操作栏及时间/时长格式化原语。
 *
 * English: Shared MyPico top status bar (time & battery), bottom action bar,
 * and time/duration formatting helpers.
 *
 * 冻结：ui_statusbar_draw() 为纯绘制，禁止 I2C 读写；PMU 轮询在 on_enter/on_tick 中进行。
 * Frozen: ui_statusbar_draw() is paint-only with no I2C access; PMU polling runs in on_enter/on_tick.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epdiy.h"
#include "ui_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

// 依据 Figma 585x1040 -> 684x1216 (×76/65) 等比映射的 MyPico 排版常量。
// MyPico layout constants scaled proportionally from Figma 585x1040 to 684x1216 (x76/65).
#define MYPICO_MARGIN 38
#define MYPICO_CONTENT_W (UI_LOCK_WIDTH - 2 * MYPICO_MARGIN)
#define MYPICO_STATUSBAR_H 56
#define MYPICO_ACTIONBAR_H 110
#define MYPICO_ACTIONBAR_Y (UI_LOCK_HEIGHT - MYPICO_ACTIONBAR_H)

// 匹配 Figma IBM Plex Sans Regular / Medium 在可变字体下的字重。
// Variable-font weights matching Figma Regular and Medium hierarchy on e-ink.
#define MYPICO_WGHT_REGULAR 400
#define MYPICO_WGHT_MEDIUM 500

// 2020-01-01 00:00:00 UTC 作为 RTC 已校准的最小阈值。
// 2020-01-01 00:00:00 UTC as the minimum valid calibrated RTC timestamp.
#define MYPICO_TIME_VALID_MIN 1577836800u

/// 刷新 PMU 电量与 RTC 时间快照（可触发 I2C，不得在 render 内调用）。
/// Refresh PMU battery and RTC time snapshots (uses I2C; never call inside render).
void ui_statusbar_poll_pmu(void);

/// 向 PMU 写入 UTC Unix 秒以校准硬件 RTC。
/// Calibrate the PMU hardware RTC with UTC Unix seconds.
bool ui_statusbar_set_unix_time(uint32_t unix_sec);

/// 读取当前缓存的 PMU UTC Unix 秒；有效返回 true。
/// Read the cached PMU UTC Unix seconds; returns true when calibrated.
bool ui_statusbar_time_valid(uint32_t* out_unix);

/// 格式化本地时间 HH:MM（无效时间输出 "--:--"）。
/// Format local time as HH:MM (outputs "--:--" when invalid).
void ui_statusbar_format_hm(uint32_t unix_sec, int16_t tz_offset_min, char out[8]);

/// 格式化本地日期 YYYY-MM-DD（无效时间输出 "--"）。
/// Format local date as YYYY-MM-DD (outputs "--" when invalid).
void ui_statusbar_format_date(uint32_t unix_sec, int16_t tz_offset_min, char out[16]);

/// 格式化累计阅读时长（如 "1h20min"、"45min"、"0min"）。
/// Format cumulative reading duration (e.g. "1h20min", "45min", "0min").
void ui_statusbar_format_duration(uint32_t total_sec, char out[24]);

/// 纯绘制：在屏幕顶部画出状态栏（左侧时间、右侧电量）。
/// Pure paint: draw the top status bar (time on left, battery percentage on right).
void ui_statusbar_draw(uint8_t* fb);

/// MyPico 底部操作栏三个标准按钮（首页 / 上一页 / 下一页）的矩形。
/// Standard rectangles for the three MyPico bottom action bar buttons (Home / Prev / Next).
EpdRect ui_mypico_bar_home_rect(void);
EpdRect ui_mypico_bar_prev_rect(void);
EpdRect ui_mypico_bar_next_rect(void);

/// 将目标视觉字高（em 像素，对齐 Figma CSS font-size × 76/65）换算为当前字体的 ttf_draw_text_px 参数。
/// Convert target visual em size (Figma CSS font-size x 76/65) to pixel_height for the active TTF font.
int ui_mypico_em_to_px(int target_em_px);

/// 纯绘制：底部灰色圆角胶囊按钮（匹配 Figma rgba(0,0,0,0.05) 风格）。
/// Pure paint: light-gray pill button matching Figma rgba(0,0,0,0.05) style.
void ui_mypico_draw_pill(uint8_t* fb, EpdRect rect, int px, const char* label, bool enabled);

/// 纯绘制：书架/笔记页共用的底部操作栏（首页 / 上一页 / 下一页）。
/// Pure paint: shared bottom action bar (Home / Prev / Next) for shelf and notes pages.
void ui_mypico_draw_action_bar(
    uint8_t* fb, const char* left_label, bool can_prev, bool can_next
);

#ifdef __cplusplus
}
#endif
