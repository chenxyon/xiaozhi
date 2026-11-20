/**
 * @file     oled_types.h
 * @brief    屏幕显示共用枚举与结构体 (C/C++ 通用)
 *
 * 所有屏幕驱动 (OledU8g2 单色屏 / TftTfspi 彩色屏...) 都依赖这个文件。
 * 面板类型、引脚配置、图标结构体、UI 导航页面定义都集中在这里,
 * 保证跨组件接口一致, 不会漂移。
 *
 * 这个文件属于 DisplayIface 接口组件:
 *   https://github.com/your-name/DisplayIface
 */
#ifndef OLED_TYPES_H
#define OLED_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================
 *  面板类型枚举
 * ========================================================= */

typedef enum {
    /* ---- 0.91" OLED ---- */
    OLED_PANEL_SSD1306_128X32 = 0,       /* 0.91" OLED, I²C (U8g2 支持) */

    /* ---- 0.96" OLED ---- */
    OLED_PANEL_SSD1306_128X64,           /* 0.96" OLED, I²C/SPI (U8g2 支持) */
    OLED_PANEL_SH1106_128X64,            /* 0.96" SH1106 变种, I²C/SPI (U8g2 支持) */
    OLED_PANEL_SH1107_64X128,            /* 0.96"/1.3" SH1107 变种, I²C/SPI (U8g2 支持) */

    /* ---- 墨水屏 (e-Paper, 单色, SPI) ----
     * 墨水屏刷新慢 (0.3~2s), 调用方注意控制 sendBuffer 频率 */
    OLED_PANEL_UC1701_DOGS102_64X128,    /* 1.02" 墨水屏, SPI 4W (U8g2 支持) */
    OLED_PANEL_SSD1606_172X72,           /* 1.54" 墨水屏, SPI 4W (U8g2 支持) */
    OLED_PANEL_SSD1607_200X200,          /* 2.13" 墨水屏, SPI 4W (U8g2 支持) */
    OLED_PANEL_SSD1607_240X240,          /* 2.9" 墨水屏, SPI 4W (U8g2 近似驱动) */

    /* ---- 2.4" / 2.8" 单色 LCD ---- */
    OLED_PANEL_SSD1322_240X128,          /* 2.4" 单色 LCD, SPI 4W (U8g2 支持) */

    /* ---- 2.4" / 2.8" 彩色 TFT (U8g2 不支持, 未来 TftTfspi) ---- */
    OLED_PANEL_ST7789_240X240,           /* 2.4" TFT, 4线 SPI */
    OLED_PANEL_ST7789_240X320,           /* 2.4"/2.8" TFT, 4线 SPI */
    OLED_PANEL_ILI9341_240X320,          /* 2.4"/2.8" TFT, 4线 SPI */
    OLED_PANEL_GC9A01_240X240,           /* 2.4" 圆形 TFT, 4线 SPI */

    /* ---- 5" 大屏 (U8g2 不支持, 未来 TftTfspi) ---- */
    OLED_PANEL_ILI9488_320X480,          /* 5" TFT, 4线 SPI (真彩) */
    OLED_PANEL_HX8357_320X480,           /* 5" 另一款, SPI (真彩) */

    OLED_PANEL_MAX
} oled_panel_t;

/* =========================================================
 *  引脚配置结构体 (根据 panel 类型选择使用哪一个)
 * ========================================================= */

/* I²C 屏 (SSD1306 0.91"/0.96", SH1106) */
typedef struct {
    gpio_num_t scl;           /* I²C SCL */
    gpio_num_t sda;           /* I²C SDA */
    uint8_t    i2c_addr;      /* 0x3C 或 0x3D */
    uint32_t   freq_hz;       /* I²C 时钟频率, 0=默认 400kHz */
} oled_i2c_pins_t;

/* SPI 屏 (彩色 TFT 2.4"/2.8", 墨水屏, 单色 LCD) */
typedef struct {
    gpio_num_t mosi;          /* SPI MOSI (SDA) */
    gpio_num_t sclk;          /* SPI SCK */
    gpio_num_t cs;            /* 片选 */
    gpio_num_t dc;            /* 数据/命令 */
    gpio_num_t rst;           /* 复位, -1=不接 */
    gpio_num_t backlight;     /* 背光, -1=不接 */
    int8_t     spi_host;      /* -1=默认 SPI_HOST, 或 SPI2_HOST/SPI3_HOST */
} oled_spi_pins_t;

/* =========================================================
 *  图标结构体 (png_to_bitmap.py 输出格式)
 * ========================================================= */

/**
 * 所有图标/bitmap 统一用这个结构体描述。
 * 数据由 png_to_bitmap.py 生成, 组件内部只认指针。
 */
typedef struct {
    const uint8_t *data;       /* bitmap 字节数组, U8g2 drawBitmap 格式 */
    uint16_t       w;          /* 宽度 (像素) */
    uint16_t       h;          /* 高度 (像素) */
    uint16_t       min_screen; /* 最小屏幕宽度才能显示 (0=任何屏都行) */
} oled_icon_t;

/* =========================================================
 *  屏幕运行时信息 (init 后自动填充)
 * ========================================================= */

typedef struct {
    uint16_t width;            /* 实际分辨率宽 */
    uint16_t height;           /* 实际分辨率高 */
    uint8_t  font_h;           /* 中文字体高度 (默认 12) */
    uint8_t  font_w;           /* 汉字等宽宽度 (默认 12) */
} oled_screen_info_t;

