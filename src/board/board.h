/**
 * @file     board.h
 * @brief    板级装配层（规则 1 分层架构）
 *
 * 职责：
 *   1. 本板全部引脚定义（唯一可信来源）
 *   2. 设备初始化编排（OLED / Flash / I2S 功放 / 麦克风 / NVS / TTS hub）
 *   3. 密钥加载（NVS 优先，规则 4-1）
 *
 * ============================================================
 *   初始化顺序（规则 5）—— main.cpp setup() 必须严格按此顺序调
 * ============================================================
 *
 *   1. board_init_devices()      —— 板级设备（OLED/Flash/I2S/功放/麦/NVS/tts_hub_prepare）
 *      前置条件：无
 *      内部依赖：secret_store_init() 最先；max98357_init() 在 inmp441_init() 之前（I2S 端口注入）
 *
 *   2. event_bus_init()          —— 事件总线（规则 9）
 *      前置条件：无
 *
 *   3. main_fsm_init()           —— 状态机订阅事件总线（规则 8）
 *      前置条件：event_bus_init() 已完成
 *
 *   4. kws_hub_set_callback + kws_hub_init + kws_hub_start  —— 唤醒引擎
 *      前置条件：main_fsm_init() 已完成（因为 KWS_EV_WAKE/TEXT 回调会 publish 事件）
 *
 *   5. WiFi 连接 → wifi_connect_or_setup() → s_on_wifi_connected()
 *      前置条件：上面 1-4 都做完（启动播报 + 事件总线就绪）
 *      s_on_wifi_connected() 内部顺序：configTime → SNTP → board_init_tts → publish EVT_WIFI_GOT_IP
 *
 *   6. board_init_tts()          —— TTS 云驱动
 *      ⚠️ 前置条件：WiFi 连上 + configTime() SNTP 对时（提前调会 ESP_ERROR_CHECK abort！）
 *      内部依赖：secret_store 已 init（读 NVS 密钥）
 *
 * ============================================================
 *
 * 修改：2026-10-03 新建（从 main.c 拆出引脚定义与设备初始化）
 * 修改：2026-11-18 拆出 board_init_tts（从 board_init_devices 移出，修复启动 crash）
 * 修改：2026-11-20 重写初始化顺序注释（规则 5）
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#include "esp_err.h"

#include "oled_u8g2.h"    /* oled_i2c_pins_t 定义 */
#include "max98357_bridge.h"
#include "w25q128.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====== C++ 组件全局实例 (供包装层 + main.cpp 直接用) ====== */
#ifdef __cplusplus
class OledU8g2;
extern OledU8g2 g_oled;
#endif

/**
 * @brief  打印本板全部引脚分配表
 */
void board_log_pinout(void);

/**
 * @brief  初始化全部外设（OLED / Flash / I2S / 功放 / 麦克风 / NVS / tts_hub_prepare）
 *
 * 前置条件：无
 * 后置条件：NVS 已 init；tts_hub 已 prepare（离线片段可用，云 TTS 还不能用）
 */
void board_init_devices(void);

/**
 * @brief  取功放配置（供 audio_output 等模块取回采样率/引脚）
 */
const max98357_pins_t *board_audio_config(void);

/**
 * @brief  TTS 云驱动初始化（讯飞鉴权）
 *
 * ⚠️ 前置条件：
 *   - WiFi 已连接 + configTime() SNTP 对时（讯飞鉴权依赖时间戳）
 *   - 提前调用会 ESP_ERROR_CHECK abort！
 *
 * 密钥来源：优先 NVS（namespace="xiaozhi"，key="xfyun_appid" 等），
 *           NVS 无值时回退到 board.c 内置默认值。
 */
void board_init_tts(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_H */
