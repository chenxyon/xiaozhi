/* tts_hub —— 厂商无关的统一入口
 * 播放侧：收到驱动回调的 PCM（16k）后线性重采样到喇叭采样率再送输出槽。
 *
 * 策略：
 *   - 固定话术 tts_hub_say()：有内置离线片段→直接播（离线、低延迟）；
 *     无片段→在线则走云 TTS，离线则仅日志提示。
 *   - 任意文本 tts_hub_speak()：走云 TTS（需联网）。
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "tts_hub.h"
#include "tts_xfyun.h"

#define TTS_SRC_HZ 16000                    /* 云 TTS / 离线片段统一输入采样率 */
#define TTS_SEG_MAX_BYTES  1200             /* 单次云合成最大文本字节数 */

static const char *TAG = "tts_hub";   /* 规则 4-8 */

/* PCM 输出槽：由 tts_hub_init() 注入，未注入时 speak/say 直接失败 */
static tts_pcm_sink_t s_sink = NULL;
static uint32_t       s_out_rate = 22050;
static volatile bool  s_online = true;
static tts_provider_t s_provider = TTS_PROVIDER_XFYUN;

/* 厂商表：新增厂商时加一行 + 实现对应 tts_drv_xxx 即可 */
static const tts_driver_t *s_drvs[] = {
    [TTS_PROVIDER_XFYUN] = &tts_drv_xfyun,
    [TTS_PROVIDER_BAIDU] = NULL,
    [TTS_PROVIDER_ALI]   = NULL,
};

static const tts_driver_t *active_drv(void)
{
    int p = (int)s_provider;
    if (p < 0 || p >= (int)(sizeof(s_drvs) / sizeof(s_drvs[0])))
        return NULL;
    return s_drvs[p];
}

esp_err_t tts_hub_init(tts_pcm_sink_t sink, uint32_t output_rate_hz,
                       tts_provider_t provider, const void *cfg)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    s_sink = sink;
    if (output_rate_hz) s_out_rate = output_rate_hz;
    s_provider = provider;

    const tts_driver_t *d = active_drv();
    if (!d) {
        ESP_LOGE(TAG, "厂商未实现 (id=%d)", (int)provider);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "当前厂商: %s  输出采样率=%lu", d->name, (unsigned long)s_out_rate);
    /* 注意：驱动 init 可能阻塞（如讯飞等 SNTP），调用方要选好时机 */
    return d->init ? d->init(cfg) : ESP_OK;
}

/* tts_hub_init 的轻量版：只注册 sink + provider，不调驱动 init。
 * 用于 WiFi 未连上但需要播离线片段的场景。驱动 init 稍后用 tts_hub_driver_init 补。 */
esp_err_t tts_hub_prepare(tts_pcm_sink_t sink, uint32_t output_rate_hz,
                          tts_provider_t provider)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    s_sink = sink;
    if (output_rate_hz) s_out_rate = output_rate_hz;
    s_provider = provider;
    ESP_LOGI(TAG, "tts_hub 基础就绪（未初始化云驱动）  输出采样率=%lu", (unsigned long)s_out_rate);
    return ESP_OK;
}

/* 补做驱动 init（需要网络就绪后调用） */
esp_err_t tts_hub_driver_init(const void *cfg)
{
    const tts_driver_t *d = active_drv();
    if (!d) return ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "初始化云驱动: %s", d->name);
    return d->init ? d->init(cfg) : ESP_OK;
}

const char *tts_hub_provider_name(void)
{
    const tts_driver_t *d = active_drv();
    return d ? d->name : "none";
}

bool tts_hub_is_busy(void)
{
    const tts_driver_t *d = active_drv();
    return d && d->is_busy ? d->is_busy() : false;
}

void tts_hub_set_online(bool online) { s_online = online; }
bool tts_hub_online(void)            { return s_online; }

