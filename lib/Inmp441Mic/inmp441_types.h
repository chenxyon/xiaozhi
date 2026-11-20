/**
 * @file     inmp441_types.h
 * @brief    INMP441 麦克风组件共享结构体（C/C++ 共用）
 */
#ifndef INMP441_TYPES_H
#define INMP441_TYPES_H

#include <stdint.h>
#include "driver/gpio.h"

/* ---------- Inmp441Mic ---------- */

typedef struct {
    int          i2s_port;     /* 共用 I2S 端口号，调用方负责与功放对齐 */
    gpio_num_t   sck;
    gpio_num_t   ws;
    gpio_num_t   sd;
} inmp441_pins_t;

typedef struct {
    uint32_t     sample_rate_hz;
    uint16_t     channels;
    uint16_t     bit_depth;     /* 固定 16，兼容 ESP-SR */
} inmp441_cfg_t;

#endif /* INMP441_TYPES_H */
