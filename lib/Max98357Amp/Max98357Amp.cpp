/**
 * @file     Max98357Amp.cpp
 * @brief    MAX98357A 功放实现 — 旧 I2S API（i2s_driver_install + i2s_write）
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2s.h"

#include "Max98357Amp.hpp"

static const char *TAG = "AUDIO";

esp_err_t Max98357Amp::init(const max98357_pins_t &pins)
{
    if (m_ready) return ESP_OK;
    m_cfg  = pins;
    m_port = (i2s_port_t)pins.port;

    i2s_config_t i2s_cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate          = (int)pins.sample_rate_hz,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT,   /* INMP441 在 LRC=0(右半帧)输出数据，ESP32 需只在右半帧采样 */
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL2 | ESP_INTR_FLAG_IRAM,
        .dma_buf_count        = (int)pins.dma_desc_num,
        .dma_buf_len          = (int)pins.dma_frame_num,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
        .fixed_mclk           = 0,
    };

    ESP_ERROR_CHECK(i2s_driver_install(m_port, &i2s_cfg, 0, NULL));

    i2s_pin_config_t pin_cfg = {
        .bck_io_num   = pins.bclk,
        .ws_io_num    = pins.lrc,
        .data_out_num = pins.din,
        .data_in_num  = pins.mic_din,
    };
    ESP_ERROR_CHECK(i2s_set_pin(m_port, &pin_cfg));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(m_port));

    m_ready = true;
    ESP_LOGI(TAG, "初始化完成 -> BCLK=%d LRC=%d 功放=%d 麦克风=%d 采样率=%u",
             (int)pins.bclk, (int)pins.lrc, (int)pins.din, (int)pins.mic_din,
             (unsigned)pins.sample_rate_hz);
    return ESP_OK;
}

