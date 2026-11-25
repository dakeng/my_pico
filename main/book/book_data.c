/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：MyPico 书籍数据持久化实现（JSON 读写、封面缓存、会话日志与 Obsidian Markdown 导出）。
 *
 * English: MyPico book data persistence (JSON I/O, cover cache, session logs,
 * and Obsidian Markdown export).
 *
 * 冻结：仅使用标准 C 文件接口与 cJSON；FatFs 替换文件前先删旧文件。
 * Frozen: Uses standard C file I/O and cJSON only; removes destination before rename for FatFs compatibility.
 */

#include "book_data.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void* data_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void* data_realloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static void* data_malloc(size_t n) { return malloc(n); }
static void* data_realloc(void* p, size_t n) { return realloc(p, n); }
#endif

static char s_root[128] = BOOK_DATA_DEFAULT_ROOT;

#define TIME_VALID_MIN 1577836800u
#define COVER_MAGIC "MP01"

/* ---- 纯整数日期与时长格式化 / Integer date and duration helpers ---- */

static void civil_from_days(int64_t z, int* year, int* month, int* day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    *year = (int)y;
    *month = (int)m;
    *day = (int)d;
}

static void format_date(uint32_t unix_sec, int16_t tz_offset_min, char out[16]) {
    if (unix_sec < TIME_VALID_MIN) {
        strcpy(out, "");
        return;
    }
    int64_t local = (int64_t)unix_sec + (int64_t)tz_offset_min * 60;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int y = 1970, m = 1, d = 1;
    civil_from_days(days, &y, &m, &d);
    if (y < 0 || y > 9999) y = 1970;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    snprintf(out, 16, "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
}

static void format_duration(uint32_t total_sec, char out[24]) {
    uint32_t total_min = total_sec / 60;
    uint32_t hours = total_min / 60;
    uint32_t mins = total_min % 60;
    if (hours > 0) snprintf(out, 24, "%uh%umin", (unsigned)hours, (unsigned)mins);
    else snprintf(out, 24, "%umin", (unsigned)mins);
}

static void strip_ext_basename(const char* path, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!path || !*path) return;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (len >= cap) len = cap - 1;
    memcpy(out, base, len);
    out[len] = 0;
}

static esp_err_t mkdir_if_missing(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return ESP_OK;
    return ESP_FAIL;
}

