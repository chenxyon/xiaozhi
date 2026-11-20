/**
 * @file     audio_output.c
 * @brief    TTS PCM 输出槽实现
 *
 * 内部串联：HPF → chunk → max98357_tts_sink
 *
 * 修改：2026-11-20 从 board.c 拆出
 */
#include <string.h>
#include "esp_log.h"

#include "driver/gpio.h"   /* AUDIO_AMP_ENABLE 等宏可能在这里，但实际由 platformio.ini build_flags 控制 */

#include "max98357_bridge.h"
#include "hpf_filter.h"

#include "audio_output.h"

static const char *TAG = "audio_out";

/* ===== 编译期开关（与 board.c 拆出前一致）===== */
#ifndef AUDIO_HPF_ENABLE
#define AUDIO_HPF_ENABLE  1
#endif

static hpf_filter_t s_hpf;
static int16_t     *s_hpf_buf;      /* chunk 临时缓冲 */
static size_t       s_chunk_samps;  /* 每块最大样本数 */

void audio_output_init(float fc_hz, float rate_hz, size_t chunk_size)
{
    s_chunk_samps = chunk_size;
    s_hpf_buf     = (int16_t *)malloc(chunk_size * sizeof(int16_t));
    if (!s_hpf_buf) {
        /* 回退：直接用静态大数组（代价是占更多 RAM）*/
        static int16_t s_stATIC_BUF[4096];
        s_hpf_buf = s_stATIC_BUF;
        s_chunk_samps = sizeof(s_stATIC_BUF) / sizeof(s_stATIC_BUF[0]);
        ESP_LOGW(TAG, "heap 分配失败，回退静态缓冲 (%zu samples)", s_chunk_samps);
    }
#if AUDIO_HPF_ENABLE
    hpf_reset(&s_hpf, fc_hz, rate_hz);
    ESP_LOGI(TAG, "HPF 高通已启用: fc=%.1fHz rate=%.0fHz", fc_hz, rate_hz);
#else
    ESP_LOGI(TAG, "HPF 高通已旁路");
#endif
}

/* tts_hub 的 PCM 输出槽是 2 参数无返回值；
 * max98357_tts_sink 是 3 参数返回 esp_err_t。这里做一层薄适配，
 * 避免为了对齐签名去改动 max98357 组件。
 *
 * 单调用者：tts_hub 异步播放有 busy 互斥（同一时刻只有一个播放任务），
 * 故滤波器状态与 s_hpf_buf 用静态存储即可，无需加锁。 */
void audio_output_tts_sink(const int16_t *pcm, size_t bytes)
{
#if !AUDIO_AMP_ENABLE
    (void)pcm; (void)bytes;
    return;   /* 功放禁用，直接丢弃 PCM，不出声 */
#else
    if ((bytes % sizeof(int16_t)) != 0) {
        (void)max98357_tts_sink(pcm, bytes, NULL);  /* 异常长度：原样保底 */
        return;
    }

#if AUDIO_HPF_ENABLE
    size_t total = bytes / sizeof(int16_t);
    for (size_t off = 0; off < total; off += s_chunk_samps) {
        size_t n = total - off;
        if (n > s_chunk_samps) n = s_chunk_samps;
        hpf_apply(&s_hpf, pcm + off, s_hpf_buf, n);
        (void)max98357_tts_sink(s_hpf_buf, n * sizeof(int16_t), NULL);
    }
#else
    (void)max98357_tts_sink(pcm, bytes, NULL);
#endif
#endif
}
