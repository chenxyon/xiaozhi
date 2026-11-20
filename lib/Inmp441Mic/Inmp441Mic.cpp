/**
 * @file     Inmp441Mic.cpp
 * @brief    INMP441 麦克风实现 — 直读共用 I2S 端口
 */
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s.h"

#include "Inmp441Mic.hpp"

static const char *TAG = "MIC";

/* ---------- 采集任务（thunk + impl）---------- */
void Inmp441Mic::mic_task_thunk(void *arg)
{
    reinterpret_cast<Inmp441Mic *>(arg)->mic_task_impl();
}

void Inmp441Mic::mic_task_impl()
{
    int16_t block[MIC_BLOCK_SAMPLES];

    static constexpr bool DIAG = true;   /* 诊断日志开关：打印音量级别，确认麦克风在采声 */
    static uint32_t s_diag_cnt;
    static double   s_diag_acc;
    static int32_t  s_diag_min = 32767;
    static int32_t  s_diag_max = -32768;

    while (m_running) {
        size_t rd = 0;
        int32_t tmp[MIC_BLOCK_SAMPLES];

        /* 直读共用 I2S（全双工模式必须持续排空接收 DMA） */
        esp_err_t ret = i2s_read((i2s_port_t)m_pins.i2s_port,
                                 tmp, MIC_BLOCK_SAMPLES * sizeof(int32_t),
                                 &rd, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || rd == 0) {
            static bool s_timeout_reported = false;
            if (!s_timeout_reported) {
                s_timeout_reported = true;
                ESP_LOGE(TAG, "i2s_read 超时！I2S 时钟可能未运行或接线错误 (ret=%s rd=%d)",
                         esp_err_to_name(ret), (int)rd);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        /* 读到数据了，标记 true（一旦为 true 就不再回退）*/
        if (!m_has_data) {
            m_has_data = true;
            ESP_LOGI(TAG, "✅ 首次采到 I2S 数据，MIC 硬件链路正常");
        }

        uint32_t n = rd / sizeof(int32_t);
        if (n > MIC_BLOCK_SAMPLES) n = MIC_BLOCK_SAMPLES;

        /* 右移 12 还原 24-bit → 16-bit + 电平统计 */
        bool blk_active = false;
        for (uint32_t i = 0; i < n; i++) {
            int32_t v = tmp[i] >> 12;
            if (v > INT16_MAX) v = INT16_MAX;
            else if (v < -INT16_MAX) v = -INT16_MAX;
            block[i] = (int16_t)v;

            s_diag_acc += (double)v * (double)v;
            if (v < s_diag_min) s_diag_min = v;
            if (v > s_diag_max) s_diag_max = v;
            if (v < QUIET_MIN || v > QUIET_MAX) blk_active = true;
        }
        if (blk_active) m_last_voice_us = esp_timer_get_time();
        s_diag_cnt += n;
        if (s_diag_cnt >= 16000) {
#if DIAG
            /* 每秒打一条音量诊断 */
            double rms = sqrt(s_diag_acc / 16000.0);
            ESP_LOGI(TAG, "诊断: RMS=%.1f 峰值=%ld~%ld 活动块=%s",
                     rms, (long)s_diag_min, (long)s_diag_max,
                     blk_active ? "YES" : "no");
#else
            (void)blk_active;
#endif
            s_diag_acc  = 0; s_diag_cnt  = 0;
            s_diag_min  = 32767; s_diag_max = -32768;
        }

        /* 入循环缓冲 */
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        for (uint32_t i = 0; i < n; i++) {
            if (m_count == MIC_BUF_SAMPLES) { m_tail++; }
            else { m_count++; }
            m_ring[m_head & MASK] = block[i];
            m_head++;
        }
        xSemaphoreGive(m_mutex);
    }
    vTaskDelete(NULL);
}

/* ---------- 公开接口 ---------- */

esp_err_t Inmp441Mic::init(const inmp441_pins_t &pins, const inmp441_cfg_t &cfg)
{
    if (m_running) return ESP_OK;

    m_pins = pins;
    m_cfg  = cfg;
    if (m_cfg.channels == 0) m_cfg.channels = 1;

    m_head = m_tail = m_count = 0;
    m_mutex = xSemaphoreCreateBinary();
    if (m_mutex == nullptr) {
        ESP_LOGE(TAG, "互斥锁创建失败");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(m_mutex);

    m_running = true;
    if (xTaskCreate(mic_task_thunk, "mic", 4096, this, 5, &m_task) != pdPASS) {
        m_running = false;
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
        ESP_LOGE(TAG, "采集任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "INMP441 就绪 -> SCK=%d WS=%d SD=%d 共用 I2S=%d 采样率=%u",
             (int)pins.sck, (int)pins.ws, (int)pins.sd, (int)pins.i2s_port,
             (unsigned)cfg.sample_rate_hz);
    return ESP_OK;
}

void Inmp441Mic::stop()
{
    if (!m_running) return;

    m_running = false;
    for (int i = 0; i < 50 && m_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (m_mutex != nullptr) {
        vSemaphoreDelete(m_mutex);
        m_mutex = nullptr;
    }
    m_task = nullptr;
    ESP_LOGI(TAG, "INMP441 采集已停止");
}

esp_err_t Inmp441Mic::read(int16_t *buf, size_t bytes, size_t *out, uint32_t timeout_ms)
{
    if (!m_running || m_mutex == nullptr || buf == nullptr ||
        bytes == 0 || (bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t need = bytes / sizeof(int16_t);
    size_t got  = 0;
    bool timed_out = false;

    while (got < need) {
        TickType_t start = xTaskGetTickCount();
        for (;;) {
            xSemaphoreTake(m_mutex, portMAX_DELAY);
            bool has_data = (m_count > 0);
            xSemaphoreGive(m_mutex);
            if (has_data) break;
            if (timeout_ms == 0) { timed_out = true; break; }
            if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) {
                timed_out = true; break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (timed_out) break;

        xSemaphoreTake(m_mutex, portMAX_DELAY);
        uint32_t take_n = (m_count < need - got) ? m_count : (uint32_t)(need - got);
        for (uint32_t i = 0; i < take_n; i++) {
            buf[got + i] = m_ring[m_tail & MASK];
            m_tail++;
        }
        m_count -= take_n;
        got += take_n;
        xSemaphoreGive(m_mutex);
    }

    if (out != nullptr) *out = got * sizeof(int16_t);
    return (got == need) ? ESP_OK : ESP_ERR_TIMEOUT;
}

int64_t Inmp441Mic::last_voice_us() { return m_last_voice_us; }
