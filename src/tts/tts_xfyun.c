/* 科大讯飞 WebSocket TTS 驱动 — Arduino 版
 *
 * 原版依赖 esp_websocket_client（ESP-IDF 原生），Arduino ESP32 Core 不带该组件。
 * 本版改为 ArduinoWebsockets（gilmaimon/ArduinoWebsockets@0.5.4），
 * 通过 ws_bridge 桥接层让 C 文件不直接碰 C++ 头文件。
 *
 * JSON 流式解析状态机、鉴权 URL 生成、mbedtls base64 解码原封不动。
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "ws_bridge.h"
#include "tts_hub.h"
#include "tts_xfyun.h"
#include "xfyun_auth.h"

static const char *TAG = "tts_xfyun";   /* 规则 4-8 */

#define XF_HOST     "tts-api.xfyun.cn"
#define XF_PORT     443
#define XF_PATH     "/v2/tts"

/* 运行时配置：init 时以结构体注入，组件内零硬编码密钥 */
static tts_xfyun_config_t s_cfg = {0};

/* ============ 流式 JSON 解析状态机（原封不动） ============ */
typedef enum {
    XS_SCAN = 0, XS_KEY, XS_COLON, XS_VSTART, XS_STRVAL, XS_NUMVAL,
} xf_state_t;

#define XF_MARK_CODE   (1u << 0)
#define XF_MARK_MSG    (1u << 1)
#define XF_MARK_AUDIO  (1u << 2)
#define XF_MARK_STATUS (1u << 3)
#define XF_B64_BUF     3072
#define XF_MSG_LEN     96

typedef struct {
    const char   *text;
    tts_pcm_cb_t  cb;
    void         *uctx;
    SemaphoreHandle_t done;
    volatile bool failed;
    ws_bridge_t  *ws;

    xf_state_t xs;
    size_t     mark;
    bool       esc;
    bool       in_audio;
    char       key[16];
    size_t     key_len;
    char       val[24];
    size_t     val_len;
    char       msg[XF_MSG_LEN];
    size_t     msg_len;
    int        code;
    bool       has_code;
    int        status;
    uint8_t    b64[XF_B64_BUF];
    size_t     b64_len;
    bool       frame_done;
} xfyun_job_t;

static volatile bool s_busy = false;
static xfyun_job_t s_job;
static char s_url[2600];

static uint32_t s_audio_batches;
static uint32_t s_audio_bytes;

static void mark_apply(xfyun_job_t *job, const char *key) {
    if      (!strcmp(key, "code"))    job->mark = XF_MARK_CODE;
    else if (!strcmp(key, "message")) job->mark = XF_MARK_MSG;
    else if (!strcmp(key, "audio"))   job->mark = XF_MARK_AUDIO;
    else if (!strcmp(key, "status"))  job->mark = XF_MARK_STATUS;
    else                              job->mark = 0;
}

static void field_end(xfyun_job_t *job) {
    job->val[job->val_len] = '\0';
    switch (job->mark) {
    case XF_MARK_CODE:    job->code = atoi(job->val); job->has_code = true; break;
    case XF_MARK_MSG: {
        size_t n = job->val_len;
        if (n > XF_MSG_LEN - 1) n = XF_MSG_LEN - 1;
        memcpy(job->msg, job->val, n); job->msg[n] = '\0'; job->msg_len = n;
        break;
    }
    case XF_MARK_STATUS:  job->status = atoi(job->val); break;
    default: break;
    }
}

static void audio_feed(xfyun_job_t *job, char ch) {
    if (ch == '=') return;
    if (ch != '\0') {
        job->b64[job->b64_len++] = (uint8_t)ch;
        if (job->b64_len < XF_B64_BUF) return;
    }
    size_t usable = job->b64_len & ~(size_t)3;
    if (usable && job->cb) {
        size_t need = usable / 4 * 3, olen = 0;
        if (mbedtls_base64_decode(job->b64, need, &olen, job->b64, usable) == 0
            && olen >= 2) {
            s_audio_batches++; s_audio_bytes += olen;
            job->cb((int16_t *)job->b64, olen / 2, 16000, job->uctx);
        }
    }
    if (ch == '\0') { job->b64_len = 0; return; }
    size_t rem = job->b64_len - usable;
    if (rem) memmove(job->b64, job->b64 + usable, rem);
    job->b64_len = rem;
}

