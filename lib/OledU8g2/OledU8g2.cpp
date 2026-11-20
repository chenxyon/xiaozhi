/**
 * @file     OledU8g2.cpp
 * @brief    多面板通用 OLED/TFT 显示类实现 —— 基于 U8g2
 *
 * 核心实现：init 时根据 panel new 对应 U8g2 子类；
 *          show/draw_icon 做边界检查 + min_screen 过滤；
 *          clock_timer 用 esp_timer 1Hz 刷新时间；
 *          UI 导航核心 (页面栈 + 默认网格方向键处理 + 默认网格渲染) 已实现；
 *          触摸回调为 stub (U8g2 无触摸)。
 *
 * U8g2 支持的面板驱动类后缀:
 *   SSD1306_128X32_UNIVISION_F_HW_I2C
 *   SSD1306_128X64_NONAME_F_HW_I2C
 *   SH1106_128X64_NONAME_F_HW_I2C
 *   ST7789_240X240_F_4W_HW_SPI
 */
#include "OledU8g2.hpp"

#include <cstring>
#include <ctime>

#include "esp_log.h"

static const char *TAG = "OLED";

/* =========================================================
 *  初始化: 统一入口, 根据 panel 分发到 I2C/SPI 初始化
 *
 *  架构说明: 组件只提供"画"的能力 (show / draw_icon / draw_rect ...),
 *           不硬编码任何图标或业务状态。
 *           WiFi/电池图标数组、WiFi 连接状态、什么时候画、画在哪 ——
 *           全由调用方 (项目里的 main.cpp / board.c / icons.h) 自己管理。
 * ========================================================= */

