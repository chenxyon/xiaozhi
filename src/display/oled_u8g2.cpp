/**
 * @file     oled_u8g2.cpp
 * @brief    薄包装层 —— 把旧 C API 桥接到 lib/OledU8g2 C++ 组件
 *
 * 历史: 2026-11-19 旧实现 (U8g2 直接操作, include Wire/U8g2lib.h)
 *       2026-11-20 重构: 内部调 g_oled 组件实例
 *
 * 设计: 调用方代码 (app_wake.c / main.cpp / board.c) 保持不动,
 *       内部全部转发给 g_oled.init/show/draw_icon/refresh.
 *       组件实例 g_oled 定义在此文件 (C++ 编译单元).
 */
#include <cstring>

#include <Arduino.h>
#include "esp_log.h"

#include "oled_u8g2.h"           /* 旧 C API 声明 */
#include "board.h"               /* 也声明了 extern g_oled (但此文件定义它) */

/* ====== C++ 组件头文件 (只能在 .cpp 里 include) ====== */
#include "OledU8g2.hpp"

static const char *TAG = "oled_u8g2";   /* 规则 4-8 */

/* ====== 唯一组件实例 (定义在 .cpp, 所有 .c/.cpp 都能 extern 拿到) ====== */
OledU8g2 g_oled;

/* ====== WiFi 状态图标 (组件不硬编码, 调用方也不自己画 —— 包装层统一处理) ======
 * 12×12 bitmap, U8g2/TFT_eSPI 通用 (行优先, 每行 2 字节) */
static const uint8_t s_wifi_icon_12x12[] = {
    0x00, 0x00,  // row 0
    0x1F, 0xC0,  // row 1
    0x20, 0x40,  // row 2
    0x40, 0x20,  // row 3
    0x80, 0x10,  // row 4
    0x40, 0x20,  // row 5
    0x3F, 0xC0,  // row 6
    0x1F, 0xC0,  // row 7
    0x00, 0x00,  // row 8
    0x06, 0x00,  // row 9
    0x00, 0x00,  // row 10
    0x00, 0x00,  // row 11
};

static const uint8_t s_wifi_off_icon_12x12[] = {
    0x00, 0x00,  // row 0
    0x80, 0x10,  // row 1
    0x40, 0x20,  // row 2
    0x20, 0x40,  // row 3
    0x10, 0x80,  // row 4
    0x09, 0x00,  // row 5
    0x09, 0x00,  // row 6
    0x10, 0x80,  // row 7
    0x20, 0x40,  // row 8
    0x40, 0x20,  // row 9
    0x80, 0x10,  // row 10
    0x00, 0x00,  // row 11
};

/* 包装层内部缓存状态 */
static bool        s_wifi_connected = false;
static const uint8_t *s_wifi_icon_ptr = s_wifi_off_icon_12x12;

/* ====== oled_icon_t 便捷包装 (让 draw_icon 调 g_oled.draw_icon) ====== */
static const oled_icon_t s_wifi_on_oled_icon = {
    .data = s_wifi_icon_12x12, .w = 12, .h = 12, .min_screen = 0
};
static const oled_icon_t s_wifi_off_oled_icon = {
    .data = s_wifi_off_icon_12x12, .w = 12, .h = 12, .min_screen = 0
};

/* =========================================================
 *  旧 C API —— 包装层转发
 * ========================================================= */

esp_err_t oled_init(const oled_i2c_pins_t *pins)
{
    if (!pins) {
        ESP_LOGE(TAG, "pins 为空");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "包装层: 调 g_oled.init() panel=SSD1306_128X32");

    oled_i2c_pins_t cpp_pins = *pins;
    esp_err_t err = g_oled.init(OLED_PANEL_SSD1306_128X32, &cpp_pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "g_oled.init 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "包装层: OLED 就绪");
    return ESP_OK;
}

bool oled_is_ready(void)
{
    return g_oled.is_ready();
}

void oled_set_wifi(bool connected)
{
    s_wifi_connected = connected;
    s_wifi_icon_ptr = connected ? s_wifi_icon_12x12 : s_wifi_off_icon_12x12;
}

void oled_show_line(const char *s, uint8_t row)
{
    if (!g_oled.is_ready() || !s) return;

    /* row 范围 0..1 (12px 中文字体 + 4px 间距 = 16px 行距, 32px 屏只能放 2 行)*/
    if (row > 1) row = 1;

    /* ⚠️ 不再 g_oled.clear() 全清 buffer —— 时钟 timer 每秒在 row 1 画时间,
     *    全清会把时钟抹掉。改成只清本 row 对应的 128×16 局部矩形。*/
    g_oled.draw_rect(0, row * 16, 128, 16, true);   /* fill=true → 涂黑 */

    g_oled.show(s, 0, row * 16);

    /* WiFi 图标固定画在 row 右下角 */
    const oled_icon_t *ic = s_wifi_connected ? &s_wifi_on_oled_icon : &s_wifi_off_oled_icon;
    g_oled.draw_icon(ic, 116, row * 16);

    /* 刷到屏幕 */
    g_oled.refresh();
}
