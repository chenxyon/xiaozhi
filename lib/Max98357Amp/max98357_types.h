/**
 * @file     max98357_types.h
 * @brief    MAX98357 音频组件共享结构体（C/C++ 共用）
 *
 * 接线/配置的结构体定义集中在这里，C 头、C++ 头、bridge 层都引用它，
 * 避免重复定义和 typedef 冲突。
 */
#ifndef MAX98357_TYPES_H
#define MAX98357_TYPES_H

#include <stdint.h>
#include "driver/gpio.h"

/* ---------- Max98357Amp ---------- */

typedef struct {
    int          port;
    gpio_num_t   bclk;
    gpio_num_t   lrc;
    gpio_num_t   din;
    gpio_num_t   mic_din;
    uint32_t     sample_rate_hz;
    uint32_t     bclk_div;
    uint32_t     dma_desc_num;
    uint32_t     dma_frame_num;
    uint32_t     beep_freq_hz;
    uint32_t     beep_ms;
    uint32_t     task_period_ms;
} max98357_pins_t;

typedef struct {
    float  freq;
    uint32_t duration_ms;
} max98357_note_t;

/* 音符常量（C/C++ 都能用） */
#define NOTE_C4  261.63f
#define NOTE_D4  293.66f
#define NOTE_E4  329.63f
#define NOTE_F4  349.23f
#define NOTE_G4  392.00f
#define NOTE_A4  440.00f
#define NOTE_B4  493.88f
#define NOTE_C5  523.25f

#define BEAT_QUARTER  250u
#define BEAT_EIGHTH   125u
#define BEAT_HALF     500u
#define BEAT_WHOLE   1000u

#endif /* MAX98357_TYPES_H */
