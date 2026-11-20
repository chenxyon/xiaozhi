/**
 * @file     Max98357Amp.hpp
 * @brief    MAX98357A I2S Class-D 功放 — C++ 类组件
 *
 * 接线/配置在 init 时一次性传入（结构体），之后只通过成员方法操作。
 * 所有声音（TTS/beep/tone/melody）走同一出口，PCM 缩放实现软件音量。
 *
 * 与 ESP-IDF 原版差异：旧 I2S API（i2s_driver_install + i2s_write/read）
 *                    替代新 API（i2s_new_channel + i2s_chan_handle_t）。
 */
#ifndef MAX98357_AMP_HPP
#define MAX98357_AMP_HPP

#include <stddef.h>
#include <stdint.h>

#include "driver/i2s.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "max98357_types.h"

/* ---------- 类 ---------- */

class Max98357Amp {
public:
    Max98357Amp() = default;
    ~Max98357Amp() = default;

    /**
     * @brief  初始化功放（接线以结构体一次性传入）
     * @param pins 引脚/参数结构体
     */
    esp_err_t init(const max98357_pins_t &pins);

    /* ---- 播放接口 ---- */
    esp_err_t play(const int16_t *data, size_t bytes, uint32_t timeout_ms = UINT32_MAX);
    esp_err_t mic_read(int16_t *data, size_t bytes, size_t *written, uint32_t timeout_ms = 200);
    esp_err_t play_beep();
    esp_err_t play_tone(float freq, uint32_t duration_ms = 0);
    esp_err_t play_melody(const max98357_note_t *melody);
    esp_err_t tts_sink(const int16_t *pcm, size_t bytes, void *ctx = nullptr);

    /* ---- 音量（0~100，默认 80） ---- */
    void set_volume(int pct);
    int  get_volume();

    /* ---- 状态 ---- */
    bool is_ready() const { return m_ready; }
    int  port() const     { return (int)m_port; }

private:
    i2s_port_t      m_port       = I2S_NUM_0;
    bool            m_ready      = false;
    TaskHandle_t    m_task       = nullptr;
    bool            m_selftest   = false;
    float           m_phase      = 0.0f;
    int             m_volume_pct = 80;
    max98357_pins_t m_cfg        = {};

    esp_err_t generate_sine(int16_t *out_buf, uint32_t *samples,
                            float freq, uint32_t duration_ms, int16_t amp);
};

#endif /* MAX98357_AMP_HPP */
