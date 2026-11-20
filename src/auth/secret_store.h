/**
 * @file     secret_store.h
 * @brief    密钥/配置安全存储（NVS 封装）
 *
 * 规则 4-1：禁止密钥/Token 硬编码在源码里。
 * 本模块用 ESP32 NVS（Non-Volatile Storage）持久化敏感配置，
 * 读不到时调用方可选用内置默认值 fallback。
 *
 * 存储 layout（namespace = "xiaozhi"）：
 *   xfyun_appid          (string)   讯飞 TTS appid
 *   xfyun_api_key        (string)   讯飞 TTS api_key
 *   xfyun_api_secret     (string)   讯飞 TTS api_secret
 *   xfyun_vcn            (string)   讯飞 TTS 发音人（可选）
 *   ai_api_key           (string)   AI 大模型 API key
 *   ai_model             (string)   AI 模型名
 *   ai_url               (string)   AI API 完整 URL
 *
 * 修改：2026-11-20 新建（规则 4-1）
 */
#ifndef SECRET_STORE_H
#define SECRET_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 NVS（只需调一次，通常在 board_init_devices 里）
 * @return ESP_OK 成功；否则失败
 */
esp_err_t secret_store_init(void);

/**
 * @brief  读一个字符串密钥
 *
 * @param key      NVS 键名
 * @param out      输出缓冲
 * @param out_sz   缓冲容量
 * @param[out] got 读到的实际长度（不含 '\0'），可为 NULL
 * @return ESP_OK 读到了（out 已 '\0' 结尾）；ESP_ERR_NOT_FOUND 不存在；其他读 NVS 错误
 */
esp_err_t secret_store_get_str(const char *key, char *out, size_t out_sz, size_t *got);

/**
 * @brief  写一个字符串密钥
 *
 * @param key   NVS 键名
 * @param val   字符串值（以 '\0' 结尾）
 * @return ESP_OK 成功；其他写 NVS 错误
 */
esp_err_t secret_store_set_str(const char *key, const char *val);

/**
 * @brief  擦除全部存储（恢复出厂）
 */
esp_err_t secret_store_erase_all(void);

#ifdef __cplusplus
}
#endif

#endif /* SECRET_STORE_H */
