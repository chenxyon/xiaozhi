/**
 * @file     main_fsm.h
 * @brief    主交互闭环状态机（规则 8：状态模式 State）
 *
 * 何时引入：
 *   当你写 `bool prev_xxx; if (xxx != prev_xxx)` 这种状态变化检测代码时 → 就是状态模式信号。
 *   原 main.cpp 有 3 处 bool 状态散在 loop/wake_cb/qa_task，本模块集中管理。
 *
 * 状态转换图：
 *                      EVT_WIFI_GOT_IP
 *   FSM_IDLE ──────────────────────────→ FSM_LISTENING
 *      ↑                                    │
 *      │  WiFi 断开                         │ EVT_WAKE_WORD
 *      │ (任意状态回 IDLE)                  ↓
 *      │                               FSM_AWAKE
 *      │                                    │
 *      │                                    │ EVT_USER_TEXT
 *      │                                    ↓
 *      │                               FSM_PROCESSING
 *      │                                    │
 *      │                                    │ EVT_AI_REPLY_READY
 *      │                                    ↓
 *      │                               FSM_SPEAKING
 *      │                                    │
 *      └──────────── FSM_LISTENING ←───────┘
 *                   EVT_TTS_DONE
 *
 * 每个状态进入时自动做 on_enter 动作：
 *   FSM_LISTENING → kws_hub_start() 恢复监听
 *   FSM_AWAKE     → kws_hub_pause() + 播唤醒回复
 *   FSM_PROCESSING→ 播 processing 提示音 + 调 AI
 *   FSM_SPEAKING  → kws_hub_pause()（避免录到自己）
 *
 * 修改：2026-11-20 新建（规则 8）
 */
#ifndef MAIN_FSM_H
#define MAIN_FSM_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSM_IDLE = 0,          // 启动中/未联网
    FSM_LISTENING,         // 待机监听中
    FSM_AWAKE,             // 已唤醒，等待指令
    FSM_PROCESSING,        // AI 思考中
    FSM_SPEAKING,          // TTS 播报中
    FSM_MAX,
} app_state_t;

/**
 * @brief  初始化状态机 + 订阅事件总线
 *
 * 会订阅：
 *   EVT_WIFI_GOT_IP / EVT_WIFI_DISCONNECTED
 *   EVT_WAKE_WORD / EVT_USER_TEXT
 *   EVT_AI_REPLY_READY / EVT_TTS_DONE
 */
esp_err_t main_fsm_init(void);

/**
 * @brief  当前状态
 */
app_state_t main_fsm_state(void);

/**
 * @brief  状态名（诊断用）
 */
const char *main_fsm_state_name(app_state_t s);

/**
 * @brief  强制转换（调试/特殊场景用）
 *
 * 通常不用，让事件总线驱动自动转换即可。
 */
void main_fsm_force(app_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_FSM_H */
