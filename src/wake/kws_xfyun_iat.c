/**
 * @file     kws_xfyun_iat.c
 * @brief    讯飞 RTASR（实时语音转写标准版）在线引擎实现
 *
 * 协议：wss://rtasr.xfyun.cn/v1/ws
 *   握手: URL 拼 appid + ts + signa（signa=base64(HMAC_SHA1(MD5(appid+ts), apiKey))）
 *   握手后: 直接发 binary audio message（16kHz/16bit/mono, 40ms=1280字节/包）
 *   结束: 发 binary {"end": true}
 *   返回: 外层 {action:"started/result/error", code, data, sid}
 *         data 是嵌套 JSON: {"cn":{"st":{"rt":[{"ws":[{"cw":[{"w":"..."}]}]}],"type":"1|0"}},"seg_id":N}
 *           type="0" = 最终段结果；type="1" = 中间结果
 *
 * 修改：2026-11-19 从旧 IAT 切到 RTASR（原生支持控制台热词表）
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "inmp441_bridge.h"
#include "ws_bridge.h"
#include "xfyun_auth.h"
#include "kws_xfyun_iat.h"

static const char *TAG = "kws_iat";   /* 规则 4-8 */

/* ===== 讯飞 RTASR 连接参数 ===== */
#define IAT_HOST    "rtasr.xfyun.cn"
#define IAT_PATH    "/v1/ws"

/* ===== 音频参数 ===== */
#define IAT_SAMPLE_RATE   16000
#define IAT_BLOCK_SAMPLES 5120   /* 320ms @16kHz，10240 bytes PCM */
#define IAT_BLOCK_BYTES   (IAT_BLOCK_SAMPLES * sizeof(int16_t))

/* ===== 静音超时 ===== */
#define IAT_SILENCE_TIMEOUT_MS  2000   /* 2 秒没声音 → 发结束帧 → 重置会话 */

/* ===== 音频缓冲大小 ===== */
#define IAT_ACK_MAX        2048
#define IAT_URL_MAX        512    /* RTASR URL 比旧 IAT 短得多 */

/* ===== 会话状态 ===== */
#define IAT_STATE_LISTENING   0   /* 监听唤醒词 */
#define IAT_STATE_AWAKE       1   /* 已唤醒，收集指令文字 */
#define IAT_STATE_ERROR      -1

/* ===== 静态状态 ===== */
static struct {
    char appid[32];
    char api_key[64];
    char api_secret[64];
    char wake_word[64];
    kws_event_cb_t cb;
    void          *cb_ctx;

    ws_bridge_t *ws;

    volatile bool  running;
    volatile bool  paused;
    TaskHandle_t   task;
    SemaphoreHandle_t done;
    volatile bool  failed;

    int    session_state;        /* IAT_STATE_xxx */
    int64_t last_voice_us;       /* 最后一次检测到语音的时间戳 */
    int64_t session_start_us;    /* 当前会话开始时间（唤醒后）*/
    char   accum_text[256];      /* 当前累积的指令文字（唤醒后）*/
    char   interim_text[128];    /* 最新一次中间识别结果 */
    char   wake_buffer[128];     /* LISTENING 状态跨帧累积识别结果，用于匹配唤醒词 */
    bool   ws_opened;            /* WebSocket 是否已连接（等 WS_BRIDGE_EVENT_OPENED）*/
    bool   first_frame_sent;     /* 首帧是否已发送 */
    int64_t session_open_us;     /* 当前会话打开时间（用于清理 wake_buffer）*/
} s;

/* ================================================================
 *  RTASR 音频发送（握手后直接发 binary message，不 JSON 不 base64）
 * ================================================================ */

/* 发送音频帧：直接 binary message */
static bool send_audio_frame(const uint8_t *audio, size_t audio_len)
{
    return ws_bridge_send_binary(s.ws, audio, audio_len);
}

/* 结束当前 RTASR 会话：发 binary {"end": true} */
static bool send_end_frame(void)
{
    ESP_LOGI(TAG, "发送结束帧 {\"end\": true}，结束当前 RTASR 会话");
    s.first_frame_sent = false;
    const char *end_msg = "{\"end\": true}";
    return ws_bridge_send_binary(s.ws, (const uint8_t *)end_msg, strlen(end_msg));
}

