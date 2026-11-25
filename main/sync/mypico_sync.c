/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 热点同步服务实现：
 * - 读取 NVS (rp_wifi/credentials) 中已保存的手机热点凭据，STA 模式连入 iPhone 热点
 * - 广播 mypico.local (mDNS) 并启动 HTTP Server 提供 /api/sync、/api/sync/markdown、/api/sync/ack
 * - 支持通过查询参数 ?ts=<unix_sec> 或 POST /api/sync/ack 顺带校准 PMU 硬件 RTC
 *
 * English: MyPico hotspot sync service implementation:
 * - Connects STA to the saved iPhone hotspot from NVS (rp_wifi/credentials)
 * - Advertises mypico.local (mDNS) and serves /api/sync, /api/sync/markdown, and /api/sync/ack
 * - Calibrates the PMU hardware RTC when ?ts=<unix_sec> or POST /api/sync/ack provides time
 */

#include "mypico_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- 纯 JSON 载荷构造（主机与设备共用）/ Pure JSON payload builder (host & device) ---- */

static void format_dur_local(uint32_t total_sec, char out[24]) {
    uint32_t total_min = total_sec / 60;
    uint32_t hours = total_min / 60;
    uint32_t mins = total_min % 60;
    if (hours > 0) snprintf(out, 24, "%uh%umin", (unsigned)hours, (unsigned)mins);
    else snprintf(out, 24, "%umin", (unsigned)mins);
}

static void civil_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned mon = mp < 10 ? mp + 3 : mp - 9;
    yr += (mon <= 2);
    *y = (int)yr;
    *m = (int)mon;
    *d = (int)day;
}

static void format_date_local(uint32_t unix_sec, int16_t tz_min, char out[16]) {
    if (unix_sec < 1577836800u) {
        strcpy(out, "");
        return;
    }
    int64_t local = (int64_t)unix_sec + (int64_t)tz_min * 60;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int y = 1970, m = 1, d = 1;
    civil_days(days, &y, &m, &d);
    if (y < 0 || y > 9999) y = 1970;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    snprintf(out, 16, "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
}

esp_err_t mypico_sync_build_json_payload(
    const book_record_t* rec, int16_t tz_offset_min, char** out_json, size_t* out_len
) {
    if (!rec || !out_json) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;
    if (out_len) *out_len = 0;

    char* md = NULL;
    size_t md_len = 0;
    esp_err_t err = book_data_build_obsidian_md(rec, tz_offset_min, &md, &md_len);
    if (err != ESP_OK) return err;

    char filename[160];
    book_data_note_filename(rec, filename, sizeof(filename));
    char dur[24];
    format_dur_local(rec->total_read_sec, dur);
    char last_read[16];
    format_date_local(rec->last_read_ts, tz_offset_min, last_read);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        free(md);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "book_id", rec->book_id);
    cJSON_AddStringToObject(root, "title", rec->title);
    cJSON_AddStringToObject(root, "author", rec->author);
    cJSON_AddStringToObject(root, "filename", filename);
    cJSON_AddNumberToObject(root, "progress", rec->pct);
    cJSON_AddNumberToObject(root, "duration_sec", rec->total_read_sec);
    cJSON_AddStringToObject(root, "duration", dur);
    cJSON_AddStringToObject(root, "last_read", last_read);
    cJSON_AddNumberToObject(root, "highlight_count", (double)rec->highlight_count);
    cJSON_AddStringToObject(root, "markdown", md);
    free(md);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    size_t n = strlen(json);
    char* copy = malloc(n + 1);
    if (!copy) {
        cJSON_free(json);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, json, n + 1);
    cJSON_free(json);

    *out_json = copy;
    if (out_len) *out_len = n;
    return ESP_OK;
}

#ifdef ESP_PLATFORM

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mypico_mdns.h"
#include "nvs.h"
#include "ui_statusbar.h"

#define TAG "mypico_sync"
#define SYNC_CONNECT_TIMEOUT_MS 20000
#define SYNC_MAX_RETRIES 4

// 与 read_pico_transfer 的 NVS 凭据布局保持完全一致，复用同一套热点配网。
// Exact same NVS credential layout as read_pico_transfer to share saved Wi-Fi provisioning.
typedef struct {
    uint8_t version;
    char ssid[33];
    char password[65];
} saved_wifi_cred_t;

