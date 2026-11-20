/**
 * @file     max98357_bridge.cpp
 * @brief    Max98357Amp — C API → C++ 类 包装实现
 *
 * 只负责把扁平函数调用转发给内部的 Max98357Amp 单例。
 * 若项目只用功放，单独编译这个文件即可，不会拉入麦克风代码。
 */
#include "max98357_bridge.h"
#include "Max98357Amp.hpp"

static Max98357Amp s_amp;

extern "C" {

/* ---------- 初始化 ---------- */

esp_err_t max98357_init(const max98357_pins_t *amp_pins)
{
    if (amp_pins == nullptr) return ESP_ERR_INVALID_ARG;
    return s_amp.init(*amp_pins);
}

/* ---------- 播放 ---------- */

esp_err_t max98357_play(const int16_t *data, size_t bytes, uint32_t timeout_ms) {
    return s_amp.play(data, bytes, timeout_ms);
}

esp_err_t max98357_play_beep() { return s_amp.play_beep(); }

esp_err_t max98357_play_tone(float freq, uint32_t duration_ms) {
    return s_amp.play_tone(freq, duration_ms);
}

esp_err_t max98357_play_melody(const max98357_note_t *melody) {
    return s_amp.play_melody(melody);
}

/* ---------- TTS sink ---------- */

esp_err_t max98357_tts_sink(const int16_t *pcm, size_t bytes, void *ctx) {
    return s_amp.tts_sink(pcm, bytes, ctx);
}

/* ---------- 状态 ---------- */

bool max98357_is_ready() { return s_amp.is_ready(); }

/* ---------- 音量 ---------- */

void max98357_set_volume(int pct) { s_amp.set_volume(pct); }
int  max98357_get_volume()        { return s_amp.get_volume(); }

}   /* extern "C" */
