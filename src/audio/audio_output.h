/**
 * @file     audio_output.h
 * @brief    TTS PCM 输出槽（HPF 滤波 → MAX98357 功放）
 *
 * 设计（规则 2-SRP）：输出适配层，把 tts_hub 的 PCM 回调接口
 *   和 max98357_tts_sink 的驱动接口衔接起来，中间串联高通滤波。
 *
 * 开关：AUDIO_HPF_ENABLE=0 即完全旁路滤波，恢复直通行为。
 *      AUDIO_AMP_ENABLE=0 直接丢弃 PCM，不出声。
 *
 * 修改：2026-11-20 从 board.c 拆出（规则 4-SRP：board.c 职责过载）
 */
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化输出槽
 *
 * @param fc_hz      高通截止频率（Hz），0 = 不开滤波
 * @param rate_hz    音频采样率（Hz）
 * @param chunk_size 每块处理的最大样本数（应与 tts_hub 分块一致）
 */
void audio_output_init(float fc_hz, float rate_hz, size_t chunk_size);

/**
 * @brief  tts_hub 注册用的 PCM sink（2 参数无返回值）
 *
 * 直接作为 board_init_devices 里 tts_hub_prepare 的 sink 参数注册。
 */
void audio_output_tts_sink(const int16_t *pcm, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_OUTPUT_H */
