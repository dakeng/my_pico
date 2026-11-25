/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 书籍持久化数据层（TF 卡 .mypico 目录）：
 * 存储书名/作者、阅读进度、累计阅读时长、最近阅读时间、划线数组、封面灰度缓存与每日阅读日志，
 * 以及生成符合 Obsidian 模板的 Markdown 内容。
 *
 * English: MyPico persistent book data layer on TF card (.mypico directory):
 * stores title/author, progress, cumulative reading duration, last-read time, highlights,
 * grayscale cover thumbnails, daily session logs, and Obsidian Markdown export.
 *
 * 冻结：不依赖 UI 或硬件外设，支持在 macOS/Linux 主机直接单测；
 * 文件写入采用临时文件替换，降低断电损坏风险。
 * Frozen: No UI or hardware dependencies so host unit tests run directly;
 * writes go through temp-file replacement to reduce power-loss corruption.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOK_DATA_DEFAULT_ROOT "/sdcard/.mypico"
#define BOOK_DATA_ID_LEN 17
#define BOOK_DATA_PATH_MAX 288
#define BOOK_DATA_TITLE_MAX 160
#define BOOK_DATA_AUTHOR_MAX 128
#define BOOK_DATA_HL_TEXT_MAX 512
#define BOOK_DATA_MAX_HIGHLIGHTS 256

// 书架卡片封面缩略图标准尺寸（对应 Figma 130×180 × 1.17）。
// Standard shelf cover thumbnail dimensions (Figma 130x180 scaled by 1.17).
#define BOOK_COVER_W 152
#define BOOK_COVER_H 210

typedef struct {
    uint32_t id;                         ///< 递增唯一 ID / Monotonic highlight ID
    uint16_t chapter;                    ///< 章节索引 / Chapter index
    uint32_t start_off;                  ///< 章节内 UTF-8 起始字节偏移 / Start UTF-8 byte offset in chapter
    uint32_t end_off;                    ///< 章节内 UTF-8 结束字节偏移（开区间） / End UTF-8 byte offset (exclusive)
    uint32_t created_ts;                 ///< 创建时间 UTC Unix 秒 / Creation UTC Unix timestamp
    char text[BOOK_DATA_HL_TEXT_MAX];    ///< 划线原文（UTF-8） / Highlighted UTF-8 text
} book_highlight_t;

typedef struct {
    char book_id[BOOK_DATA_ID_LEN];      ///< 16 位十六进制稳定 ID / 16-hex stable book ID
    char file_path[BOOK_DATA_PATH_MAX];  ///< 书籍完整路径 / Full file path
    uint32_t file_size;                  ///< 文件字节数 / File size in bytes
    char title[BOOK_DATA_TITLE_MAX];     ///< 书名 / Book title
    char author[BOOK_DATA_AUTHOR_MAX];   ///< 作者 / Book author
    bool meta_parsed;                    ///< 是否已尝试提取过元数据 / Metadata already extracted
    bool has_cover;                      ///< 是否已有缓存封面 / Cached cover exists
    uint16_t chapter;                    ///< 当前章节索引 / Current chapter index
    uint32_t byte_off;                   ///< 当前章内 UTF-8 字节偏移 / Current byte offset in chapter
    uint8_t px;                          ///< 阅读字号 / Reading font size
    uint8_t pct;                         ///< 全书进度 0..100 / Whole-book progress 0..100
    uint32_t total_read_sec;             ///< 累计阅读时长（秒） / Cumulative reading time in seconds
    uint32_t last_read_ts;               ///< 最近阅读 UTC Unix 秒 / Last-read UTC Unix timestamp
    uint32_t created_ts;                 ///< 首次建档 UTC Unix 秒 / Creation UTC Unix timestamp
    uint32_t next_hl_id;                 ///< 下一条划线 ID / Next highlight ID
    size_t highlight_count;              ///< 当前划线条数 / Highlight count
    book_highlight_t* highlights;        ///< 划线动态数组 / Dynamic highlight array
} book_record_t;

typedef struct {
    char date[16];                       ///< 本地日期 YYYY-MM-DD / Local date YYYY-MM-DD
    uint32_t total_sec;                  ///< 当日累计阅读秒数 / Total reading seconds that day
} book_session_summary_t;

