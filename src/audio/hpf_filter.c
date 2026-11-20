/**
 * @file     hpf_filter.c
 * @brief    二阶 Butterworth 高通滤波器实现（纯算法）
 *
 * 修改：2026-11-20 从 board.c 拆出
 */
#include <math.h>

#include "hpf_filter.h"

void hpf_reset(hpf_filter_t *f, float fc_hz, float rate_hz)
{
    const float w0 = 2.0f * (float)M_PI * fc_hz / rate_hz;
    const float cosw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);  /* Q = 1/sqrt(2) */
    const float a0 = 1.0f + alpha;

    f->b0 =  (1.0f + cosw) / 2.0f / a0;
    f->b1 = -(1.0f + cosw) / a0;
    f->b2 =  (1.0f + cosw) / 2.0f / a0;
    f->a1 = (-2.0f * cosw) / a0;
    f->a2 =  (1.0f - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

void hpf_apply(hpf_filter_t *f, const int16_t *in, int16_t *out, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        float x = (float)in[i];
        float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
                - f->a1 * f->y1 - f->a2 * f->y2;
        f->x2 = f->x1;
        f->x1 = x;
        f->y2 = f->y1;
        f->y1 = y;
        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        out[i] = (int16_t)y;
    }
}