static void xf_feed(xfyun_job_t *job, const char *p, size_t len) {
    if (job->frame_done) { job->frame_done = false; job->xs = XS_SCAN; }
    for (size_t i = 0; i < len && !job->frame_done; i++) {
        char ch = p[i];
        switch (job->xs) {
        case XS_SCAN:    if (ch == '"') { job->xs = XS_KEY; job->key_len = 0; } break;
        case XS_KEY:
            if (ch == '"') { job->key[job->key_len] = '\0'; job->xs = XS_COLON; }
            else if (job->key_len < sizeof(job->key) - 1) job->key[job->key_len++] = ch;
            break;
        case XS_COLON:
            if (ch == ':') { mark_apply(job, job->key); job->xs = XS_VSTART; job->val_len = 0; job->in_audio = (job->mark == XF_MARK_AUDIO); }
            break;
        case XS_VSTART:
            if (ch == '{') job->xs = XS_SCAN;
            else if (ch == '"') { job->xs = XS_STRVAL; job->esc = false; job->val_len = 0; if (job->in_audio) job->b64_len = 0; }
            else if (ch == '-' || (ch >= '0' && ch <= '9')) { job->xs = XS_NUMVAL; job->val_len = 0; if (job->val_len < sizeof(job->val) - 1) job->val_len++; }
            else if (ch == ',' || ch == '}' || ch == ']') job->xs = XS_SCAN;
            break;
        case XS_STRVAL:
            if (job->esc) { job->esc = false; if (job->in_audio) audio_feed(job, ch); }
            else if (ch == '\\') job->esc = true;
            else if (ch == '"') { if (job->in_audio) audio_feed(job, '\0'); else field_end(job); job->xs = XS_SCAN; }
            else if (job->in_audio) audio_feed(job, ch);
            else if (job->val_len < sizeof(job->val) - 1) job->val_len++;
            break;
        case XS_NUMVAL:
            if ((ch >= '0' && ch <= '9') || ch == '-') { if (job->val_len < sizeof(job->val) - 1) job->val_len++; }
            else { field_end(job); job->xs = XS_SCAN; if (ch == '}') job->frame_done = true; }
            break;
        default: break;
        }
    }
}

/* ---- 鉴权 URL ---- */
static bool build_auth_url(char *out, size_t out_n) {
    return xfyun_build_auth_url(XF_HOST, XF_PATH,
                                s_cfg.api_key, s_cfg.api_secret,
                                out, out_n) == ESP_OK;
}

/* ---- 发送文本帧 ---- */
static void send_text_frame(xfyun_job_t *job) {
    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON_AddStringToObject(common, "app_id", s_cfg.appid);
    cJSON_AddItemToObject(root, "common", common);

    cJSON *bus = cJSON_CreateObject();
    cJSON_AddStringToObject(bus, "aue", "raw");
    cJSON_AddStringToObject(bus, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(bus, "vcn", s_cfg.vcn ? s_cfg.vcn : "xiaoyan");
    cJSON_AddStringToObject(bus, "tte", "UTF8");
    cJSON_AddNumberToObject(bus, "speed", s_cfg.speed > 0 ? s_cfg.speed : 40);
    cJSON_AddNumberToObject(bus, "volume", s_cfg.volume > 0 ? s_cfg.volume : 50);
    cJSON_AddItemToObject(root, "business", bus);

    size_t tlen = strlen(job->text);
    size_t blen = (tlen + 2) / 3 * 4 + 1;
    char *b64 = (char *)malloc(blen);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)b64, blen, &olen,
                          (const unsigned char *)job->text, tlen);
    cJSON *dt = cJSON_CreateObject();
    cJSON_AddNumberToObject(dt, "status", 2);
    cJSON_AddStringToObject(dt, "text", b64);
    free(b64);
    cJSON_AddItemToObject(root, "data", dt);

    char *js = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "发送文本帧");
    ws_bridge_send_text(job->ws, js, strlen(js));
    cJSON_free(js);
    cJSON_Delete(root);
}

/* ---- ws_bridge 回调：文本帧 ---- */
static void cb_msg(const char *data, size_t len, void *ctx)
{
    xfyun_job_t *job = (xfyun_job_t *)ctx;
    if (!job || job->failed) return;

    ESP_LOGI(TAG, "ws 收到文本帧 %u 字节", (unsigned)len);
    xf_feed(job, data, len);

    if (job->has_code && job->code != 0) {
        ESP_LOGE(TAG, "code=%d msg=%s", job->code, job->msg_len ? job->msg : "(无)");
        job->failed = true;
        if (job->done) xSemaphoreGive(job->done);
        return;
    }
    if (job->status == 2) {
        ESP_LOGI(TAG, "合成结束，累计 %u 批 / %u 字节 PCM",
                 (unsigned)s_audio_batches, (unsigned)s_audio_bytes);
        if (job->done) xSemaphoreGive(job->done);
    }
}

