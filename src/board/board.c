/**
 * @file     board.c
 * @brief    板级装配层 —— 引脚定义与设备初始化（规则 1：分层架构）
 *
 * 职责：
 *   1. 本板全部引脚定义（唯一可信来源）
 *   2. 设备初始化编排（OLED / Flash / I2S 功放 / 麦克风 / NVS / TTS hub）
 *   3. 密钥加载（NVS 优先，规则 4-1）
 *
 * 设计规则（DESIGN_RULES.md v2.0）：
 *   - 板级装配层 **只做装配**，不写任何算法
 *     （高通滤波 → src/audio/hpf_filter；输出适配 → src/audio/audio_output）
 *   - 前置条件见 board.h 顶部注释（规则 5）
 *
 * 修改：2026-10-03 从 main.c 拆出
 * 修改：2026-11-20 拆出 HPF + 输出适配 → src/audio/ （规则 4-SRP）；
 *                  密钥改 NVS 优先（规则 4-1）；TAG 统一小写（规则 4-8）
 *
 * @note 引脚复用策略
 *   OLED I²C : SCL=GPIO15, SDA=GPIO16, addr=0x3C
 *   W25Q128  : CS=GPIO39, SCK=GPIO40, MOSI=GPIO41, MISO=GPIO42
 *   I2S 全双工：BCLK=GPIO20, LRC=GPIO19, DIN=GPIO18（功放）, SD=GPIO17（麦克风）
 */
#include <stdint.h>
#include <math.h>   /* board_init_tts 里 strncpy 兜底用；保留 */

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s.h"

#include "board.h"
#include "max98357_bridge.h"
#include "inmp441_bridge.h"
#include "tts_hub.h"
#include "tts_xfyun.h"
#include "secret_store.h"
#include "audio_output.h"    /* 规则 1：算法/适配层独立，board 只管装配 */

static const char *TAG = "board";   /* 规则 4-8：日志 TAG 小写模块名 */

/* ====================== 本板引脚定义（唯一可信来源） ====================== */

static const oled_i2c_pins_t k_oled_pins = {
    .scl      = GPIO_NUM_15,
    .sda      = GPIO_NUM_16,
    .i2c_addr = 0x3C,
};

static const max98357_pins_t k_audio_pins = {
    .port            = I2S_NUM_0,
    .bclk            = GPIO_NUM_20,
    .lrc             = GPIO_NUM_19,
    .din             = GPIO_NUM_18,
    .mic_din         = GPIO_NUM_17,
    .sample_rate_hz  = 16000,
    .bclk_div        = 8,
    .dma_desc_num    = 16,
    .dma_frame_num   = 256,
    .beep_freq_hz    = 440,
    .beep_ms         = 40,
    .task_period_ms  = 500,
};

static const w25q128_pins_t k_flash_pins = {
    .host             = SPI3_HOST,
    .cs               = GPIO_NUM_39,
    .sck              = GPIO_NUM_40,
    .mosi             = GPIO_NUM_41,
    .miso             = GPIO_NUM_42,
    .clk_hz           = 20 * 1000 * 1000,
    .max_transfer_sz  = 4096,
};

static const inmp441_cfg_t k_mic_cfg = {
    .sample_rate_hz = 16000,
    .channels       = 1,
};

static const inmp441_pins_t k_mic_pins = {
    .i2s_port    = 0,
    .sck         = GPIO_NUM_20,   /* 与 MAX98357 BCLK 共用 */
    .ws          = GPIO_NUM_19,   /* 与 MAX98357 LRC 共用 */
    .sd          = GPIO_NUM_17,
};

void board_log_pinout(void)
{
    ESP_LOGI(TAG, "--- OLED ---   I²C: SCL=%d SDA=%d addr=0x%02X",
             (int)k_oled_pins.scl, (int)k_oled_pins.sda, k_oled_pins.i2c_addr);
    ESP_LOGI(TAG, "--- 功放 ---   BCLK=%d LRC=%d DIN=%d MIC=%d",
             (int)k_audio_pins.bclk, (int)k_audio_pins.lrc,
             (int)k_audio_pins.din, (int)k_audio_pins.mic_din);
    ESP_LOGI(TAG, "--- Flash ---  CS=%d SCK=%d MOSI=%d MISO=%d",
             (int)k_flash_pins.cs, (int)k_flash_pins.sck,
             (int)k_flash_pins.mosi, (int)k_flash_pins.miso);
    ESP_LOGI(TAG, "--- 麦克风 --- SCK=%d WS=%d SD=%d (共用功放时钟)",
             (int)k_mic_pins.sck, (int)k_mic_pins.ws, (int)k_mic_pins.sd);
}

/* ====================== 设备初始化 ====================== */

