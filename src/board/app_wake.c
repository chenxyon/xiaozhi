/**
 * @file     app_wake.c
 * @brief    唤醒业务层：编排 kws_hub（识别）+ OLED（视觉）+ tts_hub（播报）
 *
 * 唤醒后的一次完整流程：
 *   1. OLED 显示 WAKE   （视觉反馈）
 *   2. TTS 播报「我在，请说」（听觉反馈，走异步不阻塞监听任务）
 *   3. 引擎继续识别，把用户接着说的话以 KWS_EV_TEXT 上报（当作指令）
 * 修改：2026-10-18 新建
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "kws_hub.h"
#include "tts_hub.h"
#include "oled_u8g2.h"
#include "inmp441_bridge.h"

#include "app_wake.h"
#include "app_mic_switch.h"
#include "app_ai.h"
#include "app_dir.h"
#include "app_cmd.h"

static const char *TAG = "app_wake";   /* 规则 4-8 */

/* 空闲退出：连续这么分钟没有音频（电平始终在 ±600 安静带内）就退出监听。
 * 需要改时长就改这里。 */
#define APP_IDLE_TIMEOUT_MIN   5

static bool s_inited = false;

/* 自制字库和 oled_show_line 实现已移至 oled_u8g2.cpp（U8g2 库提供真正的字库） */

/* 清掉第 row 行（页）*/
/* ============ 唤醒事件处理 ============ */

/* TTS 播报期间暂停唤醒监听，播完自动恢复（定义在文件末尾） */
static void resume_after_tts(void);

/* OLED 用两行：0 行常显状态，1 行显示事件（12px 中文字体，共 2 行）*/
#define OLED_ROW_STATUS  0
#define OLED_ROW_EVENT   1

static void on_kws_event(kws_event_t ev, const char *text, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case KWS_EV_WAKE:
        ESP_LOGI(TAG, "唤醒成功：%s", text ? text : "");
        oled_show_line("唤醒", OLED_ROW_EVENT);
        /* 异步播报，不阻塞识别任务 */
        if (!tts_hub_is_busy()) {
            tts_hub_say_async(TTS_PHRASE_WAKE_REPLY);
            /* 播报期间暂停监听，播完自动恢复 */
            resume_after_tts();
        } else {
            ESP_LOGW(TAG, "TTS 忙，跳过唤醒回应");
        }
        /* 回到待唤醒态，OLED 顶部提示继续监听 */
        vTaskDelay(pdMS_TO_TICKS(400));
        oled_show_line("监听中", OLED_ROW_EVENT);
        break;

    case KWS_EV_TEXT:
        ESP_LOGI(TAG, "提问: %s", text ? text : "");
        oled_show_line("思考中", OLED_ROW_EVENT);
        /* 唤醒后用户说的这一句当作指令：暂停监听 →
         *   1) 本地指令优先（音量控制等，低延迟省流量）
         *   2) 分机查询（播三遍）
         *   3) 通用大模型问答 */
        if (text && text[0]) {
            kws_hub_pause();
            if (app_cmd_match(text)) {
                ESP_LOGI(TAG, "本地指令已执行");
            } else if (app_dir_matches(text)) {
                app_dir_query_async(text);   /* 查分机（播三遍） */
            } else {
                app_ai_qa_async(text);       /* 通用大模型问答 */
            }
        }
        break;

    case KWS_EV_ERROR:
        ESP_LOGE(TAG, "识别出错: %s", text ? text : "?");
        oled_show_line("出错", OLED_ROW_EVENT);
        break;

    default:
        break;
    }
}

/* ============ 对外接口 ============ */

/* 空闲看门狗：监听中且连续 N 分钟没有音频 → 退出监听（进入休眠）。
 * 判据用 inmp441 记录的"最后有声音时刻"（电平超出 ±600 即算有声音）。
 * 自己播报（TTS）期间不计入空闲。 */
