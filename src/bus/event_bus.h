/**
 * @file     event_bus.h
 * @brief    轻量事件总线（发布-订阅，规则 9）
 *
 * 设计模式：发布-订阅 PubSub
 * 何时引入：一个事件要触发多个模块动作，且谁需要知道以后可能变。
 *
 * 特点：
 *   - 阻塞式（单线程 ESP32 够用，publish 时同步调所有订阅者）
 *   - 全局单例（全局只有一个 event_bus）
 *   - 每个事件类型最多 8 个订阅者（可调 MAX_HANDLERS_PER_EVENT）
 *   - 发布者不知道谁在订阅，订阅者不知道谁在发布
 *
 * 修改：2026-11-20 新建（规则 9）
 */
#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 事件类型 =====
 * 命名格式：entity.action
 * 新增事件在这里追加，不要随便改已有值（可能影响持久化或协议）。*/
typedef enum {
    EVT_NONE = 0,

    /* ---- 网络 ---- */
    EVT_WIFI_GOT_IP,        // WiFi 连上，data = NULL
    EVT_WIFI_DISCONNECTED,  // WiFi 断开，data = NULL

    /* ---- 唤醒/识别 ---- */
    EVT_WAKE_WORD,          // 唤醒词命中，data = (const char*)唤醒原文
    EVT_USER_TEXT,          // 收到用户指令文本，data = (const char*)指令

    /* ---- AI ---- */
    EVT_AI_REPLY_READY,     // AI 回复回来了，data = (const char*)回复正文

    /* ---- TTS ---- */
    EVT_TTS_STARTED,        // TTS 开始播报，data = NULL
    EVT_TTS_DONE,           // TTS 播报完毕，data = NULL

    /* ---- 上限 ---- */
    EVT_MAX,
} app_event_t;

/* ===== 订阅者/发布者接口 ===== */
typedef void (*event_handler_t)(app_event_t ev, void *data, void *ctx);

/** 每个事件最多几个订阅者（改大了占更多 RAM）*/
#ifndef EVENT_BUS_MAX_HANDLERS_PER_EVENT
#define EVENT_BUS_MAX_HANDLERS_PER_EVENT  8
#endif

/**
 * @brief  初始化事件总线（只需调一次）
 */
void event_bus_init(void);

/**
 * @brief  订阅事件
 *
 * @param ev     事件类型
 * @param h      处理函数
 * @param ctx    用户上下文（publish 时透传给 h）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 该事件订阅已满；ESP_ERR_INVALID_ARG 参数错
 */
esp_err_t event_bus_subscribe(app_event_t ev, event_handler_t h, void *ctx);

/**
 * @brief  取消订阅（h 必须和 subscribe 时传入的指针完全一致）
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 没找到这个 handler
 */
esp_err_t event_bus_unsubscribe(app_event_t ev, event_handler_t h);

/**
 * @brief  发布事件（阻塞式，同步调所有订阅者）
 *
 * @param ev   事件类型
 * @param data 事件携带的数据指针（内容由发布者和订阅者约定类型）
 *
 * 注意：data 必须在所有订阅者返回之前保持有效。
 *      不要在订阅者里调 event_bus_publish（会递归）。
 */
void event_bus_publish(app_event_t ev, void *data);

/**
 * @brief  当前状态（诊断用）
 */
bool event_bus_inited(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
