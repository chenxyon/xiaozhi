/**
 * @file     main_fsm.c
 * @brief    主交互闭环状态机实现
 *
 * 修改：2026-11-20 新建（规则 8）
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "tts_hub.h"
#include "kws_hub.h"
#include "event_bus.h"

#include "main_fsm.h"

static const char *TAG = "main_fsm";

static app_state_t s_state = FSM_IDLE;

static const char *s_state_names[FSM_MAX] = {
    "IDLE", "LISTENING", "AWAKE", "PROCESSING", "SPEAKING",
};

const char *main_fsm_state_name(app_state_t s)
{
    if (s >= 0 && s < FSM_MAX) return s_state_names[s];
    return "?";
}

app_state_t main_fsm_state(void)
{
    return s_state;
}

/* ===== 状态转换（内部）=====
 * 统一入口：自动幂等 + 自动调用 on_enter 动作 */
static void s_transition_to(app_state_t new_state)
{
    if (new_state == s_state) return;   /* 幂等 */
    if (new_state <= FSM_IDLE || new_state >= FSM_MAX) return;

    app_state_t prev = s_state;
    ESP_LOGI(TAG, "状态转换: %s → %s",
             main_fsm_state_name(prev),
             main_fsm_state_name(new_state));
    s_state = new_state;

    /* on_enter 动作 */
    switch (new_state) {
    case FSM_IDLE:
        /* 不做额外动作，等 WiFi 连上由事件总线驱动 */
        break;

    case FSM_LISTENING:
        kws_hub_resume();
        break;

    case FSM_AWAKE:
        /* 唤醒了：播"我在请说" + 暂停监听（避免录到自己的播报声）*/
        tts_hub_say_async(TTS_PHRASE_WAKE_REPLY);
        kws_hub_pause();
        break;

    case FSM_PROCESSING:
        /* 播"正在处理"提示音，然后 AI 对话（调 ai_qa_async 里会自己 pause/播）*/
        tts_hub_say_async(TTS_PHRASE_PROCESSING);
        break;

    case FSM_SPEAKING:
        /* TTS 播报中：保持 kws_pause 状态（避免录到喇叭自唤醒）*/
        kws_hub_pause();
        break;

    default:
        break;
    }
}

void main_fsm_force(app_state_t s)
{
    s_transition_to(s);
}

/* ===== 事件总线订阅者 ===== */
static void s_on_wifi(app_event_t ev, void *data, void *ctx)
{
    (void)data; (void)ctx;
    switch (ev) {
    case EVT_WIFI_GOT_IP:
        ESP_LOGI(TAG, "WiFi 已连 → LISTENING");
        s_transition_to(FSM_LISTENING);
        break;
    case EVT_WIFI_DISCONNECTED:
        ESP_LOGW(TAG, "WiFi 断开 → IDLE");
        s_transition_to(FSM_IDLE);
        break;
    default: break;
    }
}

static void s_on_kws(app_event_t ev, void *data, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case EVT_WAKE_WORD:
        if (s_state == FSM_LISTENING) {
            s_transition_to(FSM_AWAKE);
            ESP_LOGI(TAG, "唤醒词命中: %s", data ? (const char *)data : "(null)");
        }
        break;

    case EVT_USER_TEXT:
        if (s_state == FSM_AWAKE) {
            s_transition_to(FSM_PROCESSING);
        }
        break;

    default: break;
    }
}

static void s_on_tts(app_event_t ev, void *data, void *ctx)
{
    (void)data; (void)ctx;
    switch (ev) {
    case EVT_TTS_DONE:
        /* 播报完了 → 回 LISTENING（IDLE 除外，IDLE 等 WiFi）*/
        if (s_state == FSM_SPEAKING) {
            s_transition_to(FSM_LISTENING);
        } else if (s_state == FSM_AWAKE) {
            /* 唤醒回复播完了，已经在等指令，保持 AWAKE */
            ESP_LOGI(TAG, "唤醒回复已播完，继续等指令");
        } else if (s_state == FSM_PROCESSING) {
            /* processing 提示音播完，AI 还在想，保持 PROCESSING */
            ESP_LOGI(TAG, "processing 提示音播完，AI 思考中");
        }
        break;

    default: break;
    }
}

static void s_on_ai(app_event_t ev, void *data, void *ctx)
{
    (void)data; (void)ctx;
    switch (ev) {
    case EVT_AI_REPLY_READY:
        if (s_state == FSM_PROCESSING) {
            s_transition_to(FSM_SPEAKING);
        }
        break;
    default: break;
    }
}

esp_err_t main_fsm_init(void)
{
    if (!event_bus_inited()) {
        ESP_LOGE(TAG, "event_bus 还没 init！");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t e;
    e = event_bus_subscribe(EVT_WIFI_GOT_IP,        s_on_wifi,  NULL);
    if (e != ESP_OK) goto err;
    e = event_bus_subscribe(EVT_WIFI_DISCONNECTED,  s_on_wifi,  NULL);
    if (e != ESP_OK) goto err;
    e = event_bus_subscribe(EVT_WAKE_WORD,          s_on_kws,   NULL);
    if (e != ESP_OK) goto err;
    e = event_bus_subscribe(EVT_USER_TEXT,          s_on_kws,   NULL);
    if (e != ESP_OK) goto err;
    e = event_bus_subscribe(EVT_TTS_DONE,           s_on_tts,   NULL);
    if (e != ESP_OK) goto err;
    e = event_bus_subscribe(EVT_AI_REPLY_READY,     s_on_ai,    NULL);
    if (e != ESP_OK) goto err;

    s_state = FSM_IDLE;
    ESP_LOGI(TAG, "状态机就绪 (初始状态 IDLE)");
    return ESP_OK;

err:
    ESP_LOGE(TAG, "订阅事件失败: %s", esp_err_to_name(e));
    return e;
}