static SemaphoreHandle_t s_lock;
static mypico_sync_status_t s_status;
static book_record_t s_book;
static int16_t s_tz_min = 480;
static int64_t s_started_ms;
static int s_retries;
static bool s_active;
static bool s_wifi_started;
static bool s_wifi_inited;
static esp_netif_t* s_netif;
static httpd_handle_t s_http;
static esp_event_handler_instance_t s_wifi_ev;
static esp_event_handler_instance_t s_ip_ev;

static void ensure_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static bool load_saved_wifi(saved_wifi_cred_t* out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open("rp_wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, "credentials", out, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*out) || out->version != 1 || !out->ssid[0]) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static void maybe_calibrate_time_locked(uint32_t ts_unix) {
    if (ts_unix < MYPICO_TIME_VALID_MIN) return;
    if (ui_statusbar_set_unix_time(ts_unix)) {
        s_status.time_calibrated = true;
    }
    // 若书籍此前在未校时状态下阅读，顺手补齐 created_ts / last_read_ts 并保存。
    // Backfill missing created_ts / last_read_ts if the book was read before RTC calibration.
    bool updated = false;
    if (s_book.created_ts < MYPICO_TIME_VALID_MIN) {
        s_book.created_ts = ts_unix;
        updated = true;
    }
    if (s_book.last_read_ts < MYPICO_TIME_VALID_MIN) {
        s_book.last_read_ts = ts_unix;
        updated = true;
    }
    if (updated) (void)book_data_save(&s_book);
}

// Howard Hinnant days_from_civil：从公历年月日反算自 1970-01-01 起的天数。
// Howard Hinnant's days_from_civil: convert calendar Y/M/D to days since 1970-01-01.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t parse_iso_to_utc(const char* s, int16_t default_tz_min) {
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &hh, &mm, &ss) < 3) {
        if (!s || sscanf(s, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &hh, &mm, &ss) < 3) return 0;
    }
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return 0;
    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    int64_t local_sec = days * 86400 + hh * 3600 + mm * 60 + ss;
    int64_t utc_sec = local_sec - (int64_t)default_tz_min * 60;
    return utc_sec >= MYPICO_TIME_VALID_MIN ? (uint32_t)utc_sec : 0;
}