/* 重置识别状态（静音超时后 / 错误恢复 / 新会话开始）*/
static void reset_session(void)
{
    s.session_state = IAT_STATE_LISTENING;
    s.accum_text[0] = '\0';
    s.interim_text[0] = '\0';
    s.wake_buffer[0] = '\0';
    s.session_start_us = 0;
    s.session_open_us = 0;
}

/* ================================================================
 *  RTASR 结果解析（外层 JSON → data 里再嵌套一层 JSON）
 * ================================================================ */

static void handle_iat_result(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *code   = cJSON_GetObjectItem(root, "code");

    /* action = "started" → 握手成功，可以开始发音频了 */
    if (action && cJSON_IsString(action) && strcmp(action->valuestring, "started") == 0) {
        ESP_LOGI(TAG, "🎯 RTASR 握手成功（started）");
        s.first_frame_sent = true;   /* 握手完直接发音频，不需要额外首帧 */
        cJSON_Delete(root);
        return;
    }

    /* action = "error" → 鉴权失败等 */
    if (action && cJSON_IsString(action) && strcmp(action->valuestring, "error") == 0) {
        cJSON *desc = cJSON_GetObjectItem(root, "desc");
        ESP_LOGE(TAG, "RTASR 错误 code=%s desc=%s",
                 code && cJSON_IsString(code) ? code->valuestring : "?",
                 desc && cJSON_IsString(desc) ? desc->valuestring : "?");
        s.failed = true;
        cJSON_Delete(root);
        return;
    }

    /* action = "result" → 识别结果 */
    if (!action || !cJSON_IsString(action) || strcmp(action->valuestring, "result") != 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsString(data) || !data->valuestring) { cJSON_Delete(root); return; }

    /* data 里是嵌套 JSON 字符串，再 Parse 一次 */
    cJSON *inner = cJSON_Parse(data->valuestring);
    if (!inner) { cJSON_Delete(root); return; }

    /* 提取 cn.st.rt[].ws[].cw[].w 的文字 */
    cJSON *cn = cJSON_GetObjectItem(inner, "cn");
    if (!cn) { cJSON_Delete(inner); cJSON_Delete(root); return; }
    cJSON *st = cJSON_GetObjectItem(cn, "st");
    if (!st) { cJSON_Delete(inner); cJSON_Delete(root); return; }
    cJSON *rt = cJSON_GetObjectItem(st, "rt");
    if (!rt || !cJSON_IsArray(rt)) { cJSON_Delete(inner); cJSON_Delete(root); return; }

    /* 合并所有 ws 段的文字 */
    char merged[128] = {0};
    for (int r = 0; r < cJSON_GetArraySize(rt); r++) {
        cJSON *rt_item = cJSON_GetArrayItem(rt, r);
        if (!rt_item) continue;
        cJSON *ws = cJSON_GetObjectItem(rt_item, "ws");
        if (!ws) continue;
        for (int i = 0; i < cJSON_GetArraySize(ws); i++) {
            cJSON *w = cJSON_GetArrayItem(ws, i);
            cJSON *cw = cJSON_GetObjectItem(w, "cw");
            if (!cw) continue;
            cJSON *cw0 = cJSON_GetArrayItem(cw, 0);
            cJSON *w_text = cJSON_GetObjectItem(cw0, "w");
            if (w_text && cJSON_IsString(w_text)) {
                strncat(merged, w_text->valuestring, sizeof(merged) - strlen(merged) - 1);
            }
        }
    }

    /* type: "0"=最终段结果, "1"=中间结果 */
    cJSON *type = cJSON_GetObjectItem(st, "type");
    bool is_final = (type && cJSON_IsString(type) && strcmp(type->valuestring, "0") == 0);

    cJSON_Delete(inner);
    cJSON_Delete(root);

    if (merged[0] == '\0') return;

    ESP_LOGI(TAG, "RTASR 结果 [%s]: %s", is_final ? "FINAL" : "INTERIM", merged);

    /* ============ 监听唤醒词阶段 ============ */
    if (s.session_state == IAT_STATE_LISTENING) {
        if (merged[0]) {
            size_t wb_len = strlen(s.wake_buffer);
            size_t need  = strlen(merged);
            if (wb_len + need + 1 < sizeof(s.wake_buffer)) {
                strcat(s.wake_buffer, merged);
            } else {
                memmove(s.wake_buffer, merged, need + 1);
            }
        }
        /* 精确搜唤醒词（控制台热词表已保证源头正确识别"小智"）*/
        if (strstr(s.wake_buffer, s.wake_word)) {
            ESP_LOGI(TAG, "🎯 唤醒触发！累积识别到: %s", s.wake_buffer);
            if (s.cb) s.cb(KWS_EV_WAKE, s.wake_buffer, s.cb_ctx);
            s.session_state = IAT_STATE_AWAKE;
            s.session_start_us = esp_timer_get_time();
            s.accum_text[0] = '\0';
        }
    }
    /* ============ 已唤醒：收集指令 ============ */
    else if (s.session_state == IAT_STATE_AWAKE) {
        if (is_final) {
            strncat(s.accum_text, merged,
                    sizeof(s.accum_text) - strlen(s.accum_text) - 1);
            ESP_LOGI(TAG, "指令累积: %s", s.accum_text);

            /* 静音超时检测 */
            if (esp_timer_get_time() - s.session_start_us > 10000000LL) {
                ESP_LOGI(TAG, "唤醒后超过 10 秒，发指令");
                if (s.cb && s.accum_text[0]) {
                    s.cb(KWS_EV_TEXT, s.accum_text, s.cb_ctx);
                }
                send_end_frame();
                reset_session();
            }
        }
    }
}

