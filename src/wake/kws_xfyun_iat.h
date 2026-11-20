/**
 * @file     kws_xfyun_iat.h
 * @brief    讯飞 IAT（语音听写）在线引擎 — 接口层
 *
 * 功能：WebSocket 连讯飞 IAT /v2/iat，持续上送麦克风音频，
 *       实时解析增量识别文字，上报 kws_hub 事件。
 *
 * 数据流：
 *   INMP441 环形缓冲 → 320ms 分片 → base64 → WebSocket 上送
 *                                        ↓
 *                                  讯飞返回 JSON 识别结果
 *                                        ↓
 *                                  parse → 匹配唤醒词 → emit KWS_EV_WAKE
 *                                        → 累积指令文字 → emit KWS_EV_TEXT
 *
 * 鉴权：复用 xfyun_auth.c（HMAC-SHA256，与 TTS 同密钥）。
 * 传输：复用 ws_bridge（ArduinoWebsockets C 桥接层）。
 * 修改：2026-11-19 新建
 */
#ifndef KWS_XFYUN_IAT_H
#define KWS_XFYUN_IAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kws_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化讯飞 IAT 引擎
 *
 * 功能：保存配置，校验密钥，不启动网络连接和任务
 * 修改：2026-11-19 新建
 *
 * @param appid       讯飞应用 ID
 * @param api_key     讯飞 API Key
 * @param api_secret  讯飞 API Secret
 * @param wake_word   唤醒词（字符串匹配，如 "小智"）
 * @param cb          事件回调
 * @param cb_ctx      回调上下文
 */
esp_err_t kws_iat_init(const char *appid, const char *api_key, const char *api_secret,
                        const char *wake_word,
                        kws_event_cb_t cb, void *cb_ctx);

/**
 * @brief  启动 IAT 监听（建任务 + 连 WebSocket + 开始收音频）
 *
 * 修改：2026-11-19 新建
 */
esp_err_t kws_iat_start(void);

/**
 * @brief  停止 IAT 监听
 *
 * 修改：2026-11-19 新建
 */
esp_err_t kws_iat_stop(void);

/**
 * @brief  暂停 IAT 监听（任务停在上送音频，不动音频/网络）
 *
 * 修改：2026-11-19 新建
 */
esp_err_t kws_iat_pause(void);

/**
 * @brief  恢复 IAT 监听
 *
 * 修改：2026-11-19 新建
 */
esp_err_t kws_iat_resume(void);

bool kws_iat_is_listening(void);

#ifdef __cplusplus
}
#endif

#endif /* KWS_XFYUN_IAT_H */
