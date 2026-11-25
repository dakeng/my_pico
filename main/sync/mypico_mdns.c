/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：轻量 mDNS 响应器实现（mypico.local -> 设备 IPv4）。
 *
 * English: Lightweight mDNS responder implementation (mypico.local -> device IPv4).
 */

#include "mypico_mdns.h"

#include <ctype.h>
#include <string.h>

#define DNS_TYPE_A 1u
#define DNS_TYPE_ANY 255u
#define DNS_CLASS_IN 1u
#define DNS_CLASS_FLUSH 0x8001u

static uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static bool ascii_ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// 解码 DNS 域名（支持压缩指针，带跳转次数上限防循环）。
// Decode a DNS name with compression pointer support and bounded jump count.
static bool decode_dns_name(
    const uint8_t* pkt, size_t len, size_t* io_off, char* out, size_t cap
) {
    if (!pkt || !io_off || !out || !cap) return false;
    size_t pos = *io_off;
    size_t next_after_name = 0;
    bool jumped = false;
    size_t used = 0;
    int hops = 0;

    while (pos < len) {
        uint8_t l = pkt[pos];
        if (l == 0) {
            if (!jumped) next_after_name = pos + 1;
            out[used] = 0;
            *io_off = next_after_name;
            return true;
        }
        if ((l & 0xC0u) == 0xC0u) {
            if (pos + 1 >= len || ++hops > 8) return false;
            uint16_t ptr = (uint16_t)(((uint16_t)(l & 0x3Fu) << 8) | pkt[pos + 1]);
            if (ptr >= len) return false;
            if (!jumped) {
                next_after_name = pos + 2;
                jumped = true;
            }
            pos = ptr;
            continue;
        }
        if ((l & 0xC0u) != 0 || pos + 1 + l > len) return false;
        if (used > 0) {
            if (used + 1 >= cap) return false;
            out[used++] = '.';
        }
        if (used + l >= cap) return false;
        memcpy(out + used, pkt + pos + 1, l);
        used += l;
        pos += 1 + l;
    }
    return false;
}

bool mypico_mdns_matches_query(const uint8_t* pkt, size_t len, const char* hostname) {
    if (!pkt || len < 12 || !hostname || !*hostname) return false;
    uint16_t flags = read_be16(pkt + 2);
    // QR=0 表示查询报文。/ QR=0 indicates a query packet.
    if ((flags & 0x8000u) != 0) return false;
    uint16_t qdcount = read_be16(pkt + 4);
    if (qdcount == 0 || qdcount > 32) return false;

    size_t off = 12;
    for (uint16_t i = 0; i < qdcount; ++i) {
        char qname[128];
        if (!decode_dns_name(pkt, len, &off, qname, sizeof(qname))) return false;
        if (off + 4 > len) return false;
        uint16_t qtype = read_be16(pkt + off);
        uint16_t qclass = read_be16(pkt + off + 2) & 0x7FFFu;
        off += 4;
        if (qclass == DNS_CLASS_IN && (qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY)
            && ascii_ieq(qname, hostname)) {
            return true;
        }
    }
    return false;
}

size_t mypico_mdns_build_response(
    const char* hostname, uint32_t ipv4_be, uint32_t ttl_sec, uint8_t* out, size_t cap
) {
    if (!hostname || !*hostname || !out || cap < 32) return 0;
    memset(out, 0, 12);
    // QR=1, AA=1 (0x8400), ANCOUNT=1
    write_be16(out + 2, 0x8400u);
    write_be16(out + 6, 1u);

    size_t pos = 12;
    const char* p = hostname;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63 || pos + 1 + l >= cap - 15) return 0;
        out[pos++] = (uint8_t)l;
        memcpy(out + pos, p, l);
        pos += l;
        if (!dot) break;
        p = dot + 1;
    }
    if (pos + 15 > cap) return 0;
    out[pos++] = 0;
    write_be16(out + pos, DNS_TYPE_A);
    pos += 2;
    write_be16(out + pos, DNS_CLASS_FLUSH);
    pos += 2;
    write_be32(out + pos, ttl_sec);
    pos += 4;
    write_be16(out + pos, 4u);
    pos += 2;
    memcpy(out + pos, &ipv4_be, 4);
    pos += 4;
    return pos;
}

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "mypico_mdns"
#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353

static TaskHandle_t s_task;
static volatile bool s_running;
static int s_sock = -1;
static char s_host[64];
static uint32_t s_ip_be;

static void send_announcement(int sock, const struct sockaddr_in* dst, uint16_t txid_be) {
    uint8_t resp[160];
    size_t n = mypico_mdns_build_response(s_host, s_ip_be, 120, resp, sizeof(resp));
    if (n > 0) {
        resp[0] = (uint8_t)(txid_be & 0xFFu);
        resp[1] = (uint8_t)(txid_be >> 8);
        (void)sendto(sock, resp, n, 0, (const struct sockaddr*)dst, sizeof(*dst));
    }
}

static void mdns_task(void* arg) {
    (void)arg;
    struct sockaddr_in mcast = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
    };
    mcast.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    // 启动后先发两次主动通告，并每 2 秒补发一次，确保 iPhone 个人热点下快速缓存 mypico.local。
    // Announce twice immediately and re-announce every 2s for fast iOS hotspot caching.
    send_announcement(s_sock, &mcast, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    if (s_running) send_announcement(s_sock, &mcast, 0);

    uint8_t buf[512];
    int idle_ticks = 0;
    while (s_running) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
        if (n > 0) {
            if (mypico_mdns_matches_query(buf, (size_t)n, s_host)) {
                uint16_t txid = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
                ESP_LOGI(TAG, "mDNS query matched %s (port=%u)", s_host, (unsigned)ntohs(from.sin_port));
                // 同时回复单播（兼容 QU 位 / 非 5353 源端口）和多播 224.0.0.251:5353。
                // Reply both unicast (QU / legacy query) and multicast 224.0.0.251:5353.
                send_announcement(s_sock, &from, txid);
                send_announcement(s_sock, &mcast, 0);
            }
        } else {
            // recvfrom 250ms 超时计数，每 2 秒主动再广播一次。
            if (++idle_ticks >= 8) {
                idle_ticks = 0;
                send_announcement(s_sock, &mcast, 0);
            }
        }
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mypico_mdns_start(const char* hostname, uint32_t ipv4_be) {
    mypico_mdns_stop();
    if (!hostname || !*hostname || ipv4_be == 0) return ESP_ERR_INVALID_ARG;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return ESP_FAIL;

    int reuse = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    struct in_addr iface = { .s_addr = ipv4_be };
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = inet_addr(MDNS_ADDR),
        .imr_interface.s_addr = ipv4_be,
    };
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed, continuing with unicast/announcements");
    }

    unsigned char ttl = 255;
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    size_t hlen = strnlen(hostname, sizeof(s_host) - 1);
    memcpy(s_host, hostname, hlen);
    s_host[hlen] = 0;
    s_ip_be = ipv4_be;
    s_sock = sock;
    s_running = true;

    if (xTaskCreate(mdns_task, "mypico_mdns", 4096, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mypico_mdns_stop(void) {
    s_running = false;
    for (int i = 0; i < 40 && s_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

#endif
