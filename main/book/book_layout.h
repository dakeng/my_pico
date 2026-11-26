/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：借用章节 UTF-8 文本与块样式，支持可配置精排参数（行距/段距/缩进/对齐/避头尾/插图混排）分页并绘制正文。
 * English: Paginate borrowed chapter UTF-8 text and styled blocks with configurable typography and inline images.
 *
 * 冻结：不释放原文、不刷新屏幕；调用方持有字体绘制互斥锁。
 * Frozen: Never free source text or present the display; caller holds the font draw lock.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "epdiy.h"
#include "html_text.h"

typedef enum {
    BOOK_ALIGN_JUSTIFY = 0, ///< 两端对齐 / Full justification
    BOOK_ALIGN_LEFT = 1,    ///< 左对齐 / Left-aligned
} book_align_t;

typedef enum {
    BOOK_IMG_INLINE = 0,      ///< 正文混排显示（含 16 级抖动）/ Inline illustration rendering with 4bpp dithering
    BOOK_IMG_PLACEHOLDER = 1, ///< 点击占位查看 / Clickable placeholder box
    BOOK_IMG_HIDE = 2,        ///< 隐藏插图 / Hide illustrations
} book_img_mode_t;

typedef struct {
    uint8_t line_spacing_pct; ///< 行距增量百分比（相对字号，如 50 表示 1.5x），0 取默认 50 / Extra line height %
    uint8_t para_spacing_pct; ///< 段间距百分比（相对字号，0..120）/ Paragraph gap % of font size
    bool first_line_indent;   ///< 正文段落首行缩进两字 / 2-em first-line indent on body paragraphs
    uint8_t align;            ///< 默认对齐方式（book_align_t）/ Default alignment (book_align_t)
    uint8_t image_mode;       ///< 正文插图模式（book_img_mode_t）/ Image mode (book_img_mode_t)
    bool embedded_style;      ///< 启用 EPUB 内嵌样式（标题层级/引用/居中/加粗/分割线）/ Honor EPUB block styles
    bool cjk_kinsoku;         ///< 启用中文避头尾禁则与英文按词折行 / CJK kinsoku & Latin word wrapping
} book_layout_style_t;

/// 返回精排推荐默认参数。/ Return recommended rich-typography defaults.
book_layout_style_t book_layout_style_default(void);

/// 重排借用文本；空文一页，失败清空布局，超过 4096 页返回 false。/ Borrow and paginate; empty text has one page, failure clears layout, over 4096 pages fails.
bool book_layout_build(const char* utf8, size_t len, EpdRect rect, int px);
/// 借用块表；标题字号加8，块间单换行；原文与块表须存活至free。/ Borrow blocks; headings add 8 px, with one newline between blocks; text and blocks must outlive layout.
bool book_layout_build_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px);
/// 初始化并只排前两页（兼容默认样式）；后续由调用方分批推进。/ Initialize the first two pages with legacy defaults; caller advances later batches.
bool book_layout_begin_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px);
/// 带可配置精排样式初始化并只排前两页；style 为 NULL 时退回兼容默认样式。/ Begin incremental block pagination with configurable typography style.
bool book_layout_begin_blocks_styled(
    const char* utf8, size_t len, const blk_t* blocks, size_t count,
    EpdRect rect, int px, const book_layout_style_t* style
);
/// 最多增加指定页数；失败清空布局，调用方必须停止阅读。/ Add at most the requested pages; failure clears layout and requires ending reading.
bool book_layout_extend(size_t pages);
/// 是否完成整章分页。/ Whether the whole chapter has been paginated.
bool book_layout_complete(void);
/// 释放页表，不释放原文。/ Free layout storage, never the borrowed text.
void book_layout_free(void);
/// 返回已完成页数；未建立布局时为零，分批排版时不是整章总页数。/ Return completed pages, zero without a layout; incremental counts are not the chapter total.
size_t book_layout_page_count(void);
/// 使用建立布局时的宽高与字号绘图；不匹配或越界时不绘制。/ Draw with the built dimensions and size; mismatches or invalid pages do nothing.
void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px);
/// 命中本页图片占位或混排插图，返回块索引及可选矩形；未命中为 SIZE_MAX；与绘制使用相同锁。
/// Hit an image placeholder or inline image on the page, returning its block index and optional bounds; SIZE_MAX if absent.
size_t book_layout_image_at(size_t page, EpdRect rect, int x, int y, EpdRect* hit);
/// 查找字节偏移所属页，越界偏移夹到末页。/ Find page containing a byte offset; excessive offsets clamp to the last page.
size_t book_layout_page_for_offset(size_t off);
/// 返回页首偏移；已完成页数索引是待排页起点（完成后为文本末尾），更大索引返回文本长度。
/// Return a page start; index equal to completed count marks the pending page (EOF when complete), larger indexes return text length.
size_t book_layout_page_start_offset(size_t page);

typedef struct {
    size_t start_off;  ///< 章节内起始字节偏移 / Start UTF-8 byte offset in chapter
    size_t end_off;    ///< 章节内结束字节偏移（开区间） / Exclusive end UTF-8 byte offset in chapter
} book_layout_span_t;

/// 查找屏幕坐标 (x, y) 命中的 UTF-8 字符区间 [out_start, out_end) 及字形框；调用方持字体锁。
/// Hit-test screen coords (x, y) to the UTF-8 glyph byte range [out_start, out_end) and optional rect; caller holds font lock.
bool book_layout_char_at(
    size_t page, EpdRect rect, int x, int y,
    size_t* out_start, size_t* out_end, EpdRect* out_glyph_rect
);

/// 枚举页内与 [start_off, end_off) 重叠的每行下划线矩形（高 3px），返回写入数量。
/// Enumerate per-line 3px underline rectangles on a page overlapping [start_off, end_off); returns count written.
size_t book_layout_span_underlines(
    size_t page, EpdRect rect, size_t start_off, size_t end_off,
    EpdRect* out_rects, size_t max_rects
);

/// 绘制本页正文并叠加下划线列表；调用方持字体锁。
/// Draw page text with highlight underline spans overlaid; caller holds font lock.
void book_layout_draw_page_highlights(
    uint8_t* fb, size_t page, EpdRect rect, int px,
    const book_layout_span_t* spans, size_t span_count
);