/* ================================================================
 *  WebSocket 回调
 * ================================================================ */

static void cb_msg(const char *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!data || len == 0) return;
    ESP_LOGI(TAG, "📨 收到讯飞消息 len=%u: %.*s", (unsigned)len,
             (int)(len > 200 ? 200 : (int)len), data);
    handle_iat_result(data);
}

static void cb_evt(ws_bridge_event_t event, void *ctx)
{
    (void)ctx;
    switch (event) {
    case WS_BRIDGE_EVENT_OPENED:
        ESP_LOGI(TAG, "IAT WebSocket 已连接");
        s.ws_opened = true;
        break;
    case WS_BRIDGE_EVENT_CLOSED:
        ESP_LOGW(TAG, "IAT WebSocket 断开");
        s.ws_opened = false;
        break;
    case WS_BRIDGE_EVENT_ERROR:
        ESP_LOGE(TAG, "IAT WebSocket 错误");
        s.failed = true;
        if (s.done) xSemaphoreGive(s.done);
        break;
    default: break;
    }
}

/* ================================================================
 *  主任务循环
 * ================================================================ */

/* 尝试连接一次 WebSocket（返回 true=已连接且 opened）*/
static bool iat_connect_once(const char *url)
{
    if (!s.ws) return false;
    s.ws_opened = false;

    if (!ws_bridge_connect(s.ws, url, 15000)) {
        ESP_LOGW(TAG, "IAT WS connect() 失败");
        return false;
    }
    /* 等 ConnectionOpened 事件 */
    TickType_t twait = xTaskGetTickCount();
    while (!s.ws_opened && (xTaskGetTickCount() - twait) < pdMS_TO_TICKS(10000)) {
        ws_bridge_poll(s.ws);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s.ws_opened) {
        ESP_LOGW(TAG, "IAT WS 等 opened 超时");
        ws_bridge_close(s.ws);
        return false;
    }
    ESP_LOGI(TAG, "IAT WebSocket 已就绪");
    return true;
}

