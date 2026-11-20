/**
 * @file     oled_u8g2.h
 * @brief    旧 C API 声明 —— 实际实现已移至 oled_u8g2.cpp 包装层
 *
 * 设计: 此头文件只负责对外暴露 C API (供 board.c / main.c / app_wake.c 调用),
 *       底层通过 g_oled C++ 组件实例转发。
 *       oled_i2c_pins_t 直接用 DisplayIface/oled_types.h 的定义, 避免重复声明。
 */
#ifndef OLED_U8G2_H
#define OLED_U8G2_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "oled_types.h"     /* oled_i2c_pins_t 等来自 DisplayIface 组件 */

#ifdef __cplusplus
extern "C" {
#endif

/* 注意: oled_i2c_pins_t 已在 oled_types.h 里定义, 这里不再重复 */

esp_err_t oled_init(const oled_i2c_pins_t *pins);
void      oled_show_line(const char *s, uint8_t row);
bool      oled_is_ready(void);
void      oled_set_wifi(bool connected);

#ifdef __cplusplus
}
#endif

#endif /* OLED_U8G2_H */
