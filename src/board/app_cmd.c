/**
 * @file     app_cmd.c
 * @brief    本地指令解析实现
 *
 * 调用方：app_wake.c 的 KWS_EV_TEXT 分支，在 app_dir_matches / app_ai_qa_async 之前优先调用。
 *
 * 当前指令集：
 *   音量控制：
 *     "音量" + (调大|增大|放大|加|向上)        → +10
 *     "音量" + (调小|减小|降低|减|向下)        → -10
 *     "音量" + (最大|最高|开到头|最大声)       → 100
 *     "音量" + (最小|最低|最小声)             → 10
 *     "(静音|关掉声音|没声音)"               → 0
 *     "音量" + 阿拉伯数字                     → 数值本身（0~100）
 *     "音量" + 中文数字（如五十、八十、百分之五十） → 数值
 *
 * 设计注意：
 *   - "已静音" 话术在生效前播报，否则设到 0 就听不到了；
 *     实现方式：异步播完 → 回调里再设 volume=0。
 *   - 普通调音量（非 0）：先设音量 → 再播报反馈（播报本身就是对的音量）。
 *   - PCM 缩放已下沉到 max98357_play()，所有声音全局生效。
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "tts_hub.h"
#include "tts_phrases.h"
#include "max98357_bridge.h"
#include "app_cmd.h"

static const char *TAG = "app_cmd";   /* 规则 4-8 */

/* ============ 辅助：子串匹配（子串必须完整，允许不区分前后缀） ============ */
static bool contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    return strstr(hay, needle) != NULL;
}

/* ============ 中文数字解析 ============
 * 支持：一~九、十（整十 / 十几 / 几十X）、百
 * 返回 -1 表示没数字
 *
 * 例：
 *   "五"       → 5
 *   "二十"     → 20
 *   "二十五"   → 25
 *   "八十"     → 80
 *   "一百"     → 100
 *   "百分之五十" → 50（"百分之"前缀外部剥掉）
 */
static int chinese_number(const char *s)
{
    if (!s || !s[0]) return -1;

    /* 单字 0~9 */
    static const char *digits = "零〇一二三四五六七八九";
    /* 查是否单字数字开头 */
    if (strlen(s) == 1) {
        const char *p = strchr(digits, s[0]);
        if (p) return (int)(p - digits);
        /* "十"单独出现 = 10 */
        if (s[0] == '十') return 10;
        return -1;
    }

    /* 十位：十/二十/三十...九十一 */
    int result = 0;
    int i = 0;

    /* 百位简化处理："一百" → 100 */
    if (s[0] == '一' && s[1] == '百') return 100;

    /* 十位：先找"十"的位置 */
    const char *shi = strchr(s, '十');
    if (shi) {
        /* 十位系数：十前面的字（"二十"的"二"、"十五"没有 → 1） */
        if (shi > s) {
            const char *p = strchr(digits, *(shi - 1));
            result = p ? (int)(p - digits) : 1;
        } else {
            result = 1;  /* "十X" 开头，十位系数 1 */
        }
        result *= 10;
        /* 个位：十后面还有字吗？ */
        const char *ge = shi + 1;
        if (*ge && *ge != '零') {
            const char *p = strchr(digits, *ge);
            if (p) result += (int)(p - digits);
        }
        return result;
    }

    return -1;
}

/* 从文本里提取数字（阿拉伯 OR 中文），返回 0~100；-1 表示没找到 */
static int extract_number(const char *text)
{
    if (!text) return -1;

    /* 1) 阿拉伯数字扫描 */
    for (const char *p = text; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            int v = atoi(p);
            if (v >= 0 && v <= 100) return v;
        }
    }

    /* 2) 中文数字扫描：遍历可能的子串尝试解析 */
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        /* 跳过"百分之"前缀 */
        if (strncmp(text + i, "百分之", 3) == 0) i += 2;

        /* 尝试从当前位置开始 1~3 个字解析 */
        for (int j = 1; j <= 3 && i + j <= len; j++) {
            char tmp[4] = {0};
            memcpy(tmp, text + i, j);
            int v = chinese_number(tmp);
            if (v >= 0 && v <= 100) return v;
        }
    }

    return -1;
}

