/**
 * @file     main.cpp
 * @brief    语音交互闭环 —— 唤醒 → 指令 → AI → TTS 播报 → 继续监听
 *
 * 架构（v2.0，2026-11-20）：
 *   设计模式：发布-订阅（规则 9 event_bus）+ 状态模式（规则 8 main_fsm）
 *   wake_cb 纯事件转发，零阻塞；业务流程由状态机自动驱动。
 *
 * 初始化顺序（规则 5，必须按此顺序，前置条件见各模块头文件）：
 *   1. board_init_devices   → OLED/Flash/I2S/功放/麦克风/tts_hub_prepare + NVS
 *   2. event_bus_init       → 事件总线就绪
 *   3. main_fsm_init        → 状态机订阅事件总线
 *   4. kws_hub_init/start   → 唤醒引擎（如果 KWS_ENABLE=1）
 *   5. wifi_connect_or_setup→ WiFi + configTime SNTP + board_init_tts + publish EVT_WIFI_GOT_IP
 *
 * 修改：2026-11-20 重构接入 event_bus + main_fsm；wake_cb 去阻塞；TAG 统一小写
 */
#include <Arduino.h>
#include "esp_log.h"
#include "esp_sntp.h"

/* ====== WiFi + SNTP ====== */
static void sntp_sync_cb(struct timeval *tv)
{
    time_t t = tv->tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &tm);
    /* 北京时间 = UTC+8 */
    tm.tm_hour += 8;
    if (tm.tm_hour >= 24) { tm.tm_hour -= 24; tm.tm_mday++; }
    char cst[32];
    strftime(cst, sizeof(cst), "%Y-%m-%d %H:%M:%S CST", &tm);
    ESP_LOGI("app_main", "NTP 同步完成: %s / %s", ts, cst);
}

static void setup_sntp()
{
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
}

/* ====== 业务组件 ====== */
#include "board.h"
#include "max98357_bridge.h"
#include "inmp441_bridge.h"
#include "tts_hub.h"
#include "kws_hub.h"
#include "app_ai.h"
#include "app_mic_switch.h"
#include "oled_u8g2.h"
#include "OledU8g2.hpp"      /* g_oled 组件完整定义 (时钟 API) */

/* ====== 事件总线 + 状态机（规则 8 / 9）====== */
#include "event_bus.h"
#include "main_fsm.h"

/* ====== 识别/音频总开关 ======
 * 实际值在 platformio.ini build_flags 里统一定义（-DKWS_ENABLE=0 等），
 * 全局生效，工程任意文件可见。这里只做注释说明：
 *   KWS_ENABLE       1=开启唤醒识别（连 RTASR、读麦克风）
 *                    0=关闭（仅 WiFi/TTS 待机；家里安静后改回 1 重烧）
 *   AUDIO_AMP_ENABLE 1=MAX98357 功放 init，0=静音调试时关
 *   AUDIO_MIC_ENABLE 1=INMP441 MIC init，0=配合 KWS_ENABLE 一起关
 * MAX98357 和 INMP441 共用 I2S 控制器，AMP=0 时 MIC 也应设 0。*/

/* ====== WiFi ====== */
#include <WiFi.h>
#include <WiFiManager.h>
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"

/* 设置备选 DNS 服务器 —— 不覆盖路由器默认，只是追加几个备选 */
static void set_public_dns()
{
    IPAddress d0 = WiFi.dnsIP(0);
    IPAddress d1 = WiFi.dnsIP(1);
    ESP_LOGI("app_main", "路由器 DNS: %s / %s",
             d0.toString().c_str(), d1.toString().c_str());

    if (d0 == IPAddress(0, 0, 0, 0)) {
        ip_addr_t dns1, dns2;
        dns1.u_addr.ip4.addr = (119 << 24) | (29 << 16) | (29 << 8) | 29;
        dns1.type = IPADDR_TYPE_V4;
        dns2.u_addr.ip4.addr = (223 << 24) | (5 << 16) | (5 << 8) | 5;
        dns2.type = IPADDR_TYPE_V4;
        dns_setserver(0, &dns1);   /* 119.29.29.29 腾讯 DNS（国内更快）*/
        dns_setserver(1, &dns2);   /* 223.5.5.5 阿里云 */
        ESP_LOGI("app_main", "DNS 已补: 119.29.29.29 + 223.5.5.5");
    }
}

