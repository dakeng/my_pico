/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：零外部依赖的轻量 RFC 6762 mDNS A 记录响应器（mypico.local）。
 * 纯封包解析/构造逻辑可在主机直接单测，ESP 平台额外启停 UDP 224.0.0.251:5353 任务。
 *
 * English: Zero-dependency RFC 6762 mDNS A-record responder for mypico.local.
 * Packet parsing/building is pure C for host tests; ESP builds run a UDP 224.0.0.251:5353 task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 检查收到的 mDNS 查询报文是否在询问 hostname（不区分大小写）的 A 或 ANY 记录。
/// Return true if a raw mDNS query packet asks for an A or ANY record of hostname (case-insensitive).
bool mypico_mdns_matches_query(const uint8_t* pkt, size_t len, const char* hostname);

/// 构造一条针对 hostname -> ipv4_be (网络字节序) 的 mDNS A 记录响应报文，返回字节数（失败返回 0）。
/// Build an mDNS A-record response packet for hostname -> ipv4_be (network byte order); 0 on error.
size_t mypico_mdns_build_response(
    const char* hostname, uint32_t ipv4_be, uint32_t ttl_sec, uint8_t* out, size_t cap
);

#ifdef ESP_PLATFORM
#include "esp_err.h"
/// 启动后台 mDNS 响应任务并立刻发送两次主动通告。
/// Start background mDNS responder task and send two immediate announcements.
esp_err_t mypico_mdns_start(const char* hostname, uint32_t ipv4_be);
/// 停止并回收 mDNS 响应任务与 socket。
/// Stop and clean up the mDNS responder task and socket.
void mypico_mdns_stop(void);
#endif

#ifdef __cplusplus
}
#endif
