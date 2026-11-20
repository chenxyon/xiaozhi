/**
 * @file     kws_hub.c
 * @brief    唤醒/识别引擎抽象层 — 可插拔 WakeNet(离线) / 讯飞 IAT(在线)
 *
 * 数据流：
 *   INMP441 → 引擎（WakeNet 离线 / IAT 在线）→ KWS_EV_WAKE / KWS_EV_TEXT
 *
 * 默认引擎：讯飞 IAT（WakeNet 需 srmodels.bin 烧入 model 分区）。
 * WakeNet 若模型可用，自动 fallback 用离线引擎。
 *
 * 修改：2026-11-19 重写：支持多引擎可插拔
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_sleep.h"

#include "inmp441_bridge.h"
#include "kws_hub.h"
#include "kws_xfyun_iat.h"

/* ===== WakeNet 头文件（可选：模型可用时才启用）===== */
#if __has_include("esp_wn_iface.h")
  #define KWS_HAVE_WAKENET 1
  #include "esp_wn_iface.h"
  #include "esp_wn_models.h"
  #include "model_path.h"
#else
  #define KWS_HAVE_WAKENET 0
#endif

static const char *TAG = "kws_hub";   /* 规则 4-8 */

/* ===== 选中的引擎 ===== */
static kws_engine_id_t s_engine_id = KWS_ENGINE_NONE;
static const char      *s_engine_name = "none";

/* ===== 讯飞密钥（appid 共用，但各服务 apiKey 独立）===== */
static const char *s_xfyun_appid           = "5a99f947";
static const char *s_xfyun_rtasr_api_key    = "68223aa9861d99fa37726e7c5f41b649";  /* 实时语音转写专用 */
static const char *s_xfyun_old_api_key     = "e542c421f3bdf399222de862f95c9bba";  /* 旧 IAT / TTS 等服务 */
static const char *s_xfyun_old_api_secret  = "6002f68c3ecf4969ee2313113cad46da";

/* ===== WakeNet 静态状态 ===== */
#if KWS_HAVE_WAKENET
static const esp_wn_iface_t *s_wn_iface = NULL;
static model_iface_data_t   *s_wn_data  = NULL;
static srmodel_list_t       *s_models   = NULL;
static int                   s_chunk_size = 0;
static int                   s_samp_rate  = 0;
static TaskHandle_t          s_wakenet_task = NULL;
static volatile bool         s_wakenet_running = false;
static volatile bool         s_wakenet_paused  = false;
#endif

/* ===== 对外接口状态 ===== */
static kws_event_cb_t s_cb       = NULL;
static void          *s_cb_ctx   = NULL;
static char           s_wake_word[64] = "小智";   /* IAT 默认；WakeNet 会被模型值覆盖 */

/* ================================================================
 *  WakeNet 内部实现（仅当模型可用时走这条）
 * ================================================================ */
#if KWS_HAVE_WAKENET

static esp_err_t wakenet_init_from_spiffs(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "SPIFFS model 分区挂载成功，可用模型数=%d", s_models->num);

    char *model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "nihaoxiaozhi");
    if (!model_name) model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "hiesp");
    if (!model_name) model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!model_name) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "选中模型: %s", model_name);
    s_wn_iface = esp_wn_handle_from_name(model_name);
    if (!s_wn_iface) return ESP_ERR_INVALID_STATE;

    s_wn_data = s_wn_iface->create(model_name, DET_MODE_90);
    if (!s_wn_data) return ESP_ERR_NO_MEM;

    s_chunk_size = s_wn_iface->get_samp_chunksize(s_wn_data);
    s_samp_rate  = s_wn_iface->get_samp_rate(s_wn_data);
    ESP_LOGI(TAG, "WakeNet 参数: chunk=%d, rate=%d", s_chunk_size, s_samp_rate);

    char *ww = esp_srmodel_get_wake_words(s_models, model_name);
    if (ww && ww[0]) {
        strncpy(s_wake_word, ww, sizeof(s_wake_word) - 1);
        ESP_LOGI(TAG, "模型唤醒词: %s", s_wake_word);
    }
    if (s_wn_iface->set_det_threshold) {
        s_wn_iface->set_det_threshold(s_wn_data, 0.85f, 1);
    }
    return ESP_OK;
}

static void wakenet_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WakeNet 监听任务启动");
    int16_t *frame = (int16_t *)malloc(s_chunk_size * sizeof(int16_t));
    if (!frame) {
        ESP_LOGE(TAG, "PCM 帧缓冲分配失败");
        s_wakenet_running = false;
        vTaskDelete(NULL);
        return;
    }

    while (s_wakenet_running) {
        if (s_wakenet_paused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        size_t got = 0;
        esp_err_t r = inmp441_read(frame, s_chunk_size * sizeof(int16_t), &got, 200);
        if (r != ESP_OK || got < (size_t)s_chunk_size * sizeof(int16_t)) {
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        wakenet_state_t result = s_wn_iface->detect(s_wn_data, frame);
        if (result == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "🎯 唤醒触发！唤醒词=%s", s_wake_word);
            if (s_cb) s_cb(KWS_EV_WAKE, s_wake_word, s_cb_ctx);
            if (s_wn_iface->clean) s_wn_iface->clean(s_wn_data);
        }
    }
    free(frame);
    ESP_LOGI(TAG, "WakeNet 监听任务退出");
    vTaskDelete(NULL);
}