static const char *TAG = "app_main";   /* 规则 4-8：日志 TAG 小写模块名 */

#if KWS_ENABLE
/* ================================================================
 *  唤醒事件回调 —— 纯事件转发，零阻塞（规则 4-2 + 规则 9）
 *
 *  设计：回调只 publish 事件，不做任何业务流程。
 *  业务逻辑由订阅者（main_fsm / 其他模块）各自处理。
 * ================================================================ */
static void wake_cb(kws_event_t ev, const char *text, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case KWS_EV_WAKE: {
        ESP_LOGI(TAG, "唤醒触发: %s", text ? text : "");
        /* 状态机收到 EVT_WAKE_WORD 后自动进入 FSM_AWAKE，
         * 并在 on_enter 里播 "我在请说" + kws_pause() */
        event_bus_publish(EVT_WAKE_WORD, (void *)text);
        break;
    }
    case KWS_EV_TEXT: {
        ESP_LOGI(TAG, "用户指令: %s", text ? text : "");
        if (!text || !text[0]) break;

        /* 1. 通知状态机：有指令了 → 自动进 FSM_PROCESSING
         * 2. 启 AI 对话任务（内部会自己 publish EVT_AI_REPLY_READY + tts_hub_speak）*/
        event_bus_publish(EVT_USER_TEXT, (void *)text);
        app_ai_qa_async(text);
        break;
    }
    case KWS_EV_ERROR: {
        ESP_LOGW(TAG, "唤醒引擎错误: %s", text ? text : "(未知)");
        break;
    }
    }
}
#endif /* KWS_ENABLE */

/* ================================================================
 *  WiFi 连接 —— 连上后 publish 事件让所有订阅者响应
 * ================================================================ */
static void s_on_wifi_connected(bool via_portal)
{
    /* 1. WiFi 连上了先做 SNTP（board_init_tts 依赖它）*/
    set_public_dns();
    configTime(0, 0, "ntp.aliyun.com", "ntp.tencent.com", "ntp.ntsc.ac.cn");
    setup_sntp();
    delay(200);
    ESP_LOGI(TAG, "SNTP 状态: enabled=%d", esp_sntp_enabled());

    /* 2. 初始化 TTS 云驱动（必须在 SNTP 之后！）*/
    board_init_tts();
    tts_hub_set_online(true);

    /* 3. OLED 状态 + 时钟 */
    oled_set_wifi(true);
    oled_show_line("WiFi已连接", 0);
    g_oled.set_clock_slot(0, true, "%Y-%m-%d %H:%M:%S", 16, OLED_ALIGN_TOP_CENTER, 1000);
    g_oled.set_clock_slot(1, false, "", 0);
    g_oled.enable_clock(true);

    /* 4. 系统级提示音 */
    tts_hub_say_async(via_portal ? TTS_PHRASE_PORTAL_OK : TTS_PHRASE_NET_CONNECTED);

    /* 5. 发布事件（让状态机 + 其他模块自动响应）*/
    event_bus_publish(EVT_WIFI_GOT_IP, NULL);
    ESP_LOGI(TAG, "WiFi 已连（via %s），事件已发布", via_portal ? "配网" : "自动");
}

static void wifi_connect_or_setup()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin();

    Serial.printf("[%s] 正在连接 WiFi...\n", TAG);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300); Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[%s] WiFi 已连接: IP=%s RSSI=%d dBm\n",
                 TAG, WiFi.localIP().toString().c_str(), WiFi.RSSI());
        s_on_wifi_connected(false);
        return;
    }

    /* WiFiManager 配网 */
    Serial.printf("[%s] WiFi 连接失败，启动配网热点\n", TAG);
    oled_set_wifi(false);
    oled_show_line("配网中", 0);
    tts_hub_say_async(TTS_PHRASE_PORTAL_START);

    WiFiManager wm;
    wm.setTimeout(120);
    wm.autoConnect("小智配网", "12345678");

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[%s] 配网成功\n", TAG);
        s_on_wifi_connected(true);
    } else {
        Serial.printf("[%s] 配网超时，重启\n", TAG);
        delay(1000);
        ESP.restart();
    }
}

/* ================================================================
 *  Arduino 入口 —— 初始化顺序见文件顶部注释
 * ================================================================ */
