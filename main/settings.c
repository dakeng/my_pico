/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 读写。打开失败就用关机默认值，不擦除整个分区。
 *
 * NVS load/store. A failed open keeps the power-off default; the
 * partition is not erased.
 */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "settings"
#define NVS_NS "read_pico"
#define NVS_KEY_SLEEP "sleep"
#define NVS_KEY_FONT "font"
#define NVS_KEY_WAKE "lwake"
#define NVS_KEY_BOOT "lboot"
#define NVS_KEY_PICKUP "pickup"
#define NVS_KEY_BOOK_PX "bk_px"
#define NVS_KEY_BOOK_SHAKE "bk_shake"
#define NVS_KEY_TZ_MIN "tz_min"
#define NVS_KEY_AWAKE_USB "awake_usb"
#define NVS_KEY_KEEP_WIFI "keep_wifi"
#define NVS_KEY_WIFI_USB "wifi_usb"
#define NVS_KEY_RD_LINE "rd_line"
#define NVS_KEY_RD_MARGIN "rd_margin"
#define NVS_KEY_RD_PARA "rd_para"
#define NVS_KEY_RD_INDENT "rd_indent"
#define NVS_KEY_RD_ALIGN "rd_align"
#define NVS_KEY_RD_IMG "rd_img"
#define NVS_KEY_RD_STYLE "rd_style"
#define FONT_PATH_MAX 160

static app_sleep_mode_t s_sleep = APP_SLEEP_OFF;
static char s_font[FONT_PATH_MAX];
static uint8_t s_last_wake;
static uint8_t s_last_boot;
static bool s_pickup_wake;
static uint8_t s_book_px = 48;
static bool s_book_shake;
static int16_t s_tz_offset_min = 480;
static bool s_keep_awake_usb = true;
static bool s_keep_wifi = true;
static bool s_keep_wifi_usb_only = true;
static uint8_t s_rd_line = 50;
static uint8_t s_rd_margin = 40;
static uint8_t s_rd_para = 35;
static bool s_rd_indent = true;
static uint8_t s_rd_align = 0;
static uint8_t s_rd_img = 0;
static bool s_rd_style = true;

static uint8_t valid_book_px(uint8_t px) {
    return px >= 32 && px <= 72 && (px - 32) % 4 == 0 ? px : 48;
}

void app_settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init %s, use power off", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t raw = APP_SLEEP_OFF;
    if (nvs_get_u8(h, NVS_KEY_SLEEP, &raw) == ESP_OK && raw <= APP_SLEEP_OFF) {
        s_sleep = (app_sleep_mode_t)raw;
    }
    size_t font_len = sizeof(s_font);
    if (nvs_get_str(h, NVS_KEY_FONT, s_font, &font_len) != ESP_OK) {
        s_font[0] = '\0';
    }
    uint8_t wake = 0;
    if (nvs_get_u8(h, NVS_KEY_WAKE, &wake) == ESP_OK) s_last_wake = wake;
    uint8_t boot = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOT, &boot) == ESP_OK) s_last_boot = boot;
    uint8_t pickup = 0;
    if (nvs_get_u8(h, NVS_KEY_PICKUP, &pickup) == ESP_OK) s_pickup_wake = pickup != 0;
    uint8_t book_px = 48, book_shake = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_PX, &book_px) == ESP_OK) s_book_px = valid_book_px(book_px);
    if (nvs_get_u8(h, NVS_KEY_BOOK_SHAKE, &book_shake) == ESP_OK) s_book_shake = book_shake != 0;
    int16_t tz_min = 480;
    if (nvs_get_i16(h, NVS_KEY_TZ_MIN, &tz_min) == ESP_OK && tz_min >= -720 && tz_min <= 840) {
        s_tz_offset_min = tz_min;
    }
    uint8_t awake_usb = 1;
    if (nvs_get_u8(h, NVS_KEY_AWAKE_USB, &awake_usb) == ESP_OK) {
        s_keep_awake_usb = awake_usb != 0;
    }
    uint8_t keep_wifi = 1;
    if (nvs_get_u8(h, NVS_KEY_KEEP_WIFI, &keep_wifi) == ESP_OK) {
        s_keep_wifi = keep_wifi != 0;
    }
    uint8_t wifi_usb = 1;
    if (nvs_get_u8(h, NVS_KEY_WIFI_USB, &wifi_usb) == ESP_OK) {
        s_keep_wifi_usb_only = wifi_usb != 0;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_RD_LINE, &v) == ESP_OK && v >= 20 && v <= 120) s_rd_line = v;
    if (nvs_get_u8(h, NVS_KEY_RD_MARGIN, &v) == ESP_OK && v >= 16 && v <= 80) s_rd_margin = v;
    if (nvs_get_u8(h, NVS_KEY_RD_PARA, &v) == ESP_OK && v <= 120) s_rd_para = v;
    if (nvs_get_u8(h, NVS_KEY_RD_INDENT, &v) == ESP_OK) s_rd_indent = v != 0;
    if (nvs_get_u8(h, NVS_KEY_RD_ALIGN, &v) == ESP_OK && v <= 1) s_rd_align = v;
    if (nvs_get_u8(h, NVS_KEY_RD_IMG, &v) == ESP_OK && v <= 2) s_rd_img = v;
    if (nvs_get_u8(h, NVS_KEY_RD_STYLE, &v) == ESP_OK) s_rd_style = v != 0;
    nvs_close(h);
    ESP_LOGI(
        TAG, "sleep mode %s, font %s",
        app_sleep_mode_name(s_sleep),
        s_font[0] != '\0' ? s_font : "(builtin)"
    );
}

app_sleep_mode_t app_settings_sleep_mode(void) {
    return s_sleep;
}

