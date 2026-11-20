/**
 * @file     inmp441_bridge.cpp
 * @brief    Inmp441Mic — C API → C++ 类 包装实现
 *
 * 只负责把扁平函数调用转发给内部的 Inmp441Mic 单例。
 * 若项目只用麦克风，单独编译这个文件即可，不会拉入功放代码。
 */
#include "inmp441_bridge.h"
#include "Inmp441Mic.hpp"

static Inmp441Mic s_mic;

extern "C" {

/* ---------- 初始化 ---------- */

esp_err_t inmp441_init(const inmp441_pins_t *mic_pins, const inmp441_cfg_t *mic_cfg)
{
    if (mic_pins == nullptr || mic_cfg == nullptr) return ESP_ERR_INVALID_ARG;
    return s_mic.init(*mic_pins, *mic_cfg);
}

void inmp441_deinit() { s_mic.stop(); }

/* ---------- 采集 ---------- */

esp_err_t inmp441_read(int16_t *buf, size_t bytes, size_t *out, uint32_t timeout_ms) {
    return s_mic.read(buf, bytes, out, timeout_ms);
}

/* ---------- 状态 ---------- */

bool    inmp441_is_running()    { return s_mic.is_running(); }
bool    inmp441_has_data()      { return s_mic.has_data(); }
int64_t inmp441_last_voice_us() { return s_mic.last_voice_us(); }

}   /* extern "C" */
