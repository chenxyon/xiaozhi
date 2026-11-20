/**
 * @file     event_bus.c
 * @brief    轻量阻塞式事件总线实现
 *
 * 修改：2026-11-20 新建（规则 9）
 */
#include <string.h>

#include "esp_log.h"

#include "event_bus.h"

static const char *TAG = "event_bus";

typedef struct {
    event_handler_t handler;
    void           *ctx;
} subscription_t;

/* 每个事件一个订阅者数组 */
static subscription_t s_subs[EVT_MAX][EVENT_BUS_MAX_HANDLERS_PER_EVENT];
static bool s_inited = false;

void event_bus_init(void)
{
    memset(s_subs, 0, sizeof(s_subs));
    s_inited = true;
    ESP_LOGI(TAG, "事件总线就绪 (max %d handlers/event)",
             EVENT_BUS_MAX_HANDLERS_PER_EVENT);
}

esp_err_t event_bus_subscribe(app_event_t ev, event_handler_t h, void *ctx)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (ev <= EVT_NONE || ev >= EVT_MAX || !h) return ESP_ERR_INVALID_ARG;

    subscription_t *slot = s_subs[ev];
    for (int i = 0; i < EVENT_BUS_MAX_HANDLERS_PER_EVENT; i++) {
        if (slot[i].handler == NULL) {
            slot[i].handler = h;
            slot[i].ctx     = ctx;
            return ESP_OK;
        }
        if (slot[i].handler == h && slot[i].ctx == ctx) {
            return ESP_OK;   /* 已订阅过，幂等 */
        }
    }
    ESP_LOGE(TAG, "事件 %d 订阅已满", (int)ev);
    return ESP_ERR_NO_MEM;
}

esp_err_t event_bus_unsubscribe(app_event_t ev, event_handler_t h)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (ev <= EVT_NONE || ev >= EVT_MAX || !h) return ESP_ERR_INVALID_ARG;

    subscription_t *slot = s_subs[ev];
    for (int i = 0; i < EVENT_BUS_MAX_HANDLERS_PER_EVENT; i++) {
        if (slot[i].handler == h) {
            slot[i].handler = NULL;
            slot[i].ctx     = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void event_bus_publish(app_event_t ev, void *data)
{
    if (!s_inited || ev <= EVT_NONE || ev >= EVT_MAX) return;

    /* 先把有订阅的收集到临时列表，避免订阅者里 unsubscribe 导致下标漂移 */
    subscription_t *slot = s_subs[ev];
    subscription_t call_list[EVENT_BUS_MAX_HANDLERS_PER_EVENT];
    int n = 0;
    for (int i = 0; i < EVENT_BUS_MAX_HANDLERS_PER_EVENT; i++) {
        if (slot[i].handler) {
            call_list[n++] = slot[i];
        }
    }
    /* 然后逐个调 */
    for (int i = 0; i < n; i++) {
        call_list[i].handler(ev, data, call_list[i].ctx);
    }
}

bool event_bus_inited(void)
{
    return s_inited;
}