static uint32_t parse_ts_from_query(httpd_req_t* req, int16_t tz_min) {
    char query[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return 0;
    char val[64] = {0};
    if (httpd_query_key_value(query, "ts", val, sizeof(val)) == ESP_OK) {
        unsigned long ts = strtoul(val, NULL, 10);
        if (ts >= MYPICO_TIME_VALID_MIN) return (uint32_t)ts;
    }
    if (httpd_query_key_value(query, "iso", val, sizeof(val)) == ESP_OK) {
        // 简单解码 %3A -> ':' 与 %20/'+' -> ' '，兼容快捷指令自动 URL 编码。
        // Decode %3A -> ':' and %20/'+' -> ' ' for iOS Shortcuts URL encoding.
        char decoded[64] = {0};
        size_t o = 0;
        for (size_t i = 0; val[i] && o + 1 < sizeof(decoded); ++i) {
            if (val[i] == '%' && val[i + 1] && val[i + 2]) {
                unsigned byte = 0;
                if (sscanf(val + i + 1, "%2x", &byte) == 1) {
                    decoded[o++] = (char)byte;
                    i += 2;
                    continue;
                }
            }
            decoded[o++] = val[i] == '+' ? ' ' : val[i];
        }
        decoded[o] = 0;
        return parse_iso_to_utc(decoded, tz_min);
    }
    return 0;
}

static esp_err_t handle_get_sync(httpd_req_t* req) {
    ESP_LOGI(TAG, "sync request: method=%d uri=%s", req->method, req->uri);
    char* json = NULL;
    size_t json_len = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ts = parse_ts_from_query(req, s_tz_min);
    if (ts) maybe_calibrate_time_locked(ts);
    esp_err_t err = mypico_sync_build_json_payload(&s_book, s_tz_min, &json, &json_len);
    if (err == ESP_OK) {
        s_status.pull_count++;
        s_status.state = MYPICO_SYNC_DONE;
        snprintf(s_status.message, sizeof(s_status.message), "已发送笔记数据给 iPhone 快捷指令。");
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK || !json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "export failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, json, (ssize_t)json_len);
    free(json);
    return res;
}

static esp_err_t handle_get_markdown(httpd_req_t* req) {
    ESP_LOGI(TAG, "markdown request: uri=%s", req->uri);
    char* md = NULL;
    size_t md_len = 0;
    char filename[160] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ts = parse_ts_from_query(req, s_tz_min);
    if (ts) maybe_calibrate_time_locked(ts);
    esp_err_t err = book_data_build_obsidian_md(&s_book, s_tz_min, &md, &md_len);
    book_data_note_filename(&s_book, filename, sizeof(filename));
    if (err == ESP_OK) {
        s_status.pull_count++;
        s_status.state = MYPICO_SYNC_DONE;
        snprintf(s_status.message, sizeof(s_status.message), "已发送 Markdown 笔记。");
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK || !md) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "export failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    httpd_resp_set_hdr(req, "X-MyPico-Filename", filename);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, md, (ssize_t)md_len);
    free(md);
    return res;
}

static esp_err_t handle_post_ack(httpd_req_t* req) {
    char body[128] = {0};
    int len = req->content_len;
    if (len > 0 && len < (int)sizeof(body)) {
        int got = httpd_req_recv(req, body, (size_t)len);
        if (got > 0) {
            body[got] = 0;
            cJSON* obj = cJSON_Parse(body);
            if (obj) {
                const cJSON* ts = cJSON_GetObjectItemCaseSensitive(obj, "ts");
                if (cJSON_IsNumber(ts) && ts->valuedouble >= MYPICO_TIME_VALID_MIN) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    maybe_calibrate_time_locked((uint32_t)ts->valuedouble);
                    s_status.state = MYPICO_SYNC_DONE;
                    xSemaphoreGive(s_lock);
                }
                cJSON_Delete(obj);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t start_http_server(void) {
    if (s_http) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_http, &cfg);
    if (err != ESP_OK) return err;

    // 精确子路由优先注册，其余 GET 请求均兜底返回同步 JSON（兼容根路径、末尾斜杠或编码空格）。
    // Register specific sub-routes first, then wildcard-fallback GET to sync JSON (handles /, trailing /, etc.).
    static const httpd_uri_t routes[] = {
        { .uri = "/api/sync/markdown*", .method = HTTP_GET, .handler = handle_get_markdown },
        { .uri = "/api/sync/ack*", .method = HTTP_POST, .handler = handle_post_ack },
        { .uri = "/*", .method = HTTP_GET, .handler = handle_get_sync },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        (void)httpd_register_uri_handler(s_http, &routes[i]);
    }
    return ESP_OK;
}

static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)base;
    (void)data;
    if (!s_active) return;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_status.state == MYPICO_SYNC_CONNECTING && s_retries < SYNC_MAX_RETRIES) {
            s_retries++;
            xSemaphoreGive(s_lock);
            esp_wifi_connect();
        } else {
            if (s_status.state != MYPICO_SYNC_DONE) {
                s_status.state = MYPICO_SYNC_ERROR;
                snprintf(
                    s_status.message, sizeof(s_status.message),
                    "无法连上热点，请确认手机热点已开且启用「最大兼容性」。"
                );
            }
            xSemaphoreGive(s_lock);
        }
    }
}