static void idle_watch_task(void *arg)
{
    (void)arg;
    int64_t base = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (!kws_hub_is_listening() || tts_hub_is_busy()) {
            base = esp_timer_get_time();   /* 没在听 / 正在播报：不计空闲 */
            continue;
        }

        int64_t lv = inmp441_last_voice_us();
        if (lv > base) base = lv;          /* 有新声音：刷新基线 */

        int64_t idle_us = esp_timer_get_time() - base;
        if (idle_us > (int64_t)APP_IDLE_TIMEOUT_MIN * 60 * 1000000LL) {
            ESP_LOGW(TAG, "空闲 %d 分钟无音频，退出唤醒监听", APP_IDLE_TIMEOUT_MIN);
            oled_show_line("休眠", OLED_ROW_EVENT);
            kws_hub_stop();
            base = esp_timer_get_time();
        }
    }
}

esp_err_t app_wake_init(void)
{
    /* 只做一次初始化：WiFi 重连后会再次调用本函数，重复注册别名会把
     * 别名槽位占满，重复 init 也会多建一套引擎状态。 */
    if (!s_inited) {
        kws_hub_set_callback(on_kws_event, NULL);

        /* 唤醒词：主词取自 Kconfig，这里再挂几个常用别名。
         * 想改成别的叫法，改这里或调 app_wake_set_word() 即可。 */
        kws_hub_add_wake_alias("小智同学");
        kws_hub_add_wake_alias("小智");

        esp_err_t init_err = kws_hub_init(KWS_ENGINE_NONE); /* 引擎取 Kconfig 默认 */
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "唤醒引擎初始化失败: %s", esp_err_to_name(init_err));
            return init_err;
        }

        /* 空闲退出看门狗（只建一次） */
        xTaskCreate(idle_watch_task, "idle_wd", 4096, NULL, 2, NULL);

        s_inited = true;
    }

    esp_err_t err = ESP_OK;
    if (app_mic_switch_is_on()) {
        err = kws_hub_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动监听失败: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        ESP_LOGI(TAG, "麦克风开关为关，暂不启动监听");
    }

    oled_show_line("监听中", OLED_ROW_EVENT);
    ESP_LOGI(TAG, "唤醒已就绪，引擎=%s 唤醒词=%s",
             kws_hub_engine_name(), kws_hub_get_wake_word());
    return ESP_OK;
}

esp_err_t app_wake_stop(void)
{
    return kws_hub_stop();
}

esp_err_t app_wake_set_word(const char *word)
{
    return kws_hub_set_wake_word(word);
}

esp_err_t app_wake_add_alias(const char *word)
{
    return kws_hub_add_wake_alias(word);
}
/* TTS 播报期间暂停唤醒监听，等播完自动恢复。
 * 两个原因：
 *   1. 麦克风会录到喇叭的声音 ——「我在，请说」会被自己识别成指令；
 *   2. 唤醒引擎与 TTS 各占一份 TLS 会话，同时在线很容易抢不到连续堆块
 *      （实测 heap 剩余 80KB 但碎片化，TLS 握手直接失败）。 */
static void tts_then_resume_task(void *arg)
{
    (void)arg;
    /* 先等 TTS 真正开始：tts_hub_speak_async 只是建了任务就返回，
     * s_busy 要等 speak_task 跑起来才置位。不先等就会立刻误判「已播完」。 */
    for (int i = 0; i < 30 && !tts_hub_is_busy(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* 再等真正播完，最多 15s */
    for (int i = 0; i < 150 && tts_hub_is_busy(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    kws_hub_resume();
    vTaskDelete(NULL);
}

static void resume_after_tts(void)
{
    kws_hub_pause();
    if (xTaskCreate(tts_then_resume_task, "wake_resume", 3072, NULL, 3, NULL)
        != pdPASS) {
        kws_hub_resume();     /* 建任务失败就直接恢复，别卡在暂停态 */
    }
}
