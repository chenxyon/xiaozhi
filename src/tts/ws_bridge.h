/**
 * @file     ws_bridge.h
 * @brief    ArduinoWebsockets → C 桥接层
 *
 * 作用：把 C++ 的 ArduinoWebsockets 封装成纯 C 接口，
 *       让 tts_xfyun.c（C 文件）不用直接接触 C++ 头文件。
 *
 * 修改：2026-11-16 新建（esp_websocket_client 在 Arduino Core 里缺失）
 */
#ifndef WS_BRIDGE_H
#define WS_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明句柄 */
typedef struct ws_bridge ws_bridge_t;

/** 事件类型（与 ArduinoWebsockets 对齐） */
typedef enum {
    WS_BRIDGE_EVENT_OPENED,
    WS_BRIDGE_EVENT_CLOSED,
    WS_BRIDGE_EVENT_ERROR,
} ws_bridge_event_t;

/** 文本消息回调 */
typedef void (*ws_bridge_on_msg_t)(const char *data, size_t len, void *ctx);

/** 事件回调 */
typedef void (*ws_bridge_on_evt_t)(ws_bridge_event_t event, void *ctx);

/**
 * @brief  创建 WS 客户端（不连接）
 */
ws_bridge_t *ws_bridge_create(void);

/**
 * @brief  注册回调（必须在 connect 之前）
 */
void ws_bridge_set_callbacks(ws_bridge_t *wb,
                             ws_bridge_on_msg_t  on_msg,
                             ws_bridge_on_evt_t on_evt,
                             void *ctx);

/**
 * @brief  连接到 wss://... URL（阻塞，内部会 poll 直到连上/超时）
 * @param url      完整 URL，如 "wss://tts-api.xfyun.cn/v2/tts?...鉴权..."
 * @param timeout_ms 连接超时
 * @return true 连接已建立
 */
bool ws_bridge_connect(ws_bridge_t *wb, const char *url, int timeout_ms);

/**
 * @brief  发送文本帧
 */
bool ws_bridge_send_text(ws_bridge_t *wb, const char *data, size_t len);

/**
 * @brief  发送二进制帧（实时语音转写等需要发纯音频 binary message）
 */
bool ws_bridge_send_binary(ws_bridge_t *wb, const uint8_t *data, size_t len);

/**
 * @brief  驱动事件循环（需在 while 里周期性调用）
 */
void ws_bridge_poll(ws_bridge_t *wb);

/**
 * @brief  主动关闭
 */
void ws_bridge_close(ws_bridge_t *wb);

/**
 * @brief  销毁
 */
void ws_bridge_destroy(ws_bridge_t *wb);

/**
 * @brief  是否已连接
 */
bool ws_bridge_connected(ws_bridge_t *wb);

#ifdef __cplusplus
}
#endif

#endif /* WS_BRIDGE_H */
