/**
 * @file     app_ai.h
 * @brief    AI 大模型接入（天翼云 eaichat，OpenAI 兼容 /chat/completions）
 *
 * 能力：
 *  - 联网后自检：请求一次模型，成功播报「AI模型连接成功。」；
 *  - 语音问答：把一句提问发给大模型，取回复文字，交 TTS 播报后恢复监听。
 */
#ifndef APP_AI_H
#define APP_AI_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 联网成功（GOT_IP）时调用：启动一次连接自检
 *
 * 等待 TTS 空闲后请求模型；成功播报「AI模型连接成功。」，失败播报失败音。
 */
void app_ai_on_online(void);

/**
 * @brief 轻量连通性探测：仅对 AI 主机做一次 TCP connect（不建立 TLS 会话）
 *
 * 用途：开机自检"是否连上 AI 服务"，避免在它后面紧跟 TTS 时因两个 TLS 会话
 *       叠加而中途断流。真正的模型对话仍走 app_ai_chat()。
 */
esp_err_t app_ai_probe(void);

/**
 * @brief 同步对话：把 question 发给大模型，取回回复正文
 * @param question 用户提问（UTF-8）
 * @param out      输出缓冲（回复正文）
 * @param out_sz   out 容量
 * @return ESP_OK 成功（out 内为回复），否则失败
 */
esp_err_t app_ai_chat(const char *question, char *out, size_t out_sz);

/**
 * @brief 语音问答异步任务：AI 对话 → TTS 播报 → 恢复监听
 *
 * 内部会 strdup(question)，调用方无需保活；应在调用前 kws_hub_pause()。
 */
void app_ai_qa_async(const char *question);

#ifdef __cplusplus
}
#endif

#endif /* APP_AI_H */
