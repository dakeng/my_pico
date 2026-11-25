/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 核心算法主机单测：
 * - book_layout 字符坐标反查、多行下划线矩形枚举与绘制
 * - book_epub 元数据（dc:title / dc:creator）与封面缩略图解码裁切
 * - mypico_mdns RFC 6762 查询匹配与 A 记录响应报文构造
 *
 * English: Host unit tests for MyPico core modules:
 * - book_layout character hit-testing, multi-line underline enumeration and rendering
 * - book_epub metadata (dc:title / dc:creator) and cover thumbnail decoding/cropping
 * - mypico_mdns RFC 6762 query matching and A-record response building
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book_epub.h"
#include "book_layout.h"
#include "mypico_mdns.h"

/* ---- 主机测试用绘图/字宽桩 / Host test draw & font metric stubs ---- */

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    (void)x;
    (void)y;
    (void)color;
    (void)fb;
}

int ttf_ascender_px(int px) {
    return (px * 4) / 5;
}

int ttf_text_width_px(int px, const char* text) {
    if (!text) return 0;
    int width = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p;) {
        if (*p < 0x80) { width += px / 2; ++p; }
        else {
            size_t n = (*p >= 0xC2 && *p <= 0xDF) ? 2 : (*p >= 0xE0 && *p <= 0xEF) ? 3 : 4;
            width += px;
            p += n;
        }
    }
    return width;
}

void ttf_draw_text_px(
    uint8_t* fb, int x, int y, int px, const char* text,
    enum EpdFontFlags align, uint8_t fg, uint8_t bg
) {
    (void)fb; (void)x; (void)y; (void)px; (void)text; (void)align; (void)fg; (void)bg;
}

static void test_layout_highlights(void) {
    // 简单中文段落（每字符在 stub 下按字号等宽测量）。
    // Simple Chinese paragraph (fixed glyph width in host layout stub).
    const char* text = "女人的本性，阻止了我用笔靠近想要表达的痛苦。真的需要一个奇迹。\n后来时间过去了。";
    EpdRect rect = { .x = 38, .y = 64, .width = 240, .height = 600 };
    assert(book_layout_build(text, strlen(text), rect, 40));
    assert(book_layout_page_count() == 1);

    // 命中第 1 行第 1 个字（"女"，3 字节 UTF-8）。
    // Hit first glyph on line 1 ("女", 3-byte UTF-8).
    size_t s0 = 0, e0 = 0;
    EpdRect gr0 = {0};
    assert(book_layout_char_at(0, rect, 45, 75, &s0, &e0, &gr0));
    assert(s0 == 0 && e0 == 3);
    assert(gr0.x == 38 && gr0.y == 64);

    // 命中第 2 行字符，跨行计算下划线。
    // Hit a character on line 2 and compute multi-line underlines.
    size_t s1 = 0, e1 = 0;
    assert(book_layout_char_at(0, rect, 120, 140, &s1, &e1, NULL));
    assert(s1 > e0 && e1 > s1);

    EpdRect bars[16];
    size_t n = book_layout_span_underlines(0, rect, s0, e1, bars, 16);
    assert(n >= 2);
    assert(bars[0].height == 3 && bars[0].width > 0);
    assert(bars[1].height == 3 && bars[1].width > 0);
    assert(bars[1].y > bars[0].y);

    // 绘制带下划线的页面（确保无越界/ASan 洁净）。
    // Render page with highlights (ASan/UBSan cleanliness check).
    uint8_t fb[16] = {0};
    book_layout_span_t span = { .start_off = s0, .end_off = e1 };
    book_layout_draw_page_highlights(fb, 0, rect, 40, &span, 1);

    book_layout_free();
    printf("test_layout_highlights: PASS\n");
}

static void test_mdns(void) {
    // 构造一条询问 "mypico.local" A 记录的 mDNS 查询报文。
    // Build an mDNS query packet asking for "mypico.local" type A.
    uint8_t query[] = {
        0x00, 0x00,             // ID
        0x00, 0x00,             // Flags (QR=0 query)
        0x00, 0x01,             // QDCOUNT=1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        6, 'M', 'y', 'P', 'i', 'c', 'o',
        5, 'l', 'o', 'c', 'a', 'l',
        0,
        0x00, 0x01,             // QTYPE=A
        0x00, 0x01,             // QCLASS=IN
    };
    assert(mypico_mdns_matches_query(query, sizeof(query), "mypico.local"));
    assert(!mypico_mdns_matches_query(query, sizeof(query), "other.local"));

    // 构造响应报文：172.20.10.2
    uint8_t ip_bytes[4] = { 172, 20, 10, 2 };
    uint32_t ipv4_be = 0;
    memcpy(&ipv4_be, ip_bytes, 4);

    uint8_t resp[128];
    size_t rlen = mypico_mdns_build_response("mypico.local", ipv4_be, 120, resp, sizeof(resp));
    assert(rlen > 28);
    // QR=1, AA=1 -> 0x8400, ANCOUNT=1
    assert(resp[2] == 0x84 && resp[3] == 0x00);
    assert(resp[6] == 0x00 && resp[7] == 0x01);
    // 末尾 4 字节为 IPv4 地址。
    assert(memcmp(resp + rlen - 4, ip_bytes, 4) == 0);
    // 响应报文本身不应被识别成查询。
    assert(!mypico_mdns_matches_query(resp, rlen, "mypico.local"));

    printf("test_mdns: PASS\n");
}

static void test_epub_meta_and_cover(const char* epub_path) {
    book_epub_meta_t meta;
    assert(book_epub_read_meta(epub_path, &meta) == ESP_OK);
    assert(strcmp(meta.title, "页边和听写") == 0);
    assert(strcmp(meta.author, "埃琳娜·费兰特") == 0);
    assert(meta.has_cover);
    assert(strstr(meta.cover_path, "cover.png") != NULL);

    uint8_t* thumb = NULL;
    assert(book_epub_load_cover_thumb(epub_path, 152, 210, &thumb) == ESP_OK);
    assert(thumb != NULL);
    // 验证缩略图像素为有效灰度（非全空且内存范围完整可读）。
    size_t sum = 0;
    for (size_t i = 0; i < 152u * 210u; ++i) sum += thumb[i];
    assert(sum > 0);
    free(thumb);

    printf("test_epub_meta_and_cover: PASS\n");
}

int main(int argc, char** argv) {
    test_layout_highlights();
    test_mdns();
    if (argc > 1) {
        test_epub_meta_and_cover(argv[1]);
    }
    printf("mypico_host_test: ALL PASS\n");
    return 0;
}
