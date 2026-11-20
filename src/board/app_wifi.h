/**
 * @file     app_wifi.h
 * @brief    WiFi 业务层接口（只做事件转发 + 填项目特有配置）
 *
 * 设计说明：
 *   - 状态查询 / 扫描 / 配网页 / 强制配网等通用能力，直接用 wifi_manager 组件
 *     的 API（wifi_manager_is_connected / wifi_manager_get_status /
 *      wifi_manager_scan_all / wifi_manager_start_config_portal），
 *     不在本层包一层没意义的转发，故本头文件不再声明它们。
 *   - 本层唯一职责：把 wifi_manager 事件转发给上层（如 app_voice 做播报），
 *     并填入项目特有的配网热点名/密码。
 */
#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  WiFi 事件上层通知回调（供 app_voice 等订阅播报）
 */
typedef void (*app_wifi_notify_cb_t)(wifi_mgr_event_t event, void *ctx);

/**
 * @brief  注册上层事件通知（播报等逻辑由上层承接，本层不碰音频）
 *
 * @note   必须在 app_wifi_init() 之前调用，否则收不到连接成功事件。
 */
void app_wifi_set_notify_cb(app_wifi_notify_cb_t cb, void *ctx);

/**
 * @brief  初始化 WiFi 管理器
 *
 * 通用默认项取自 wifi_manager_default_config()，本函数只额外填入
 * 本项目的配网热点名/密码。
 */
void app_wifi_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WIFI_H */