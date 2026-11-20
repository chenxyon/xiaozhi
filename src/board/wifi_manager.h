/* wifi_manager.h — Arduino 版简化 stub
 *
 * 原项目用 ESP-IDF 的 wifi_manager 组件。Arduino 里用 WiFiManager 库，
 * 业务层（app_wifi.c / app_voice.c）引用了 wifi_manager 的事件枚举、
 * 配置结构体和初始化函数，这里提供完整 stub 让编译能过。
 * 实际 WiFi 配网逻辑在 main.cpp 里用 WiFiManager 库实现，
 * 这些 stub 函数返回 ESP_OK，事件不会被触发。
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** WiFi 事件类型（与原组件保持一致的枚举值） */
typedef enum {
    WIFI_MGR_EV_AP_STARTED = 0,      /* 配网热点已开启 */
    WIFI_MGR_EV_CONNECTED,           /* WiFi STA 已连接路由器 */
    WIFI_MGR_EV_GOT_IP,              /* STA 拿到 IP */
    WIFI_MGR_EV_DISCONNECTED,        /* STA 断开 */
    WIFI_MGR_EV_AP_STOPPED,          /* 配网热点关闭 */
    WIFI_MGR_EV_CONFIG_PORTAL_OPEN,  /* 配网页开 */
    WIFI_MGR_EV_CONFIG_PORTAL_CLOSED,/* 配网页关 */
} wifi_mgr_event_t;

/** 事件回调 */
typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_event_t event, void *ctx);

/** WiFi 管理器配置（原组件结构体，这里简化） */
typedef struct {
    const char            *fallback_ssid;     /* 配网热点 SSID */
    const char            *fallback_pw;       /* 配网热点密码 */
    wifi_mgr_event_cb_t    event_cb;          /* 事件回调 */
    void                  *event_cb_ctx;      /* 回调上下文 */
    uint32_t               connect_timeout_s;  /* 连接超时（秒） */
    bool                   auto_reconnect;     /* 自动重连 */
    bool                   enable_ntp;         /* 自动对时 */
    int                    ntp_portal_timeout_s; /* 配网页超时 */
} wifi_manager_config_t;

/* ===== stub 实现：Arduino 模式下 WiFi 由 main.cpp 的 WiFiManager 管理，
 * 这些函数返回成功但不做任何实际工作 ===== */

static inline wifi_manager_config_t wifi_manager_default_config(void)
{
    wifi_manager_config_t cfg = {
        .fallback_ssid        = "XiaoZhi-Setup",
        .fallback_pw          = "12345678",
        .event_cb             = NULL,
        .event_cb_ctx         = NULL,
        .connect_timeout_s    = 30,
        .auto_reconnect       = true,
        .enable_ntp           = true,
        .ntp_portal_timeout_s = 120,
    };
    return cfg;
}

static inline esp_err_t wifi_manager_init(const wifi_manager_config_t *cfg)
{
    (void)cfg;
    /* Arduino 模式下 WiFi 已在 main.cpp 初始化，这里什么都不做 */
    return ESP_OK;
}

static inline bool wifi_manager_is_connected(void) { return false; }
static inline const char *wifi_manager_get_status(void) { return "stub"; }

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */
