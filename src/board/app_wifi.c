/**
 * @file     app_wifi.c
 * @brief    WiFi 业务层 —— 只保留「项目特有」的部分
 *
 * 功能：填项目特有的配网热点名/密码，把 wifi_manager 事件转发给上层播报。
 *       通用能力（默认配置、状态查询、扫描、配网页）都在 wifi_manager 组件里，
 *       本层不再重复实现。
 *
 * 设计：组件已自带 [0/4]~[4/4] 过程日志，本层不再打重复日志，
 *       只做「事件 → 上层播报」的转发，职责单一。
 */
#include "esp_log.h"
#include "wifi_manager.h"
#include "app_wifi.h"

static const char *TAG = "app_wifi";   /* 规则 4-8 */

/* 上层通知回调：app_voice 注册进来收 WiFi 事件做播报，本层不碰音频 */
static app_wifi_notify_cb_t s_notify_cb = NULL;
static void *s_notify_ctx = NULL;

void app_wifi_set_notify_cb(app_wifi_notify_cb_t cb, void *ctx)
{
    s_notify_cb = cb;
    s_notify_ctx = ctx;
}

/* 组件已打过过程日志，这里只转发 */
static void on_wifi_event(wifi_mgr_event_t event, void *ctx)
{
    (void)ctx;
    if (s_notify_cb) {
        s_notify_cb(event, s_notify_ctx);
    }
}

void app_wifi_init(void)
{
    /* 通用默认项（重试/自动重连/NTP/信道策略）由组件给出 */
    wifi_manager_config_t cfg = wifi_manager_default_config();

    /* 仅以下为项目特有：配网热点名与密码属于产品标识 */
    cfg.fallback_ssid = "XIAOLE_WIFI";
    cfg.fallback_pw   = "12345678";

    cfg.event_cb     = on_wifi_event;
    cfg.event_cb_ctx = NULL;

    ESP_ERROR_CHECK(wifi_manager_init(&cfg));
    ESP_LOGI(TAG, "WiFi 初始化完成，配网热点=%s", cfg.fallback_ssid);
}