/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：独立 EPUB 书源，按 OPF spine 顺序提供章节与文本块。
 * English: Independent EPUB source exposing chapters and text blocks in OPF spine order.
 *
 * 冻结：进度使用 spine 原始 HTML 未压缩字节的累计值，不是 ZIP 物理偏移；不接入主循环。
 * Frozen: Progress uses cumulative uncompressed source HTML bytes, not physical ZIP offsets; no event-loop integration.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "html_text.h"

typedef struct book_epub book_epub_t;
/// 打开 EPUB；失败时 out 置空，成功后由 close 释放。/ Open EPUB; clear out on failure, close owns successful cleanup.
esp_err_t book_epub_open(const char *path, book_epub_t **out);
/// 关闭 ZIP 并释放目录，可传 NULL。/ Close ZIP and free the index; NULL is allowed.
void book_epub_close(book_epub_t *book);
/// 返回 spine 章节数，上限 32768。/ Return the spine chapter count, at most 32768.
size_t book_epub_chapter_count(const book_epub_t *book);
/// 复制 UTF-8 标题，容量不足报错而不截断。/ Copy a UTF-8 title; report insufficient capacity instead of truncating.
esp_err_t book_epub_chapter_title(const book_epub_t *book, size_t index, char *buf, size_t cap);
/// 加载 UTF-8、标题/段落及图片引用，不解码图片；调用方 html_text_free。/ Load UTF-8, styled blocks and image references without decoding images; caller uses html_text_free.
esp_err_t book_epub_load(book_epub_t *book, size_t index, html_text_t *out);
/// 明确请求后解码本章单幅本地图片；失败输出清空，成功像素由调用方 free，不修改正文或分页。
/// Decode one local image on explicit request; clear outputs on failure, caller frees successful pixels; text and pagination stay unchanged.
esp_err_t book_epub_load_image(book_epub_t *book, size_t index, const char *reference,
                              uint8_t **pixels, uint16_t *width, uint16_t *height);
/// spine 原始 HTML 未压缩字节总量，不等于 EPUB 文件大小。/ Total uncompressed source HTML bytes in the spine, not EPUB file size.
uint32_t book_epub_total_bytes(const book_epub_t *book);
/// 该章之前所有 spine 原始 HTML 字节累计值。/ Sum of source HTML bytes preceding this chapter in the spine.
uint32_t book_epub_chapter_byte_offset(const book_epub_t *book, size_t index);

typedef struct {
    char title[160];       ///< 书名（dc:title） / Book title (dc:title)
    char author[128];      ///< 作者（dc:creator） / Author (dc:creator)
    bool has_cover;        ///< 是否存在封面图条目 / Whether a cover image entry was found
    char cover_path[512];  ///< 封面在 ZIP 中的路径 / Cover image path inside the ZIP
} book_epub_meta_t;

/// 轻量读取 EPUB 的书名、作者和封面路径，不加载章节正文。
/// Read EPUB title, author and cover ZIP path lightly without loading chapter bodies.
esp_err_t book_epub_read_meta(const char *path, book_epub_meta_t *out);

/// 解码 EPUB 封面并等比居中裁切为 target_w × target_h 灰度图（8bpp），调用方 free。
/// Decode EPUB cover and center-crop/scale to target_w x target_h 8bpp grayscale; caller frees output.
esp_err_t book_epub_load_cover_thumb(
    const char *path, uint16_t target_w, uint16_t target_h, uint8_t **out_pixels
);
