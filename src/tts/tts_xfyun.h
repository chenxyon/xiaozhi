/**
 * @file     tts_xfyun.h
 * @brief    科大讯飞 WebSocket TTS 驱动
 *
 * 鉴权密钥 + 业务参数在 init 时以 tts_xfyun_config_t 结构体一次性传入，
 * 组件内不硬编码任何密钥。
 */
#ifndef TTS_XFYUN_H
#define TTS_XFYUN_H

#include "tts_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 讯飞驱动配置 —— init 时以结构体一次性传入
 */
typedef struct {
    const char *appid;       /*!< 讯飞 APPID */
    const char *api_key;     /*!< 讯飞 API Key */
    const char *api_secret;  /*!< 讯飞 API Secret */
    const char *vcn;         /*!< 发音人，如 "xiaoyan"；nullptr 用默认 "xiaoyan" */
    int speed;               /*!< 语速 0-100；<=0 用默认 40 */
    int volume;              /*!< 音量 0-100；<=0 用默认 50 */
} tts_xfyun_config_t;

/* 驱动实例（由 tts_hub 内部引用） */
extern const tts_driver_t tts_drv_xfyun;

#ifdef __cplusplus
}
#endif

#endif /* TTS_XFYUN_H */
