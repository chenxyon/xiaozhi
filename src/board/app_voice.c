/**
 * @file     app_voice.c
 * @brief    语音播报业务层（本项目私有，有功放才编译）
 *
 * 功能：WiFi 连上 → TTS 在线合成“小智，已启动”，失败/忙时回落小星星旋律；
 *       进配网模式 → 两声短促提示音。
 *
 * @note     事件跑在 WiFi 事件任务里，只能调 tts_hub_speak_async()，
 *           不能调阻塞版 tts_hub_speak()，否则卡死事件循环。
 */
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "max98357_bridge.h"
#include "tts_hub.h"
#include "app_voice.h"
#include "app_wake.h"
#include "app_ai.h"
#include "app_dir.h"

/* 开机自检：验证 ESP32 能拉取并解析分机通讯录（仅日志，不播报）。
 * 验证通过后可改 0 或删除。 */
#define APP_DIR_SELFTEST   0

static const char *TAG = "app_voice";   /* 规则 4-8 */

/* 提示音：配网/上电用两声短促音（本地合成，不联网） */
static const max98357_note_t s_portal_beep[] = {
    { NOTE_E4, BEAT_EIGHTH }, { NOTE_G4, BEAT_EIGHTH },
    { 0, 0 }
};

static bool s_played_success_tone = false;
static bool s_wake_started = false;

/* 启动音：用**本地提示音**而不是在线 TTS。
 *
 * 原因：唤醒引擎和 TTS 各占一份 TLS 会话，而 TLS/PSA 的工作缓冲走内部 RAM
 * （PSRAM 帮不上）。两者并发会在握手阶段报 PSA -141（内部内存不足）。
 * 实测在线问候音偶发卡到 30s 超时，把唤醒启动一起拖垮。
 * 启动音只是提示，用本地音零成本、零风险；TTS 留给唤醒后的「我在，请说」，
 * 那条路径已经有 pause/resume 保护。 */
static void wake_start_now(void)
{
    if (s_wake_started) return;
    if (app_wake_init() == ESP_OK) {
        s_wake_started = true;
    } else {
        ESP_LOGE(TAG, "唤醒功能启动失败");
    }
}

static bool s_ai_checked = false;
static volatile bool s_boot_running = false;

/* 开机/重连序列：启动音 → AI 自检播报 → 再启动监听。
 * 关键：整个音频期间「不启动监听」，否则麦克风会听到「小智已启动」把自己叫醒，
 * 导致"我在，请说"与"AI模型连接成功"抢播、相互打断（实测现象）。 */
static void boot_seq_task(void *arg)
{
    (void)arg;

    /* 确保音频期间不在监听 */
    if (s_wake_started) { app_wake_stop(); s_wake_started = false; }

    /* 1) 启动提示音（本轮） */
    if (!s_played_success_tone) {
        tts_hub_say_async(TTS_PHRASE_STARTUP);
        s_played_success_tone = true;
    }
    for (int i = 0; i < 30 && !tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 300 && tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));

    /* 2) AI 自检（仅一次）：用轻量 TCP 探测 */
    if (!s_ai_checked) {
        s_ai_checked = true;
        esp_err_t e = app_ai_probe();
        tts_hub_speak_async(e == ESP_OK ? "智能大模型连接成功。" : "智能大模型连接失败。");
        for (int i = 0; i < 30 && !tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        /* 上限 8s，即便 TTS 异常卡住也继续往下走，保证监听最终启动 */
        for (int i = 0; i < 80 && tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    }

#if APP_DIR_SELFTEST
    /* 分机通讯录自检（仅日志）：确认 ESP32 能拉取/解析 /api/employees */
    app_dir_selftest("姜维");
#endif

    /* 3) 音频都播完，才启动唤醒监听 */
    wake_start_now();
    s_boot_running = false;
    vTaskDelete(NULL);
}

void app_voice_on_wifi_event(wifi_mgr_event_t event, void *ctx)
{
    (void)ctx;
    switch (event) {
    case WIFI_MGR_EV_AP_STARTED:
        tts_hub_set_online(false);   /* 配网阶段无外网 */
#if AUDIO_AMP_ENABLE
        max98357_play_melody(s_portal_beep);
#endif
        s_played_success_tone = false;
        break;

    case WIFI_MGR_EV_GOT_IP:
        tts_hub_set_online(true);
        /* 联网后走统一开机序列：启动音 → AI 自检 → 再启动监听 */
        if (!s_boot_running) {
            s_boot_running = true;
            if (xTaskCreate(boot_seq_task, "boot_seq", 16384, NULL, 4, NULL) != pdPASS) {
                s_boot_running = false;
                wake_start_now();          /* 建任务失败兜底：至少启动监听 */
            }
        }
        break;

    case WIFI_MGR_EV_DISCONNECTED:
        tts_hub_set_online(false);
        s_played_success_tone = false;
        /* 断网后在线唤醒引擎已无意义，停掉省流量；重连成功会自动再启动 */
        if (s_wake_started) {
            app_wake_stop();
            s_wake_started = false;
        }
        break;

    default:
        break;
    }
}
