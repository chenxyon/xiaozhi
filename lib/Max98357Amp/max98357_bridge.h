/**
 * @file     max98357_bridge.h
 * @brief    Max98357Amp C API（给 C 代码调用的扁平接口）
 *
 * 内部持有一个全局单例 Max98357Amp，通过 extern "C" 暴露给 C 调用方。
 * 若只想用功放组件（不用 Inmp441Mic），只 include 这个头 + 链接
 * max98357_bridge.cpp 即可，不会拉入任何麦克风代码。
 */
#ifndef MAX98357_BRIDGE_H
#define MAX98357_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "max98357_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 初始化 ---------- */

/**
 * @brief 初始化功放（I2S 端口由调用方决定，可与麦克风共用）
 * @param amp_pins  接线配置（port/bclk/lrc/din/sample_rate 等）
 */
esp_err_t max98357_init(const max98357_pins_t *amp_pins);

/* ---------- 播放 ---------- */
esp_err_t max98357_play(const int16_t *data, size_t bytes, uint32_t timeout_ms);
esp_err_t max98357_play_beep(void);
esp_err_t max98357_play_tone(float freq, uint32_t duration_ms);
esp_err_t max98357_play_melody(const max98357_note_t *melody);

/* ---------- TTS sink ---------- */
esp_err_t max98357_tts_sink(const int16_t *pcm, size_t bytes, void *ctx);

/* ---------- 状态 ---------- */
bool max98357_is_ready(void);

/* ---------- 音量 ---------- */
void max98357_set_volume(int pct);
int  max98357_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* MAX98357_BRIDGE_H */