/* 播放 16k s16le 单声道 PCM：需要时重采样到 s_out_rate 后分片送输出槽 */
static void play_pcm16(const int16_t *in, size_t samples)
{
    if (!in || samples == 0) return;
    if (!s_sink) {
        ESP_LOGE(TAG, "未注册 PCM 输出槽");
        return;
    }

    /* 采样率一致：直通，零开销、零失真（board 采样率 16000 时走这里） */
    if (s_out_rate == TTS_SRC_HZ) {
        const size_t CHUNK = 2048;
        for (size_t o = 0; o < samples; o += CHUNK) {
            size_t n = (samples - o) > CHUNK ? CHUNK : (samples - o);
            s_sink(in + o, n * sizeof(int16_t));
        }
        return;
    }

    const uint32_t DST = s_out_rate;
    size_t out_n = samples * DST / TTS_SRC_HZ;
    int16_t *out = malloc(out_n * sizeof(int16_t));
    if (!out) {  /* 内存不足则按原采样直接送输出槽（会变调，仅保底） */
        s_sink(in, samples * sizeof(int16_t));
        return;
    }
    for (size_t i = 0; i < out_n; i++) {
        float pos = (float)i * TTS_SRC_HZ / DST;
        size_t idx = (size_t)pos;
        float frac = pos - idx;
        int16_t a = in[idx < samples ? idx : samples - 1];
        int16_t b = in[(idx + 1) < samples ? idx + 1 : samples - 1];
        out[i] = (int16_t)(a + (b - a) * frac);
    }
    const size_t CHUNK = 2048;
    for (size_t o = 0; o < out_n; o += CHUNK) {
        size_t n = (out_n - o) > CHUNK ? CHUNK : (out_n - o);
        s_sink(out + o, n * sizeof(int16_t));
    }
    free(out);
}

/* 云 TTS 解出 PCM 的回调（src 固定 16k） */
static void cloud_play_cb(const int16_t *in, size_t samples,
                          uint32_t src_hz, void *ctx)
{
    (void)src_hz; (void)ctx;
    play_pcm16(in, samples);
}

/* ==================== 长文本分段 ==================== */

/* 返回 UTF-8 字符字节数（按首字节判断，非法则当 1 处理） */
static size_t u8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 若 p 处开始是一个"句读标点"，返回其字节数；否则 0。
 * 分段时在标点之后断开，朗读更自然。 */
static size_t punct_len(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    if (u[0] == '\n' || u[0] == '.' || u[0] == '!' || u[0] == '?' ||
        u[0] == ';' || u[0] == ',' || u[0] == ' ') {
        return 1;
    }
    if (u[0] == 0xE3 && u[1] == 0x80 && (u[2] == 0x82 || u[2] == 0x81)) return 3; /* 。、 */
    if (u[0] == 0xEF && u[1] == 0xBC) {                                            /* ，！？；： */
        unsigned char c = u[2];
        if (c == 0x8C || c == 0x81 || c == 0x9F || c == 0x9B || c == 0x9A) return 3;
    }
    return 0;
}

/* 从 s 中取一段，长度不超过 maxb：优先在标点后断开，否则按 UTF-8 边界硬切。
 * 返回该段字节数（至少 1 个字符）。 */
static size_t next_segment_len(const char *s, size_t total, size_t maxb)
{
    if (total <= maxb) return total;

    size_t cut = 0, i = 0;
    while (i < total && i < maxb) {
        size_t pl = punct_len(s + i);
        size_t cl = pl ? pl : u8_len((unsigned char)s[i]);
        if (pl) cut = i + pl;              /* 分割点 = 标点之后 */
        i += cl;
    }
    if (cut > 0) return cut;

    size_t c = 0;                          /* 无标点：UTF-8 边界硬切 */
    while (c < total) {
        size_t cl = u8_len((unsigned char)s[c]);
        if (c + cl > maxb) break;
        c += cl;
    }
    return c ? c : u8_len((unsigned char)s[0]);
}

