/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 里的用户设置：睡眠档、字体路径、上次唤醒/开机原因、拿起唤醒开关。
 *
 * User settings in NVS: sleep mode, font path, last wake/boot reason,
 * pickup-wake switch.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 默认关机。浅睡：按键回原页；拿起唤醒默认关。软睡：SOFT_SLEEP 拉低 EN，再短按开机。关机：EN=0，长按开机。
/// Default is off. Light: key returns to the page; pickup-wake defaults off. Soft sleep: SOFT_SLEEP drops EN, then a short press boots. Off: EN=0, long-press to boot.
typedef enum {
    APP_SLEEP_LIGHT = 0,
    APP_SLEEP_DEEP = 1,
    APP_SLEEP_OFF = 2,
} app_sleep_mode_t;

void app_settings_init(void);
app_sleep_mode_t app_settings_sleep_mode(void);
void app_settings_set_sleep_mode(app_sleep_mode_t mode);
const char* app_sleep_mode_name(app_sleep_mode_t mode);
/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* app_settings_font_path(void);
void app_settings_set_font_path(const char* path);
/// 上次浅睡唤醒源（app_wake_source_t），掉电也保留。/ Last light-sleep wake source (app_wake_source_t); kept across power loss.
uint8_t app_settings_last_wake(void);
void app_settings_set_last_wake(uint8_t src);
/// 最近一次非 0 的 PMU wake_reason。STATUS 报 0 时用这个回显。/ Last non-zero PMU wake_reason. Used when STATUS reports 0.
uint8_t app_settings_last_boot(void);
void app_settings_set_last_boot(uint8_t reason);
/// 浅睡拿起唤醒。默认关；有加速度计也不会自动开。/ Light-sleep pickup wake. Defaults off; an accelerometer does not turn it on.
bool app_settings_pickup_wake(void);
void app_settings_set_pickup_wake(bool on);
/// 阅读默认字号，32..72、步长 4，默认 48。/ Default reading size, 32..72 in steps of 4, initially 48.
uint8_t app_settings_book_px(void);
/// 无效字号恢复 48。/ Invalid sizes fall back to 48.
void app_settings_set_book_px(uint8_t px);
/// 实验性晃动翻页，默认关闭。/ Experimental shake page turn, off by default.
bool app_settings_book_shake(void);
/// 保存实验性晃动翻页开关。/ Persist the experimental shake page-turn switch.
void app_settings_set_book_shake(bool on);
/// 本地时区偏移（分钟），默认 +480（UTC+8）。/ Local timezone offset in minutes, defaulting to +480 (UTC+8).
int16_t app_settings_tz_offset_min(void);
/// 保存本地时区偏移（-720..+840）。/ Persist local timezone offset (-720..+840).
void app_settings_set_tz_offset_min(int16_t minutes);
/// 充电或连接 USB 数据线时阻止空闲自动休眠，默认开启。
/// Prevent idle auto-sleep while charging or connected to a USB data host; defaults on.
bool app_settings_keep_awake_usb(void);
/// 保存充电/USB 不休眠开关。/ Persist the keep-awake-on-charging/USB switch.
void app_settings_set_keep_awake_usb(bool on);
/// 保持 Wi-Fi 连接总开关（配合 WIFI_PS_MAX_MODEM 省电），默认开启。
/// Master switch to keep STA Wi-Fi connected (using WIFI_PS_MAX_MODEM); defaults on.
bool app_settings_keep_wifi(void);
/// 保存保持 Wi-Fi 连接开关。/ Persist the keep-Wi-Fi-connected switch.
void app_settings_set_keep_wifi(bool on);
/// 仅在充电或连接 USB 数据线时保持 Wi-Fi，拔线后自动断开省电，默认开启。
/// Keep Wi-Fi only while charging or USB-connected; disconnects on battery. Defaults on.
bool app_settings_keep_wifi_usb_only(void);
/// 保存仅充电/USB 保持 Wi-Fi 开关。/ Persist the keep-Wi-Fi-only-on-charging/USB switch.
void app_settings_set_keep_wifi_usb_only(bool on);

/// 阅读行距增量百分比（相对字号，如 50 表示 1.5x 行高），默认 50。
/// Reading line-spacing extra percentage relative to font size (50 = 1.5x line height); default 50.
uint8_t app_settings_reader_line_pct(void);
void app_settings_set_reader_line_pct(uint8_t pct);
/// 阅读左右页边距（像素，16..80），默认 40。/ Reading horizontal page margin in pixels (16..80); default 40.
uint8_t app_settings_reader_margin(void);
void app_settings_set_reader_margin(uint8_t margin_px);
/// 阅读段间距百分比（相对字号，0..120），默认 35。/ Paragraph spacing percentage of font size (0..120); default 35.
uint8_t app_settings_reader_para_pct(void);
void app_settings_set_reader_para_pct(uint8_t pct);
/// 阅读正文首行缩进（2 个全角字宽），默认开启。/ First-line 2-em indent for body paragraphs; default on.
bool app_settings_reader_indent(void);
void app_settings_set_reader_indent(bool on);
/// 阅读对齐方式：0=两端对齐，1=左对齐，默认 0。/ Reading alignment: 0=justify, 1=left; default 0.
uint8_t app_settings_reader_align(void);
void app_settings_set_reader_align(uint8_t align);
/// 阅读正文插图模式：0=混排显示，1=占位点击，2=隐藏，默认 0。/ Inline image mode: 0=inline, 1=placeholder, 2=hide; default 0.
uint8_t app_settings_reader_img_mode(void);
void app_settings_set_reader_img_mode(uint8_t mode);
/// 启用 EPUB 内嵌样式（标题层级/引用/居中/加粗/分割线），默认开启。/ Enable EPUB embedded styles; default on.
bool app_settings_reader_epub_style(void);
void app_settings_set_reader_epub_style(bool on);
