/**
 * @file     app_ai.c
 * @brief    AI 大模型接入（天翼云 eaichat，OpenAI 兼容 /chat/completions）
 *
 * 接口：POST https://eaichat.ctyun.cn/ai/platform/v2/cp/chat/completions
 *   Header: Authorization: Bearer <api-key>, Content-Type: application/json
 *   Body  : {"model":M,"messages":[{"role":"user","content":"..."}],"stream":false}
 *   返回  : OpenAI JSON，正文在 choices[0].message.content。
 *
 * 密钥来源：优先 NVS（规则 4-1），无值时用内置默认值兜底。
 * 修改：2026-11-20 引入 secret_store，去掉硬编码 AI_URL/AI_MODEL/AI_KEY
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "tts_hub.h"
#include "kws_hub.h"
#include "secret_store.h"
#include "event_bus.h"

#include "app_ai.h"

static const char *TAG = "app_ai";

/* ===== 默认接入参数（仅 NVS 无值时兜底）===== */
static const char *DFLT_AI_URL   = "https://eaichat.ctyun.cn/ai/platform/v2/cp/chat/completions";
static const char *DFLT_AI_HOST  = "eaichat.ctyun.cn";
static const char *DFLT_AI_MODEL = "deepseek-chat";
static const char *DFLT_AI_KEY   = "sk-16d0c7b05e70ffe24357494bb0743704dd373801bc1237c101ad99141a5e6df9f412d3e5";

#define AI_RX_CAP     16384      /* 响应缓冲上限 */
#define AI_QA_STACK   16384       /* HTTPS(TLS) + cJSON 需要较大栈 */
#define AI_REPLY_MAX  1024

/* ===== 运行时配置（NVS 优先，懒加载）===== */
static char s_url[128];
static char s_host[64];
static char s_model[48];
static char s_key[128];
static bool s_cfg_loaded = false;

static char  *s_rx;             /* 响应缓冲（优先 PSRAM，规则 7-1） */
static size_t s_rxlen;
static bool   s_rx_ovf;

/* 从 s_url 里提取 host：https://eaichat.ctyun.cn/xxx → eaichat.ctyun.cn */
static void s_extract_host_from_url(const char *url, char *host, size_t host_sz)
{
    const char *start = strstr(url, "://");
    if (start) start += 3;
    else start = url;
    const char *end = start;
    while (*end && *end != '/' && *end != ':') end++;
    size_t n = (size_t)(end - start);
    if (n >= host_sz) n = host_sz - 1;
    memcpy(host, start, n);
    host[n] = '\0';
}

/* 懒加载配置：第一次调 AI 时才读 NVS */
static void s_ensure_cfg(void)
{
    if (s_cfg_loaded) return;
    s_cfg_loaded = true;

    char url[128] = { 0 };
    char host[64] = { 0 };
    char model[48] = { 0 };
    char key[128] = { 0 };

    /* 读 NVS，不存在就用默认值 */
    if (secret_store_get_str("ai_url", url, sizeof(url), NULL) != ESP_OK) {
        strncpy(url, DFLT_AI_URL, sizeof(url) - 1);
        ESP_LOGW(TAG, "NVS 无 ai_url，使用默认值");
    }
    if (secret_store_get_str("ai_model", model, sizeof(model), NULL) != ESP_OK) {
        strncpy(model, DFLT_AI_MODEL, sizeof(model) - 1);
        ESP_LOGW(TAG, "NVS 无 ai_model，使用默认值");
    }
    if (secret_store_get_str("ai_api_key", key, sizeof(key), NULL) != ESP_OK) {
        strncpy(key, DFLT_AI_KEY, sizeof(key) - 1);
        ESP_LOGW(TAG, "NVS 无 ai_api_key，使用默认值");
    }
    /* host 优先 NVS，没有就从 url 提取 */
    if (secret_store_get_str("ai_host", host, sizeof(host), NULL) != ESP_OK) {
        s_extract_host_from_url(url, host, sizeof(host));
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    strncpy(s_host, host, sizeof(s_host) - 1);
    strncpy(s_model, model, sizeof(s_model) - 1);
    strncpy(s_key, key, sizeof(s_key) - 1);

    ESP_LOGI(TAG, "AI 配置就绪: model=%s host=%s", s_model, s_host);
}

/* 简单 JSON 字符串转义（" \ 控制字符） */
static void json_escape(const char *in, char *out, size_t out_n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < out_n; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) { o += (size_t)snprintf(out + o, out_n - o, "\\u%04x", c); }
            else          { out[o++] = (char)c; }
        }
    }
    out[o] = '\0';
}

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data && e->data_len > 0) {
        if (s_rxlen + e->data_len < AI_RX_CAP) {
            memcpy(s_rx + s_rxlen, e->data, e->data_len);
            s_rxlen += e->data_len;
            s_rx[s_rxlen] = '\0';
        } else {
            s_rx_ovf = true;
        }
    }
    return ESP_OK;
}

/* 从 choices[0].message.content 取正文；若混入 </think> 则取其后的内容 */
static void extract_content(const char *json, char *out, size_t out_sz)
{
    out[0] = '\0';
    cJSON *j = cJSON_Parse(json);
    if (!j) return;
    cJSON *choices = cJSON_GetObjectItem(j, "choices");
    cJSON *c0 = cJSON_GetArrayItem(choices, 0);
    cJSON *msg = cJSON_GetObjectItem(c0, "message");
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content) && content->valuestring) {
        const char *s = content->valuestring;
        const char *tag = strstr(s, "<｜end▁of▁thinking｜>");
        if (tag) s = tag + 8;   /* 跳过思考段 */
        strncpy(out, s, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
    cJSON_Delete(j);
}

esp_err_t app_ai_probe(void)
{
    s_ensure_cfg();   /* 确保配置已加载 */

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(s_host, "443", &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "探测失败：域名解析失败");
        return ESP_FAIL;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) { freeaddrinfo(res); return ESP_FAIL; }

    struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int r = connect(s, res->ai_addr, res->ai_addrlen);
    close(s);
    freeaddrinfo(res);
    if (r == 0) {
        ESP_LOGI(TAG, "连通性探测 OK (%s:443)", s_host);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "连通性探测失败 (connect rc=%d)", r);
    return ESP_FAIL;
}

