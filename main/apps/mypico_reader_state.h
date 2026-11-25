/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 书架、笔记与阅读页之间的轻量共享契约。
 *
 * English: Lightweight shared handoff contract between MyPico shelf, notes and reader pages.
 */

#pragma once

#include <stdint.h>

#include "app.h"
#include "book_data.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_launcher;
extern const app_desc_t app_mypico_shelf;
extern const app_desc_t app_mypico_reader;
extern const app_desc_t app_transfer;

/// 指定阅读页下次进入时要打开的书籍路径与文件大小。
/// Set the target book path and size for the next entry into app_mypico_reader.
void mypico_reader_set_target(const char* file_path, uint32_t file_size);

/// 通知书架下次进入时刷新数据（例如刚完成阅读、新增或删除划线）。
/// Mark the shelf dirty so its next entry reloads updated stats and highlights.
void mypico_shelf_mark_dirty(void);

#ifdef __cplusplus
}
#endif
