/**
 * @file     app_mic_switch.h
 * @brief    麦克风开关：轻触按键（GPIO38）翻转麦克风监听，LED（GPIO21）指示状态
 *
 * 行为：LED 亮 = 麦克风开（正在监听），LED 灭 = 麦克风关。
 *       按键「按下并抬起」一次即翻转：亮→灭（关麦）、灭→亮（开麦）。
 */
#ifndef APP_MIC_SWITCH_H
#define APP_MIC_SWITCH_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化按键与 LED，并创建按键扫描任务
 *
 * 说明：默认开机为「麦克风开、LED 亮」。按键任务会持续检测
 *       「按下→抬起」并翻转状态。仅应在 board_init_devices() 之后调用。
 */
esp_err_t app_mic_switch_init(void);

/** @brief 当前麦克风是否处于开启（监听）状态 */
bool app_mic_switch_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MIC_SWITCH_H */