esp_err_t app_ai_chat(const char *question, char *out, size_t out_sz)
{
    if (!question || !question[0] || !out || out_sz == 0) return ESP_ERR_INVALID_ARG;

    s_ensure_cfg();   /* 确保配置已加载（规则 3.1 延迟初始化） */

    if (!s_rx) {
        s_rx = heap_caps_malloc(AI_RX_CAP, MALLOC_CAP_SPIRAM);   /* 规则 7-1 PSRAM 优先 */
        if (!s_rx) s_rx = malloc(AI_RX_CAP);
        if (!s_rx) { ESP_LOGE(TAG, "响应缓冲分配失败"); return ESP_ERR_NO_MEM; }
    }
    s_rxlen = 0; s_rx_ovf = false; s_rx[0] = '\0';

    char esc[1400];
    json_escape(question, esc, sizeof(esc));

    size_t body_n = strlen(esc) + 256;
    char *body = malloc(body_n);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body, body_n,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"stream\":false}",
             s_model, esc);

    char auth[220];
    snprintf(auth, sizeof(auth), "Bearer %s", s_key);

    esp_http_client_config_t cfg = {
        .url               = s_url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 40000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_evt,
        .buffer_size       = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(body); return ESP_FAIL; }

    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Connection", "close");   /* 及时释放，别和后续 TTS 的 TLS 抢占 */
    esp_http_client_set_post_field(c, body, strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);

    if (err != ESP_OK || status != 200 || s_rxlen == 0) {
        ESP_LOGE(TAG, "AI 失败 (err=%s status=%d rx=%u ovf=%d)",
                 esp_err_to_name(err), status, (unsigned)s_rxlen, s_rx_ovf);
        return ESP_FAIL;
    }

    extract_content(s_rx, out, out_sz);
    if (out[0] == '\0') {
        ESP_LOGE(TAG, "AI 回复为空（可能超长被截断 ovf=%d）", s_rx_ovf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AI 回复: %s", out);
    return ESP_OK;
}

/* ===== 语音问答：AI → publish EVT_AI_REPLY_READY → TTS 播报 → 状态机自动回 LISTENING =====
 *
 * 设计（规则 8/9）：
 *   - AI 对话结果通过 event_bus 发布，状态机自动从 PROCESSING → SPEAKING
 *   - 状态机在 SPEAKING 下播完 TTS → 自动进 LISTENING（并 kws_hub_resume）
 *   - qa_task 不再直接调 kws_hub_resume，让状态机统一管
 *   - qa_task 不再直接调 kws_hub_pause（wake_cb 或状态机已 pause 过了）*/
static void qa_task(void *arg)
{
    char *q = (char *)arg;
    char *reply = malloc(AI_REPLY_MAX);

    if (!reply) {
        event_bus_publish(EVT_AI_REPLY_READY, (void *)"系统内存不足。");
        tts_hub_speak_async("系统内存不足。");
    } else {
        reply[0] = '\0';
        if (app_ai_chat(q, reply, AI_REPLY_MAX) == ESP_OK) {
            event_bus_publish(EVT_AI_REPLY_READY, reply);
            tts_hub_speak_async(reply);
        } else {
            event_bus_publish(EVT_AI_REPLY_READY, (void *)"ai_fail");
            tts_hub_speak_async("抱歉，AI 暂时没有回应。");
        }
        free(reply);
    }
    free(q);

    /* 等 TTS 播完（状态机会自动 FSM_SPEAKING → FSM_LISTENING + kws_hub_resume）
     * 这里只是等一下确保事件总线订阅者有机会响应完 */
    vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(NULL);
}

void app_ai_qa_async(const char *question)
{
    /* 状态机已在 FSM_PROCESSING，TTS/HUB 暂停都由状态机管，
     * 这里只负责把 AI 对话甩给一个 FreeRTOS 任务异步执行 */
    char *c = question ? strdup(question) : NULL;
    if (!c) { tts_hub_speak_async("系统内存不足。"); return; }
    if (xTaskCreate(qa_task, "ai_qa", AI_QA_STACK, c, 4, NULL) != pdPASS) {
        free(c);
        ESP_LOGE(TAG, "AI QA 任务创建失败");
        tts_hub_speak_async("系统繁忙，请稍后再试。");
    }
}

/* ===== 联网自检：连接成功播报 ===== */
static void boot_ping_task(void *arg)
{
    (void)arg;
    while (tts_hub_is_busy()) vTaskDelay(pdMS_TO_TICKS(300));
    vTaskDelay(pdMS_TO_TICKS(500));

    char reply[64];
    esp_err_t e = app_ai_chat("你好", reply, sizeof(reply));
    tts_hub_speak_async(e == ESP_OK ? "AI模型连接成功。" : "AI模型连接失败。");
    vTaskDelete(NULL);
}

void app_ai_on_online(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    if (xTaskCreate(boot_ping_task, "ai_ping", AI_QA_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "AI 自检任务创建失败");
    }
}
