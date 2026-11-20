/**
 * @file     Inmp441Mic.hpp
 * @brief    INMP441 I2S 数字 MEMS 麦克风 — C++ 类组件
 *
 * 接线/配置在 init 时一次性传入（结构体）。
 * 本麦克风与 Max98357Amp 共用 I2S 端口（全双工），init 时需要传入共用端口号。
 * 采集任务通过 i2s_read() 直读，不依赖功放类实例。
 */
#ifndef INMP441_MIC_HPP
#define INMP441_MIC_HPP

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "inmp441_types.h"

/* ---------- 类 ---------- */

class Inmp441Mic {
public:
    Inmp441Mic() = default;
    ~Inmp441Mic() = default;

    /**
     * @brief  初始化麦克风（接线+配置以结构体一次性传入）
     * @param pins 引脚+共用 I2S 端口号
     * @param cfg  采集参数
     */
    esp_err_t init(const inmp441_pins_t &pins, const inmp441_cfg_t &cfg);

    void stop();

    esp_err_t read(int16_t *buf, size_t bytes, size_t *out, uint32_t timeout_ms);
    bool      is_running() const { return m_running; }
    bool      has_data()   const { return m_has_data; }
    int64_t   last_voice_us();

private:
    static constexpr uint32_t MIC_BUF_SAMPLES   = 16384;
    static constexpr uint32_t MIC_BLOCK_SAMPLES = 512;
    static constexpr int32_t  QUIET_MIN = -660;
    static constexpr int32_t  QUIET_MAX =  600;

    TaskHandle_t      m_task    = nullptr;
    SemaphoreHandle_t m_mutex   = nullptr;
    volatile bool     m_running = false;
    volatile bool     m_has_data = false;
    inmp441_cfg_t     m_cfg     = {};
    inmp441_pins_t    m_pins    = {};

    int16_t   m_ring[MIC_BUF_SAMPLES];
    uint32_t  m_head  = 0;
    uint32_t  m_tail  = 0;
    uint32_t  m_count = 0;
    static constexpr uint32_t MASK = MIC_BUF_SAMPLES - 1;

    volatile int64_t m_last_voice_us = 0;

    static void mic_task_thunk(void *arg);
    void        mic_task_impl();
};

#endif /* INMP441_MIC_HPP */