/* 关闭 WS 但保留 ws_bridge 对象（resume 时复用）*/
static void iat_disconnect(void)
{
    if (s.ws_opened && s.ws) {
        if (s.first_frame_sent) {
            send_end_frame();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ws_bridge_close(s.ws);
    }
    s.ws_opened = false;
    s.first_frame_sent = false;
    reset_session();
}

static void iat_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "讯飞 IAT 监听任务启动，唤醒词=\"%s\"", s.wake_word);

    /* 等 SNTP 对时 —— 必须等对完再生成鉴权 URL，否则 ts expired */
    if (xfyun_wait_sntp(15) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 未同步，放弃 RTASR 连接（等下次重连再试）");
        vTaskDelay(pdMS_TO_TICKS(5000));   /* 等 WiFi/NTP 有机会恢复 */
        vTaskDelete(NULL);
        return;
    }

    /* 1. 生成 RTASR 鉴权 URL（放堆上，避免栈溢出）*/
    char *url = (char *)heap_caps_malloc(IAT_URL_MAX, MALLOC_CAP_SPIRAM);
    if (!url) url = (char *)malloc(IAT_URL_MAX);
    if (!url) { ESP_LOGE(TAG, "URL 缓冲分配失败"); vTaskDelete(NULL); return; }
    if (xfyun_build_rtasr_url(s.appid, s.api_key, url, IAT_URL_MAX) != ESP_OK) {
        ESP_LOGE(TAG, "RTASR 鉴权 URL 生成失败");
        s.failed = true;
        free(url);
        vTaskDelete(NULL);
        return;
    }

    /* 2. 创建 ws_bridge（全程只创建一次）*/
    s.ws = ws_bridge_create();
    if (!s.ws) { ESP_LOGE(TAG, "ws_bridge 创建失败"); free(url); vTaskDelete(NULL); return; }
    ws_bridge_set_callbacks(s.ws, cb_msg, cb_evt, NULL);

    /* 3. 主循环：pause 时关 WS，resume 时重建；断线自动重连 */
    int16_t *block = (int16_t *)heap_caps_malloc(IAT_BLOCK_BYTES, MALLOC_CAP_SPIRAM);
    if (!block) block = (int16_t *)malloc(IAT_BLOCK_BYTES);
    if (!block) { ESP_LOGE(TAG, "block 缓冲分配失败"); free(url); goto _cleanup; }

    int64_t last_silence_check = esp_timer_get_time();
    int     reconnect_interval_ms = 1000;   /* 重连间隔递增 */

    while (s.running) {
        /* ==== pause 状态：关 WS，释放 TLS，省电 ==== */
        if (s.paused) {
            if (s.ws_opened) {
                ESP_LOGI(TAG, "pause 中，关闭 IAT WebSocket");
                iat_disconnect();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            reconnect_interval_ms = 1000;   /* 恢复后重置重连间隔 */
            continue;
        }

        /* ==== WS 未连接 → 尝试重连 ==== */
        if (!s.ws_opened) {
            if (iat_connect_once(url)) {
                reconnect_interval_ms = 1000;
            } else {
                ESP_LOGW(TAG, "IAT WS 重连失败，%d ms 后重试", reconnect_interval_ms);
                vTaskDelay(pdMS_TO_TICKS(reconnect_interval_ms));
                /* 指数退避：最多 15s */
                if (reconnect_interval_ms < 15000) reconnect_interval_ms *= 2;
            }
            continue;
        }

        /* ==== 正常工作：poll + 读麦克风 + 发帧 ==== */
        ws_bridge_poll(s.ws);

        size_t got = 0;
        esp_err_t r = inmp441_read(block, IAT_BLOCK_BYTES, &got, 500);
        if (r != ESP_OK || got < IAT_BLOCK_BYTES) {
            static uint32_t err_count = 0;
            if (err_count++ % 100 == 0) {
                ESP_LOGW(TAG, "MIC read r=%d got=%u/%u", (int)r, (unsigned)got, (unsigned)IAT_BLOCK_BYTES);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 音量检测 */
        int32_t max_abs = 0;
        int32_t sum = 0;
        for (int i = 0; i < IAT_BLOCK_SAMPLES; i++) {
            int32_t v = block[i] < 0 ? -block[i] : block[i];
            if (v > max_abs) max_abs = v;
            sum += v;
        }
        if (max_abs > 600) {
            s.last_voice_us = esp_timer_get_time();
        }
        /* 每 5 秒打一次音量，便于验证麦克风 */
        static int64_t last_vol_log = 0;
        if ((esp_timer_get_time() - last_vol_log) > 5000000LL) {
            last_vol_log = esp_timer_get_time();
            ESP_LOGI(TAG, "MIC音量: max=%d avg=%d 有声=%d",
                     (int)max_abs, (int)(sum / IAT_BLOCK_SAMPLES), max_abs > 600);
        }

        /* 发送音频帧 — RTASR 握手完直接发 binary PCM，不需要 status 区分首/中/尾帧
         * 注意：握手 started 事件里会设置 s.first_frame_sent=true */
        if (!s.first_frame_sent) {
            /* 还没收到 started 事件，暂时不发（WS 虽连上但 RTASR 还没握手成功）*/
            vTaskDelay(pdMS_TO_TICKS(20));
            ws_bridge_poll(s.ws);   /* poll 一下尽快收到 started */
            continue;
        }

        if (send_audio_frame((uint8_t *)block, IAT_BLOCK_BYTES)) {
            /* 静音门控通过后正常发，不需要额外日志 */
        } else {
            ESP_LOGW(TAG, "发送音频帧失败，可能 WS 已断");
            iat_disconnect();
            continue;
        }

        /* 静音超时检测 */
        int64_t now = esp_timer_get_time();
        if (now - last_silence_check >= pdMS_TO_TICKS(200) * portTICK_PERIOD_MS) {
            last_silence_check = now;
            bool is_silent = (now - s.last_voice_us) > IAT_SILENCE_TIMEOUT_MS * 1000LL;

            if (s.session_state == IAT_STATE_AWAKE && is_silent && s.accum_text[0]) {
                ESP_LOGI(TAG, "静音超时 2s，发送指令");
                if (s.cb) s.cb(KWS_EV_TEXT, s.accum_text, s.cb_ctx);
                send_end_frame();
                reset_session();
                vTaskDelay(pdMS_TO_TICKS(300));
                s.first_frame_sent = false;
            }
        }
    }

    /* ==== 任务退出 ==== */
    iat_disconnect();
    if (s.ws) { ws_bridge_destroy(s.ws); s.ws = NULL; }

_cleanup:
    free(block);
    free(url);

    ESP_LOGI(TAG, "讯飞 IAT 监听任务退出");
    vTaskDelete(NULL);
}

/* ================================================================
 *  对外接口
 * ================================================================ */

esp_err_t kws_iat_init(const char *appid, const char *api_key, const char *api_secret,
                        const char *wake_word,
                        kws_event_cb_t cb, void *cb_ctx)
{
    if (!appid || !api_key || !api_secret || !wake_word || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s, 0, sizeof(s));
    strncpy(s.appid, appid, sizeof(s.appid) - 1);
    strncpy(s.api_key, api_key, sizeof(s.api_key) - 1);
    strncpy(s.api_secret, api_secret, sizeof(s.api_secret) - 1);
    strncpy(s.wake_word, wake_word, sizeof(s.wake_word) - 1);
    s.cb = cb;
    s.cb_ctx = cb_ctx;

    ESP_LOGI(TAG, "讯飞 IAT 初始化：appid=%s 唤醒词=[%s]", s.appid, s.wake_word);
    return ESP_OK;
}

esp_err_t kws_iat_start(void)
{
    if (s.running) { ESP_LOGW(TAG, "已在监听"); return ESP_OK; }
    if (!s.wake_word[0]) { ESP_LOGE(TAG, "未初始化"); return ESP_ERR_INVALID_STATE; }

    /* 不在这里等 SNTP —— WiFi 还没连！在 iat_task() 真正连 WS 之前再等 */

    s.running = true;
    s.paused = false;
    s.failed = false;

    BaseType_t ok = xTaskCreate(iat_task, "iat", 24576, NULL, 4, &s.task);
    if (ok != pdPASS) {
        s.running = false;
        ESP_LOGE(TAG, "IAT 任务创建失败（堆不够）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kws_iat_stop(void)
{
    s.running = false;
    s.paused = false;
    reset_session();

    for (int i = 0; i < 50 && s.task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s.task = NULL;
    return ESP_OK;
}

esp_err_t kws_iat_pause(void)
{
    s.paused = true;
    return ESP_OK;
}

esp_err_t kws_iat_resume(void)
{
    s.paused = false;
    return ESP_OK;
}

bool kws_iat_is_listening(void)
{
    return s.running && !s.paused;
}
