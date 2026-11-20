/**
 * @file     ws_bridge.cpp
 * @brief    ArduinoWebsockets → C 桥接层（C++ 实现）
 *
 * 修改：2026-11-16 新建
 */
#include "ws_bridge.h"

#include <ArduinoWebsockets.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

static const char *TAG = "ws_bridge";   /* 规则 4-8 */

struct ws_bridge {
    websockets::WebsocketsClient client;
    ws_bridge_on_msg_t  on_msg;
    ws_bridge_on_evt_t on_evt;
    void *ctx;
    bool connected;
    bool error_seen;
};

extern "C" ws_bridge_t *ws_bridge_create(void)
{
    return new ws_bridge_t();
}

extern "C" void ws_bridge_set_callbacks(ws_bridge_t *wb,
                                        ws_bridge_on_msg_t  on_msg,
                                        ws_bridge_on_evt_t on_evt,
                                        void *ctx)
{
    if (!wb) return;
    wb->on_msg  = on_msg;
    wb->on_evt = on_evt;
    wb->ctx     = ctx;
}

static void _attach_handlers(ws_bridge_t *wb)
{
    using namespace websockets;

    wb->client.onMessage([wb](WebsocketsMessage msg) {
        if (wb->on_msg && msg.isText() && msg.isComplete()) {
            wb->on_msg(msg.c_str(), msg.length(), wb->ctx);
        }
    });

    wb->client.onEvent([wb](WebsocketsEvent event, WSInterfaceString) {
        switch (event) {
        case WebsocketsEvent::ConnectionOpened:
            wb->connected = true;
            if (wb->on_evt) wb->on_evt(WS_BRIDGE_EVENT_OPENED, wb->ctx);
            break;
        case WebsocketsEvent::ConnectionClosed:
            wb->connected = false;
            if (wb->on_evt) wb->on_evt(WS_BRIDGE_EVENT_CLOSED, wb->ctx);
            break;
        case WebsocketsEvent::GotPing:
        case WebsocketsEvent::GotPong:
            break;
        }
    });
}