const char* app_sleep_mode_name(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "light";
        case APP_SLEEP_DEEP: return "deep";
        case APP_SLEEP_OFF: return "off";
        default: return "?";
    }
}

void app_settings_set_sleep_mode(app_sleep_mode_t mode) {
    if (mode > APP_SLEEP_OFF) mode = APP_SLEEP_OFF;
    s_sleep = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SLEEP, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

const char* app_settings_font_path(void) {
    return s_font;
}

static void nvs_put_u8(const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_last_wake(void) {
    return s_last_wake;
}

void app_settings_set_last_wake(uint8_t src) {
    if (src == s_last_wake) return;
    s_last_wake = src;
    nvs_put_u8(NVS_KEY_WAKE, src);
}

uint8_t app_settings_last_boot(void) {
    return s_last_boot;
}

void app_settings_set_last_boot(uint8_t reason) {
    if (reason == 0 || reason == s_last_boot) return;
    s_last_boot = reason;
    nvs_put_u8(NVS_KEY_BOOT, reason);
}

bool app_settings_pickup_wake(void) {
    return s_pickup_wake;
}

void app_settings_set_pickup_wake(bool on) {
    if (s_pickup_wake == on) return;
    s_pickup_wake = on;
    nvs_put_u8(NVS_KEY_PICKUP, on ? 1 : 0);
}

void app_settings_set_font_path(const char* path) {
    if (path == NULL) path = "";
    strlcpy(s_font, path, sizeof(s_font));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_FONT, s_font);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_px(void) {
    return s_book_px;
}

void app_settings_set_book_px(uint8_t px) {
    px = valid_book_px(px);
    if (s_book_px == px) return;
    s_book_px = px;
    nvs_put_u8(NVS_KEY_BOOK_PX, px);
}

bool app_settings_book_shake(void) {
    return s_book_shake;
}

void app_settings_set_book_shake(bool on) {
    if (s_book_shake == on) return;
    s_book_shake = on;
    nvs_put_u8(NVS_KEY_BOOK_SHAKE, on ? 1 : 0);
}

int16_t app_settings_tz_offset_min(void) {
    return s_tz_offset_min;
}

void app_settings_set_tz_offset_min(int16_t minutes) {
    if (minutes < -720 || minutes > 840) minutes = 480;
    if (s_tz_offset_min == minutes) return;
    s_tz_offset_min = minutes;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i16(h, NVS_KEY_TZ_MIN, minutes);
    nvs_commit(h);
    nvs_close(h);
}

bool app_settings_keep_awake_usb(void) {
    return s_keep_awake_usb;
}

void app_settings_set_keep_awake_usb(bool on) {
    if (s_keep_awake_usb == on) return;
    s_keep_awake_usb = on;
    nvs_put_u8(NVS_KEY_AWAKE_USB, on ? 1 : 0);
}

bool app_settings_keep_wifi(void) {
    return s_keep_wifi;
}

void app_settings_set_keep_wifi(bool on) {
    if (s_keep_wifi == on) return;
    s_keep_wifi = on;
    nvs_put_u8(NVS_KEY_KEEP_WIFI, on ? 1 : 0);
}

bool app_settings_keep_wifi_usb_only(void) {
    return s_keep_wifi_usb_only;
}

void app_settings_set_keep_wifi_usb_only(bool on) {
    if (s_keep_wifi_usb_only == on) return;
    s_keep_wifi_usb_only = on;
    nvs_put_u8(NVS_KEY_WIFI_USB, on ? 1 : 0);
}

uint8_t app_settings_reader_line_pct(void) {
    return s_rd_line;
}

void app_settings_set_reader_line_pct(uint8_t pct) {
    if (pct < 20 || pct > 120) pct = 50;
    if (s_rd_line == pct) return;
    s_rd_line = pct;
    nvs_put_u8(NVS_KEY_RD_LINE, pct);
}

uint8_t app_settings_reader_margin(void) {
    return s_rd_margin;
}

void app_settings_set_reader_margin(uint8_t margin_px) {
    if (margin_px < 16 || margin_px > 80) margin_px = 40;
    if (s_rd_margin == margin_px) return;
    s_rd_margin = margin_px;
    nvs_put_u8(NVS_KEY_RD_MARGIN, margin_px);
}

uint8_t app_settings_reader_para_pct(void) {
    return s_rd_para;
}

void app_settings_set_reader_para_pct(uint8_t pct) {
    if (pct > 120) pct = 35;
    if (s_rd_para == pct) return;
    s_rd_para = pct;
    nvs_put_u8(NVS_KEY_RD_PARA, pct);
}

bool app_settings_reader_indent(void) {
    return s_rd_indent;
}

void app_settings_set_reader_indent(bool on) {
    if (s_rd_indent == on) return;
    s_rd_indent = on;
    nvs_put_u8(NVS_KEY_RD_INDENT, on ? 1 : 0);
}

uint8_t app_settings_reader_align(void) {
    return s_rd_align;
}

void app_settings_set_reader_align(uint8_t align) {
    if (align > 1) align = 0;
    if (s_rd_align == align) return;
    s_rd_align = align;
    nvs_put_u8(NVS_KEY_RD_ALIGN, align);
}

uint8_t app_settings_reader_img_mode(void) {
    return s_rd_img;
}

void app_settings_set_reader_img_mode(uint8_t mode) {
    if (mode > 2) mode = 0;
    if (s_rd_img == mode) return;
    s_rd_img = mode;
    nvs_put_u8(NVS_KEY_RD_IMG, mode);
}

bool app_settings_reader_epub_style(void) {
    return s_rd_style;
}

void app_settings_set_reader_epub_style(bool on) {
    if (s_rd_style == on) return;
    s_rd_style = on;
    nvs_put_u8(NVS_KEY_RD_STYLE, on ? 1 : 0);
}
