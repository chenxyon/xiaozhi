/**
 * @file     OledU8g2.hpp
 * @brief    多面板通用 OLED/TFT 显示类 —— 基于 U8g2
 *
 * 统一接口适配 SSD1306/SH1106 (I²C) 和 ST7789/ILI9341 (SPI)。
 * init(panel, pins) 时传入面板类型 + 对应引脚结构体指针 (void*),
 * 组件内部自动 new 对应 U8g2 子类。
 *
 * UI 导航/按键交互接口已预留 (register_page / navigate_to / send_key ...),
 * 实现见 cpp，当前 U8g2 单色屏只实现网格导航核心, 触摸回调为空壳。
 *
 * 图标统一用 oled_icon_t 结构体, 由 png_to_bitmap.py 生成。
 */
#ifndef OLED_U8G2_HPP
#define OLED_U8G2_HPP

#include <Arduino.h>       /* U8g2 依赖 Arduino core */
#include <Wire.h>          /* I²C */
#include <SPI.h>           /* SPI 全局对象, init_spi 要用 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "U8g2lib.h"

#include "oled_types.h"
#include "display_iface.h"   /* 统一接口 IDisplay, OledU8g2 继承它 */

/* =========================================================
 *  C++ 类 —— 继承统一接口 IDisplay
 *
 *  项目代码可以:
 *    IDisplay *disp = new OledU8g2();
 *    disp->init(...); disp->show(...);  // 统一接口
 *
 *  或直接:
 *    OledU8g2 oled;
 *    oled.init(...); oled.show(...);   // 具体类特有方法也能用
 * ========================================================= */

class OledU8g2 : public IDisplay {
public:
    OledU8g2() = default;
    ~OledU8g2() = default;

    /* ---------- 初始化 ---------- */

    /**
     * @brief  初始化屏幕
     * @param panel  面板类型枚举
     * @param pins   引脚结构体指针 (根据 panel 类型选 oled_i2c_pins_t 或 oled_spi_pins_t)
     *               void* 设计让单一签名兼容所有屏, 内部按 panel 校验字段
     */
    esp_err_t init(oled_panel_t panel, const void *pins);
    esp_err_t deinit();

    /* =========================================================
     *  基础绘制 API (像素坐标, x/y 自动边界检查)
     * ========================================================= */

    /**
     * @brief  显示一行文字 (UTF-8, 支持 GB2312 中文 + ASCII)
     * @param text  字符串
     * @param x     左上角像素 x
     * @param y     顶部像素 y (字体位置 TOP)
     */
    esp_err_t show(const char *text, uint16_t x, uint16_t y);

    /**
     * @brief  显示文字 + 自动对齐 (OLED_ALIGN_*)
     */
    esp_err_t show_aligned(const char *text, oled_align_t align, uint16_t margin = 0);

    /**
     * @brief  绘制一个 oled_icon_t 图标 (png_to_bitmap.py 输出格式)
     * @param icon  图标指针 (内部读 icon->w, icon->h, min_screen 自动过滤)
     */
    esp_err_t draw_icon(const oled_icon_t *icon, uint16_t x, uint16_t y);

    /**
     * @brief  绘制图标 + 自动对齐
     */
    esp_err_t draw_icon_aligned(const oled_icon_t *icon, oled_align_t align, uint16_t margin = 0);

