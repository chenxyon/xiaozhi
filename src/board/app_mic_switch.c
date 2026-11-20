/**
 * @file     app_mic_switch.c
 * @brief    麦克风开关：GPIO38 轻触按键翻转、GPIO21 LED 指示（PWM 调亮度）
 *
 * 逻辑：
 *   - LED 亮 = 麦克风开（kws_hub 监听中）；LED 灭 = 麦克风关。
 *   - 按键接 GND（内部上拉），「按下→抬起」一次翻转一次状态。
 *   - LED 用 LEDC PWM 输出，常亮亮度降低（见 LED_BRIGHT_PERCENT）。
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "kws_hub.h"
#include "tts_hub.h"

#include "app_mic_switch.h"

static const char *TAG = "app_mic_switch";   /* 规则 4-8 */

/* 硬件引脚定义 */
#define MIC_SW_BTN_GPIO      GPIO_NUM_38   /* 轻触开关（接 GND，内部上拉） */
#define MIC_LED_GPIO         GPIO_NUM_21   /* 状态 LED */

/* LED 极性：1=高电平点亮（阳极经电阻接 GPIO、阴极接 GND）；
 * 若你的灯是低电平点亮（阳极接 3V3），改成 0。 */
#define MIC_LED_ACTIVE_HIGH  1

/* 常亮亮度百分比（0-100）。觉得太亮就调小，太暗就调大。 */
#define LED_BRIGHT_PERCENT   25

/* ---- LEDC PWM 配置 ---- */
#define LEDC_MODE            LEDC_LOW_SPEED_MODE
#define LEDC_TIMER           LEDC_TIMER_0
#define LEDC_CHANNEL         LEDC_CHANNEL_0
#define LEDC_DUTY_RES        LEDC_TIMER_8_BIT       /* 0-255 */
#define LEDC_FREQ_HZ         2000                    /* 2kHz，看不到闪烁 */
#define LEDC_FULL_DUTY       255

#define LED_DUTY_BRIGHT      ((LEDC_FULL_DUTY * LED_BRIGHT_PERCENT) / 100)

/* 根据极性换算 duty：高电平点亮 → 亮=duty、灭=0；低电平点亮取反 */
#if MIC_LED_ACTIVE_HIGH
#define LED_DUTY_ON          (LED_DUTY_BRIGHT)
#define LED_DUTY_OFF         (0)
#else
#define LED_DUTY_ON          (LEDC_FULL_DUTY - LED_DUTY_BRIGHT)
#define LED_DUTY_OFF         (LEDC_FULL_DUTY)
#endif

/* 按键轮询与去抖 */
#define BTN_POLL_MS          20
#define BTN_PRESS_CONFIRM    2       /* 连续 2 次(≈40ms)低电平才算按下 */

static volatile bool s_mic_on = true;

static void led_set(bool on)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, on ? LED_DUTY_ON : LED_DUTY_OFF);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/* 等 TTS 真正开始并播完（避免 speak_async 还没置 busy 就误判已结束） */
static void wait_tts_done(void)
{
    for (int i = 0; i < 30 && !tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 400 && tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));
}

/* 播报任务：等上一次 TTS 空闲再播，避免因 busy 被丢弃；开麦时播完再启动监听 */
static void mic_announce_task(void *arg)
{
    bool on = (bool)(intptr_t)arg;

    /* 前一次播报可能还没结束（例如刚按过关麦就立刻按开麦） */
    for (int i = 0; i < 50 && tts_hub_is_busy(); i++) vTaskDelay(pdMS_TO_TICKS(100));

    const char *msg = on ? "麦克风已开启。" : "麦克风已关闭。";
    esp_err_t e = tts_hub_speak_async(msg);
    for (int i = 0; i < 20 && e != ESP_OK; i++) {   /* 仍旧忙就重试 */
        vTaskDelay(pdMS_TO_TICKS(100));
        e = tts_hub_speak_async(msg);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "播报未能启动(%s)", esp_err_to_name(e));

    wait_tts_done();

    if (on && s_mic_on) {
        esp_err_t r = kws_hub_start();
        ESP_LOGI(TAG, "麦克风开：LED 亮，唤醒监听%s",
                 (r == ESP_OK) ? "已开启" : "（等待联网就绪后开启）");
    }
    vTaskDelete(NULL);
}

/* 应用一次开/关：更新 LED + 语音反馈 + 监听状态 */
static void apply_state(bool on)
{
    s_mic_on = on;
    led_set(on);

    if (!on) kws_hub_stop();   /* 关麦立即停监听，避免继续录音 */

    if (xTaskCreate(mic_announce_task, "mic_ann", 4096, (void *)(intptr_t)on, 4, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "播报任务创建失败");
        if (on) kws_hub_start();   /* 兜底：至少能开麦监听 */
    }

    ESP_LOGI(TAG, "%s", on ? "麦克风开：LED 亮" : "麦克风关：LED 灭，已停止监听");
}

static void button_task(void *arg)
{
    (void)arg;

    bool pressed = false;
    int  low_cnt = 0;

    for (;;) {
        int level = gpio_get_level(MIC_SW_BTN_GPIO);   /* 0=按下，1=抬起 */

        if (level == 0) {
            if (low_cnt < BTN_PRESS_CONFIRM) low_cnt++;
            if (low_cnt >= BTN_PRESS_CONFIRM && !pressed) {
                pressed = true;            /* 确认按下 */
            }
        } else {
            if (pressed) {
                pressed  = false;          /* 抬起 → 翻转 */
                apply_state(!s_mic_on);
            }
            low_cnt = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

static esp_err_t led_pwm_init(void)
{
    gpio_reset_pin(MIC_LED_GPIO);

    ledc_timer_config_t tcfg = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) return err;

    ledc_channel_config_t ccfg = {
        .gpio_num   = MIC_LED_GPIO,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = LED_DUTY_OFF,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ccfg);
}

esp_err_t app_mic_switch_init(void)
{
    /* LED：LEDC PWM 输出（GPIO21） */
    esp_err_t err = led_pwm_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED PWM 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 按键输入（GPIO38，内部上拉，按下接 GND） */
    gpio_reset_pin(MIC_SW_BTN_GPIO);
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << MIC_SW_BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&btn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键 GPIO 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 启动自检：LED 闪 3 下（确认引脚/接线/极性都能看到） */
    ESP_LOGI(TAG, "LED 自检：闪烁 3 次（GPIO%d，亮度%d%%）",
             (int)MIC_LED_GPIO, LED_BRIGHT_PERCENT);
    for (int i = 0; i < 3; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    /* 默认开机：麦克风开、LED 亮 */
    s_mic_on = true;
    led_set(true);

    if (xTaskCreate(button_task, "mic_sw", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "麦克风开关就绪：按键=GPIO%d，LED=GPIO%d（默认开，亮度%d%%）",
             (int)MIC_SW_BTN_GPIO, (int)MIC_LED_GPIO, LED_BRIGHT_PERCENT);
    return ESP_OK;
}

bool app_mic_switch_is_on(void)
{
    return s_mic_on;
}
