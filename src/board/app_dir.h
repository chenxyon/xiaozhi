/**
 * @file     app_dir.h
 * @brief    员工分机查询：从 954n2fwj.qwenwork.host 通讯录查分机并语音播报
 *
 * 数据源：GET https://954n2fwj.qwenwork.host/api/employees
 *         → {"ok":true,"data":[{"name":"姜维","ext":"8001",...}, ...]}
 */
#ifndef APP_DIR_H
#define APP_DIR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 这句文本是否是"查分机"意图（含"分机"） */
bool app_dir_matches(const char *text);

/* 异步查询：解析姓名 → 查通讯录 → 播「XX，分机是 XXXX。」三遍 → 恢复监听 */
void app_dir_query_async(const char *question);

/* 自检：查询指定姓名并打日志（不播报），用于验证联网/解析是否可用 */
void app_dir_selftest(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* APP_DIR_H */