/* ---- ws_bridge 回调：事件 ---- */
static void cb_evt(ws_bridge_event_t event, void *ctx)
{
    xfyun_job_t *job = (xfyun_job_t *)ctx;
    if (!job) return;
    switch (event) {
    case WS_BRIDGE_EVENT_OPENED:
        ESP_LOGI(TAG, "ws 已连接");
        break;
    case WS_BRIDGE_EVENT_CLOSED:
        ESP_LOGI(TAG, "ws 断开 (audio_bytes=%u)", (unsigned)s_audio_bytes);
        /* 讯飞 TTS 是短连接：服务端发完音频后主动 close → 正常结束。
         * 只有没收到任何音频就断开，才视为真正的连接失败。
         * 无论成功与否都 give 信号量，让 while 循环退出。 */
        if (s_audio_bytes == 0) {
            ESP_LOGW(TAG, "ws 断开时未收到任何音频 → 判定为失败");
            job->failed = true;
        }
        if (job->done) xSemaphoreGive(job->done);
        break;
    case WS_BRIDGE_EVENT_ERROR:
        ESP_LOGE(TAG, "ws 错误");
        job->failed = true;
        if (job->done) xSemaphoreGive(job->done);
        break;
    }
}

/* ---- 驱动接口 ---- */
static esp_err_t xfyun_init(const void *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "xfyun_init: cfg 为空（须传入 tts_xfyun_config_t）");
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *(const tts_xfyun_config_t *)cfg;
    if (!s_cfg.appid || !s_cfg.api_key || !s_cfg.api_secret) {
        ESP_LOGE(TAG, "讯飞配置缺失（appid/api_key/api_secret 不能为空）");
        return ESP_ERR_INVALID_ARG;
    }
    /* 业务参数默认值兜底 */
    if (!s_cfg.vcn)  s_cfg.vcn  = "xiaoyan";
    if (s_cfg.speed <= 0)   s_cfg.speed   = 40;
    if (s_cfg.volume <= 0)  s_cfg.volume  = 50;

    /* 鉴权前先等 SNTP 对时（讯飞做时钟偏移检查） */
    ESP_ERROR_CHECK(xfyun_wait_sntp(20));
    ESP_LOGI(TAG, "讯飞驱动就绪 vcn=%s speed=%d volume=%d",
             s_cfg.vcn, s_cfg.speed, s_cfg.volume);
    return ESP_OK;
}

static bool xfyun_busy(void)       { return s_busy; }

static esp_err_t xfyun_speak(const char *text, tts_pcm_cb_t cb, void *ctx) {
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    s_busy = true;

    xfyun_job_t *job = &s_job;
    memset(job, 0, sizeof(*job));
    job->text = text; job->cb = cb; job->uctx = ctx;
    job->done = xSemaphoreCreateBinary();
    s_audio_batches = 0; s_audio_bytes = 0;

    xfyun_wait_sntp(20);

    if (!build_auth_url(s_url, sizeof(s_url))) {
        ESP_LOGE(TAG, "鉴权 URL 生成失败");
        /* 离线提示"鉴权失败"。仅在有片段时播，避免无片段回退云 TTS 造成递归 */
        if (tts_hub_has_clip(TTS_PHRASE_AUTH_FAILED)) {
            tts_hub_say(TTS_PHRASE_AUTH_FAILED);
        }
        s_busy = false; return ESP_FAIL;
    }

    ESP_LOGI(TAG, "鉴权 OK，剩余堆=%u", (unsigned)esp_get_free_heap_size());

    /* 创建 bridge + 注册回调 */
    ws_bridge_t *wb = ws_bridge_create();
    if (!wb) { s_busy = false; return ESP_ERR_NO_MEM; }
    ws_bridge_set_callbacks(wb, cb_msg, cb_evt, job);
    job->ws = wb;

    if (!ws_bridge_connect(wb, s_url, 15000)) {
        ESP_LOGE(TAG, "ws 连接失败");
        ws_bridge_destroy(wb);
        s_busy = false; return ESP_FAIL;
    }

    send_text_frame(job);

    /* 驱动 poll 直到合成结束（最多 30s） */
    TickType_t t0 = xTaskGetTickCount();
    while (!job->failed) {
        ws_bridge_poll(wb);
        if (xSemaphoreTake(job->done, pdMS_TO_TICKS(10)) == pdTRUE) break;
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(30000)) {
            ESP_LOGE(TAG, "ws 等待合成超时");
            break;
        }
    }

    ws_bridge_close(wb);
    ws_bridge_destroy(wb);
    vSemaphoreDelete(job->done);

    ESP_LOGI(TAG, "音频解码: %u 批 / %u 字节 PCM",
             (unsigned)s_audio_batches, (unsigned)s_audio_bytes);
    bool ok = !job->failed && s_audio_bytes > 0;
    s_busy = false;
    return ok ? ESP_OK : ESP_FAIL;
}

const tts_driver_t tts_drv_xfyun = {
    .name = "xfyun",
    .init = xfyun_init,
    .speak = xfyun_speak,
    .is_busy = xfyun_busy,
};