    /* ---- 图形基础 ---- */
    esp_err_t draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fill = false);
    esp_err_t draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    esp_err_t clear();

    /* ---- 手动刷新 (U8g2 内部) ---- */
    esp_err_t refresh();

    /* =========================================================
     *  时钟显示 —— 支持多行独立时钟槽 + 对齐 + 独立刷新频率
     *
     *  设计: 内部 1 个 esp_timer (默认 1s tick), 回调里遍历所有 active slot
     *        每个 slot 可独立 strftime fmt + y 坐标 + 对齐 + 刷新间隔
     *  用法:
     *    oled.set_clock_slot(0, true, "%H:%M:%S",      14, OLED_ALIGN_TOP_CENTER, 1000);
     *    oled.set_clock_slot(1, true, "%Y-%m-%d 周%u", 24, OLED_ALIGN_TOP_CENTER, 60000);
     *    oled.enable_clock(true);
     *
     *  也可以只用 1 个 slot (屏幕小的时候):
     *    oled.set_clock_slot(0, true, "%Y-%m-%d %H:%M:%S", 16, OLED_ALIGN_TOP_CENTER);
     *    oled.enable_clock(true);
     * ========================================================= */

    /**
     * @brief  注册/更新一个时钟槽
     * @param index      槽位索引 0..OLED_MAX_CLOCKS-1 (当前 MAX=2)
     * @param active     true=启用这个槽, false=禁用
     * @param fmt        strftime 格式串 (时间/日期自由组合)
     * @param y          顶部像素 y (组件内部自己算 x 对齐)
     * @param align      水平对齐 (OLED_ALIGN_TOP_CENTER/RIGHT/LEFT 或 NONE 用精确 x)
     * @param refresh_ms 独立刷新间隔 (默认 1000ms=1s; 日期建议 60000ms=1min)
     */
    esp_err_t set_clock_slot(uint8_t index, bool active, const char *fmt,
                              uint16_t y,
                              oled_align_t align = OLED_ALIGN_TOP_CENTER,
                              uint32_t refresh_ms = 1000);

    /**
     * @brief  启用/停止所有时钟槽
     * @param on true=启用 (内部启动 1s tick esp_timer), false=停止
     */
    esp_err_t enable_clock(bool on);
    esp_err_t disable_clock();

    /**
     * @brief  立即手动触发一次时钟刷新 (调试/紧急情况下用)
     */
    void       refresh_clock_now();

    /* =========================================================
     *  状态栏 (预留: 组件只提供画的能力, 不管理业务状态)
     *  调用方自己决定: draw_icon(&wifi_icon, x, y) + draw_icon(&battery_icon, x, y)
     * ========================================================= */

    esp_err_t set_status_bar(const oled_status_bar_cfg_t &cfg);

    /* =========================================================
     *  UI 导航 (预留接口, 核心网格导航已实现)
     * ========================================================= */

    /**
     * @brief  注册一个页面 (最多 16 个)
     */
    esp_err_t register_page(const oled_page_t &page);

    /**
     * @brief  跳转到指定 URI 的页面 (压入页面栈, 最多 8 层)
     */
    esp_err_t navigate_to(const char *uri);

    /**
     * @brief  返回上一页 (弹出页面栈)
     */
    esp_err_t navigate_back();

    /**
     * @brief  发送一个按键事件 (外部按键中断/轮询后调用)
     */
    esp_err_t send_key(oled_key_t key);

    /**
     * @brief  手动设置焦点索引
     */
    esp_err_t set_focus(uint8_t index);

    /* ---- 图标网格绘制 (预留, 当前实现基础版) ---- */
    esp_err_t draw_grid(const oled_grid_item_t *items, uint8_t count,
                        const oled_grid_cfg_t &cfg,
                        uint16_t start_x = 0, uint16_t start_y = 0);

    /* ---- 页面指示器 (底部小圆点, 预留) ---- */
    esp_err_t draw_page_indicator(const oled_page_ctrl_t &ctrl,
                                   uint16_t x, uint16_t y);

    /* =========================================================
     *  触摸 (预留, U8g2 暂不实现)
     * ========================================================= */

    esp_err_t register_touch_callback(oled_touch_cb_t cb, void *user_ctx = nullptr);

    /* =========================================================
     *  查询 API
     * ========================================================= */

    bool                        is_ready()      const { return m_ready; }
    const oled_screen_info_t&   screen()        const { return m_screen; }
    oled_panel_t                panel()         const { return m_panel; }
    const oled_page_t*          current_page()  const;
    uint8_t                     focus_index()    const { return m_focus_index; }

private:
    /* ---- U8g2 基类指针, init 时指向具体子类 ---- */
    U8G2 *m_u8g2 = nullptr;

    /* ---- 当前面板信息 ---- */
    oled_panel_t          m_panel   = OLED_PANEL_MAX;
    oled_screen_info_t    m_screen  = {};
    bool                  m_ready   = false;

    /* ---- 时钟槽数组 ---- */
    static constexpr uint8_t OLED_MAX_CLOCKS = 2;
    struct ClockSlot {
        bool          active      = false;
        uint16_t      y           = 0;
        oled_align_t  align       = OLED_ALIGN_TOP_CENTER;
        uint16_t      margin      = 0;
        char          fmt[32]     = "%H:%M:%S";
        uint32_t      refresh_ms  = 1000;
        uint32_t      last_tick_ms= 0;
    };
    ClockSlot           m_clocks[OLED_MAX_CLOCKS];
    esp_timer_handle_t  m_clock_timer = nullptr;

    /* ---- UI 导航内部状态 ---- */
    oled_page_t           m_pages[16];
    uint8_t               m_page_count  = 0;
    const oled_page_t*    m_page_stack[8];
    uint8_t               m_page_depth  = 0;
    uint8_t               m_focus_index = 0;

    /* ---- 触摸 (预留) ---- */
    oled_touch_cb_t       m_touch_cb    = nullptr;
    void*                 m_touch_ctx    = nullptr;

    /* ---- 内部辅助 ---- */

    // init 时根据 panel 创建 U8g2 子类
    esp_err_t init_i2c(oled_panel_t panel, const oled_i2c_pins_t &pins);
    esp_err_t init_spi(oled_panel_t panel, const oled_spi_pins_t &pins);

    // esp_timer 静态回调 -> 成员函数
    static void IRAM_ATTR clock_timer_cb(void *arg);
    void        refresh_clock();

    // 默认网格按键处理 (方向键移动焦点, OK 跳转, CANCEL 返回)
    bool        default_key_handler(const oled_page_t *page, oled_key_t key,
                                    uint8_t *focus_index);

    // 默认网格渲染 (遍历 items 画图标+文字+焦点框)
    void        default_grid_render(const oled_page_t *page, uint8_t focus_index);

    // 根据 items 数量 + 屏幕宽算网格列数
    uint8_t     calc_cols_for_grid(const oled_page_t *page) const;

    // 对齐坐标计算
    uint16_t    align_x(oled_align_t align, uint16_t obj_w, uint16_t margin) const;
    uint16_t    align_y(oled_align_t align, uint16_t obj_h, uint16_t margin) const;
};

#endif /* OLED_U8G2_HPP */