esp_err_t tts_hub_speak(const char *text)
{
    const tts_driver_t *d = active_drv();
    if (!d || !d->speak) return ESP_ERR_NOT_SUPPORTED;
    if (!s_sink) {
        ESP_LOGE(TAG, "未注册 PCM 输出槽");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_online) {
        ESP_LOGW(TAG, "离线，无法合成任意文本: %s", text);
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = strlen(text);
    if (total == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t last = ESP_OK;
    size_t off = 0;
    int seg = 0;
    while (off < total && s_online) {
        size_t len = next_segment_len(text + off, total - off, TTS_SEG_MAX_BYTES);
        if (len == 0) break;

        char segbuf[TTS_SEG_MAX_BYTES + 4];
        memcpy(segbuf, text + off, len);
        segbuf[len] = '\0';

        if (seg > 0) vTaskDelay(pdMS_TO_TICKS(200));  /* 让上一段 TLS 收尾再开下一段 */
        ESP_LOGI(TAG, "[%s] 分段合成 %d：%u 字节", d->name, seg, (unsigned)len);
        esp_err_t r = d->speak(segbuf, cloud_play_cb, NULL);
        if (r != ESP_OK) {
            last = r;
            ESP_LOGW(TAG, "第 %d 段合成失败(%s)，继续后续段", seg, esp_err_to_name(r));
        }
        off += len;
        seg++;
    }

    ESP_LOGI(TAG, "[%s] 播放%s（共 %d 段）", d->name,
             last == ESP_OK ? "完成" : "有失败", seg);
    return last;
}

static bool offline_have(tts_phrase_t ph)
{
    return ph < TTS_PHRASE_MAX && tts_clips[ph].data && tts_clips[ph].len >= 2;
}

bool tts_hub_has_clip(tts_phrase_t ph) { return offline_have(ph); }

esp_err_t tts_hub_say(tts_phrase_t ph)
{
    if (ph >= TTS_PHRASE_MAX) return ESP_ERR_INVALID_ARG;

    /* 1) 离线片段优先：离线、低延迟、断网也能播 */
    if (offline_have(ph)) {
        ESP_LOGI(TAG, "离线片段播放: %s", tts_phrase_text(ph));
        play_pcm16((const int16_t *)tts_clips[ph].data, tts_clips[ph].len / 2);
        return ESP_OK;
    }
    /* 2) 无片段：在线走云 TTS，离线仅日志 */
    const char *text = tts_phrase_text(ph);
    if (s_online) {
        ESP_LOGI(TAG, "无离线片段，回退云 TTS: %s", text);
        return tts_hub_speak(text);
    }
    ESP_LOGW(TAG, "离线且无片段，跳过: %s", text);
    return ESP_ERR_NOT_FOUND;
}

/* ==================== 异步封装 ==================== */

typedef struct { char *text; tts_phrase_t ph; bool is_phrase; } async_arg_t;

static void async_task(void *arg)
{
    async_arg_t *a = arg;
    if (a->is_phrase) tts_hub_say(a->ph);
    else              tts_hub_speak(a->text);
    free(a->text);
    free(a);
    vTaskDelete(NULL);
}

static esp_err_t spawn(async_arg_t *a)
{
    /* 16KB：xfyun_speak 里 s_url[2600] + cJSON + TLS buffer 较占栈 */
    if (xTaskCreate(async_task, "tts_hub", 16384, a, 6, NULL) != pdPASS) {
        free(a->text); free(a);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tts_hub_speak_async(const char *text)
{
    ESP_LOGI(TAG, "tts_hub_speak_async(\"%s\") 开始, busy=%d, online=%d, sink=%p",
             text ? text : "(null)",
             tts_hub_is_busy() ? 1 : 0,
             s_online ? 1 : 0,
             (void *)s_sink);
    if (tts_hub_is_busy()) {
        ESP_LOGW(TAG, "tts_hub_speak_async 被拒：busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sink) {
        ESP_LOGE(TAG, "tts_hub_speak_async 被拒：PCM 输出槽未注册（tts_hub_init 有没有被调？）");
        return ESP_ERR_INVALID_STATE;
    }
    async_arg_t *a = malloc(sizeof(*a));
    if (!a) { ESP_LOGE(TAG, "tts_hub_speak_async malloc 失败"); return ESP_ERR_NO_MEM; }
    a->text = strdup(text); a->ph = 0; a->is_phrase = false;
    if (!a->text) { free(a); ESP_LOGE(TAG, "tts_hub_speak_async strdup 失败"); return ESP_ERR_NO_MEM; }
    esp_err_t r = spawn(a);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "tts_hub_speak_async spawn 失败: %s", esp_err_to_name(r));
        free(a->text); free(a);
    } else {
        ESP_LOGI(TAG, "tts_hub_speak_async 任务 spawn 成功，堆剩余=%u",
                 (unsigned)esp_get_free_heap_size());
    }
    return r;
}

esp_err_t tts_hub_say_async(tts_phrase_t ph)
{
    if (tts_hub_is_busy()) return ESP_ERR_INVALID_STATE;
    async_arg_t *a = malloc(sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    a->text = NULL; a->ph = ph; a->is_phrase = true;
    return spawn(a);
}
