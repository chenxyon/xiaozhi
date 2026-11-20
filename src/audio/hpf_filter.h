/**
 * @file     hpf_filter.h
 * @brief    二阶 Butterworth 高通滤波器（纯算法，设备无关）
 *
 * 设计（规则 1 分层架构）：算法模块独立，不依赖任何硬件或业务组件。
 * 任何 PCM 音频流都可以用它滤直流偏置 / 超低频。
 *
 * 实现：RBJ cookbook 二阶高通，Q=1/√2，fc 可在 reset 时指定。
 *       300Hz 以上衰减 <0.2dB，50Hz 约 -9dB、100Hz 约 -1.5dB。
 *
 * 能力边界：数字滤波只能消除数字域的超低频，
 *   供电纹波耦合到 DAC 之后的嗡声，任何数字滤波都消不掉。
 *
 * 单调用者：状态用静态存储即可，无需加锁。
 *
 * 修改：2026-11-20 从 board.c 拆出（规则 4-SRP：board.c 职责过载）
 */
#ifndef HPF_FILTER_H
#define HPF_FILTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b0, b1, b2;   /* 分子系数（已除以 a0） */
    float a1, a2;       /* 分母系数（已除以 a0，差分方程中取减号） */
    float x1, x2, y1, y2;
} hpf_filter_t;

/**
 * @brief  计算系数 + 清零历史状态
 *
 * @param f           滤波器实例
 * @param fc_hz       截止频率（Hz）
 * @param rate_hz     采样率（Hz）
 */
void hpf_reset(hpf_filter_t *f, float fc_hz, float rate_hz);

/**
 * @brief  对 PCM 逐样本滤波
 *
 * @param f      已 reset 的滤波器实例
 * @param in     输入（只读，可能指向 flash 常量片段）
 * @param out    输出（可与 in 同一地址做 in-place）
 * @param samples 样本数
 */
void hpf_apply(hpf_filter_t *f, const int16_t *in, int16_t *out, size_t samples);

#ifdef __cplusplus
}
#endif

#endif /* HPF_FILTER_H */
