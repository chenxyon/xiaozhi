/**
 * @file     display_iface.h
 * @brief    屏幕显示统一接口 —— 纯虚基类 IDisplay
 *
 * 所有屏幕驱动组件 (OledU8g2 单色屏 / TftTfspi 彩色屏 / ...)
 * 都继承这个基类, 保证跨组件接口一致, 不会漂移。
 *
 * 使用示例:
 *   #include "display_iface.h"
 *   #include "OledU8g2.hpp"      // 或未来的 TftTfspi.hpp
 *
 *   IDisplay *disp = new OledU8g2();   // 单色屏
 *   // IDisplay *disp = new TftTfspi();   // 彩色屏 (未来)
 *   disp->init(OLED_PANEL_SSD1306_128X32, &i2c_pins);
 *   disp->show("Hello", 0, 0);
 *   disp->refresh();
 *   delete disp;
 *
 * 这个文件属于 DisplayIface 接口组件:
 *   https://github.com/your-name/DisplayIface
 */
#ifndef DISPLAY_IFACE_H
#define DISPLAY_IFACE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "oled_types.h"

#ifdef __cplusplus

/* =========================================================
 *  纯虚基类: 统一所有屏幕驱动的公共接口
 *
 *  子类必实现:  show / draw_icon / draw_rect / draw_line
 *               / clear / refresh / screen / is_ready / panel
 *  彩色屏专属:  set_color / draw_image (单色屏返回 NOT_SUPPORTED)
 * ========================================================= */

class IDisplay {
public:
    virtual ~IDisplay() = default;

    /* ---------- 初始化 ---------- */

    /**
     * @brief  初始化屏幕
     * @param panel  面板类型枚举 (oled_panel_t)
     * @param pins   引脚结构体指针 (根据 panel 类型选 oled_i2c_pins_t 或 oled_spi_pins_t)
     *               void* 设计让单一签名兼容所有屏, 内部按 panel 校验字段
     */
    virtual esp_err_t init(oled_panel_t panel, const void *pins) = 0;
    virtual esp_err_t deinit() { return ESP_OK; }

    /* ---------- 基础绘制 (像素坐标, x/y 子类负责边界检查) ---------- */

    /** 显示文字 (UTF-8, 支持 GB2312 中文 + ASCII) */
    virtual esp_err_t show(const char *text, uint16_t x, uint16_t y) = 0;

    /** 绘制 oled_icon_t 图标 (png_to_bitmap.py 输出格式, 组件自动 clip + min_screen 过滤) */
    virtual esp_err_t draw_icon(const oled_icon_t *icon, uint16_t x, uint16_t y) = 0;

    /** 矩形 (fill=false=空心 drawRect, fill=true=实心 drawBox) */
    virtual esp_err_t draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fill = false) = 0;

    /** 直线 */
    virtual esp_err_t draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) = 0;

    /** 清除整个屏幕缓冲 (调用后需 refresh()) */
    virtual esp_err_t clear() = 0;

    /** 将缓冲刷新到屏幕 (墨水屏刷新慢, 注意调用频率) */
    virtual esp_err_t refresh() = 0;

    /* ---------- 彩色屏专属 (单色屏返回 ESP_ERR_NOT_SUPPORTED, 不崩) ---------- */

    /** 设置前景/背景色 (RGB565), 单色屏忽略 */
    virtual esp_err_t set_color(uint16_t fg_rgb565, uint16_t bg_rgb565 = 0xFFFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /** 绘制 RGB565 图像 (w*h 像素, 从 (x,y) 开始铺) */
    virtual esp_err_t draw_image(const uint16_t *rgb565, uint16_t w, uint16_t h,
                                  uint16_t x, uint16_t y) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* ---------- 查询 ---------- */

    /** 当前屏幕分辨率/字体信息 (init 后自动填充) */
    virtual const oled_screen_info_t& screen() const = 0;

    /** 是否初始化成功 */
    virtual bool is_ready() const = 0;

    /** 当前面板类型 */
    virtual oled_panel_t panel() const = 0;
};

#endif /* __cplusplus */

#endif /* DISPLAY_IFACE_H */