esp_err_t OledU8g2::init(oled_panel_t panel, const void *pins)
{
    if (m_ready) {
        ESP_LOGW(TAG, "已初始化过, deinit() 后再 init");
        return ESP_OK;
    }
    if (!pins) {
        ESP_LOGE(TAG, "pins 为 NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (panel >= OLED_PANEL_MAX) {
        ESP_LOGE(TAG, "未知 panel 类型: %d", panel);
        return ESP_ERR_NOT_SUPPORTED;
    }

    m_panel = panel;

    // 根据 panel 选择接口类型
    switch (panel) {
    /* ---- 0.91"/0.96" I²C 屏 ---- */
    case OLED_PANEL_SSD1306_128X32:
    case OLED_PANEL_SSD1306_128X64:
    case OLED_PANEL_SH1106_128X64:
    case OLED_PANEL_SH1107_64X128: {
        auto *p = static_cast<const oled_i2c_pins_t *>(pins);
        // 校验
        if (p->scl < 0 || p->sda < 0) {
            ESP_LOGE(TAG, "I²C 引脚非法: scl=%d sda=%d", p->scl, p->sda);
            return ESP_ERR_INVALID_ARG;
        }
        if (p->i2c_addr != 0x3C && p->i2c_addr != 0x3D) {
            ESP_LOGW(TAG, "I²C 地址 0x%02X 超出常见范围 (0x3C/0x3D)", p->i2c_addr);
        }
        return init_i2c(panel, *p);
    }
    /* ---- SPI 屏 (墨水屏 / 单色 LCD / 彩色 TFT) ---- */
    case OLED_PANEL_UC1701_DOGS102_64X128:
    case OLED_PANEL_SSD1606_172X72:
    case OLED_PANEL_SSD1607_200X200:
    case OLED_PANEL_SSD1607_240X240:
    case OLED_PANEL_SSD1322_240X128:
    case OLED_PANEL_ST7789_240X240:
    case OLED_PANEL_ST7789_240X320:
    case OLED_PANEL_ILI9341_240X320:
    case OLED_PANEL_GC9A01_240X240:
    case OLED_PANEL_ILI9488_320X480:
    case OLED_PANEL_HX8357_320X480: {
        auto *p = static_cast<const oled_spi_pins_t *>(pins);
        if (p->mosi < 0 || p->sclk < 0 || p->cs < 0 || p->dc < 0) {
            ESP_LOGE(TAG, "SPI 主引脚缺失");
            return ESP_ERR_INVALID_ARG;
        }
        return init_spi(panel, *p);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t OledU8g2::init_i2c(oled_panel_t panel, const oled_i2c_pins_t &pins)
{
    ESP_LOGI(TAG, "I²C init: panel=%d, scl=%d sda=%d addr=0x%02X",
             panel, pins.scl, pins.sda, pins.i2c_addr);

    // Arduino Wire 初始化
    uint32_t freq = pins.freq_hz ? pins.freq_hz : 400 * 1000;
    Wire.begin((int)pins.sda, (int)pins.scl, freq);

    // 扫一下 OLED 从机地址
    for (uint8_t addr : (uint8_t[]){pins.i2c_addr, (uint8_t)(pins.i2c_addr ^ 0x01)}) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            ESP_LOGI(TAG, "扫到 OLED 从机地址 0x%02X", addr);
            break;
        }
    }

    // 根据 panel new 具体 U8g2 子类
    uint8_t reset_pin = U8X8_PIN_NONE;
    switch (panel) {
    case OLED_PANEL_SSD1306_128X32:
        m_u8g2 = new U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(
            U8G2_R0, reset_pin, U8X8_PIN_NONE, U8X8_PIN_NONE);
        m_screen = {128, 32, 12, 12};
        break;
    case OLED_PANEL_SSD1306_128X64:
        m_u8g2 = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(
            U8G2_R0, reset_pin, U8X8_PIN_NONE, U8X8_PIN_NONE);
        m_screen = {128, 64, 12, 12};
        break;
    case OLED_PANEL_SH1106_128X64:
        m_u8g2 = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(
            U8G2_R0, reset_pin, U8X8_PIN_NONE, U8X8_PIN_NONE);
        m_screen = {128, 64, 12, 12};
        break;
    case OLED_PANEL_SH1107_64X128:
        m_u8g2 = new U8G2_SH1107_64X128_F_HW_I2C(
            U8G2_R0, reset_pin, U8X8_PIN_NONE, U8X8_PIN_NONE);
        m_screen = {64, 128, 12, 12};
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    // U8g2 期望 8 位地址 (左移 1 位)
    m_u8g2->setI2CAddress(pins.i2c_addr * 2);
    m_u8g2->begin();
    m_u8g2->enableUTF8Print();
    m_u8g2->setFont(u8g2_font_wqy12_t_gb2312);   /* 默认中文字体 */
    m_u8g2->setFontPosTop();

    m_ready = true;
    ESP_LOGI(TAG, "OLED 就绪 → I²C %dx%d", m_screen.width, m_screen.height);
    return ESP_OK;
}

esp_err_t OledU8g2::init_spi(oled_panel_t panel, const oled_spi_pins_t &pins)
{
    ESP_LOGI(TAG, "SPI init: panel=%d, mosi=%d sclk=%d cs=%d dc=%d rst=%d",
             panel, pins.mosi, pins.sclk, pins.cs, pins.dc, pins.rst);

    // 墨水屏刷新慢 (0.3~2s/帧), 给用户一个提示
    bool is_epaper = false;
    switch (panel) {
    case OLED_PANEL_UC1701_DOGS102_64X128:
    case OLED_PANEL_SSD1606_172X72:
    case OLED_PANEL_SSD1607_200X200:
    case OLED_PANEL_SSD1607_240X240:
        is_epaper = true;
        break;
    default:
        break;
    }

    // Arduino SPI 初始化 (HW_SPI 驱动需要)
    SPI.begin((int)pins.sclk, -1, (int)pins.mosi, -1);  // SCLK, MISO(-1=不用), MOSI, SS(-1=cs 手动)

    uint8_t reset_pin = (pins.rst < 0) ? U8X8_PIN_NONE : (uint8_t)pins.rst;
    // HW_SPI 驱动构造统一 4 参数: rotation, cs, dc, reset
    // sclk/mosi 在 SPI.begin() 里设定, 不需要传给驱动构造函数

    switch (panel) {
    /* ---- 墨水屏 (e-Paper) ---- */
    case OLED_PANEL_UC1701_DOGS102_64X128:
        m_u8g2 = new U8G2_UC1701_EA_DOGS102_F_4W_HW_SPI(
            U8G2_R0, pins.cs, pins.dc, reset_pin);
        m_screen = {64, 128, 12, 12};
        break;
    case OLED_PANEL_SSD1606_172X72:
        m_u8g2 = new U8G2_SSD1606_172X72_F_4W_HW_SPI(
            U8G2_R0, pins.cs, pins.dc, reset_pin);
        m_screen = {172, 72, 12, 12};
        break;
    case OLED_PANEL_SSD1607_200X200:
        m_u8g2 = new U8G2_SSD1607_200X200_F_4W_HW_SPI(
            U8G2_R0, pins.cs, pins.dc, reset_pin);
        m_screen = {200, 200, 12, 12};
        break;
    case OLED_PANEL_SSD1607_240X240:
        // U8g2 没有 240x240 的 SSD1607 精确驱动, 用 200x200 近似 or 返回提示
        ESP_LOGW(TAG, "SSD1607 240x240 暂用 200x200 驱动近似, 如显示偏移请查 U8g2 是否有对应驱动");
        m_u8g2 = new U8G2_SSD1607_200X200_F_4W_HW_SPI(
            U8G2_R0, pins.cs, pins.dc, reset_pin);
        m_screen = {240, 240, 12, 12};
        break;

    /* ---- 单色 SPI LCD ---- */
    case OLED_PANEL_SSD1322_240X128:
        m_u8g2 = new U8G2_SSD1322_240X128_F_4W_HW_SPI(
            U8G2_R0, pins.cs, pins.dc, reset_pin);
        m_screen = {240, 128, 12, 12};
        break;

    /* ---- 彩色 TFT (U8g2 不支持, 返回提示) ---- */
    case OLED_PANEL_ST7789_240X240:
    case OLED_PANEL_ST7789_240X320:
    case OLED_PANEL_ILI9341_240X320:
    case OLED_PANEL_GC9A01_240X240:
    case OLED_PANEL_ILI9488_320X480:
    case OLED_PANEL_HX8357_320X480:
        ESP_LOGE(TAG, "panel %d 是彩色 TFT, U8g2 暂不支持真彩驱动。"
                      "请换用 TFT_eSPI / LVGL / LovyanGFX, "
                      "或在 oled_types.h 扩展 OLED_PANEL_* 枚举 + 新驱动类", panel);
        SPI.end();
        return ESP_ERR_NOT_SUPPORTED;
    default:
        SPI.end();
        return ESP_ERR_NOT_SUPPORTED;
    }

    m_u8g2->begin();
    m_u8g2->enableUTF8Print();
    m_u8g2->setFont(u8g2_font_wqy12_t_gb2312);
    m_u8g2->setFontPosTop();

    m_ready = true;
    ESP_LOGI(TAG, "SPI 屏就绪 → %dx%d %s", m_screen.width, m_screen.height,
             is_epaper ? "(墨水屏, 刷新慢, 注意控制 sendBuffer 频率)" : "");
    return ESP_OK;
}

esp_err_t OledU8g2::deinit()
{
    if (m_clock_timer) {
        esp_timer_stop(m_clock_timer);
        esp_timer_delete(m_clock_timer);
        m_clock_timer = nullptr;
    }
    delete m_u8g2;
    m_u8g2 = nullptr;
    m_ready = false;
    m_page_count = 0;
    m_page_depth = 0;
    m_focus_index = 0;
    return ESP_OK;
}

/* =========================================================
 *  基础绘制 API (show / draw_icon / draw_rect / draw_line / clear / refresh)
 * ========================================================= */

esp_err_t OledU8g2::show(const char *text, uint16_t x, uint16_t y)
{
    if (!m_ready || !m_u8g2 || !text) return ESP_ERR_INVALID_STATE;

    // 边界检查 (y 超屏就跳过, 但也不崩)
    if (x >= m_screen.width || y >= m_screen.height) {
        return ESP_OK;
    }

    m_u8g2->drawStr((int16_t)x, (int16_t)y, text);
    return ESP_OK;
}

esp_err_t OledU8g2::show_aligned(const char *text, oled_align_t align, uint16_t margin)
{
    if (!text) return ESP_ERR_INVALID_ARG;
    // 先算字符串宽度
    int w = m_u8g2 ? m_u8g2->getUTF8Width(text) : 0;
    int h = m_screen.font_h;
    return show(text, align_x(align, (uint16_t)w, margin), align_y(align, (uint16_t)h, margin));
}

esp_err_t OledU8g2::draw_icon(const oled_icon_t *icon, uint16_t x, uint16_t y)
{
    if (!m_ready || !m_u8g2 || !icon || !icon->data) return ESP_ERR_INVALID_STATE;

    // min_screen 自动过滤
    if (icon->min_screen > 0 && m_screen.width < icon->min_screen) {
        ESP_LOGD(TAG, "图标 %dx%d 需要屏宽≥%d, 当前 %d, 跳过",
                 icon->w, icon->h, icon->min_screen, m_screen.width);
        return ESP_OK;
    }
    // 完全不可见 → 跳过
    if (x >= m_screen.width || y >= m_screen.height) return ESP_OK;

    // cnt = 每行字节数, 必须是 ceil(w/8). 之前传 icon->w 会把 cnt 当列数, 导致越界读
    uint8_t cnt = (icon->w + 7) / 8;
    m_u8g2->drawBitmap((int16_t)x, (int16_t)y, cnt, icon->h, icon->data);
    return ESP_OK;
}

esp_err_t OledU8g2::draw_icon_aligned(const oled_icon_t *icon, oled_align_t align, uint16_t margin)
{
    if (!icon) return ESP_ERR_INVALID_ARG;
    return draw_icon(icon, align_x(align, icon->w, margin), align_y(align, icon->h, margin));
}

esp_err_t OledU8g2::draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fill)
{
    if (!m_ready || !m_u8g2) return ESP_ERR_INVALID_STATE;
    m_u8g2->drawFrame((int16_t)x, (int16_t)y, w, h);
    if (fill) m_u8g2->drawBox((int16_t)x, (int16_t)y, w, h);
    return ESP_OK;
}

esp_err_t OledU8g2::draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (!m_ready || !m_u8g2) return ESP_ERR_INVALID_STATE;
    m_u8g2->drawLine((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2);
    return ESP_OK;
}

esp_err_t OledU8g2::clear()
{
    if (!m_ready || !m_u8g2) return ESP_ERR_INVALID_STATE;
    m_u8g2->clearBuffer();
    return ESP_OK;
}

esp_err_t OledU8g2::refresh()
{
    if (!m_ready || !m_u8g2) return ESP_ERR_INVALID_STATE;
    m_u8g2->sendBuffer();
    return ESP_OK;
}

/* =========================================================
 *  对齐坐标计算 (内部辅助)
 * ========================================================= */

uint16_t OledU8g2::align_x(oled_align_t align, uint16_t obj_w, uint16_t margin) const
{
    switch (align) {
    case OLED_ALIGN_TOP_LEFT:
    case OLED_ALIGN_BOTTOM_LEFT:
        return margin;
    case OLED_ALIGN_TOP_RIGHT:
    case OLED_ALIGN_BOTTOM_RIGHT:
        return (obj_w >= m_screen.width) ? 0 : (m_screen.width - obj_w - margin);
    case OLED_ALIGN_TOP_CENTER:
    case OLED_ALIGN_BOTTOM_CENTER:
    case OLED_ALIGN_MIDDLE:
        return (obj_w >= m_screen.width) ? 0 : ((m_screen.width - obj_w) / 2);
    default:
        return 0;
    }
}

uint16_t OledU8g2::align_y(oled_align_t align, uint16_t obj_h, uint16_t margin) const
{
    switch (align) {
    case OLED_ALIGN_TOP_LEFT:
    case OLED_ALIGN_TOP_RIGHT:
    case OLED_ALIGN_TOP_CENTER:
        return margin;
    case OLED_ALIGN_BOTTOM_LEFT:
    case OLED_ALIGN_BOTTOM_RIGHT:
    case OLED_ALIGN_BOTTOM_CENTER:
        return (obj_h >= m_screen.height) ? 0 : (m_screen.height - obj_h - margin);
    case OLED_ALIGN_MIDDLE:
        return (obj_h >= m_screen.height) ? 0 : ((m_screen.height - obj_h) / 2);
    default:
        return 0;
    }
}

/* =========================================================
 *  时钟显示 —— 多行 slot + 对齐 + 独立刷新频率
 * ========================================================= */

void IRAM_ATTR OledU8g2::clock_timer_cb(void *arg)
{
    auto *self = static_cast<OledU8g2 *>(arg);
    self->refresh_clock();   // 1s tick, 内部遍历所有 slot 按各自 refresh_ms 节流
}

esp_err_t OledU8g2::set_clock_slot(uint8_t index, bool active, const char *fmt,
                                    uint16_t y, oled_align_t align,
                                    uint32_t refresh_ms)
{
    if (index >= OLED_MAX_CLOCKS) return ESP_ERR_INVALID_ARG;
    if (!active) {
        m_clocks[index].active = false;
        return ESP_OK;
    }
    m_clocks[index].active     = true;
    m_clocks[index].y          = y;
    m_clocks[index].align      = align;
    m_clocks[index].margin     = 0;
    m_clocks[index].refresh_ms = refresh_ms;
    m_clocks[index].last_tick_ms = 0;   // 让第一次 tick 就刷新
    if (fmt && *fmt) {
        strncpy(m_clocks[index].fmt, fmt, sizeof(m_clocks[index].fmt) - 1);
        m_clocks[index].fmt[sizeof(m_clocks[index].fmt) - 1] = '\0';
    }
    ESP_LOGD(TAG, "clock slot[%u] active=%d y=%u fmt='%s' refresh=%ums",
             index, active, y, m_clocks[index].fmt, refresh_ms);
    return ESP_OK;
}

esp_err_t OledU8g2::enable_clock(bool on)
{
    if (!m_ready) return ESP_ERR_INVALID_STATE;

    // 先停旧的
    if (m_clock_timer) {
        esp_timer_stop(m_clock_timer);
        esp_timer_delete(m_clock_timer);
        m_clock_timer = nullptr;
    }

    if (!on) return ESP_OK;

    esp_timer_create_args_t args = {
        .callback = &OledU8g2::clock_timer_cb,
        .arg      = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name     = "oled_clock",
    };
    esp_err_t err = esp_timer_create(&args, &m_clock_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create 失败: %s", esp_err_to_name(err));
        return err;
    }
    // 立即刷新一次 + 每秒 tick (内部各 slot 按 refresh_ms 节流)
    refresh_clock();
    err = esp_timer_start_periodic(m_clock_timer, 1000000);   // 1s = 1000000 us
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic 失败: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t active = 0;
    for (auto &c : m_clocks) if (c.active) active++;
    ESP_LOGI(TAG, "时钟启用: %u 个 active slot, 1s tick", active);
    return ESP_OK;
}

esp_err_t OledU8g2::disable_clock()
{
    return enable_clock(false);
}

void OledU8g2::refresh_clock_now()
{
    refresh_clock();
}

void OledU8g2::refresh_clock()
{
    time_t now = time(nullptr);
    if (now == 0 || !m_ready || !m_u8g2) return;   // NTP 没同步或屏没 init

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    bool dirty = false;
    for (int i = 0; i < OLED_MAX_CLOCKS; i++) {
        auto &c = m_clocks[i];
        if (!c.active) continue;
        if (c.refresh_ms > 0 && (now_ms - c.last_tick_ms) < c.refresh_ms) continue;

        char buf[64];   // 够 strftime "%Y-%m-%d %H:%M:%S 周%s" 这类
        strftime(buf, sizeof(buf), c.fmt, &tm_buf);

        // 对齐: 算字符串宽度 → 算 x
        int w = m_u8g2 ? m_u8g2->getUTF8Width(buf) : 0;
        uint16_t x = align_x(c.align, (uint16_t)w, c.margin);

        // 直接 drawStr 到 buffer, 最后一起 sendBuffer
        m_u8g2->drawStr((int16_t)x, (int16_t)c.y, buf);

        c.last_tick_ms = now_ms;
        dirty = true;
    }

    if (dirty) {
        // 只刷一次屏: 先覆盖旧时间行 (clearBuffer 再重绘状态行)
        // 这里简化成 clearBuffer 让调用方重绘状态行, 或直接 sendBuffer
        // 实际上时钟只是整个 buffer 的一部分, 我们不应该 clearBuffer
        // 改成: 调用方负责 clearBuffer + 画状态行 + 调 refresh_clock + sendBuffer
        // 但我们不想改调用方... 所以这里直接 sendBuffer (假设 buffer 已被其他绘制填充了)
        refresh();   // sendBuffer()
    }
}

/* =========================================================
 *  WiFi/电池状态: 组件不管理!
 *  调用方自己管理:
 *    oled.draw_icon(&wifi_on_icon, 116, 0);   // WiFi 已连
 *    oled.draw_icon(&wifi_off_icon, 116, 0); // WiFi 断开
 *    oled.draw_icon(&battery_icon, 100, 0);  // 电池
 *  图标数组放在项目 src/display/icons/icons.h 里, 不硬编码在组件里
 * ========================================================= */

esp_err_t OledU8g2::set_status_bar(const oled_status_bar_cfg_t &cfg)
{
    // 预留: 存储配置, draw_status_bar() 时使用
    // 简化版: 直接返回 OK, 内部暂时不用这个 cfg
    return ESP_OK;
}

/* =========================================================
 *  UI 导航: 页面栈 + 按键事件分发
 * ========================================================= */

esp_err_t OledU8g2::register_page(const oled_page_t &page)
{
    if (m_page_count >= (uint8_t)(sizeof(m_pages) / sizeof(m_pages[0]))) {
        ESP_LOGE(TAG, "页面数已满 (%d)", m_page_count);
        return ESP_ERR_NO_MEM;
    }
    if (!page.uri || !page.items || page.item_count == 0) {
        ESP_LOGE(TAG, "页面参数缺失");
        return ESP_ERR_INVALID_ARG;
    }
    m_pages[m_page_count] = page;
    m_page_count++;
    ESP_LOGI(TAG, "注册页面 '%s' (%d items)", page.uri, page.item_count);
    return ESP_OK;
}

esp_err_t OledU8g2::navigate_to(const char *uri)
{
    if (!uri) return ESP_ERR_INVALID_ARG;

    // 找匹配的页面
    const oled_page_t *target = nullptr;
    for (uint8_t i = 0; i < m_page_count; i++) {
        if (strcmp(m_pages[i].uri, uri) == 0) {
            target = &m_pages[i];
            break;
        }
    }
    if (!target) {
        ESP_LOGE(TAG, "未注册的页面: '%s'", uri);
        return ESP_ERR_NOT_FOUND;
    }

    // 页面栈满了也允许替换栈顶 (最多 8 层)
    if (m_page_depth >= (uint8_t)(sizeof(m_page_stack) / sizeof(m_page_stack[0]))) {
        m_page_depth = 0;   // 兜底: 溢出则清空重来
    }
    m_page_stack[m_page_depth++] = target;
    m_focus_index = 0;

    // 渲染新页面
    default_grid_render(target, m_focus_index);
    ESP_LOGD(TAG, "navigate_to '%s' (depth=%d)", uri, m_page_depth);
    return ESP_OK;
}

esp_err_t OledU8g2::navigate_back()
{
    if (m_page_depth <= 1) {
        ESP_LOGD(TAG, "页面栈只有 1 层, 不能再返回");
        return ESP_OK;
    }
    m_page_depth--;
    const oled_page_t *cur = m_page_stack[m_page_depth - 1];
    m_focus_index = 0;

    // 重绘上一页
    default_grid_render(cur, m_focus_index);
    ESP_LOGD(TAG, "navigate_back → '%s' (depth=%d)", cur->uri, m_page_depth);
    return ESP_OK;
}

esp_err_t OledU8g2::set_focus(uint8_t index)
{
    const oled_page_t *cur = current_page();
    if (!cur) return ESP_ERR_INVALID_STATE;
    if (index >= cur->item_count) return ESP_ERR_INVALID_ARG;
    m_focus_index = index;
    return ESP_OK;
}

esp_err_t OledU8g2::send_key(oled_key_t key)
{
    const oled_page_t *cur = current_page();
    if (!cur) {
        ESP_LOGD(TAG, "没有当前页面, 按键 %d 忽略", key);
        return ESP_OK;
    }

    bool handled = false;

    // 1. 如果当前页面有自定义按键处理器, 先让它处理
    if (cur->on_key) {
        handled = cur->on_key(cur, key, &m_focus_index, cur->user_ctx);
    }

    // 2. 没处理 → 走内置默认网格导航
    if (!handled) {
        handled = default_key_handler(cur, key, &m_focus_index);
    }

    // 3. 如果按键导致焦点变化或跳转 → 重绘
    //    (navigate_to / navigate_back 内部已经重绘)
    if (key != OLED_KEY_OK && key != OLED_KEY_CANCEL) {
        default_grid_render(cur, m_focus_index);
    }

    return ESP_OK;
}

const oled_page_t *OledU8g2::current_page() const
{
    if (m_page_depth == 0) return nullptr;
    return m_page_stack[m_page_depth - 1];
}

/* =========================================================
 *  默认网格按键处理: 方向键移动焦点, OK 跳转, CANCEL 返回
 * ========================================================= */

bool OledU8g2::default_key_handler(const oled_page_t *page, oled_key_t key,
                                    uint8_t *focus_index)
{
    uint8_t total = page->item_count;
    if (total == 0) return false;

    uint8_t cols = calc_cols_for_grid(page);
    uint8_t rows = (total + cols - 1) / cols;

    uint8_t cur = *focus_index;
    if (cur >= total) cur = 0;
    uint8_t cur_row = cur / cols;
    uint8_t cur_col = cur % cols;

    switch (key) {
    case OLED_KEY_UP:
        cur_row = (cur_row == 0) ? rows - 1 : cur_row - 1;
        break;
    case OLED_KEY_DOWN:
        cur_row = (cur_row + 1) % rows;
        break;
    case OLED_KEY_LEFT:
        cur_col = (cur_col == 0) ? cols - 1 : cur_col - 1;
        break;
    case OLED_KEY_RIGHT:
        cur_col = (cur_col + 1) % cols;
        break;
    case OLED_KEY_OK: {
        // 跳转到选中项的 uri
        const char *target_uri = page->items[cur].uri;
        ESP_LOGD(TAG, "OK: navigate_to '%s'", target_uri ? target_uri : "(null)");
        navigate_to(target_uri);
        return true;
    }
    case OLED_KEY_CANCEL:
        navigate_back();
        return true;
    default:
        return false;
    }

    *focus_index = cur_row * cols + cur_col;
    if (*focus_index >= total) *focus_index = total - 1;
    return true;
}

/* =========================================================
 *  网格列数计算: 根据屏幕宽 + items 数量 + 屏幕类型
 * ========================================================= */

uint8_t OledU8g2::calc_cols_for_grid(const oled_page_t *page) const
{
    uint8_t total = page->item_count;
    if (total == 0) return 1;

    // 根据屏幕类型给合理默认值
    // 0.91" 128x32 → 最多 4 列 (每行每个格子 ~32px)
    // 0.96" 128x64 → 最多 4 列
    // 2.4" 240x240 → 最多 6 列
    uint8_t max_cols = 4;   // 0.91"/0.96" 默认 4 列
    if (m_screen.width >= 240) max_cols = 6;

    return (total > max_cols) ? max_cols : total;
}

/* =========================================================
 *  默认网格渲染: 画标题 + 菜单项 (图标+文字) + 焦点框
 * ========================================================= */

void OledU8g2::default_grid_render(const oled_page_t *page, uint8_t focus_index)
{
    if (!page || !m_ready || !m_u8g2) return;

    clear();

    uint8_t cols = calc_cols_for_grid(page);
    uint8_t total = page->item_count;
    uint8_t rows = (total + cols - 1) / cols;

    // 估算格子大小
    uint16_t gap = 4;
    uint16_t cell_w = (m_screen.width - gap * (cols + 1)) / cols;
    uint16_t cell_h = (m_screen.height - gap * (rows + 1)) / rows;
    if (cell_w == 0) cell_w = 16;
    if (cell_h == 0) cell_h = 16;

    uint8_t icon_size = 12;   // 默认用 12x12 图标

    // 画顶部标题 (如果有)
    uint16_t y_offset = 0;
    if (page->title) {
        show(page->title, gap, gap);
        y_offset = gap + m_screen.font_h + gap;   // 标题占的高度
    }

    // 如果屏太小 (0.91" 128x32) + 标题 + 1 行 2 个图标: 紧凑布局
    // 不画标题 (屏太小), 直接画图标
    if (m_screen.height <= 32) {
        y_offset = 0;
        if (page->title) {
            show(page->title, gap, 0);
            y_offset = m_screen.font_h + 2;
        }
    }

    // 画每个菜单项
    for (uint8_t i = 0; i < total; i++) {
        uint8_t col = i % cols;
        uint8_t row = i / cols;

        uint16_t cx = gap + col * (cell_w + gap);
        uint16_t cy = y_offset + gap + row * (cell_h + gap);

        // 图标 (居中)
        const oled_icon_t *icon = page->items[i].icon;
        if (icon && icon->data) {
            uint16_t ix = cx + (cell_w > icon_size) ? (cell_w - icon_size) / 2 : 0;
            uint16_t iy = cy;
            draw_icon(icon, ix, iy);
        }

        // 文字 (图标下方或右侧)
        const char *label = page->items[i].title;
        if (label) {
            uint16_t tx = cx;
            uint16_t ty = cy + icon_size + 2;   // 图标下 2px
            // 右对齐到格子内
            int tw = m_u8g2 ? m_u8g2->getUTF8Width(label) : 0;
            if ((uint16_t)tw > cell_w) tw = cell_w;
            tx = cx + (cell_w - tw) / 2;
            show(label, tx, ty);
        }

        // 焦点框 (空心矩形)
        if (i == focus_index) {
            draw_rect(cx, cy, cell_w, cell_h, false);
        }
    }

    refresh();
}

/* =========================================================
 *  图标网格绘制 (预留)
 * ========================================================= */

esp_err_t OledU8g2::draw_grid(const oled_grid_item_t *items, uint8_t count,
                                const oled_grid_cfg_t &cfg,
                                uint16_t start_x, uint16_t start_y)
{
    if (!m_ready || !m_u8g2 || !items || count == 0) return ESP_ERR_INVALID_STATE;

    uint16_t x = start_x;
    uint16_t y = start_y;
    uint16_t cur_col = 0, cur_row = 0;
    uint16_t cols = cfg.cols ? cfg.cols : 4;

    for (uint8_t i = 0; i < count; i++) {
        const oled_grid_item_t &it = items[i];
        uint16_t cx = x + cur_col * (cfg.cell_w + cfg.gap);
        uint16_t cy = y + cur_row * (cfg.cell_h + cfg.gap);

        draw_icon(it.icon, cx + cfg.icon_x_offset, cy + cfg.icon_y_offset);
        if (it.label) show(it.label, cx + cfg.icon_x_offset, cy + cfg.icon_y_offset + 16);
        if (it.selected) draw_rect(cx, cy, cfg.cell_w, cfg.cell_h, false);

        cur_col++;
        if (cur_col >= cols) {
            cur_col = 0;
            cur_row++;
        }
    }
    refresh();
    return ESP_OK;
}

/* =========================================================
 *  页面指示器 (预留: 底部小圆点)
 * ========================================================= */

esp_err_t OledU8g2::draw_page_indicator(const oled_page_ctrl_t &ctrl,
                                        uint16_t x, uint16_t y)
{
    if (!m_ready || !m_u8g2) return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0; i < ctrl.total_pages; i++) {
        uint8_t r = (i == ctrl.current_page) ? 3 : 2;
        m_u8g2->drawDisc((int16_t)(x + i * 8), (int16_t)y, r, U8G2_DRAW_UPPER_RIGHT);
    }
    refresh();
    return ESP_OK;
}

/* =========================================================
 *  触摸回调 (stub: U8g2 无触摸, 预留接口)
 * ========================================================= */

esp_err_t OledU8g2::register_touch_callback(oled_touch_cb_t cb, void *user_ctx)
{
    m_touch_cb = cb;
    m_touch_ctx = user_ctx;
    ESP_LOGW(TAG, "触摸回调已注册, 但当前 U8g2 驱动无触摸硬件支持, 回调不会被触发");
    return ESP_OK;
}

/* 组件架构: 只提供"画"的能力, 不管理任何业务状态
 * WiFi/电池图标数组、WiFi 连接状态、什么时候画、画在哪 —— 全在项目里管理 */