static esp_err_t write_file_atomic(const char* path, const void* data, size_t len) {
    char tmp[256];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    FILE* f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    size_t written = len ? fwrite(data, 1, len, f) : 0;
    int close_rc = fclose(f);
    if (written != len || close_rc != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- 根目录与 ID / Root and ID ---- */

void book_data_set_root(const char* root_dir) {
    if (!root_dir || !*root_dir) {
        strncpy(s_root, BOOK_DATA_DEFAULT_ROOT, sizeof(s_root) - 1);
        s_root[sizeof(s_root) - 1] = 0;
        return;
    }
    strncpy(s_root, root_dir, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = 0;
}

const char* book_data_get_root(void) {
    return s_root;
}

esp_err_t book_data_ensure_dirs(void) {
    char sub[192];
    if (mkdir_if_missing(s_root) != ESP_OK) return ESP_FAIL;
    if (snprintf(sub, sizeof(sub), "%s/books", s_root) >= (int)sizeof(sub)) return ESP_ERR_INVALID_SIZE;
    if (mkdir_if_missing(sub) != ESP_OK) return ESP_FAIL;
    if (snprintf(sub, sizeof(sub), "%s/covers", s_root) >= (int)sizeof(sub)) return ESP_ERR_INVALID_SIZE;
    if (mkdir_if_missing(sub) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

void book_data_make_id(const char* file_path, char out_id[BOOK_DATA_ID_LEN]) {
    if (!out_id) return;
    const char* base = file_path ? strrchr(file_path, '/') : NULL;
    const char* key = base ? base + 1 : (file_path ? file_path : "");
    // FNV-1a 64-bit hash -> 16 位十六进制 ASCII ID。
    // FNV-1a 64-bit hash -> 16-char lowercase hex ASCII ID.
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char* p = (const unsigned char*)key; *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    snprintf(out_id, BOOK_DATA_ID_LEN, "%016llx", (unsigned long long)h);
}

void book_data_record_init(book_record_t* rec) {
    if (!rec) return;
    memset(rec, 0, sizeof(*rec));
    rec->px = 44;
    rec->next_hl_id = 1;
}

void book_data_record_free(book_record_t* rec) {
    if (!rec) return;
    free(rec->highlights);
    rec->highlights = NULL;
    rec->highlight_count = 0;
}

/* ---- JSON 序列化与反序列化 / JSON serialization ---- */

static esp_err_t book_json_path(const char* book_id, char* out, size_t cap) {
    if (!book_id || !*book_id) return ESP_ERR_INVALID_ARG;
    int n = snprintf(out, cap, "%s/books/%s.json", s_root, book_id);
    return (n > 0 && (size_t)n < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t book_cover_path(const char* book_id, char* out, size_t cap) {
    if (!book_id || !*book_id) return ESP_ERR_INVALID_ARG;
    int n = snprintf(out, cap, "%s/covers/%s.raw", s_root, book_id);
    return (n > 0 && (size_t)n < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void copy_json_str(const cJSON* obj, const char* key, char* dst, size_t cap) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring || !cap) return;
    strncpy(dst, item->valuestring, cap - 1);
    dst[cap - 1] = 0;
}

static uint32_t read_json_u32(const cJSON* obj, const char* key, uint32_t def) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) return def;
    return (uint32_t)item->valuedouble;
}

static esp_err_t parse_record_json(const char* text, book_record_t* out) {
    cJSON* root = cJSON_Parse(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    copy_json_str(root, "book_id", out->book_id, sizeof(out->book_id));
    copy_json_str(root, "file_path", out->file_path, sizeof(out->file_path));
    out->file_size = read_json_u32(root, "file_size", out->file_size);
    copy_json_str(root, "title", out->title, sizeof(out->title));
    copy_json_str(root, "author", out->author, sizeof(out->author));

    const cJSON* mp = cJSON_GetObjectItemCaseSensitive(root, "meta_parsed");
    if (cJSON_IsBool(mp)) out->meta_parsed = cJSON_IsTrue(mp);
    const cJSON* hc = cJSON_GetObjectItemCaseSensitive(root, "has_cover");
    if (cJSON_IsBool(hc)) out->has_cover = cJSON_IsTrue(hc);

    out->chapter = (uint16_t)read_json_u32(root, "chapter", 0);
    out->byte_off = read_json_u32(root, "byte_off", 0);
    uint32_t px = read_json_u32(root, "px", 44);
    out->px = (px >= 28 && px <= 72) ? (uint8_t)px : 44;
    uint32_t pct = read_json_u32(root, "pct", 0);
    out->pct = pct > 100 ? 100 : (uint8_t)pct;
    out->total_read_sec = read_json_u32(root, "total_read_sec", 0);
    out->last_read_ts = read_json_u32(root, "last_read_ts", 0);
    out->created_ts = read_json_u32(root, "created_ts", 0);
    out->next_hl_id = read_json_u32(root, "next_hl_id", 1);

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "highlights");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > BOOK_DATA_MAX_HIGHLIGHTS) n = BOOK_DATA_MAX_HIGHLIGHTS;
        if (n > 0) {
            out->highlights = data_malloc((size_t)n * sizeof(*out->highlights));
            if (!out->highlights) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            size_t count = 0;
            for (int i = 0; i < n; ++i) {
                const cJSON* h = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsObject(h)) continue;
                book_highlight_t* dst = &out->highlights[count];
                memset(dst, 0, sizeof(*dst));
                dst->id = read_json_u32(h, "id", (uint32_t)(count + 1));
                dst->chapter = (uint16_t)read_json_u32(h, "chapter", 0);
                dst->start_off = read_json_u32(h, "start_off", 0);
                dst->end_off = read_json_u32(h, "end_off", 0);
                dst->created_ts = read_json_u32(h, "created_ts", 0);
                copy_json_str(h, "text", dst->text, sizeof(dst->text));
                if (dst->end_off > dst->start_off && dst->text[0]) {
                    if (dst->id >= out->next_hl_id) out->next_hl_id = dst->id + 1;
                    ++count;
                }
            }
            out->highlight_count = count;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t book_data_load_by_id(const char* book_id, book_record_t* out) {
    if (!book_id || !*book_id || !out) return ESP_ERR_INVALID_ARG;
    book_data_record_free(out);
    book_data_record_init(out);
    strncpy(out->book_id, book_id, sizeof(out->book_id) - 1);

    char path[256];
    esp_err_t err = book_json_path(book_id, path, sizeof(path));
    if (err != ESP_OK) return err;

    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ESP_FAIL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ESP_FAIL; }
    char* buf = data_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return ESP_FAIL; }
    buf[sz] = 0;

    err = parse_record_json(buf, out);
    free(buf);
    if (err != ESP_OK) {
        book_data_record_free(out);
        book_data_record_init(out);
        strncpy(out->book_id, book_id, sizeof(out->book_id) - 1);
        return err;
    }
    return ESP_OK;
}

esp_err_t book_data_load(const char* file_path, uint32_t file_size, book_record_t* out) {
    if (!file_path || !*file_path || !out) return ESP_ERR_INVALID_ARG;
    char id[BOOK_DATA_ID_LEN];
    book_data_make_id(file_path, id);

    esp_err_t err = book_data_load_by_id(id, out);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
        book_data_record_free(out);
        book_data_record_init(out);
        strcpy(out->book_id, id);
        strncpy(out->file_path, file_path, sizeof(out->file_path) - 1);
        out->file_size = file_size;
        strip_ext_basename(file_path, out->title, sizeof(out->title));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    strncpy(out->file_path, file_path, sizeof(out->file_path) - 1);
    if (file_size) out->file_size = file_size;
    if (!out->title[0]) strip_ext_basename(file_path, out->title, sizeof(out->title));
    return ESP_OK;
}

esp_err_t book_data_save(const book_record_t* rec) {
    if (!rec || !rec->book_id[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = book_data_ensure_dirs();
    if (err != ESP_OK) return err;

    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "book_id", rec->book_id);
    cJSON_AddStringToObject(root, "file_path", rec->file_path);
    cJSON_AddNumberToObject(root, "file_size", rec->file_size);
    cJSON_AddStringToObject(root, "title", rec->title);
    cJSON_AddStringToObject(root, "author", rec->author);
    cJSON_AddBoolToObject(root, "meta_parsed", rec->meta_parsed);
    cJSON_AddBoolToObject(root, "has_cover", rec->has_cover);
    cJSON_AddNumberToObject(root, "chapter", rec->chapter);
    cJSON_AddNumberToObject(root, "byte_off", rec->byte_off);
    cJSON_AddNumberToObject(root, "px", rec->px);
    cJSON_AddNumberToObject(root, "pct", rec->pct);
    cJSON_AddNumberToObject(root, "total_read_sec", rec->total_read_sec);
    cJSON_AddNumberToObject(root, "last_read_ts", rec->last_read_ts);
    cJSON_AddNumberToObject(root, "created_ts", rec->created_ts);
    cJSON_AddNumberToObject(root, "next_hl_id", rec->next_hl_id ? rec->next_hl_id : 1);

    cJSON* arr = cJSON_AddArrayToObject(root, "highlights");
    if (!arr) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < rec->highlight_count; ++i) {
        const book_highlight_t* h = &rec->highlights[i];
        cJSON* item = cJSON_CreateObject();
        if (!item) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
        cJSON_AddNumberToObject(item, "id", h->id);
        cJSON_AddNumberToObject(item, "chapter", h->chapter);
        cJSON_AddNumberToObject(item, "start_off", h->start_off);
        cJSON_AddNumberToObject(item, "end_off", h->end_off);
        cJSON_AddNumberToObject(item, "created_ts", h->created_ts);
        cJSON_AddStringToObject(item, "text", h->text);
        cJSON_AddItemToArray(arr, item);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    char path[256];
    err = book_json_path(rec->book_id, path, sizeof(path));
    if (err == ESP_OK) err = write_file_atomic(path, json, strlen(json));
    cJSON_free(json);
    return err;
}

/* ---- 划线管理 / Highlight management ---- */

esp_err_t book_data_add_highlight(
    book_record_t* rec, uint16_t chapter, uint32_t start_off, uint32_t end_off,
    uint32_t created_ts, const char* text, uint32_t* out_id
) {
    if (!rec || !text || !text[0] || end_off <= start_off) return ESP_ERR_INVALID_ARG;
    if (rec->highlight_count >= BOOK_DATA_MAX_HIGHLIGHTS) return ESP_ERR_NO_MEM;

    // 去重：若同章有完全相同的起止偏移，直接返回已有 ID。
    // Deduplicate identical chapter + range; return existing ID.
    for (size_t i = 0; i < rec->highlight_count; ++i) {
        if (rec->highlights[i].chapter == chapter
            && rec->highlights[i].start_off == start_off
            && rec->highlights[i].end_off == end_off) {
            if (out_id) *out_id = rec->highlights[i].id;
            return ESP_OK;
        }
    }

    book_highlight_t* grown = data_realloc(
        rec->highlights, (rec->highlight_count + 1) * sizeof(*rec->highlights)
    );
    if (!grown) return ESP_ERR_NO_MEM;
    rec->highlights = grown;

    // 按 (chapter, start_off) 有序插入，保证笔记页和导出的阅读顺序一致。
    // Insert ordered by (chapter, start_off) so notes and exports follow reading order.
    size_t pos = rec->highlight_count;
    while (pos > 0) {
        const book_highlight_t* prev = &rec->highlights[pos - 1];
        if (prev->chapter < chapter || (prev->chapter == chapter && prev->start_off <= start_off)) {
            break;
        }
        rec->highlights[pos] = rec->highlights[pos - 1];
        --pos;
    }

    book_highlight_t* dst = &rec->highlights[pos];
    memset(dst, 0, sizeof(*dst));
    if (rec->next_hl_id == 0) rec->next_hl_id = 1;
    dst->id = rec->next_hl_id++;
    dst->chapter = chapter;
    dst->start_off = start_off;
    dst->end_off = end_off;
    dst->created_ts = created_ts;

    // 截断时保证落在 UTF-8 码点边界上。
    // Truncate cleanly at a UTF-8 codepoint boundary.
    size_t tlen = strlen(text);
    if (tlen >= sizeof(dst->text)) {
        tlen = sizeof(dst->text) - 1;
        while (tlen > 0 && ((unsigned char)text[tlen] & 0xC0) == 0x80) --tlen;
    }
    memcpy(dst->text, text, tlen);
    dst->text[tlen] = 0;

    rec->highlight_count++;
    if (!rec->created_ts && created_ts >= TIME_VALID_MIN) rec->created_ts = created_ts;
    if (out_id) *out_id = dst->id;
    return ESP_OK;
}

esp_err_t book_data_delete_highlight(book_record_t* rec, uint32_t id) {
    if (!rec) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < rec->highlight_count; ++i) {
        if (rec->highlights[i].id == id) {
            for (size_t j = i + 1; j < rec->highlight_count; ++j) {
                rec->highlights[j - 1] = rec->highlights[j];
            }
            rec->highlight_count--;
            if (rec->highlight_count == 0) {
                free(rec->highlights);
                rec->highlights = NULL;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int book_data_find_highlight_at(const book_record_t* rec, uint16_t chapter, uint32_t off) {
    if (!rec || !rec->highlights) return -1;
    for (size_t i = 0; i < rec->highlight_count; ++i) {
        const book_highlight_t* h = &rec->highlights[i];
        if (h->chapter == chapter && off >= h->start_off && off < h->end_off) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- 阅读会话日志 / Reading session logs ---- */

esp_err_t book_data_append_session(
    const char* book_id, uint32_t ts_unix, uint32_t duration_sec, int16_t tz_offset_min
) {
    if (!book_id || !*book_id || duration_sec == 0) return ESP_OK;
    esp_err_t err = book_data_ensure_dirs();
    if (err != ESP_OK) return err;

    char path[192];
    if (snprintf(path, sizeof(path), "%s/sessions.jsonl", s_root) >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char date[16];
    format_date(ts_unix, tz_offset_min, date);
    if (!date[0]) strcpy(date, "未校时");

    FILE* f = fopen(path, "ab");
    if (!f) return ESP_FAIL;
    fprintf(
        f, "{\"date\":\"%s\",\"sec\":%u,\"id\":\"%s\",\"ts\":%u}\n",
        date, (unsigned)duration_sec, book_id, (unsigned)ts_unix
    );
    return fclose(f) == 0 ? ESP_OK : ESP_FAIL;
}

int book_data_load_recent_days(
    book_session_summary_t* out, int max_days, int16_t tz_offset_min, uint32_t* out_total_sec
) {
    (void)tz_offset_min;
    if (out_total_sec) *out_total_sec = 0;
    if (!out || max_days <= 0) return 0;

    char path[192];
    if (snprintf(path, sizeof(path), "%s/sessions.jsonl", s_root) >= (int)sizeof(path)) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    int count = 0;
    uint32_t total = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        cJSON* obj = cJSON_Parse(line);
        if (!obj) continue;
        const cJSON* dj = cJSON_GetObjectItemCaseSensitive(obj, "date");
        const cJSON* sj = cJSON_GetObjectItemCaseSensitive(obj, "sec");
        if (cJSON_IsString(dj) && dj->valuestring && cJSON_IsNumber(sj) && sj->valuedouble > 0) {
            uint32_t sec = (uint32_t)sj->valuedouble;
            total += sec;
            int found = -1;
            for (int i = 0; i < count; ++i) {
                if (strcmp(out[i].date, dj->valuestring) == 0) { found = i; break; }
            }
            if (found >= 0) {
                out[found].total_sec += sec;
            } else {
                // 保持日期降序插入，仅保留最近 max_days 天。
                // Keep descending date order up to max_days.
                int pos = count < max_days ? count : max_days - 1;
                if (count < max_days) {
                    count++;
                } else if (strcmp(dj->valuestring, out[max_days - 1].date) <= 0) {
                    cJSON_Delete(obj);
                    continue;
                }
                while (pos > 0 && strcmp(dj->valuestring, out[pos - 1].date) > 0) {
                    out[pos] = out[pos - 1];
                    --pos;
                }
                memset(&out[pos], 0, sizeof(out[pos]));
                strncpy(out[pos].date, dj->valuestring, sizeof(out[pos].date) - 1);
                out[pos].total_sec = sec;
            }
        }
        cJSON_Delete(obj);
    }
    fclose(f);
    if (out_total_sec) *out_total_sec = total;
    return count;
}

/* ---- 4bpp 封面缩略图缓存 / 4bpp cover thumbnail cache ---- */

esp_err_t book_data_cover_save(const char* book_id, uint16_t w, uint16_t h, const uint8_t* gray8) {
    if (!book_id || !*book_id || !w || !h || !gray8) return ESP_ERR_INVALID_ARG;
    esp_err_t err = book_data_ensure_dirs();
    if (err != ESP_OK) return err;

    size_t pixels = (size_t)w * h;
    size_t packed_len = (pixels + 1) / 2;
    size_t total = 8 + packed_len;
    uint8_t* buf = data_malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;

    memcpy(buf, COVER_MAGIC, 4);
    buf[4] = (uint8_t)(w & 0xFF);
    buf[5] = (uint8_t)(w >> 8);
    buf[6] = (uint8_t)(h & 0xFF);
    buf[7] = (uint8_t)(h >> 8);

    uint8_t* dst = buf + 8;
    for (size_t i = 0; i < packed_len; ++i) {
        uint8_t p0 = gray8[i * 2] >> 4;
        uint8_t p1 = (i * 2 + 1 < pixels) ? (gray8[i * 2 + 1] >> 4) : 0x0F;
        dst[i] = (uint8_t)((p0 << 4) | p1);
    }

    char path[256];
    err = book_cover_path(book_id, path, sizeof(path));
    if (err == ESP_OK) err = write_file_atomic(path, buf, total);
    free(buf);
    return err;
}

esp_err_t book_data_cover_load(
    const char* book_id, uint16_t* out_w, uint16_t* out_h, uint8_t** out_gray8
) {
    if (!book_id || !*book_id || !out_w || !out_h || !out_gray8) return ESP_ERR_INVALID_ARG;
    *out_w = 0;
    *out_h = 0;
    *out_gray8 = NULL;

    char path[256];
    esp_err_t err = book_cover_path(book_id, path, sizeof(path));
    if (err != ESP_OK) return err;

    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, COVER_MAGIC, 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t w = (uint16_t)(hdr[4] | ((uint16_t)hdr[5] << 8));
    uint16_t h = (uint16_t)(hdr[6] | ((uint16_t)hdr[7] << 8));
    if (!w || !h || w > 648 || h > 1000) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pixels = (size_t)w * h;
    size_t packed_len = (pixels + 1) / 2;
    uint8_t* packed = data_malloc(packed_len);
    uint8_t* gray8 = data_malloc(pixels);
    if (!packed || !gray8) {
        free(packed);
        free(gray8);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(packed, 1, packed_len, f);
    fclose(f);
    if (rd != packed_len) {
        free(packed);
        free(gray8);
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < packed_len; ++i) {
        gray8[i * 2] = (uint8_t)(packed[i] & 0xF0);
        if (i * 2 + 1 < pixels) gray8[i * 2 + 1] = (uint8_t)((packed[i] & 0x0F) << 4);
    }
    free(packed);
    *out_w = w;
    *out_h = h;
    *out_gray8 = gray8;
    return ESP_OK;
}

/* ---- Obsidian Markdown 导出 / Obsidian Markdown export ---- */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
    bool oom;
} strbuf_t;

static void sb_append_n(strbuf_t* sb, const char* s, size_t n) {
    if (sb->oom || !n) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t next = sb->cap ? sb->cap * 2 : 1024;
        while (next < sb->len + n + 1) next *= 2;
        char* grown = data_realloc(sb->buf, next);
        if (!grown) { sb->oom = true; return; }
        sb->buf = grown;
        sb->cap = next;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}

static void sb_append(strbuf_t* sb, const char* s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_yaml_value(strbuf_t* sb, const char* s) {
    if (!s || !*s) return;
    bool needs_quote = false;
    for (const char* p = s; *p; ++p) {
        if (*p == ':' || *p == '#' || *p == '"' || *p == '\'' || *p == '[' || *p == ']'
            || *p == '{' || *p == '}' || *p == '\n' || *p == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) {
        sb_append(sb, s);
        return;
    }
    sb_append(sb, "\"");
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') sb_append_n(sb, "\\", 1);
        if (*p == '\n' || *p == '\r') sb_append_n(sb, " ", 1);
        else sb_append_n(sb, p, 1);
    }
    sb_append(sb, "\"");
}

void book_data_note_filename(const book_record_t* rec, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    const char* src = (rec && rec->title[0]) ? rec->title : (rec ? rec->book_id : "book");
    size_t used = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && used + 4 < cap; ++p) {
        char c = (char)*p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        out[used++] = c;
    }
    while (used > 0 && (out[used - 1] == ' ' || out[used - 1] == '.')) --used;
    if (used == 0 && cap > 5) {
        memcpy(out, "note", 4);
        used = 4;
    }
    if (used + 3 < cap) {
        memcpy(out + used, ".md", 4);
    } else {
        out[used] = 0;
    }
}

esp_err_t book_data_build_obsidian_md(
    const book_record_t* rec, int16_t tz_offset_min, char** out_md, size_t* out_len
) {
    if (!rec || !out_md) return ESP_ERR_INVALID_ARG;
    *out_md = NULL;
    if (out_len) *out_len = 0;

    char created_str[16] = {0};
    char last_read_str[16] = {0};
    char dur_str[24] = {0};
    uint32_t created_ts = rec->created_ts ? rec->created_ts : rec->last_read_ts;
    format_date(created_ts, tz_offset_min, created_str);
    format_date(rec->last_read_ts, tz_offset_min, last_read_str);
    format_duration(rec->total_read_sec, dur_str);

    strbuf_t sb = {0};

    // 属性严格按用户 Figma 模板：仅写入书名和 created，其余保持原样。
    // Frontmatter strictly follows the user's Figma template: only title and created are populated.
    sb_append(&sb, "---\n分类:\n  - \"[[阅读笔记]]\"\n书名: ");
    sb_append_yaml_value(&sb, rec->title);
    sb_append(&sb, "\ncreated: ");
    sb_append(&sb, created_str);
    sb_append(&sb, "\ntags:\n---\n\n");

    // 正文记录阅读进度、阅读时长、最近阅读及全部划线。
    // Body records reading progress, duration, last read date, and all highlights.
    sb_append(&sb, "## 阅读记录\n\n");
    if (rec->author[0]) {
        sb_append(&sb, "- 作者：");
        sb_append(&sb, rec->author);
        sb_append(&sb, "\n");
    }
    char line[96];
    snprintf(line, sizeof(line), "- 阅读进度：%u%%\n", (unsigned)rec->pct);
    sb_append(&sb, line);
    snprintf(line, sizeof(line), "- 阅读时长：%s\n", dur_str);
    sb_append(&sb, line);
    snprintf(
        line, sizeof(line), "- 最近阅读：%s\n",
        last_read_str[0] ? last_read_str : "--"
    );
    sb_append(&sb, line);
    snprintf(line, sizeof(line), "- 划线数：%u\n\n", (unsigned)rec->highlight_count);
    sb_append(&sb, line);

    sb_append(&sb, "## 划线\n\n");
    if (rec->highlight_count == 0) {
        sb_append(&sb, "暂无划线。\n");
    } else {
        for (size_t i = 0; i < rec->highlight_count; ++i) {
            const char* t = rec->highlights[i].text;
            sb_append(&sb, "> ");
            for (const char* p = t; *p; ++p) {
                if (*p == '\r') continue;
                if (*p == '\n') sb_append(&sb, "\n> ");
                else sb_append_n(&sb, p, 1);
            }
            sb_append(&sb, "\n\n");
        }
    }

    if (sb.oom || !sb.buf) {
        free(sb.buf);
        return ESP_ERR_NO_MEM;
    }
    *out_md = sb.buf;
    if (out_len) *out_len = sb.len;
    return ESP_OK;
}