void board_init_devices(void)
{
    /* 1. NVS —— 密钥/配置存储（规则 4-1，禁止密钥硬编码）
     * 必须最先 init，后面 board_init_tts() / AI 模块都要读 */
    secret_store_init();

    /* 2. OLED（I²C SSD1306 0.91"）*/
    if (oled_init(&k_oled_pins) != ESP_OK) {
        ESP_LOGE(TAG, "OLED 初始化失败，跳过显示功能");
    } else {
        oled_show_line("小智已启动", 0);
    }

    /* 3. Flash */
    if (w25q128_init(&k_flash_pins) == ESP_OK) {
        uint8_t jedec[3] = { 0 };
        if (w25q128_read_jedec_id(jedec) != ESP_OK) {
            ESP_LOGW(TAG, "Flash 未检出，暂不提供存储功能");
        }
    } else {
        ESP_LOGE(TAG, "Flash 初始化失败");
    }

    /* 4. I2S + 功放 + 麦克风（共用 I2S 全双工）*/
#if AUDIO_AMP_ENABLE || AUDIO_MIC_ENABLE
    ESP_ERROR_CHECK(max98357_init(&k_audio_pins));
#endif

#if AUDIO_AMP_ENABLE
    /* audio_output 初始化：HPF 高通 80Hz + chunk 2048 */
    audio_output_init(80.0f, k_audio_pins.sample_rate_hz, 2048);
#else
    ESP_LOGI(TAG, "功放已禁用（I2S 时钟仍开，供麦克风使用）");
#endif

#if AUDIO_MIC_ENABLE
    inmp441_pins_t mic_pins = k_mic_pins;
    mic_pins.i2s_port = k_audio_pins.port;
    ESP_ERROR_CHECK(inmp441_init(&mic_pins, &k_mic_cfg));
#else
    ESP_LOGI(TAG, "麦克风已禁用");
#endif

    /* 5. tts_hub 基础就绪
     *    - 注册 PCM 输出槽 = audio_output_tts_sink（HPF → max98357）
     *    - provider = 讯飞
     *    ⚠️ 此处不调云驱动 init（讯飞需 SNTP，WiFi 前调会 abort）
     *    离线片段播放可正常工作；云 TTS 要等 board_init_tts() 在 configTime 之后调 */
    ESP_ERROR_CHECK(tts_hub_prepare(audio_output_tts_sink,
                                     k_audio_pins.sample_rate_hz,
                                     TTS_PROVIDER_XFYUN));
}

/* TTS 云驱动初始化 —— 必须在 WiFi + SNTP 就绪之后调用！（规则 5 前置条件）
 * 讯飞鉴权需要时间戳，SNTP 没对时会直接 abort。
 *
 * 密钥来源：优先 NVS（规则 4-1），NVS 无值时用内置默认值兜底，
 * 方便首次烧录后直接跑通；用户可通过配网页面或串口命令写入自有密钥。*/
void board_init_tts(void)
{
    /* 默认值（兜底用）*/
    static const char *DFLT_APPID      = "5a99f947";
    static const char *DFLT_API_KEY    = "e542c421f3bdf399222de862f95c9bba";
    static const char *DFLT_API_SECRET = "6002f68c3ecf4969ee2313113cad46da";
    static const char *DFLT_VCN        = "xiaoyan";

    char appid[32] = { 0 };
    char api_key[64] = { 0 };
    char api_secret[64] = { 0 };
    char vcn[24] = { 0 };

    #define TRY_NVS(key, out, out_sz, dflt)                      \
        do {                                                      \
            if (secret_store_get_str(key, out, out_sz, NULL) != ESP_OK) { \
                strncpy(out, dflt, out_sz - 1);                   \
                out[out_sz - 1] = '\0';                           \
                ESP_LOGW(TAG, "NVS 无 %s，使用默认值", key);       \
            }                                                     \
        } while (0)

    TRY_NVS("xfyun_appid",      appid,      sizeof(appid),      DFLT_APPID);
    TRY_NVS("xfyun_api_key",    api_key,    sizeof(api_key),    DFLT_API_KEY);
    TRY_NVS("xfyun_api_secret", api_secret, sizeof(api_secret), DFLT_API_SECRET);
    TRY_NVS("xfyun_vcn",        vcn,        sizeof(vcn),        DFLT_VCN);

    #undef TRY_NVS

    tts_xfyun_config_t cfg = {
        .appid      = appid,
        .api_key    = api_key,
        .api_secret = api_secret,
        .vcn        = vcn,
        .speed      = 40,
        .volume     = 50,
    };
    ESP_ERROR_CHECK(tts_hub_driver_init(&cfg));
    ESP_LOGI(TAG, "TTS 云驱动已就绪 (appid=%s)", appid);
}

const max98357_pins_t *board_audio_config(void)
{
    return &k_audio_pins;
}