/* =========================================================
 *  对齐枚举 (跨屏布局方便)
 * ========================================================= */

typedef enum {
    OLED_ALIGN_NONE = 0,
    OLED_ALIGN_TOP_LEFT,
    OLED_ALIGN_TOP_RIGHT,
    OLED_ALIGN_TOP_CENTER,
    OLED_ALIGN_BOTTOM_LEFT,
    OLED_ALIGN_BOTTOM_RIGHT,
    OLED_ALIGN_BOTTOM_CENTER,
    OLED_ALIGN_MIDDLE,
} oled_align_t;

/* =========================================================
 *  按键类型枚举 (6 个物理按键)
 * ========================================================= */

typedef enum {
    OLED_KEY_NONE = 0,
    OLED_KEY_UP,
    OLED_KEY_DOWN,
    OLED_KEY_LEFT,
    OLED_KEY_RIGHT,
    OLED_KEY_OK,               /* 确认/选中 */
    OLED_KEY_CANCEL,           /* 取消/返回 */
    OLED_KEY_MAX
} oled_key_t;

/* =========================================================
 *  UI 导航: 菜单项 / 页面 / 回调
 * ========================================================= */

/* 单个菜单项 (图标网格里的一个格子) */
typedef struct oled_menu_item {
    const char               *uri;       /* 唯一标识, 如 "/settings" */
    const char               *title;     /* 显示文字, 如 "设置" */
    const oled_icon_t        *icon;      /* 图标 (可传 NULL) */
    uint16_t                  x, y;      /* 坐标 (网格布局时组件自动算, 可传 0) */
    uint16_t                  w, h;      /* 格子大小 (可传 0) */
} oled_menu_item_t;

/* 按键回调: 返回 true=我处理了, false=让组件走默认网格导航 */
typedef bool (*oled_key_cb_t)(const struct oled_page *page, oled_key_t key,
                              uint8_t *focus_index, void *user_ctx);

/* 渲染回调: 自定义页面自己画整个页面 */
typedef void (*oled_render_cb_t)(const struct oled_page *page, uint8_t focus_index,
                                  void *user_ctx);

/* 一个可导航页面 */
typedef struct oled_page {
    const char               *uri;           /* 本页面 URI, 如 "/" 或 "/settings" */
    const char               *title;         /* 页面标题 (顶部显示) */
    const oled_menu_item_t   *items;         /* 菜单项数组 */
    uint8_t                   item_count;     /* 菜单项数量 */
    oled_key_cb_t             on_key;         /* 自定义按键处理器 (NULL=默认网格导航) */
    oled_render_cb_t          on_render;      /* 自定义渲染 (NULL=默认网格渲染) */
    void                     *user_ctx;       /* 回调私有数据, 原样传回 */
} oled_page_t;

/* =========================================================
 *  状态栏配置
 * ========================================================= */

typedef struct {
    bool     enable;
    uint16_t x, y;             /* 位置 (像素) */
    uint16_t height;           /* 状态栏高度 */
    bool     show_title;       /* 左侧显示页面标题 */
    bool     show_wifi;        /* WiFi 状态图标 */
    bool     show_battery;     /* 电池图标 */
    bool     show_clock;       /* 时间文字 */
} oled_status_bar_cfg_t;

/* =========================================================
 *  触摸事件 (预留, U8g2 暂不实现)
 * ========================================================= */

typedef enum {
    OLED_TOUCH_NONE = 0,
    OLED_TOUCH_DOWN,
    OLED_TOUCH_UP,
    OLED_TOUCH_MOVE,
    OLED_TOUCH_SWIPE_LEFT,
    OLED_TOUCH_SWIPE_RIGHT,
    OLED_TOUCH_SWIPE_UP,
    OLED_TOUCH_SWIPE_DOWN,
} oled_touch_evt_t;

typedef struct {
    oled_touch_evt_t type;
    uint16_t         x, y;
    uint32_t         duration_ms;
} oled_touch_event_t;

typedef void (*oled_touch_cb_t)(const oled_touch_event_t *evt, void *user_ctx);

/* =========================================================
 *  图标网格配置 (draw_grid 用)
 * ========================================================= */

typedef struct {
    uint8_t  cols;              /* 列数, 0=组件自动算 */
    uint8_t  rows;              /* 行数, 0=组件自动算 */
    uint16_t cell_w;            /* 格子宽 (像素) */
    uint16_t cell_h;            /* 格子高 (像素) */
    uint16_t gap;               /* 格子间距 */
    uint8_t  icon_x_offset;     /* 图标在格子内的 x 偏移 */
    uint8_t  icon_y_offset;     /* 图标在格子内的 y 偏移 */
} oled_grid_cfg_t;

/* 单图标 + 选中状态 */
typedef struct {
    const oled_icon_t *icon;
    const char         *label;
    uint16_t            x, y;
    uint16_t            w, h;
    bool                selected;
} oled_grid_item_t;

/* =========================================================
 *  页面指示器 (底部小圆点或 "1/3" 文字)
 * ========================================================= */

typedef struct {
    uint8_t  current_page;
    uint8_t  total_pages;
    const char **page_names;
} oled_page_ctrl_t;

#ifdef __cplusplus
}
#endif

#endif /* OLED_TYPES_H */
