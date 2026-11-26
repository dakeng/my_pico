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

/* ---- SNTP 网络校时 / SNTP network time calibration ---- */

typedef enum {
    MYPICO_SNTP_IDLE = 0,        ///< 未启动 / Idle
    MYPICO_SNTP_NO_WIFI,         ///< 尚未配置 Wi-Fi / No saved Wi-Fi credentials
    MYPICO_SNTP_CONNECTING,      ///< 正在连接已保存 Wi-Fi / Connecting to saved Wi-Fi
    MYPICO_SNTP_SYNCING,         ///< 已联网，等待 NTP 响应 / Querying NTP servers
    MYPICO_SNTP_DONE,            ///< 校时成功并已关闭 Wi-Fi / Synced and Wi-Fi stopped
    MYPICO_SNTP_ERROR,           ///< 连接或对时失败 / Connect or NTP timeout/error
} mypico_sntp_state_t;

typedef struct {
    mypico_sntp_state_t state;   ///< 当前校时状态 / Current SNTP state
    char ssid[33];               ///< 目标 Wi-Fi 名称 / Target Wi-Fi SSID
    uint32_t synced_unix;        ///< 校时得到的 UTC Unix 秒 / Calibrated UTC Unix seconds
    char message[128];           ///< 展示给用户的状态提示 / User-facing status text
} mypico_sntp_status_t;

/// 启动一次 SNTP 网络校时（复用已保存 Wi-Fi；完成后自动写入 PMU RTC 并关闭 Wi-Fi）。
/// Start one-shot SNTP time sync using saved Wi-Fi (writes PMU RTC and stops Wi-Fi on completion).
esp_err_t mypico_sntp_start(void);

/// 停止 SNTP 并释放 Wi-Fi 资源；离开设置页时调用。
/// Stop SNTP and release Wi-Fi resources; call when leaving Settings.
void mypico_sntp_stop(void);

/// 在主循环任务中驱动 SNTP 写 RTC、收尾关网与超时检查。
/// Drive RTC write-back, Wi-Fi teardown and timeouts from the UI task.
void mypico_sntp_poll(int64_t now_ms);

/// 复制线程安全的 SNTP 状态快照供 render 纯绘制使用。
/// Copy a thread-safe SNTP status snapshot for pure rendering.
void mypico_sntp_get_status(mypico_sntp_status_t* out);

/* ---- 全局 Wi-Fi 常驻管理 / Persistent STA Wi-Fi keep-alive ---- */

/// 主循环周期调用：按「保持 Wi-Fi 连接」「仅充电/USB 保持」策略自动连/断 Wi-Fi，
/// 首次联网静默 SNTP 校时，断网失败自动退避防空转耗电。
/// Periodic event-loop tick: connects/disconnects STA Wi-Fi per user settings,
/// runs silent initial SNTP calibration, and backs off on unreachable APs.
void mypico_wifi_tick(int64_t now_ms);

/// 让出或收回 Wi-Fi 所有权（进入/离开「传书 Transfer」时调用）。
/// Yield or reclaim Wi-Fi ownership around Transfer page entry/exit.
void mypico_wifi_yield(bool yield);

/// 锁屏/休眠前挂起并关闭 Wi-Fi，浅睡唤醒后恢复策略。
/// Suspend Wi-Fi before lock/sleep and resume policy after light-sleep wake.
void mypico_wifi_suspend(bool suspend);

/// 设置变更后立即生效（清除退避并立刻按新策略调整 Wi-Fi 状态）。
/// Apply updated Wi-Fi keep-alive settings immediately.
void mypico_wifi_policy_changed(int64_t now_ms);

/// 查询常驻 Wi-Fi 当前是否已连上；已连接时返回 true 并写出 SSID（可选）。
/// Check whether STA Wi-Fi is currently connected; optionally writes SSID.
bool mypico_wifi_is_connected(char* out_ssid, size_t cap);

#ifdef __cplusplus
}
#endif