esp_err_t Max98357Amp::play(const int16_t *data, size_t bytes, uint32_t timeout_ms)
{
    if (!m_ready || data == NULL || bytes == 0 || (bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t samples = bytes / sizeof(int16_t);
    int32_t *buf = (int32_t *)malloc(samples * sizeof(int32_t));
    if (buf == NULL) return ESP_ERR_NO_MEM;

    /* PCM 缩放：gain = volume_pct / 100.0f，clip 到 [-32768, 32767] */
    float gain = (float)m_volume_pct / 100.0f;
    for (size_t i = 0; i < samples; i++) {
        float v = (float)data[i] * gain;
        if (v > 32767.0f) v = 32767.0f;
        else if (v < -32768.0f) v = -32768.0f;
        buf[i] = (int32_t)((int16_t)v) << 16;
    }

    size_t written = 0;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                         : pdMS_TO_TICKS(timeout_ms);
    esp_err_t err = i2s_write(m_port, buf, samples * sizeof(int32_t), &written, ticks);
    free(buf);

    if (err != ESP_OK) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            ESP_LOGE(TAG, "I2S 写失败: %s", esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t Max98357Amp::mic_read(int16_t *data, size_t bytes, size_t *written, uint32_t timeout_ms)
{
    if (!m_ready || data == NULL || bytes == 0 || (bytes % sizeof(int16_t)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t samples = bytes / sizeof(int16_t);
    int32_t *tmp = (int32_t *)malloc(samples * sizeof(int32_t));
    if (tmp == NULL) return ESP_ERR_NO_MEM;

    size_t rd = 0;
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                         : pdMS_TO_TICKS(timeout_ms);
    esp_err_t ret = i2s_read(m_port, tmp, samples * sizeof(int32_t), &rd, ticks);
    size_t n = rd / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        int32_t v = tmp[i] >> 12;
        if (v > INT16_MAX) v = INT16_MAX;
        else if (v < -INT16_MAX) v = -INT16_MAX;
        data[i] = (int16_t)v;
    }
    free(tmp);

    if (written != NULL) *written = n * sizeof(int16_t);
    return ret;
}

esp_err_t Max98357Amp::play_beep()
{
    if (!m_ready) return ESP_ERR_INVALID_STATE;

    const uint32_t half_samples = m_cfg.sample_rate_hz / (2 * m_cfg.beep_freq_hz);
    const int      samples      = (int)(m_cfg.sample_rate_hz * m_cfg.beep_ms / 1000);
    const int16_t  amp          = 16384;

    if (half_samples == 0 || samples <= 0) return ESP_ERR_INVALID_ARG;

    int16_t *beep = (int16_t *)malloc((size_t)samples * sizeof(int16_t));
    if (beep == NULL) return ESP_ERR_NO_MEM;

    for (int i = 0; i < samples; i++) {
        beep[i] = (i % (int)half_samples < (int)(half_samples / 2)) ? amp : -amp;
    }

    esp_err_t ret = play(beep, (size_t)samples * sizeof(int16_t), UINT32_MAX);
    free(beep);
    ESP_LOGI(TAG, "播放 %u Hz 方波 (%u ms)",
             (unsigned)m_cfg.beep_freq_hz, (unsigned)m_cfg.beep_ms);
    return ret;
}

esp_err_t Max98357Amp::generate_sine(int16_t *out_buf, uint32_t *samples,
                                     float freq, uint32_t duration_ms, int16_t amp)
{
    if (samples == NULL || duration_ms == 0) return ESP_ERR_INVALID_ARG;
    uint32_t total = (uint32_t)m_cfg.sample_rate_hz * duration_ms / 1000;
    if (total == 0) return ESP_ERR_INVALID_ARG;
    *samples = total;
    if (out_buf == NULL) return ESP_OK;
    if (freq <= 0.0f) { memset(out_buf, 0, total * sizeof(int16_t)); return ESP_OK; }

    float phase_inc = 2.0f * (float)M_PI * freq / (float)m_cfg.sample_rate_hz;
    for (uint32_t i = 0; i < total; i++) {
        out_buf[i] = (int16_t)(amp * sinf(m_phase));
        m_phase += phase_inc;
        if (m_phase >= 2.0f * (float)M_PI) m_phase -= 2.0f * (float)M_PI;
    }
    return ESP_OK;
}

esp_err_t Max98357Amp::play_tone(float freq, uint32_t duration_ms)
{
    if (!m_ready) return ESP_ERR_INVALID_STATE;
    if (duration_ms == 0) duration_ms = m_cfg.beep_ms;

    uint32_t samples = 0;
    esp_err_t err = generate_sine(nullptr, &samples, freq, duration_ms, 16384);
    if (err != ESP_OK) return err;

    int16_t *buf = (int16_t *)malloc(samples * sizeof(int16_t));
    if (buf == NULL) return ESP_ERR_NO_MEM;

    err = generate_sine(buf, &samples, freq, duration_ms, 16384);
    if (err == ESP_OK) err = play(buf, samples * sizeof(int16_t), UINT32_MAX);
    free(buf);
    return err;
}

esp_err_t Max98357Amp::play_melody(const max98357_note_t *melody)
{
    if (!m_ready || melody == NULL) return ESP_ERR_INVALID_ARG;
    for (uint32_t i = 0; melody[i].duration_ms != 0 || melody[i].freq != 0; i++) {
        if (melody[i].freq <= 0.0f && melody[i].duration_ms == 0) break;
        esp_err_t err = play_tone(melody[i].freq, melody[i].duration_ms);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t Max98357Amp::tts_sink(const int16_t *pcm, size_t bytes, void *ctx)
{
    (void)ctx;
    return play(pcm, bytes, 5000);   /* TTS PCM 播放超时 5s */
}

void Max98357Amp::set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    m_volume_pct = pct;
    ESP_LOGI(TAG, "音量设置: %d%%", pct);
}

int Max98357Amp::get_volume() { return m_volume_pct; }
