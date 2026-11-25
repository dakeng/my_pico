/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 热点同步服务接口：STA 连接已保存的 iPhone 热点，启动 mDNS (mypico.local)
 * 与 HTTP API 供 iPhone 快捷指令一键拉取单本书的进度、时长与划线 Markdown，并回写 PMU RTC。
 *
 * English: MyPico hotspot sync service interface: connects in STA mode to the saved iPhone
 * hotspot, starts mDNS (mypico.local) and an HTTP API for one-tap iOS Shortcut export of a
 * single book's progress, duration and highlights Markdown, and calibrates the PMU RTC.
 *
 * 冻结：仅在同步界面启动，离开即停止并释放 Wi-Fi / HTTP / mDNS 资源；不泄露 Wi-Fi 密码。
 * Frozen: Runs only while the sync overlay/page is active; stop releases Wi-Fi/HTTP/mDNS; never expose passwords.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "book_data.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MYPICO_SYNC_HOSTNAME "mypico.local"

typedef enum {
    MYPICO_SYNC_IDLE = 0,        ///< 未启动 / Idle
    MYPICO_SYNC_NO_WIFI,         ///< 尚未配置 Wi-Fi 热点 / No saved Wi-Fi credentials
    MYPICO_SYNC_CONNECTING,      ///< 正在连接手机热点 / Connecting to hotspot
    MYPICO_SYNC_READY,           ///< 已连上热点，等待快捷指令拉取 / Ready for Shortcut pull
    MYPICO_SYNC_DONE,            ///< 快捷指令已完成拉取 / Pull completed
    MYPICO_SYNC_ERROR,           ///< 连接超时或启动失败 / Connection timeout or start error
} mypico_sync_state_t;

typedef struct {
    mypico_sync_state_t state;   ///< 当前同步状态 / Current sync state
    char ssid[33];               ///< 目标热点名称 / Target hotspot SSID
    char ip[20];                 ///< 获取到的局域网 IP / Acquired IPv4 address
    char book_title[160];        ///< 当前同步书名 / Book title being synced
    char note_filename[160];     ///< 输出 Markdown 文件名 / Exported Markdown filename
    uint32_t pull_count;         ///< 已被快捷指令拉取次数 / Number of pulls served
    bool time_calibrated;        ///< 是否在本次同步中校准了 RTC / Whether RTC was calibrated
    char message[128];           ///< 展示给用户的状态提示 / User-facing status message
} mypico_sync_status_t;

/// 为单本书启动热点同步服务（内部拷贝书籍快照并生成初始导出内容）。
/// Start single-book hotspot sync (copies the book snapshot internally).
esp_err_t mypico_sync_start(const book_record_t* rec, int16_t tz_offset_min);

/// 停止同步服务并释放所有网络与任务资源；可重复调用。
/// Stop sync service and release all network/task resources; safe to call repeatedly.
void mypico_sync_stop(void);

/// 驱动超时检查（由 UI on_tick 定期调用）。
/// Drive connection timeout checks (called periodically from UI on_tick).
void mypico_sync_poll(int64_t now_ms);

/// 复制线程安全的状态快照供 render 纯绘制使用。
/// Copy a thread-safe status snapshot for pure rendering.
void mypico_sync_get_status(mypico_sync_status_t* out);

/// 构建同步 HTTP JSON 响应体（纯逻辑，供主机单测复用；调用方 free）。
/// Build the sync HTTP JSON payload (pure helper for host tests; caller frees output).
esp_err_t mypico_sync_build_json_payload(
    const book_record_t* rec, int16_t tz_offset_min, char** out_json, size_t* out_len
);

#ifdef __cplusplus
}
#endif
