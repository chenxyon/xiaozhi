/**
 * @file     inmp441_bridge.h
 * @brief    Inmp441Mic C API（给 C 代码调用的扁平接口）
 *
 * 内部持有一个全局单例 Inmp441Mic，通过 extern "C" 暴露给 C 调用方。
 * 若只想用麦克风组件（不用 Max98357Amp），只 include 这个头 + 链接
 * inmp441_bridge.cpp 即可，不会拉入任何功放代码。
 */
#ifndef INMP441_BRIDGE_H
#define INMP441_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "inmp441_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 初始化 ---------- */

/**
 * @brief 初始化麦克风（I2S 端口由调用方决定，通常与功放共用）
 * @param mic_pins 接线配置（i2s_port/sck/ws/sd）
 * @param mic_cfg  采集参数
 */
esp_err_t inmp441_init(const inmp441_pins_t *mic_pins, const inmp441_cfg_t *mic_cfg);

void inmp441_deinit(void);

/* ---------- 采集 ---------- */

esp_err_t inmp441_read(int16_t *buf, size_t bytes, size_t *out, uint32_t timeout_ms);

/* ---------- 状态 ---------- */

bool    inmp441_is_running(void);
bool    inmp441_has_data(void);
int64_t inmp441_last_voice_us(void);

#ifdef __cplusplus
}
#endif

#endif /* INMP441_BRIDGE_H */