static esp_err_t wakenet_start(void)
{
    if (!s_wn_data) return ESP_ERR_INVALID_STATE;
    if (s_wakenet_running) return ESP_OK;
    s_wakenet_running = true;
    s_wakenet_paused  = false;
    BaseType_t ok = xTaskCreate(wakenet_task, "wakenet", 4096, NULL, 4, &s_wakenet_task);
    if (ok != pdPASS) { s_wakenet_running = false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static esp_err_t wakenet_stop(void)
{
    s_wakenet_running = false;
    s_wakenet_paused  = false;
    for (int i = 0; i < 50 && s_wakenet_task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_wakenet_task = NULL;
    return ESP_OK;
}

#endif /* KWS_HAVE_WAKENET */

/* ================================================================
 *  对外接口
 * ================================================================ */

esp_err_t kws_hub_set_callback(kws_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t kws_hub_init(kws_engine_id_t engine)
{
    if (engine == KWS_ENGINE_NONE) {
        /* 用户没指定：先试 WakeNet，有模型就用离线；否则默认 IAT */
        ESP_LOGI(TAG, "未指定引擎，尝试 WakeNet 离线模型...");
#if KWS_HAVE_WAKENET
        if (wakenet_init_from_spiffs() == ESP_OK) {
            s_engine_id = KWS_ENGINE_ESP_SR;
            s_engine_name = "esp-sr-wakenet9";
            ESP_LOGI(TAG, "使用 WakeNet 离线引擎 ✅");
            return ESP_OK;
        }
#endif
        ESP_LOGW(TAG, "WakeNet 模型不可用，回退到讯飞 IAT 在线引擎");
        engine = KWS_ENGINE_XFYUN_IAT;
    }

    if (engine == KWS_ENGINE_XFYUN_IAT) {
        s_engine_id = KWS_ENGINE_XFYUN_IAT;
        s_engine_name = "xfyun-iat";
        ESP_LOGI(TAG, "初始化讯飞 IAT 在线引擎...");
        return kws_iat_init(s_xfyun_appid, s_xfyun_rtasr_api_key, "",
                            s_wake_word, s_cb, s_cb_ctx);
    }

#if KWS_HAVE_WAKENET
    if (engine == KWS_ENGINE_ESP_SR) {
        s_engine_id = KWS_ENGINE_ESP_SR;
        s_engine_name = "esp-sr-wakenet9";
        return wakenet_init_from_spiffs();
    }
#endif

    ESP_LOGE(TAG, "不支持的引擎: %d", (int)engine);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t kws_hub_set_wake_word(const char *word)
{
    if (!word || !word[0]) return ESP_ERR_INVALID_ARG;
    strncpy(s_wake_word, word, sizeof(s_wake_word) - 1);
    s_wake_word[sizeof(s_wake_word) - 1] = '\0';
    /* IAT 引擎需要重新 init 才能生效（因为回调里要传 wake_word）*/
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) {
        kws_iat_init(s_xfyun_appid, s_xfyun_rtasr_api_key, "",
                      s_wake_word, s_cb, s_cb_ctx);
    }
    ESP_LOGI(TAG, "唤醒词设置为: %s", s_wake_word);
    return ESP_OK;
}

esp_err_t kws_hub_add_wake_alias(const char *word)
{
    /* WakeNet 别名不起作用（模型固定），IAT 已支持任意唤醒词 */
    (void)word;
    return ESP_OK;
}

const char *kws_hub_get_wake_word(void)
{
    return s_wake_word;
}

esp_err_t kws_hub_start(void)
{
    ESP_LOGI(TAG, "启动监听（引擎=%s, 唤醒词=[%s]）", s_engine_name, s_wake_word);
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) return kws_iat_start();
#if KWS_HAVE_WAKENET
    if (s_engine_id == KWS_ENGINE_ESP_SR)   return wakenet_start();
#endif
    return ESP_ERR_INVALID_STATE;
}

esp_err_t kws_hub_stop(void)
{
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) return kws_iat_stop();
#if KWS_HAVE_WAKENET
    if (s_engine_id == KWS_ENGINE_ESP_SR)   return wakenet_stop();
#endif
    return ESP_OK;
}

esp_err_t kws_hub_pause(void)
{
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) return kws_iat_pause();
#if KWS_HAVE_WAKENET
    if (s_engine_id == KWS_ENGINE_ESP_SR)   { s_wakenet_paused = true; return ESP_OK; }
#endif
    return ESP_OK;
}

esp_err_t kws_hub_resume(void)
{
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) return kws_iat_resume();
#if KWS_HAVE_WAKENET
    if (s_engine_id == KWS_ENGINE_ESP_SR)   { s_wakenet_paused = false; return ESP_OK; }
#endif
    return ESP_OK;
}

bool kws_hub_is_listening(void)
{
    if (s_engine_id == KWS_ENGINE_XFYUN_IAT) return kws_iat_is_listening();
#if KWS_HAVE_WAKENET
    if (s_engine_id == KWS_ENGINE_ESP_SR)   return s_wakenet_running && !s_wakenet_paused;
#endif
    return false;
}

const char *kws_hub_engine_name(void)
{
    return s_engine_name;
}