extern "C" bool ws_bridge_connect(ws_bridge_t *wb, const char *url, int timeout_ms)
{
    if (!wb || !url) return false;

    _attach_handlers(wb);

    /* ===== DNS 预检（用 ESP-IDF 原生 getaddrinfo，绕过 Arduino hostByName 的缓存 bug）===== */
    char host[64] = {0};
    if (strncmp(url, "wss://", 6) == 0) {
        const char *p = url + 6;
        int i = 0;
        while (*p && *p != '/' && *p != ':' && *p != '?' && i < (int)sizeof(host) - 1) {
            host[i++] = *p++;
        }
    }
    IPAddress dns_ip;
    bool dns_ok = false;
    if (host[0] != 0) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        uint32_t dns_deadline = millis() + 10000;
        int attempt = 0;
        while (!dns_ok && millis() < dns_deadline) {
            attempt++;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
                uint32_t ip_raw = sin->sin_addr.s_addr;
                /* getaddrinfo 可能返回 0.0.0.0（DNS 未就绪），要重试 */
                if (ip_raw != 0 && ip_raw != htonl(INADDR_LOOPBACK)) {
                    dns_ip = IPAddress(ip_raw);
                    dns_ok = true;
                } else {
                    ESP_LOGW(TAG, "DNS attempt %d: getaddrinfo 返回 %lu，等 500ms",
                             attempt, (unsigned long)ip_raw);
                }
                freeaddrinfo(res);
            } else {
                ESP_LOGW(TAG, "DNS attempt %d: getaddrinfo 失败", attempt);
            }
            if (!dns_ok) vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    ESP_LOGI(TAG, "DNS 预检: host=%s 解析=%s ip=%s",
             host[0] ? host : "(未知)",
             dns_ok ? "成功" : "失败（诊断用，不阻断连接）",
             dns_ok ? dns_ip.toString().c_str() : "无");
    /* DNS 预检只做诊断，不阻断 —— ArduinoWebsockets 会自己再走一遍，
     * 那条路径用的是 WiFiClient 的 DNS（和 TTS 成功时的路径一致）*/
    if (!dns_ok) {
        ESP_LOGW(TAG, "DNS 预检失败，但继续尝试连接...");
    }

    /* ===== 诊断 2：TLS 需要大块连续内部堆（~40KB），碎片化会秒败 ===== */
    ESP_LOGI(TAG, "内存: 内部堆剩余=%u 最小空闲=%u 总堆=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)esp_get_free_heap_size());

    /* ===== 诊断 3：裸 TCP 443 测试 —— 如果也秒败，是网络层/WiFi 配置问题；
     * 如果成功，说明 ArduinoWebsockets 库或 TLS 层有 bug。 */
    {
        ::WiFiClient test;
        uint32_t t0 = millis();
        bool tcp_ok = test.connect(dns_ip, 443);
        uint32_t tcost = millis() - t0;
        ESP_LOGI(TAG, "裸 TCP 443 测试: ip=%s result=%d, 耗时=%u ms",
                 dns_ip.toString().c_str(), tcp_ok ? 1 : 0, tcost);
        if (tcp_ok) {
            test.stop();
            ESP_LOGI(TAG, "→ TCP 通，问题在 ArduinoWebsockets 库或 TLS 层");
        } else {
            ESP_LOGE(TAG, "→ TCP 也败，问题在网络层/WiFi 配置");
        }
    }

    /* ===== 先放开 SSL 验证试试（ESP32-S3 内置 CA 链有时不全）。
     * 如果 setInsecure() 能连上，说明是 SSL 证书验证问题，
     * 后面再补正确的 CA 列表。 */
    wb->client.setInsecure();

    ESP_LOGI(TAG, "开始连接: %s", url);
    ESP_LOGI(TAG, "SSL 验证已放开 (setInsecure)");

    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    bool ret = wb->client.connect(url);
    uint32_t cost_ms = (uint32_t)(xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "connect() 返回=%d, connected flag=%d, 耗时=%u ms",
             ret ? 1 : 0, wb->connected ? 1 : 0, cost_ms);

    if (!ret) {
        /* connect() 是同步接口：返回 false = DNS/TCP/TLS 已死，
         * 不再空等（旧版这里死循环 poll 直到超时，错误被吞掉） */
        ESP_LOGE(TAG, "连接失败（connect()=false），间隔 %u ms —— "
                 "DNS 通但握手死，多半是 TLS 内存/网络层问题",
                 cost_ms);
        return false;
    }

    /* connect()=true 但事件回调还没置位时短轮询（防御性，通常瞬间完成） */
    if (!wb->connected) {
        uint32_t tw = (uint32_t)xTaskGetTickCount();
        while (!wb->connected) {
            wb->client.poll();
            if ((uint32_t)xTaskGetTickCount() - tw >= pdMS_TO_TICKS(timeout_ms)) {
                ESP_LOGE(TAG, "连接超时（事件未触发），已等 %d ms", timeout_ms);
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    ESP_LOGI(TAG, "ws_bridge_connect 成功: connected=%d",
             wb->connected ? 1 : 0);
    return wb->connected;
}

extern "C" bool ws_bridge_send_text(ws_bridge_t *wb, const char *data, size_t len)
{
    if (!wb || !wb->connected) return false;
    return wb->client.send(String(data, (unsigned int)len));
}

extern "C" bool ws_bridge_send_binary(ws_bridge_t *wb, const uint8_t *data, size_t len)
{
    if (!wb || !wb->connected) return false;
    return wb->client.send((const char *)data, len);
}

extern "C" void ws_bridge_poll(ws_bridge_t *wb)
{
    if (!wb) return;
    wb->client.poll();
}

extern "C" void ws_bridge_close(ws_bridge_t *wb)
{
    if (!wb) return;
    if (wb->connected) {
        wb->client.close();
        wb->connected = false;
    }
}

extern "C" void ws_bridge_destroy(ws_bridge_t *wb)
{
    if (!wb) return;
    delete wb;
}

extern "C" bool ws_bridge_connected(ws_bridge_t *wb)
{
    return wb ? wb->connected : false;
}