/// 覆盖数据根目录（主要用于主机单测，默认 "/sdcard/.mypico"）。
/// Override data root directory (primarily for host unit tests; defaults to "/sdcard/.mypico").
void book_data_set_root(const char* root_dir);
const char* book_data_get_root(void);

/// 确保 .mypico / books / covers 目录存在。
/// Ensure .mypico, books and covers directories exist.
esp_err_t book_data_ensure_dirs(void);

/// 根据文件路径（取 basename）生成 16 位十六进制稳定 book_id。
/// Generate a stable 16-hex book_id from the file path's basename.
void book_data_make_id(const char* file_path, char out_id[BOOK_DATA_ID_LEN]);

/// 初始化或释放 book_record_t 持有的动态内存。
/// Initialize or free dynamic memory owned by book_record_t.
void book_data_record_init(book_record_t* rec);
void book_data_record_free(book_record_t* rec);

/// 加载指定书籍记录；若磁盘尚无记录则按默认值初始化并返回 ESP_OK。
/// Load a book record; initializes defaults and returns ESP_OK if not yet stored on disk.
esp_err_t book_data_load(const char* file_path, uint32_t file_size, book_record_t* out);

/// 按 book_id 直接加载记录（供同步服务使用）。
/// Load a book record directly by book_id (used by the sync service).
esp_err_t book_data_load_by_id(const char* book_id, book_record_t* out);

/// 将书籍记录原子写入 JSON 文件。
/// Atomically write a book record to its JSON file.
esp_err_t book_data_save(const book_record_t* rec);

/// 新增一条划线（按章号+起始偏移有序插入，合并或覆盖重叠区间）；返回分配的 ID。
/// Add a highlight (ordered by chapter + start_off); optionally returns the assigned ID.
esp_err_t book_data_add_highlight(
    book_record_t* rec, uint16_t chapter, uint32_t start_off, uint32_t end_off,
    uint32_t created_ts, const char* text, uint32_t* out_id
);

/// 按 ID 删除指定划线；未找到返回 ESP_ERR_NOT_FOUND。
/// Delete a highlight by ID; returns ESP_ERR_NOT_FOUND if absent.
esp_err_t book_data_delete_highlight(book_record_t* rec, uint32_t id);

/// 查找某章某字节偏移命中的划线，返回索引；未命中返回 -1。
/// Find the highlight covering a byte offset in a chapter; returns -1 if none.
int book_data_find_highlight_at(const book_record_t* rec, uint16_t chapter, uint32_t off);

/// 追加一条阅读会话记录到 sessions.jsonl。
/// Append one reading session entry to sessions.jsonl.
esp_err_t book_data_append_session(
    const char* book_id, uint32_t ts_unix, uint32_t duration_sec, int16_t tz_offset_min
);

/// 汇总最近若干天的阅读时长（按日期降序返回），并输出总时长。
/// Aggregate reading durations for recent days (descending by date) and total seconds.
int book_data_load_recent_days(
    book_session_summary_t* out, int max_days, int16_t tz_offset_min, uint32_t* out_total_sec
);

/// 保存 8bpp 灰度封面为 4bpp 压缩缓存文件。
/// Save an 8bpp grayscale cover thumbnail as a packed 4bpp cache file.
esp_err_t book_data_cover_save(const char* book_id, uint16_t w, uint16_t h, const uint8_t* gray8);

/// 读取 4bpp 封面缓存并展开为 8bpp 灰度像素（每像素高4位有效），调用方 free。
/// Load a cached 4bpp cover into an 8bpp grayscale buffer; caller frees output.
esp_err_t book_data_cover_load(
    const char* book_id, uint16_t* out_w, uint16_t* out_h, uint8_t** out_gray8
);

/// 生成符合用户模板的 Obsidian Markdown 笔记（调用方 free *out_md）。
/// Build the Obsidian Markdown note matching the user's template (caller frees *out_md).
esp_err_t book_data_build_obsidian_md(
    const book_record_t* rec, int16_t tz_offset_min, char** out_md, size_t* out_len
);

/// 生成清理掉非法文件名字符的 Obsidian 笔记文件名（如 "页边和听写.md"）。
/// Build a filesystem-safe Obsidian note filename (e.g. "页边和听写.md").
void book_data_note_filename(const book_record_t* rec, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
