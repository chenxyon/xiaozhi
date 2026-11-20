/**
 * @file     app_cmd.h
 * @brief    本地指令解析 —— 唤醒后用户说的话，优先在这里匹配
 *
 * 匹配成功 → 直接执行，不走大模型（低延迟、省流量）。
 * 匹配失败 → 交给 app_wake 原有流程（app_dir / app_ai）。
 *
 * 当前支持：音量控制（调大/调小/静音/直接数字/中文数字）
 */
#ifndef APP_CMD_H
#define APP_CMD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  尝试把用户输入解析成本地指令
 *
 * @param text  唤醒后用户说的原话（讯飞 ASR 转成的文本）
 * @return true = 已匹配并启动执行；false = 不是本地指令，调用方继续走 AI
 */
bool app_cmd_match(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* APP_CMD_H */