void setup()
{
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_INFO);
    delay(500);

    Serial.println();
    Serial.printf("=== ESP32-S3 智能语音终端 ===\n");
    Serial.printf("  芯片: %s  堆: %u  PSRAM: %u\n",
             ESP.getChipModel(),
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getFreePsram());

    /* 1. 板级初始化（board_init_devices 内部已 secret_store_init + oled + flash + i2s + tts_hub_prepare）*/
    board_log_pinout();
    board_init_devices();

    /* 1.5 功放自检（仅功放开启时才做）*/
#if AUDIO_AMP_ENABLE
    Serial.printf("[%s] 功放自检...\n", TAG);
    if (max98357_is_ready()) {
        max98357_play_beep();
        delay(100);
        max98357_play_beep();
        Serial.printf("[%s] 功放自检完成\n", TAG);
    }
#endif

    /* 2. 麦克风按键开关 */
    app_mic_switch_init();

    /* ===== 规则 8/9：事件总线 + 状态机（必须在 WiFi/KWS 之前 init）===== */
    event_bus_init();
    main_fsm_init();

    /* 3. 唤醒引擎（讯飞 IAT 在线 / WakeNet 离线自动选）*/
#if KWS_ENABLE
    Serial.printf("[%s] 启动唤醒引擎...\n", TAG);
    kws_hub_set_callback(wake_cb, NULL);
    esp_err_t we = kws_hub_init(KWS_ENGINE_NONE);
    if (we != ESP_OK) {
        Serial.printf("[%s] 唤醒引擎初始化失败: %s\n", TAG, esp_err_to_name(we));
    } else {
        Serial.printf("[%s] 唤醒引擎就绪，引擎=%s\n", TAG, kws_hub_engine_name());
    }
#else
    esp_err_t we = ESP_FAIL;
    Serial.printf("[%s] 识别总开关关闭（KWS_ENABLE=0），跳过唤醒引擎\n", TAG);
#endif

    /* 4. 开机播报 + WiFi 连接
     * WiFi 连接成功后会自动 publish EVT_WIFI_GOT_IP → 状态机自动进入 LISTENING */
    tts_hub_say_async(TTS_PHRASE_STARTUP);
    wifi_connect_or_setup();

    /* 5. 等启动播报 + WiFi 播报播完，再开始监听（避免自唤醒）*/
    int w = 0;
    while (tts_hub_is_busy() && w < 150) { delay(100); w++; }
    delay(300);

    /* 6. KWS start（状态机在 FSM_LISTENING 也会 resume，但首次启动我们主动 start）*/
#if KWS_ENABLE
    if (we == ESP_OK) {
        kws_hub_start();
        Serial.printf("[%s] 待机中 —— 说「%s」唤醒  (状态=%s)\n",
                      TAG, kws_hub_get_wake_word(), main_fsm_state_name(main_fsm_state()));
    } else {
        Serial.printf("[%s] 待机中（识别已关闭）\n", TAG);
    }
#endif
}

/* ================================================================
 *  主循环 —— 只做 WiFi 状态检测 + publish 事件
 *  TTS↔监听协调由状态机自动处理（规则 8）
 * ================================================================ */
void loop()
{
    static unsigned long last_check = 0;
    static bool prev_up = false;   /* 规则 5：bool prev_xxx + if 变化 → 状态模式信号，但
                                    *         这里 WiFi 检测是周期性轮询而非业务流程状态，
                                    *         保持简单 bool 即可，业务流程状态由 main_fsm 管理 */

    if (millis() - last_check > 10000) {
        last_check = millis();

        bool up = (WiFi.status() == WL_CONNECTED);
        tts_hub_set_online(up);

        if (up != prev_up) {
            prev_up = up;
            if (up) {
                /* 重连成功（非正常启动路径）*/
                ESP_LOGI(TAG, "WiFi 恢复连接");
                oled_set_wifi(true);
                oled_show_line("WiFi已连接", 0);
                tts_hub_say_async(TTS_PHRASE_NET_CONNECTED);
                event_bus_publish(EVT_WIFI_GOT_IP, NULL);
            } else {
                ESP_LOGW(TAG, "WiFi 断开");
                oled_set_wifi(false);
                oled_show_line("WiFi断开", 0);
                tts_hub_say_async(TTS_PHRASE_NET_DISCONNECTED);
                event_bus_publish(EVT_WIFI_DISCONNECTED, NULL);
                WiFi.reconnect();
            }
        }
    }

    delay(50);
}