/* ============ 执行层 ============ */

/* ============ 内部任务：等静音话术播完再归零 ============ */
static void cmd_mute_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 80 && tts_hub_is_busy(); i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    max98357_set_volume(0);
    vTaskDelete(NULL);
}

/* 静音执行：先播完"已静音"，再设 volume=0（否则听不到） */
static void do_mute_async(void)
{
    int vol = max98357_get_volume();
    if (vol == 0) {
        tts_hub_say_async(TTS_PHRASE_VOLUME_ZERO);
        return;
    }
    /* 先设临时音量 10% 让话术能播出来，播完再归零 */
    max98357_set_volume(10);
    tts_hub_say_async(TTS_PHRASE_VOLUME_ZERO);

    /* 等话术播完（最多 8s）再真正归零 */
    xTaskCreate(cmd_mute_task, "cmd_mute", 2048, NULL, 4, NULL);
}

/* 普通调音量：直接设值 + 播反馈 */
static void do_set_volume(int target)
{
    int cur = max98357_get_volume();
    if (cur == target) {
        ESP_LOGI(TAG, "音量已是 %d%%，不重复", target);
        return;
    }
    max98357_set_volume(target);

    if (target == 100) {
        tts_hub_say_async(TTS_PHRASE_VOLUME_MAX);
    } else if (target <= 10) {
        tts_hub_say_async(TTS_PHRASE_VOLUME_MIN);
    } else if (target > cur) {
        tts_hub_say_async(TTS_PHRASE_VOLUME_UP);
    } else {
        tts_hub_say_async(TTS_PHRASE_VOLUME_DOWN);
    }
}

/* ============ 入口 ============ */

bool app_cmd_match(const char *text)
{
    if (!text || !text[0]) return false;

    ESP_LOGI(TAG, "指令解析: %s", text);

#if !AUDIO_AMP_ENABLE
    /* 功放禁用：音量/静音指令无意义，静默跳过 */
    if (contains(text, "音量") || contains(text, "静音") ||
        contains(text, "关掉声音") || contains(text, "没声音")) {
        ESP_LOGI(TAG, "功放已禁用，忽略音量指令: %s", text);
        return true;
    }
    return false;
#else
    /* ---- 静音 ---- */
    if (contains(text, "静音") || contains(text, "关掉声音") ||
        (contains(text, "没声音") && !contains(text, "没声音了"))) {
        do_mute_async();
        return true;
    }

    /* ---- 不含"音量"关键词的不是音量指令 ---- */
    if (!contains(text, "音量")) return false;

    /* ---- 极限值 ---- */
    if (contains(text, "最大") || contains(text, "最高") ||
        contains(text, "开到头") || contains(text, "最大声")) {
        do_set_volume(100);
        return true;
    }
    if (contains(text, "最小") || contains(text, "最低") || contains(text, "最小声")) {
        do_set_volume(10);   /* 不设 0：避免"音量已最小"播完突然没声 */
        return true;
    }

    /* ---- 数字："音量 80"、"音量调到八十"、"音量百分之五十" ---- */
    int num = extract_number(text);
    if (num >= 0) {
        if (num == 0) {
            do_mute_async();
        } else {
            do_set_volume(num);
        }
        return true;
    }

    /* ---- 纯增减 ---- */
    if (contains(text, "调大") || contains(text, "增大") ||
        contains(text, "放大") || contains(text, "向上") ||
        (contains(text, "加") && contains(text, "音量"))) {
        do_set_volume(max98357_get_volume() + 10);
        return true;
    }
    if (contains(text, "调小") || contains(text, "减小") ||
        contains(text, "降低") || contains(text, "向下") ||
        (contains(text, "减") && contains(text, "音量"))) {
        do_set_volume(max98357_get_volume() - 10);
        return true;
    }

    ESP_LOGI(TAG, "不是音量指令");
    return false;
#endif
}