static void on_ip_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)base;
    if (!s_active || id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
    if (ev->esp_netif != s_netif) return;

    char ip_str[20];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "Connected to hotspot, IP=%s", ip_str);

    esp_err_t http_err = start_http_server();
    if (http_err == ESP_OK) {
        (void)mypico_mdns_start(MYPICO_SYNC_HOSTNAME, ev->ip_info.ip.addr);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_status.ip, ip_str);
    if (http_err == ESP_OK) {
        s_status.state = MYPICO_SYNC_READY;
        snprintf(s_status.message, sizeof(s_status.message), "热点已连接，现在点击 iPhone 快捷指令即可拉取。");
    } else {
        s_status.state = MYPICO_SYNC_ERROR;
        snprintf(s_status.message, sizeof(s_status.message), "同步服务启动失败。");
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t clone_book_record(const book_record_t* src, book_record_t* dst) {
    book_data_record_free(dst);
    *dst = *src;
    dst->highlights = NULL;
    if (src->highlight_count > 0 && src->highlights) {
        size_t bytes = src->highlight_count * sizeof(*dst->highlights);
        dst->highlights = malloc(bytes);
        if (!dst->highlights) {
            dst->highlight_count = 0;
            return ESP_ERR_NO_MEM;
        }
        memcpy(dst->highlights, src->highlights, bytes);
    }
    return ESP_OK;
}

esp_err_t mypico_sync_start(const book_record_t* rec, int16_t tz_offset_min) {
    if (!rec) return ESP_ERR_INVALID_ARG;
    ensure_lock();
    mypico_sync_stop();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.book_title, sizeof(s_status.book_title), "%s", rec->title);
    book_data_note_filename(rec, s_status.note_filename, sizeof(s_status.note_filename));
    s_tz_min = tz_offset_min;
    esp_err_t clone_err = clone_book_record(rec, &s_book);
    xSemaphoreGive(s_lock);
    if (clone_err != ESP_OK) return clone_err;

    saved_wifi_cred_t cred;
    if (!load_saved_wifi(&cred)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.state = MYPICO_SYNC_NO_WIFI;
        snprintf(
            s_status.message, sizeof(s_status.message),
            "尚未保存热点 Wi-Fi。请先在「传书 Transfer」或「设置」中选择你的 iPhone 热点并输入密码。"
        );
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", cred.ssid);
    s_status.state = MYPICO_SYNC_CONNECTING;
    snprintf(s_status.message, sizeof(s_status.message), "正在连接 %s…", cred.ssid);
    s_started_ms = esp_timer_get_time() / 1000;
    s_retries = 0;
    s_active = true;
    xSemaphoreGive(s_lock);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    (void)esp_netif_set_hostname(s_netif, "mypico");

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) goto fail;
    s_wifi_inited = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &s_wifi_ev);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, &s_ip_ev);
    if (err != ESP_OK) goto fail;

    wifi_config_t sta = {0};
    size_t ssid_len = strlen(cred.ssid);
    if (ssid_len > sizeof(sta.sta.ssid)) ssid_len = sizeof(sta.sta.ssid);
    size_t pass_len = strlen(cred.password);
    if (pass_len > sizeof(sta.sta.password)) pass_len = sizeof(sta.sta.password);
    memcpy(sta.sta.ssid, cred.ssid, ssid_len);
    memcpy(sta.sta.password, cred.password, pass_len);
    sta.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    memset(&cred, 0, sizeof(cred));

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    memset(&sta, 0, sizeof(sta));
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;
    return ESP_OK;

fail:
    memset(&cred, 0, sizeof(cred));
    mypico_sync_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = MYPICO_SYNC_ERROR;
    snprintf(s_status.message, sizeof(s_status.message), "Wi-Fi 初始化失败 (%s)", esp_err_to_name(err));
    xSemaphoreGive(s_lock);
    return err;
}

void mypico_sync_stop(void) {
    ensure_lock();
    s_active = false;
    mypico_mdns_stop();

    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (s_wifi_ev) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_ev);
        s_wifi_ev = NULL;
    }
    if (s_ip_ev) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_ev);
        s_ip_ev = NULL;
    }
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_inited) {
        (void)esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    book_data_record_free(&s_book);
    xSemaphoreGive(s_lock);
}

void mypico_sync_poll(int64_t now_ms) {
    if (!s_active || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.state == MYPICO_SYNC_CONNECTING
        && now_ms - s_started_ms >= SYNC_CONNECT_TIMEOUT_MS) {
        s_status.state = MYPICO_SYNC_ERROR;
        snprintf(
            s_status.message, sizeof(s_status.message),
            "连接超时，请检查 iPhone 个人热点是否已打开并开启「最大兼容性」。"
        );
    }
    xSemaphoreGive(s_lock);
}

void mypico_sync_get_status(mypico_sync_status_t* out) {
    if (!out) return;
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

#endif
