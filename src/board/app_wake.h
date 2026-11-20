/**
 * @file     app_wake.h
 * @brief    唤醒业务层（薄壳）：唤醒 -> OLED 提示 + TTS 回应 + 接收指令文本
 *
 * 设计说明：
 *   - 本文件只做「编排」，不含任何识别/匹配逻辑 —— 那些都在 kws_hub 组件里，
 *     换成 ESP-SR 离线引擎时本文件一行都不用改。
 *   - 视觉反馈用 OLED 短状态词（本项目 OLED 为 I²C SSD1306 0.91"，
 *     U8g2 库提供现成 ASCII 字库，足够显示状态用）。
 */
#ifndef APP_WAKE_H
#define APP_WAKE_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化唤醒业务（设置唤醒词并开始监听）
 *
 * 功能：注册 kws_hub 回调 -> 设唤醒词 -> 初始化引擎 -> 启动监听
 * 修改：2026-10-18 新建
 *
 * @return ESP_OK 成功（监听已启动）
 */
esp_err_t app_wake_init(void);

/**
 * @brief  停止监听
 *
 * 功能：停止 kws_hub 监听任务
 * 修改：2026-10-18 新建
 */
esp_err_t app_wake_stop(void);

/**
 * @brief  运行时更换唤醒词
 *
 * 功能：唤醒词可按产品需要随时改，例如改成「小智同学」
 * 修改：2026-10-18 新建
 *
 * @param word 唤醒词，不可为空
 */
esp_err_t app_wake_set_word(const char *word);

/**
 * @brief  追加唤醒词别名
 *
 * 功能：主词之外再接受若干种叫法
 * 修改：2026-10-18 新建
 */
esp_err_t app_wake_add_alias(const char *word);

#ifdef __cplusplus
}
#endif

#endif /* APP_WAKE_H */